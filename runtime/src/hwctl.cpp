// Hardware control registers (0x02000000): link port (stubbed), game pad,
// timer and wait control. Registers are 8 bits wide at 4-byte strides.

#include "system.h"

#include <climits>

namespace vb {

namespace {

struct Timer {
    bool enabled = false, zstat = false, inten = false, fast = false; // fast: 20 us ticks
    uint16_t reload = 0, counter = 0xFFFF;
    int64_t last = 0; // cycle of the last processed tick
    int64_t period() const { return fast ? 400 : 2000; }
} g_timer;

uint16_t g_pad_latch = 0x0002;
uint8_t g_scr = 0x00, g_wcr = 0x00;

void timer_tick() {
    if (g_timer.counter == 0) {
        g_timer.counter = g_timer.reload;
    } else if (--g_timer.counter == 0) {
        g_timer.zstat = true;
    }
}

} // namespace

void hwctl_reset() {
    g_timer = Timer{};
    g_pad_latch = 0x0002;
    g_scr = 0;
    g_wcr = 0;
}

int64_t timer_update(int64_t now) {
    if (!g_timer.enabled) {
        g_timer.last = now;
        return INT64_MAX;
    }
    const int64_t p = g_timer.period();
    while (now - g_timer.last >= p) {
        g_timer.last += p;
        timer_tick();
    }
    if (!g_timer.inten || g_timer.zstat) return INT64_MAX;
    const int64_t ticks = g_timer.counter ? g_timer.counter : (int64_t)g_timer.reload + 1;
    return g_timer.last + ticks * p;
}

bool timer_irq() {
    return g_timer.zstat && g_timer.inten;
}

uint8_t hwctl_read(uint32_t a, int64_t now) {
    switch ((a >> 2) & 0xF) {
    case 0x0: return 0x6D;                         // CCR
    case 0x1: return 0x62;                         // CCSR
    case 0x2: return 0x00;                         // CDTR
    case 0x3: return 0x00;                         // CDRR
    case 0x4: return (uint8_t)g_pad_latch;         // SDLR
    case 0x5: return (uint8_t)(g_pad_latch >> 8);  // SDHR
    case 0x6: timer_update(now); return (uint8_t)g_timer.counter;        // TLR
    case 0x7: timer_update(now); return (uint8_t)(g_timer.counter >> 8); // THR
    case 0x8:                                                            // TCR
        timer_update(now);
        return (uint8_t)(0xE4 | (g_timer.enabled ? 1 : 0) | (g_timer.zstat ? 2 : 0) | (g_timer.inten ? 8 : 0) |
                         (g_timer.fast ? 0x10 : 0));
    case 0x9: return (uint8_t)(0xFC | g_wcr);      // WCR
    case 0xA: return (uint8_t)(0x4C | (g_scr & 0x80)); // SCR: never busy
    default: return 0;
    }
}

void hwctl_write(uint32_t a, uint8_t v, int64_t now) {
    switch ((a >> 2) & 0xF) {
    case 0x6:
        timer_update(now);
        g_timer.reload = (uint16_t)((g_timer.reload & 0xFF00) | v);
        g_timer.counter = g_timer.reload;
        reschedule();
        break;
    case 0x7:
        timer_update(now);
        g_timer.reload = (uint16_t)((g_timer.reload & 0x00FF) | (v << 8));
        g_timer.counter = g_timer.reload;
        reschedule();
        break;
    case 0x8: {
        timer_update(now);
        const bool wasEnabled = g_timer.enabled;
        if (v & 0x04) g_timer.zstat = false;
        g_timer.enabled = v & 0x01;
        g_timer.inten = v & 0x08;
        g_timer.fast = v & 0x10;
        if (g_timer.enabled && !wasEnabled) g_timer.last = now;
        reschedule();
        break;
    }
    case 0x9: g_wcr = v & 3; break;
    case 0xA:
        g_scr = v;
        if (v & 0x04) g_pad_latch = host_pad_state(); // hardware read of the controller
        break;
    default: break;
    }
}

} // namespace vb
