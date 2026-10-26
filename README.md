# VirtualBoyRecomp

A static recompiler for Nintendo Virtual Boy games, in the spirit of N64Recomp and
PS2Recomp. It translates a game's NEC V810 machine code into C++ ahead of time and
links it against a small runtime that reimplements the Virtual Boy hardware, giving a
native executable for Windows, Linux or macOS.

The repository contains only the recompiler and the runtime. No ROMs, game configs or
generated code are included; you supply your own ROM dump and a small config file.

There is no CPU interpreter or JIT: every instruction the game executes comes from the
generated C++. If execution ever reaches code that the analysis did not find, the
runtime stops and reports the address so it can be added to the game's config.

## How it works

```
 ROM (.vb) ──► vbrecomp ──► generated/rec_*.cpp ──┐
                 │                                ├──► C++ compiler ──► native game
 games/x.toml ───┘           runtime (vbrt) ──────┘
```

**Recompiler (`recompiler/`)**
- `v810_decoder` decodes every V810 instruction, including the Nintendo extensions
  (XB, XH, REV, MPYHW, TRNC.SW) and the bit-string instructions.
- `analysis` finds functions starting from the reset, interrupt and exception vectors
  and follows JAL calls. Each function is the code reachable through branches,
  JR and fall-through. It also resolves:
  - constant indirect jumps (`movhi`/`movea`/`jmp [r1]`, e.g. the interrupt stubs),
  - compiler switch tables (`shl 2` / `ld.w table[rX]` / `jmp [rX]`),
  - computed jumps into unrolled code (`jmp [base - n*12]`, e.g. software renderers),
  - code pointers stored in ROM tables (optional scan with plausibility checks).
- `codegen` emits one C++ function per V810 function, with the original disassembly
  as comments. Blocks become labels and branches become `goto`. Flags that are
  overwritten before anyone reads them are not computed.

**Execution model**
- All CPU state lives in a `vb::Cpu` struct shared by every generated function.
- `jal` becomes a direct C++ call. `jmp [lp]` sets `pc` and returns. After a call
  the caller checks that `pc` is the expected return address; if not, it returns too,
  until something can resume at `pc`.
- Every function can be entered at any of its block labels (a `switch` at the
  top). A top-level dispatcher therefore handles any control transfer that doesn't
  fit the call/return pattern (computed jumps, unusual returns, `reti` to a new
  place) without needing an interpreter.
- Each block adds its cycle cost. When the next hardware event is due, the runtime
  catches the hardware up and takes interrupts *synchronously*. It runs the handler
  through the same dispatcher until `reti` returns to the interrupted block.

**Runtime (`runtime/`)**
- `memory`: the 27-bit Virtual Boy bus. Work RAM, ROM and VRAM are inlined
  fast paths; everything else goes through the slow path.
- `vip`: the video chip. Worlds (normal, H-bias, affine, OBJ), BG maps, characters,
  palettes, frame buffer pairs, drawing done 8 rows at a time, display timing and
  interrupts (FRAMESTART, GAMESTART, XPEND, LFBEND, RFBEND, SBHIT).
- `vsu`: the sound chip. Five wavetable channels, sweep/modulation, noise, envelopes,
  duration; 48 kHz output.
- `hwctl`: hardware timer (and its interrupt), game pad, wait control.
- `host_sdl`: SDL2 window, stereo display modes, audio, keyboard and game controller.

## Building

Requirements: CMake ≥ 3.16, a C++17 compiler (GCC, Clang or MSVC) and SDL2.

| OS | Install SDL2 |
|----|--------------|
| Debian/Ubuntu | `sudo apt install libsdl2-dev cmake ninja-build g++` |
| Fedora | `sudo dnf install SDL2-devel cmake ninja-build gcc-c++` |
| macOS | `brew install sdl2 cmake ninja` |
| Windows (MSYS2 UCRT64) | `pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL2}` |
| Windows (Visual Studio) | `vcpkg install sdl2:x64-windows`, then pass `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |

Without a game config, the build produces only `vbrecomp` and the runtime library:

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

To recompile a game, create a config (see [Adding a game](#adding-a-game)) and point
CMake at it. The executable is named after the config file. `-DVB_ROM=<file.vb>`
overrides the ROM path in the config.

```bash
cmake -S . -B build -G Ninja -DVB_GAME_CONFIG=games/mygame.toml
```

```bash
cmake --build build
```

```bash
./build/mygame
```

The build compiles `vbrecomp`, recompiles the ROM into `build/generated/<game>/` and
compiles the result. If the ROM, the config or the recompiler changes, the next build
recompiles automatically. The executable checks the ROM's CRC32 at startup. It still
needs the ROM at run time for graphics, tables and music, and uses the path from the
build by default (or the first command-line argument).

## Controls

| Virtual Boy | Keyboard | Controller |
|-------------|----------|------------|
| Left D-pad | Arrow keys | D-pad / left stick |
| Right D-pad | I J K L | Right stick |
| A / B | X / Z | B / A (Nintendo layout) |
| L / R | Q / E | Shoulders / triggers |
| Start / Select | Enter / Right Shift | Start / Back |

F1 left eye · F2 right eye · F3 red/cyan anaglyph · F4 side by side · F11 fullscreen ·
Tab (hold) fast forward · Esc quit.

## Adding a game

1. Copy [games/example.toml](games/example.toml) to `games/<name>.toml` (anything in
   `games/` other than the example is ignored by git) and set `rom` to your dump:
   ```toml
   rom = "../roms/<file>.vb"
   crc32 = 0x12345678        # optional, refuses other dumps
   scan_code_pointers = true
   entries = []              # extra function entry points
   ```
2. `cmake -S . -B build-<name> -DVB_GAME_CONFIG=games/<name>.toml` and build.
3. Play. If the game stops with *"Execution reached address 0x…, which was not
   recompiled"*, the address is also appended to `unrecompiled.txt`. Add it to
   `entries` and rebuild.

Inspecting a ROM:

```bash
./build/recompiler/vbrecomp games/mygame.toml --stats
```

```bash
./build/recompiler/vbrecomp --disasm roms/mygame.vb FFFFFFF0 16
```

## Testing without a window

The runtime has hooks driven by environment variables, useful for regression tests
and CI:

| Variable | Effect |
|----------|--------|
| `VB_HEADLESS=1` | No window, audio or frame pacing (runs as fast as possible) |
| `VB_FRAMES=N` | Quit after N frames |
| `VB_INPUT=start:300:10,a:400:5` | Hold a button from frame F for D frames (`a b l r start select lup ldown lleft lright rup rdown rleft rright`) |
| `VB_SCREENSHOTS=100,200` | Save `shot_<frame>.bmp` (directory: `VB_SHOT_DIR`) |
| `VB_WAV=out.wav` | Record the audio output |

```bash
VB_HEADLESS=1 VB_FRAMES=1200 VB_INPUT="start:300:5,a:600:5" VB_SCREENSHOTS=600,1200 ./build/mygame
```

## Known limitations

- Hardware timing is approximate. Instruction costs are averages, VIP drawing takes a
  fixed time per block, and wait states and the instruction cache are not modelled.
- The VIP column table / REST brightness repeat are ignored. Brightness is a linear
  map of BRTA/BRTB/BRTC.
- Link port and cartridge expansion are stubs.
- Code that a game copies into RAM and runs from there is not supported yet. The
  recompiler only handles code in ROM.

ROMs are not included; use your own dumps.
