// Hardware scheduler: keeps the VIP, timer and VSU in step with the CPU cycle
// counter and combines their interrupt lines.

#include "system.h"
#include "vbrt/cpu.h"

#include <algorithm>

namespace vb {

void hw_reset() {
    vip_reset();
    vsu_reset();
    hwctl_reset();
}

int64_t hw_update(int64_t now) {
    const int64_t t = timer_update(now);
    const int64_t v = vip_update(now);
    vsu_update(now);
    return std::min(t, v);
}

void reschedule() {
    if (g_cpu.next_event > g_cpu.cycles) g_cpu.next_event = g_cpu.cycles;
}

int hw_pending_irq() {
    // Level 4: VIP, 3: link port, 2: cartridge, 1: timer, 0: game pad.
    if (vip_irq()) return 4;
    if (timer_irq()) return 1;
    return -1;
}

} // namespace vb
