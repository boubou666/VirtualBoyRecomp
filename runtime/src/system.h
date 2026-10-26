// Internal interfaces between runtime modules.
#pragma once

#include <cstdint>
#include <string>

namespace vb {

// ---- Hardware scheduler (system.cpp) ----
// Advances all hardware to `now` and returns the cycle of the next event.
int64_t hw_update(int64_t now);
// Highest pending interrupt level (0-4), or -1.
int hw_pending_irq();
void hw_reset();
// Forces vb::service() to run at the next block boundary (after interrupt-relevant state changes).
void reschedule();

// ---- VIP (vip.cpp) ----
void vip_reset();
int64_t vip_update(int64_t now); // returns next VIP event time
bool vip_irq();
uint16_t vip_read16(uint32_t a);
void vip_write16(uint32_t a, uint16_t v);
uint8_t* vip_chr_mirror(uint32_t a); // 0x78000-0x7FFFF CHR window -> VRAM pointer
// Converts the displayed frame buffer of one eye into 8-bit intensities (384x224, column-major source).
void vip_get_eye(int eye, uint8_t* out /* 384*224 */);

// ---- VSU (vsu.cpp) ----
void vsu_reset();
void vsu_write(uint32_t a, uint8_t v, int64_t now);
void vsu_update(int64_t now);
// Moves generated stereo samples into `out`; returns frame count.
size_t vsu_take_samples(int16_t* out, size_t maxFrames);
constexpr int AUDIO_RATE = 48000;

// ---- Hardware control: timer, game pad, wait control (hwctl.cpp) ----
void hwctl_reset();
uint8_t hwctl_read(uint32_t a, int64_t now);
void hwctl_write(uint32_t a, uint8_t v, int64_t now);
int64_t timer_update(int64_t now);
bool timer_irq();

// ---- Cartridge RAM ----
extern uint8_t g_sram[0x10000];

// ---- Host (host_sdl.cpp) ----
bool host_init(const std::string& title);
void host_shutdown();
// Called once per emulated frame (50 Hz): presents video, queues audio, polls
// input and paces execution. Throws QuitRequest when the user closes the window.
void host_frame();
uint16_t host_pad_state(); // Virtual Boy SDR bit layout
[[noreturn]] void fatal(const char* fmt, ...);
void log(const char* fmt, ...);

struct QuitRequest {};

} // namespace vb
