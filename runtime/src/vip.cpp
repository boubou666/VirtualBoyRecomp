// VIP: Virtual Boy video processor.
//
// Timing model (20 ms / 400000-cycle frames):
//   frame start   FRAMESTART; every FRMCYC+1 frames GAMESTART, and if drawing is
//                 enabled the frame buffer pairs swap and drawing begins
//   drawing       28 blocks of 8 rows, rendered progressively; XPEND at the end
//   display       left eye 3-8 ms (LFBEND), right eye 13-18 ms (RFBEND)
//   frame end     the displayed buffers are handed to the host
//
// VRAM layout:
//   0x00000 left FB 0   0x06000 CHR 0-511     0x08000 left FB 1   0x0E000 CHR 512-1023
//   0x10000 right FB 0  0x16000 CHR 1024-1535 0x18000 right FB 1  0x1E000 CHR 1536-2047
//   0x20000 BG maps / parameter tables, 0x3D800 world attributes, 0x3DC00 column
//   tables, 0x3E000 object attributes. Frame buffers are column-major, 2 bpp,
//   64 bytes (256 rows) per column.

#include "system.h"
#include "vbrt/cpu.h"
#include "vbrt/memory.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace vb {

namespace {

constexpr int SCREEN_W = 384, SCREEN_H = 224;
constexpr int64_t FRAME_CYCLES = CPU_CLOCK / 50;
constexpr int64_t LFB_START = 60000, LFB_END = 160000;
constexpr int64_t RFB_START = 260000, RFB_END = 360000;
constexpr int64_t FCLK_LOW = 200000;
constexpr int64_t BLOCK_CYCLES = 3000;
constexpr int BLOCKS = SCREEN_H / 8;

enum : uint16_t {
    INT_SCANERR = 0x0001, INT_LFBEND = 0x0002, INT_RFBEND = 0x0004, INT_GAMESTART = 0x0008,
    INT_FRAMESTART = 0x0010, INT_SBHIT = 0x2000, INT_XPEND = 0x4000, INT_TIMEERR = 0x8000,
};

struct State {
    uint16_t intpnd, intenb;
    uint16_t dpctrl, xpctrl;
    uint8_t brt[3], rest, frmcyc, bkcol;
    uint16_t spt[4];
    uint8_t gplt[4], jplt[4];

    int64_t frame_start;
    int frame_counter;
    int draw_fb, display_fb;
    bool drawing;
    int64_t draw_start;
    int draw_block;
    bool lfb_done, rfb_done;
    bool first_frame;
} s;

inline int32_t sext(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return (int32_t)((v ^ m) - m);
}

inline uint16_t vr16(uint32_t a) {
    return load16(g_vram + (a & 0x3FFFE));
}

// ---------------------------------------------------------------------------
// Rendering

struct Target {
    int y0, y1;                 // rows being drawn [y0, y1)
    uint8_t pix[8][SCREEN_W];   // 2-bit color per pixel
    void put(int x, int y, int c) { pix[y - y0][x] = (uint8_t)c; }
};

// Character pixel (0 = transparent) for a BG/OBJ cell at pixel (px, py).
inline int char_pixel(uint16_t cell, int px, int py) {
    if (cell & 0x2000) px = 7 - px;
    if (cell & 0x1000) py = 7 - py;
    const uint32_t ch = cell & 0x7FF;
    const uint32_t addr = 0x6000 + (ch >> 9) * 0x8000 + (ch & 511) * 16 + py * 2;
    return (vr16(addr) >> (px * 2)) & 3;
}

struct BgLayout {
    int scx, scy, base;
    bool overplane;
    uint16_t overCell;
    int wpx, hpx;
};

inline int bg_sample(const BgLayout& L, int sx, int sy) {
    uint16_t cell;
    if (L.overplane && (sx < 0 || sx >= L.wpx || sy < 0 || sy >= L.hpx)) {
        cell = L.overCell;
    } else {
        sx &= L.wpx - 1;
        sy &= L.hpx - 1;
        const int map = (L.base + ((sy >> 9) << L.scx) + (sx >> 9)) & 15;
        cell = vr16(0x20000 + map * 0x2000 + ((sy >> 3) & 63) * 128 + ((sx >> 3) & 63) * 2);
    }
    const int v = char_pixel(cell, sx & 7, sy & 7);
    if (!v) return -1;
    return (s.gplt[cell >> 14] >> (v * 2)) & 3;
}

void draw_bg_world(int eye, const uint16_t* w, Target& t) {
    const int bgm = (w[0] >> 12) & 3;
    BgLayout L;
    L.scx = (w[0] >> 10) & 3;
    L.scy = (w[0] >> 8) & 3;
    L.base = w[0] & 15;
    L.overplane = w[0] & 0x80;
    L.overCell = vr16(0x20000 + (uint32_t)w[10] * 2);
    L.wpx = 512 << L.scx;
    L.hpx = 512 << L.scy;

    const int gx = sext(w[1], 10), gp = sext(w[2], 10), gy = (int16_t)w[3];
    const int mx = sext(w[4], 13), mp = sext(w[5], 15), my = sext(w[6], 13);
    const int width = (w[7] & 0x1FFF) + 1, height = w[8] + 1;
    const uint32_t param = 0x20000 + (uint32_t)w[9] * 2;

    const int x0 = gx + (eye ? gp : -gp);
    const int xs = std::max(0, x0), xe = std::min(SCREEN_W, x0 + width);
    const int ys = std::max(t.y0, gy), ye = std::min(t.y1, gy + height);
    if (xs >= xe || ys >= ye) return;

    for (int y = ys; y < ye; y++) {
        const int row = y - gy;
        if (bgm == 2) {
            // Affine: per-row parameters MX, MP, MY (13.3) and DX, DY (7.9).
            const uint32_t e = param + (uint32_t)row * 16;
            const int32_t amx = (int16_t)vr16(e), amp = (int16_t)vr16(e + 2), amy = (int16_t)vr16(e + 4);
            const int32_t dx = (int16_t)vr16(e + 6), dy = (int16_t)vr16(e + 8);
            const int shift = ((eye == 0 && amp < 0) || (eye == 1 && amp >= 0)) ? std::abs(amp) : 0;
            for (int x = xs; x < xe; x++) {
                const int32_t i = x - x0 + shift;
                const int32_t sx = ((amx << 6) + dx * i) >> 9;
                const int32_t sy = ((amy << 6) + dy * i) >> 9;
                const int c = bg_sample(L, sx, sy);
                if (c >= 0) t.put(x, y, c);
            }
        } else {
            int hofs = 0;
            if (bgm == 1) hofs = sext(vr16(param + (uint32_t)row * 4 + (eye ? 2 : 0)), 13);
            const int sy = my + row;
            const int sx0 = mx + (eye ? mp : -mp) + hofs - x0;
            for (int x = xs; x < xe; x++) {
                const int c = bg_sample(L, sx0 + x, sy);
                if (c >= 0) t.put(x, y, c);
            }
        }
    }
}

void draw_obj_world(int eye, int group, Target& t) {
    const int start = s.spt[group] & 1023;
    const int end = group > 0 ? ((s.spt[group - 1] + 1) & 1023) : 0;
    int i = start;
    for (int n = 0; n < 1024; n++) {
        const uint32_t o = 0x3E000 + (uint32_t)i * 8;
        const uint16_t w1 = vr16(o + 2);
        if (w1 & (eye ? 0x4000 : 0x8000)) {
            const int jx = sext(vr16(o), 10), jp = sext(w1, 10);
            int jy = vr16(o + 4) & 0xFF;
            if (jy >= 224) jy -= 256;
            const uint16_t cell = vr16(o + 6);
            const int x0 = jx + (eye ? jp : -jp);
            const int pal = s.jplt[cell >> 14];
            for (int y = std::max(t.y0, jy); y < std::min(t.y1, jy + 8); y++) {
                for (int px = 0; px < 8; px++) {
                    const int x = x0 + px;
                    if (x < 0 || x >= SCREEN_W) continue;
                    const int v = char_pixel(cell, px, y - jy);
                    if (v) t.put(x, y, (pal >> (v * 2)) & 3);
                }
            }
        }
        if (i == end) break;
        i = (i - 1) & 1023;
    }
}

void render_block(int block) {
    Target t;
    t.y0 = block * 8;
    t.y1 = t.y0 + 8;
    for (int eye = 0; eye < 2; eye++) {
        std::memset(t.pix, s.bkcol & 3, sizeof t.pix);
        int group = 3;
        for (int wi = 31; wi >= 0; wi--) {
            uint16_t w[16];
            for (int k = 0; k < 16; k++) w[k] = vr16(0x3D800 + wi * 32 + k * 2);
            if (w[0] & 0x40) break; // END
            if (!(w[0] & 0xC000)) continue;
            const bool on = w[0] & (eye ? 0x4000 : 0x8000);
            if (((w[0] >> 12) & 3) == 3) {
                if (on) draw_obj_world(eye, group, t);
                group = (group - 1) & 3;
            } else if (on) {
                draw_bg_world(eye, w, t);
            }
        }
        // Pack into the frame buffer being drawn: 2 bytes (8 rows) per column.
        uint8_t* fb = g_vram + (eye ? 0x10000 : 0) + s.draw_fb * 0x8000 + t.y0 / 4;
        for (int x = 0; x < SCREEN_W; x++) {
            for (int b = 0; b < 2; b++) {
                const int r = b * 4;
                fb[x * 64 + b] = (uint8_t)(t.pix[r][x] | (t.pix[r + 1][x] << 2) | (t.pix[r + 2][x] << 4) |
                                           (t.pix[r + 3][x] << 6));
            }
        }
    }
}

// ---------------------------------------------------------------------------

void start_frame(int64_t t) {
    s.frame_start = t;
    s.lfb_done = s.rfb_done = false;
    s.intpnd |= INT_FRAMESTART;
    if (++s.frame_counter > s.frmcyc) {
        s.frame_counter = 0;
        s.intpnd |= INT_GAMESTART;
        if (s.xpctrl & 0x0002) {
            s.draw_fb ^= 1;
            s.display_fb = s.draw_fb ^ 1;
            s.drawing = true;
            s.draw_start = t;
            s.draw_block = 0;
        }
    }
}

} // namespace

void vip_reset() {
    s = State{};
    s.first_frame = true;
    s.frame_counter = 0;
    s.draw_fb = 1;
    s.display_fb = 0;
    start_frame(0);
    s.intpnd = 0;
}

int64_t vip_update(int64_t now) {
    for (;;) {
        enum { EV_FRAME, EV_BLOCK, EV_LFB, EV_RFB } ev = EV_FRAME;
        int64_t t = s.frame_start + FRAME_CYCLES;
        if (s.drawing) {
            const int64_t tb = s.draw_start + (s.draw_block + 1) * BLOCK_CYCLES;
            if (tb < t) { t = tb; ev = EV_BLOCK; }
        }
        if (!s.lfb_done && s.frame_start + LFB_END < t) { t = s.frame_start + LFB_END; ev = EV_LFB; }
        if (!s.rfb_done && s.frame_start + RFB_END < t) { t = s.frame_start + RFB_END; ev = EV_RFB; }
        if (t > now) return t;

        switch (ev) {
        case EV_BLOCK: {
            render_block(s.draw_block);
            if (s.draw_block == ((s.xpctrl >> 8) & 0x1F)) s.intpnd |= INT_SBHIT;
            if (++s.draw_block >= BLOCKS) {
                s.drawing = false;
                s.intpnd |= INT_XPEND;
            }
            break;
        }
        case EV_LFB:
            s.lfb_done = true;
            if (s.dpctrl & 0x0002) s.intpnd |= INT_LFBEND;
            break;
        case EV_RFB:
            s.rfb_done = true;
            if (s.dpctrl & 0x0002) s.intpnd |= INT_RFBEND;
            break;
        case EV_FRAME:
            host_frame();
            start_frame(t);
            break;
        }
    }
}

bool vip_irq() {
    return (s.intpnd & s.intenb) != 0;
}

uint8_t* vip_chr_mirror(uint32_t a) {
    const uint32_t o = a & 0x7FFF;
    return g_vram + 0x6000 + (o >> 13) * 0x8000 + (o & 0x1FFF);
}

uint16_t vip_read16(uint32_t a) {
    const int64_t now = g_cpu.cycles;
    switch (a & 0x7E) {
    case 0x00: vip_update(now); return s.intpnd;
    case 0x02: return s.intenb;
    case 0x20:
    case 0x22: {
        vip_update(now);
        uint16_t v = s.dpctrl & 0x0702;
        const int64_t rel = now - s.frame_start;
        if (s.dpctrl & 0x0002) {
            if (rel >= LFB_START && rel < LFB_END) v |= s.display_fb ? 0x10 : 0x04;
            if (rel >= RFB_START && rel < RFB_END) v |= s.display_fb ? 0x20 : 0x08;
        }
        v |= 0x40; // SCANRDY
        if (rel < FCLK_LOW) v |= 0x80;
        return v;
    }
    case 0x24: return s.brt[0];
    case 0x26: return s.brt[1];
    case 0x28: return s.brt[2];
    case 0x2A: return s.rest;
    case 0x2E: return s.frmcyc;
    case 0x30: return 0;
    case 0x40:
    case 0x42: {
        vip_update(now);
        uint16_t v = s.xpctrl & 0x0002;
        if (s.drawing) {
            v |= s.draw_fb ? 0x0008 : 0x0004;
            v |= 0x8000;
            v |= (uint16_t)(std::min(s.draw_block, BLOCKS - 1) << 8);
        }
        return v;
    }
    case 0x44: return 2; // VIP version
    case 0x48: case 0x4A: case 0x4C: case 0x4E: return s.spt[((a & 0x7E) - 0x48) / 2];
    case 0x60: case 0x62: case 0x64: case 0x66: return s.gplt[((a & 0x7E) - 0x60) / 2];
    case 0x68: case 0x6A: case 0x6C: case 0x6E: return s.jplt[((a & 0x7E) - 0x68) / 2];
    case 0x70: return s.bkcol;
    default: return 0;
    }
}

void vip_write16(uint32_t a, uint16_t v) {
    vip_update(g_cpu.cycles);
    switch (a & 0x7E) {
    case 0x02: s.intenb = v & 0xE01F; reschedule(); break;
    case 0x04: s.intpnd &= ~v; break;
    case 0x22:
        if (v & 0x0001) { // DPRST
            s.intpnd &= ~(INT_SCANERR | INT_LFBEND | INT_RFBEND | INT_GAMESTART | INT_FRAMESTART | INT_TIMEERR);
            s.intenb &= ~(INT_SCANERR | INT_LFBEND | INT_RFBEND | INT_GAMESTART | INT_FRAMESTART | INT_TIMEERR);
        }
        s.dpctrl = v & 0x0702;
        break;
    case 0x24: s.brt[0] = (uint8_t)v; break;
    case 0x26: s.brt[1] = (uint8_t)v; break;
    case 0x28: s.brt[2] = (uint8_t)v; break;
    case 0x2A: s.rest = (uint8_t)v; break;
    case 0x2E: s.frmcyc = v & 0xF; break;
    case 0x42:
        if (v & 0x0001) { // XPRST
            s.intpnd &= ~(INT_SBHIT | INT_XPEND | INT_TIMEERR);
            s.intenb &= ~(INT_SBHIT | INT_XPEND | INT_TIMEERR);
            s.drawing = false;
        }
        s.xpctrl = v & 0x1F02;
        break;
    case 0x48: case 0x4A: case 0x4C: case 0x4E: s.spt[((a & 0x7E) - 0x48) / 2] = v & 0x3FF; break;
    case 0x60: case 0x62: case 0x64: case 0x66: s.gplt[((a & 0x7E) - 0x60) / 2] = v & 0xFC; break;
    case 0x68: case 0x6A: case 0x6C: case 0x6E: s.jplt[((a & 0x7E) - 0x68) / 2] = v & 0xFC; break;
    case 0x70: s.bkcol = v & 3; break;
    default: break;
    }
}

void vip_get_eye(int eye, uint8_t* out) {
    if (!(s.dpctrl & 0x0002)) {
        std::memset(out, 0, SCREEN_W * SCREEN_H);
        return;
    }
    // Brightness: color 1 = BRTA, 2 = BRTB, 3 = BRTA + BRTB + BRTC.
    uint8_t lut[4];
    const int lv[4] = {0, s.brt[0], s.brt[1], s.brt[0] + s.brt[1] + s.brt[2]};
    for (int i = 0; i < 4; i++) lut[i] = (uint8_t)std::min(255, lv[i] * 2);

    const uint8_t* fb = g_vram + (eye ? 0x10000 : 0) + s.display_fb * 0x8000;
    for (int x = 0; x < SCREEN_W; x++) {
        const uint8_t* col = fb + x * 64;
        for (int y = 0; y < SCREEN_H; y++) out[y * SCREEN_W + x] = lut[(col[y >> 2] >> ((y & 3) * 2)) & 3];
    }
}

} // namespace vb
