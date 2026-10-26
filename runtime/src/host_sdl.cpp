// SDL2 host: window, stereo display modes, audio output, input and pacing.

#include "system.h"
#include "vbrt/cpu.h"

#include <SDL.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vb {

namespace {

constexpr int EYE_W = 384, EYE_H = 224;

enum DisplayMode { MODE_LEFT, MODE_RIGHT, MODE_ANAGLYPH, MODE_SIDE_BY_SIDE, MODE_COUNT };
const char* const kModeNames[MODE_COUNT] = {"left eye", "right eye", "red/cyan anaglyph", "side by side"};

SDL_Window* g_window = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
int g_texture_w = 0;
SDL_AudioDeviceID g_audio = 0;
SDL_GameController* g_pad = nullptr;
DisplayMode g_mode = MODE_LEFT;
bool g_fast_forward = false;
uint64_t g_next_frame = 0;
std::vector<uint8_t> g_eyes[2];
std::vector<uint32_t> g_pixels;
std::string g_title;

// Automation for testing (environment variables):
//   VB_HEADLESS=1             no window, no pacing, no audio
//   VB_FRAMES=N               quit after N frames
//   VB_INPUT="start:300:10,a:400:5"   hold a button from frame F for D frames
//   VB_SCREENSHOTS="100,200"  save shot_<frame>.bmp at those frames (VB_SHOT_DIR, default ".")
struct ScriptedPress {
    int bit, start, len;
};
bool g_headless = false;
long g_frame = 0, g_max_frames = -1;
std::vector<ScriptedPress> g_script;
std::vector<long> g_shots;
std::string g_shot_dir = ".";
FILE* g_wav = nullptr; // VB_WAV=path: record audio
uint32_t g_wav_bytes = 0;

void wav_header(FILE* f, uint32_t dataBytes) {
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(AUDIO_RATE); u32(AUDIO_RATE * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
}

int button_bit(const std::string& n) {
    static const struct { const char* name; int bit; } names[] = {
        {"a", 2}, {"b", 3}, {"r", 4}, {"l", 5}, {"rup", 6}, {"rright", 7}, {"lright", 8}, {"lleft", 9},
        {"ldown", 10}, {"lup", 11}, {"start", 12}, {"select", 13}, {"rleft", 14}, {"rdown", 15},
        {"up", 11}, {"down", 10}, {"left", 9}, {"right", 8},
    };
    for (auto& e : names)
        if (n == e.name) return e.bit;
    return -1;
}

void parse_automation() {
    if (const char* v = std::getenv("VB_HEADLESS")) g_headless = *v && *v != '0';
    if (const char* v = std::getenv("VB_FRAMES")) g_max_frames = std::atol(v);
    if (const char* v = std::getenv("VB_SHOT_DIR")) g_shot_dir = v;
    if (const char* v = std::getenv("VB_WAV")) {
        g_wav = std::fopen(v, "wb");
        if (g_wav) wav_header(g_wav, 0);
    }
    if (const char* v = std::getenv("VB_SCREENSHOTS")) {
        std::string s = v;
        for (size_t p = 0; p < s.size();) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            g_shots.push_back(std::atol(s.substr(p, c - p).c_str()));
            p = c + 1;
        }
    }
    if (const char* v = std::getenv("VB_INPUT")) {
        std::string s = v;
        for (size_t p = 0; p < s.size();) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            std::string item = s.substr(p, c - p);
            p = c + 1;
            size_t a = item.find(':'), b = item.find(':', a + 1);
            if (a == std::string::npos) continue;
            ScriptedPress sp;
            sp.bit = button_bit(item.substr(0, a));
            sp.start = std::atoi(item.substr(a + 1, b - a - 1).c_str());
            sp.len = b == std::string::npos ? 5 : std::atoi(item.substr(b + 1).c_str());
            if (sp.bit >= 0) g_script.push_back(sp);
        }
    }
}

void save_screenshot(long frame) {
    const int w = g_texture_w;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(g_pixels.data(), w, EYE_H, 32, w * 4,
                                                           SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    const std::string path = g_shot_dir + "/shot_" + std::to_string(frame) + ".bmp";
    if (SDL_SaveBMP(surf, path.c_str()) == 0) log("saved %s\n", path.c_str());
    SDL_FreeSurface(surf);
}

void ensure_texture(int w) {
    if (g_texture && g_texture_w == w) return;
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, EYE_H);
    g_texture_w = w;
    SDL_RenderSetLogicalSize(g_renderer, w, EYE_H);
}

void open_first_controller() {
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++)
        if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
}

void present() {
    vip_get_eye(0, g_eyes[0].data());
    vip_get_eye(1, g_eyes[1].data());
    const int w = g_mode == MODE_SIDE_BY_SIDE ? EYE_W * 2 : EYE_W;
    ensure_texture(w);
    g_pixels.resize((size_t)w * EYE_H);
    for (int y = 0; y < EYE_H; y++) {
        uint32_t* row = &g_pixels[(size_t)y * w];
        const uint8_t* l = &g_eyes[0][(size_t)y * EYE_W];
        const uint8_t* r = &g_eyes[1][(size_t)y * EYE_W];
        switch (g_mode) {
        case MODE_LEFT:
            for (int x = 0; x < EYE_W; x++) row[x] = 0xFF000000u | ((uint32_t)l[x] << 16);
            break;
        case MODE_RIGHT:
            for (int x = 0; x < EYE_W; x++) row[x] = 0xFF000000u | ((uint32_t)r[x] << 16);
            break;
        case MODE_ANAGLYPH:
            for (int x = 0; x < EYE_W; x++) row[x] = 0xFF000000u | ((uint32_t)l[x] << 16) | (r[x] << 8) | r[x];
            break;
        default:
            for (int x = 0; x < EYE_W; x++) {
                row[x] = 0xFF000000u | ((uint32_t)l[x] << 16);
                row[x + EYE_W] = 0xFF000000u | ((uint32_t)r[x] << 16);
            }
            break;
        }
    }
    SDL_UpdateTexture(g_texture, nullptr, g_pixels.data(), w * 4);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
}

void push_audio() {
    vsu_update(g_cpu.cycles);
    int16_t buf[4096 * 2];
    size_t n;
    while ((n = vsu_take_samples(buf, 4096)) > 0) {
        if (g_wav) {
            std::fwrite(buf, 4, n, g_wav);
            g_wav_bytes += (uint32_t)(n * 4);
        }
        if (!g_audio || g_fast_forward) continue;
        // Keep latency bounded: drop audio if more than ~150 ms is queued.
        if (SDL_GetQueuedAudioSize(g_audio) < (Uint32)(AUDIO_RATE * 4 * 15 / 100))
            SDL_QueueAudio(g_audio, buf, (Uint32)(n * 4));
    }
}

void handle_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            throw QuitRequest{};
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: throw QuitRequest{};
            case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4:
                g_mode = (DisplayMode)(e.key.keysym.sym - SDLK_F1);
                log("display mode: %s\n", kModeNames[g_mode]);
                break;
            case SDLK_F11: {
                const bool fs = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(g_window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            case SDLK_TAB: g_fast_forward = true; break;
            default: break;
            }
            break;
        case SDL_KEYUP:
            if (e.key.keysym.sym == SDLK_TAB) g_fast_forward = false;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_first_controller();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && !SDL_GameControllerGetAttached(g_pad)) {
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                open_first_controller();
            }
            break;
        default: break;
        }
    }
}

void pace() {
    const uint64_t freq = SDL_GetPerformanceFrequency();
    const uint64_t frame = freq / 50;
    uint64_t now = SDL_GetPerformanceCounter();
    if (g_fast_forward || g_next_frame == 0 || now > g_next_frame + frame * 5) {
        g_next_frame = now + frame; // start over after stalls
        return;
    }
    while (now < g_next_frame) {
        const uint64_t ms = (g_next_frame - now) * 1000 / freq;
        if (ms > 1) SDL_Delay((Uint32)(ms - 1));
        now = SDL_GetPerformanceCounter();
    }
    g_next_frame += frame;
}

} // namespace

bool host_init(const std::string& title) {
    g_title = title;
    parse_automation();
    if (g_headless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    const Uint32 subsystems = g_headless ? SDL_INIT_VIDEO : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER;
    if (SDL_Init(subsystems) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    g_window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, EYE_W * 3, EYE_H * 3,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    if (!g_renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    g_eyes[0].assign(EYE_W * EYE_H, 0);
    g_eyes[1].assign(EYE_W * EYE_H, 0);
    ensure_texture(EYE_W);

    SDL_AudioSpec want{}, have{};
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else log("warning: no audio device: %s\n", SDL_GetError());

    open_first_controller();
    log("controls: arrows = left D-pad, I/J/K/L = right D-pad, X = A, Z = B, Q/E = L/R,\n"
        "          Enter = Start, Right Shift = Select, F1-F4 = display mode, F11 = fullscreen,\n"
        "          Tab = fast forward, Esc = quit\n");
    return true;
}

void host_shutdown() {
    if (g_wav) {
        wav_header(g_wav, g_wav_bytes);
        std::fclose(g_wav);
    }
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void host_frame() {
    g_frame++;
    handle_events();
    push_audio();
    present();
    if (std::find(g_shots.begin(), g_shots.end(), g_frame) != g_shots.end()) save_screenshot(g_frame);
    if (g_max_frames >= 0 && g_frame >= g_max_frames) throw QuitRequest{};
    if (!g_headless) pace();
}

uint16_t host_pad_state() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint16_t v = 0x0002; // bit 1 is always set; bit 0 (low battery) clear
    auto set = [&](bool on, int bit) { if (on) v |= (uint16_t)(1u << bit); };

    set(k[SDL_SCANCODE_DOWN], 10);
    set(k[SDL_SCANCODE_UP], 11);
    set(k[SDL_SCANCODE_LEFT], 9);
    set(k[SDL_SCANCODE_RIGHT], 8);
    set(k[SDL_SCANCODE_K], 15);
    set(k[SDL_SCANCODE_J], 14);
    set(k[SDL_SCANCODE_I], 6);
    set(k[SDL_SCANCODE_L], 7);
    set(k[SDL_SCANCODE_X], 2);
    set(k[SDL_SCANCODE_Z], 3);
    set(k[SDL_SCANCODE_Q], 5);
    set(k[SDL_SCANCODE_E], 4);
    set(k[SDL_SCANCODE_RETURN], 12);
    set(k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE], 13);

    for (const ScriptedPress& sp : g_script)
        set(g_frame >= sp.start && g_frame < sp.start + sp.len, sp.bit);

    if (g_pad) {
        auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(g_pad, b) != 0; };
        auto axis = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(g_pad, a); };
        constexpr int DZ = 16000;
        set(btn(SDL_CONTROLLER_BUTTON_DPAD_UP) || axis(SDL_CONTROLLER_AXIS_LEFTY) < -DZ, 11);
        set(btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || axis(SDL_CONTROLLER_AXIS_LEFTY) > DZ, 10);
        set(btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || axis(SDL_CONTROLLER_AXIS_LEFTX) < -DZ, 9);
        set(btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || axis(SDL_CONTROLLER_AXIS_LEFTX) > DZ, 8);
        set(axis(SDL_CONTROLLER_AXIS_RIGHTY) < -DZ, 6);
        set(axis(SDL_CONTROLLER_AXIS_RIGHTY) > DZ, 15);
        set(axis(SDL_CONTROLLER_AXIS_RIGHTX) < -DZ, 14);
        set(axis(SDL_CONTROLLER_AXIS_RIGHTX) > DZ, 7);
        set(btn(SDL_CONTROLLER_BUTTON_B), 2); // Nintendo layout: A on the right
        set(btn(SDL_CONTROLLER_BUTTON_A), 3);
        set(btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > DZ, 5);
        set(btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > DZ, 4);
        set(btn(SDL_CONTROLLER_BUTTON_START), 12);
        set(btn(SDL_CONTROLLER_BUTTON_BACK), 13);
    }
    return v;
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fflush(stderr);
}

void fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "fatal: %s\n", buf);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_title.empty() ? "vbrecomp" : g_title.c_str(), buf, g_window);
    std::exit(1);
}

} // namespace vb
