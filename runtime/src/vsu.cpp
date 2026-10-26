// VSU: Virtual Boy sound unit. Five wavetable channels (32 x 6-bit samples),
// channel 5 additionally has frequency sweep/modulation, channel 6 is noise.
// The VSU is advanced lazily (on register writes and once per frame) and
// produces 48 kHz stereo samples for the host.

#include "system.h"
#include "vbrt/cpu.h"

#include <algorithm>
#include <vector>

namespace vb {

namespace {

constexpr int64_t DURATION_CYCLES = 76800;  // 3.84 ms
constexpr int64_t ENVELOPE_CYCLES = 307200; // 15.36 ms
constexpr int64_t SWEEP_CYCLES = 19200;     // 0.96 ms (x8 when SxSWP bit 7 is set)

struct Channel {
    uint8_t intCtrl, lrv, ev0, ev1, ram, swp;
    uint16_t freq, effFreq;
    int envelope, envCounter, interval;
    int wavePos;
    int64_t freqCounter;
    // channel 5 only
    int sweepCounter, sweepDivider, modPos;
};

struct State {
    uint8_t wave[5][32];
    int8_t mod[32];
    Channel ch[6];
    uint16_t lfsr;
    int64_t last;
    int64_t durAcc, envAcc, sweepAcc;
    int64_t phase; // sample clock accumulator (units: cycles * AUDIO_RATE)
    float hpL, hpR, prevL, prevR;
} s;

std::vector<int16_t> g_samples; // interleaved stereo
constexpr size_t MAX_BUFFERED = AUDIO_RATE / 2 * 2;

int64_t period(int c) {
    return (2048 - s.ch[c].effFreq) * (c == 5 ? 40 : 4);
}

void start_channel(int c) {
    Channel& ch = s.ch[c];
    ch.effFreq = ch.freq;
    ch.freqCounter = period(c);
    ch.wavePos = 0;
    ch.envelope = ch.ev0 >> 4;
    ch.envCounter = (ch.ev0 & 7) + 1;
    ch.interval = (ch.intCtrl & 0x1F) + 1;
    if (c == 4) {
        ch.sweepCounter = (ch.swp >> 4) & 7;
        ch.sweepDivider = (ch.swp & 0x80) ? 8 : 1;
        ch.modPos = 0;
    }
    if (c == 5) s.lfsr = 1;
}

void duration_tick() {
    for (Channel& ch : s.ch) {
        if ((ch.intCtrl & 0xA0) == 0xA0 && --ch.interval <= 0) ch.intCtrl &= ~0x80;
    }
}

void envelope_tick() {
    for (Channel& ch : s.ch) {
        if (!(ch.intCtrl & 0x80) || !(ch.ev1 & 1)) continue;
        if (--ch.envCounter > 0) continue;
        ch.envCounter = (ch.ev0 & 7) + 1;
        if (ch.ev0 & 8) {
            if (ch.envelope < 15) ch.envelope++;
            else if (ch.ev1 & 2) ch.envelope = ch.ev0 >> 4;
        } else {
            if (ch.envelope > 0) ch.envelope--;
            else if (ch.ev1 & 2) ch.envelope = ch.ev0 >> 4;
        }
    }
}

void sweep_tick() {
    Channel& ch = s.ch[4];
    if (!(ch.intCtrl & 0x80) || !(ch.ev1 & 0x40)) return;
    if (--ch.sweepDivider > 0) return;
    ch.sweepDivider = (ch.swp & 0x80) ? 8 : 1;
    const int interval = (ch.swp >> 4) & 7;
    if (!interval || --ch.sweepCounter > 0) return;
    ch.sweepCounter = interval;
    if (ch.ev1 & 0x10) {
        // Modulation: frequency = base + modulation table entry.
        if (ch.modPos >= 32) {
            if (!(ch.ev1 & 0x20)) return;
            ch.modPos = 0;
        }
        ch.effFreq = (uint16_t)((ch.freq + s.mod[ch.modPos++]) & 0x7FF);
    } else {
        const int delta = ch.effFreq >> (ch.swp & 7);
        const int f = (ch.swp & 8) ? ch.effFreq + delta : ch.effFreq - delta;
        if (f > 2047) ch.intCtrl &= ~0x80;
        else ch.effFreq = (uint16_t)std::max(f, 0);
    }
}

void advance(int64_t cycles) {
    for (int c = 0; c < 6; c++) {
        Channel& ch = s.ch[c];
        if (!(ch.intCtrl & 0x80)) continue;
        ch.freqCounter -= cycles;
        while (ch.freqCounter <= 0) {
            ch.freqCounter += period(c);
            if (c == 5) {
                static const int taps[8] = {14, 10, 13, 4, 8, 6, 9, 11};
                const int fb = ((s.lfsr >> 7) ^ (s.lfsr >> taps[(ch.ev1 >> 4) & 7])) & 1;
                s.lfsr = (uint16_t)(((s.lfsr << 1) & 0x7FFF) | fb);
            } else {
                ch.wavePos = (ch.wavePos + 1) & 31;
            }
        }
    }
    for (s.durAcc += cycles; s.durAcc >= DURATION_CYCLES; s.durAcc -= DURATION_CYCLES) duration_tick();
    for (s.envAcc += cycles; s.envAcc >= ENVELOPE_CYCLES; s.envAcc -= ENVELOPE_CYCLES) envelope_tick();
    for (s.sweepAcc += cycles; s.sweepAcc >= SWEEP_CYCLES; s.sweepAcc -= SWEEP_CYCLES) sweep_tick();
}

void emit_sample() {
    int l = 0, r = 0;
    for (int c = 0; c < 6; c++) {
        const Channel& ch = s.ch[c];
        if (!(ch.intCtrl & 0x80)) continue;
        int sample;
        if (c == 5) sample = (s.lfsr & 1) ? 63 : 0;
        else sample = (ch.ram & 7) < 5 ? s.wave[ch.ram & 7][ch.wavePos] : 0;
        int la = ch.envelope * (ch.lrv >> 4), ra = ch.envelope * (ch.lrv & 15);
        if (la) la = (la >> 3) + 1;
        if (ra) ra = (ra >> 3) + 1;
        l += sample * la;
        r += sample * ra;
    }
    // DC-blocking high-pass filter (VSU output is unsigned).
    const float fl = (float)l, fr = (float)r;
    s.hpL = 0.995f * (s.hpL + fl - s.prevL);
    s.hpR = 0.995f * (s.hpR + fr - s.prevR);
    s.prevL = fl;
    s.prevR = fr;
    auto clip = [](float v) { return (int16_t)std::clamp(v * 5.0f, -32768.0f, 32767.0f); };
    if (g_samples.size() >= MAX_BUFFERED) g_samples.erase(g_samples.begin(), g_samples.begin() + 2);
    g_samples.push_back(clip(s.hpL));
    g_samples.push_back(clip(s.hpR));
}

} // namespace

void vsu_reset() {
    s = State{};
    g_samples.clear();
}

void vsu_update(int64_t now) {
    while (s.last < now) {
        // Cycles until the next output sample.
        const int64_t until = (CPU_CLOCK - s.phase + AUDIO_RATE - 1) / AUDIO_RATE;
        const int64_t step = std::max<int64_t>(1, std::min(until, now - s.last));
        advance(step);
        s.last += step;
        s.phase += step * AUDIO_RATE;
        if (s.phase >= CPU_CLOCK) {
            s.phase -= CPU_CLOCK;
            emit_sample();
        }
    }
}

void vsu_write(uint32_t a, uint8_t v, int64_t now) {
    vsu_update(now);
    const uint32_t o = a & 0x7FF;
    if (o < 0x280) {
        s.wave[o >> 7][(o >> 2) & 31] = v & 63;
    } else if (o < 0x300) {
        s.mod[(o >> 2) & 31] = (int8_t)v;
    } else if (o >= 0x400 && o < 0x580) {
        const int c = (int)((o - 0x400) >> 6);
        Channel& ch = s.ch[c];
        switch ((o >> 2) & 15) {
        case 0:
            ch.intCtrl = v;
            if (v & 0x80) start_channel(c);
            break;
        case 1: ch.lrv = v; break;
        case 2: ch.freq = (uint16_t)((ch.freq & 0x700) | v); ch.effFreq = ch.freq; break;
        case 3: ch.freq = (uint16_t)((ch.freq & 0xFF) | ((v & 7) << 8)); ch.effFreq = ch.freq; break;
        case 4: ch.ev0 = v; ch.envelope = v >> 4; break;
        case 5: ch.ev1 = v; break;
        case 6: ch.ram = v & 7; break;
        case 7: if (c == 4) ch.swp = v; break;
        default: break;
        }
    } else if (o == 0x580) {
        if (v & 1)
            for (Channel& ch : s.ch) ch.intCtrl &= ~0x80;
    }
}

size_t vsu_take_samples(int16_t* out, size_t maxFrames) {
    const size_t frames = std::min(maxFrames, g_samples.size() / 2);
    std::copy(g_samples.begin(), g_samples.begin() + frames * 2, out);
    g_samples.erase(g_samples.begin(), g_samples.begin() + frames * 2);
    return frames;
}

} // namespace vb
