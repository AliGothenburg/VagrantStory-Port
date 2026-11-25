# Vagrant Story - Native Port & Static Recompilation Engine (`vsport`)

**English** | [Türkçe](README.tr.md)

A modular native port and static recompilation framework for **Vagrant Story (PS1 -> Windows x64)**,
written from scratch in C++20. It also runs on Linux, Steam Deck and Android through SDL3.

The game, the settings window and the release notes are available in **English and Turkish**.
The language follows your system by default; change it with **Language / Dil** at the top right of
the settings window (`language=0` automatic, `1` English, `2` Turkish in `VagrantStory.ini`).
Contributions are welcome in either language.

You need your own copy of the game disc. No game data is included in this repository.

## Architecture

| Module | Files | Role |
|---|---|---|
| **CD-ROM XA / ISO9660** | `include/vsport/cdrom_iso.hpp`, `src/cdrom_iso.cpp` | `.cue` / `.bin` (2352-byte Mode 2 XA) and `.iso` (2048-byte) reader, `SYSTEM.CNF` parser and direct LBA sector access |
| **PS1 CD-ROM XA controller** | `include/vsport/psx_cdrom.hpp`, `src/psx_cdrom.cpp` | Indexed hardware registers `0x1F801800..0x1F801803`, `CdlSetloc`/`CdlReadN`/`CdlPause`, the `INT1..INT3` IRQ2 state machine and DMA3 streaming |
| **PS-X EXE & `.PRG` analyser** | `include/vsport/psx_exe.hpp`, `src/psx_exe.cpp` | `SLES_027.54` / `SLUS_010.40` core loader, plus an automatic `JAL->Prologue` KSEG0 base address resolver for every `.PRG` overlay on the disc (`BATTLE`, `INITBTL`, `TITLE`, `ENDING`, `SCREFF2`, `MAINMENU`, `MENU0..MENUF`) |
| **MIPS R3000A + COP2 decoder** | `include/vsport/mips_decoder.hpp`, `src/mips_decoder.cpp` | Full MIPS I + COP0 + GTE (`COP2`) decoder and disassembler |
| **CFG & C++20 static recompiler** | `include/vsport/cfg_lifter.hpp`, `src/cfg_lifter.cpp` | Function discovery with `.data` filtering, C++20 code generation that keeps branch delay slots correct, and a 64-bit FNV-1a overlay signature registry for **21 modules / 3,138 native x64 functions** |
| **GTE (Geometry Transform Engine)** | `include/vsport/gte.hpp`, `src/gte.cpp` | Fixed-point 3D perspective transform (`RTPS`/`RTPT`), face culling (`NCLIP`), depth sorting (`AVSZ3`/`AVSZ4`), lighting and **16:9 widescreen projection scaling** |
| **PS1 MDEC video decoder** | `include/vsport/psx_mdec.hpp`, `src/psx_mdec.cpp` | Hardware MDEC at `0x1F801820` / `0x1F801824`, RLE and inverse quantisation (IQ), separable 2D IDCT, BT.601 YCrCb→RGB and a striped `DMA0`/`DMA1` decoder for **24-bit `RGB888` `.STR` full-motion video (FMV)** |
| **PS1 GPU & software rasteriser** | `include/vsport/psx_gpu.hpp`, `src/psx_gpu.cpp` | 1024×512 RGB555 VRAM, `GP0`/`GP1` packet processor (`Y1..Y2` vertical range + 24-bit `RGB888` display mode), `ABR 0..3` semi-transparency, CLUT-textured triangle/quad/sprite drawing, and the DMA6 OTC and DMA2 linked-list (ordering table) pipeline |
| **Direct3D 11 hardware renderer** | `include/vsport/d3d11_renderer.hpp`, `src/d3d11_renderer.cpp` | Direct `R16_UINT` VRAM upload to the GPU, an HLSL RGB555/RGB888 decoder and 3 scaling modes (*Sharp pixels*, *Bilinear*, *Trinitron CRT*) |
| **PS1 SPU ADPCM & WASAPI audio** | `include/vsport/psx_spu.hpp`, `src/psx_spu.cpp`, `src/win32_audio.cpp` | 512 KB SPU sound RAM, `DMA4` wave bank transfer, 24 hardware voices, Sony 4-bit ADPCM IIR + ADSR envelope synthesis and 44.1 kHz stereo WASAPI output (about 40–50 ms queue, WinMM `waveOut` as a fallback) or `.wav` output |
| **Hybrid AOT host runtime** | `include/vsport/runtime.hpp`, `src/runtime.cpp` | 2 MB / 8 MB RAM, 1 KB scratchpad, BIOS `A0`/`B0`/`C0` + root counter HLE, full-duplex `SIO0` controller, and an **overlay-aware native x64 AOT + interpreter hybrid dispatcher** with page-epoch caching |
| **Languages** | `include/vsport/i18n.hpp`, `src/i18n.cpp` | `txt(english, turkish)` picks the player-facing text in the chosen language; logs and developer output are English |

## Playing

`./build.ps1` copies the game to `dist/VagrantStory.exe` at the end of the build. Double-click
that file to play (no console window opens):

- The **settings window** opens first. Switch pages with the buttons on the left (Game, Graphics,
  Display, Audio, Controls, Advanced, Achievements):
  - **Game:** choose the game folder (the `.cue`/`.bin`/`.iso` and `.sbi` inside it are found
    automatically), or choose the files one by one. The window checks the disc (its release, and
    the `.sbi` state for the European release).
  - **Graphics:** internal resolution (1× original 320×240, 2×–8×; 5× for 1080p screens),
    renderer, graphics card, rim light, vertex precision, 3D texture filter.
  - **Display:** window / fullscreen, **aspect ratio** (4:3, 10:7, 3:2, 16:10, 16:9 or fill the
    window), window size (480p, 720p, 960p, 1080p, 1200p, 1440p, 4K; the width follows the
    ratio), filter (sharp pixels, smooth, CRT scanlines), integer scaling.
  - **Renderer:** who draws the high-resolution picture. **Graphics card** (default, Direct3D 11)
    is much faster; **Processor** is the older software renderer. Their output is almost pixel for
    pixel the same. If the graphics card cannot be started, the game switches to the processor by
    itself.
  - **Graphics card:** on computers with more than one graphics card (for example a laptop with
    integrated + NVIDIA graphics), which one to use. **Automatic** (default) picks the most
    powerful one; if the chosen card is no longer present, automatic choice is used again.
  - **Character rim light:** in cutscenes and menus the game first draws characters bright white,
    shifted ~1 pixel towards the light, and then draws them normally (the original rim light
    effect). At high internal resolution this shift becomes a hard, thick band. **Soft** (default)
    draws the light semi-transparent and fades it outwards; **Sharp** turns it into a thin bright
    line; **Off** does not draw it.
  - **Vertex precision:** the PS1 geometry unit (GTE) rounds every point to a whole pixel, so
    models and edges wobble as they move. **Wobble fix** (internal resolution 2× and up) keeps the
    exact position the GTE computed and uses it in the high-resolution picture.
    **Wobble fix + perspective textures** (default) also uses the depth of the corners to draw
    textures in correct perspective: textures on floors and walls no longer bend and slide along
    the diagonals (the PS1 spreads textures linearly on screen). Polygons whose depth is unknown are
    drawn as on the PS1. Because the game copies these points with its own code, they are matched
    by pixel value (about 90% of corners); when several points fall on the same pixel, that corner
    stays at its original position. The game itself and the original (1×) picture do not change.
  - **Widen the 3D scene to the ratio:** when on, the 3D scene is not stretched at wide ratios and
    more of it shows at the sides (no gaps at the edges were seen in the scenes tried). 2D text and
    gauges (HP/MP, menus, damage numbers, effects) are not stretched at internal resolution 2× and
    up: the 2D layer stays in the middle of the screen as one piece with its original 4:3 layout
    (so gauges such as HP/MP sit near the edge of the 4:3 area, not the edge of the screen). At 1×
    and with "Vertex precision" off, 2D elements are still stretched. When off, the picture is
    stretched to the chosen ratio.
  - In window mode the window keeps the chosen ratio (when you drag its edge and when you change
    the ratio with F3), so no black bars are left around it. In fullscreen choose your screen's
    ratio (for example 16:9 on a 16:9 monitor) or use "fill". Integer scaling only applies at 1×
    internal resolution.
  - **Audio:** mute, volume, **reverb** (the PS1 reverb unit; the game uses it heavily for rooms and
    dungeons, on by default) and **cubic interpolation** (a cubic instead of linear blend between
    samples; slightly cleaner highs, off by default; the real PS1 is close to linear). Command
    line: `--reverb 0/1`, `--cubic`.
  - **Vibration:** the game uses DualShock vibration; it works with Xbox / XInput compatible
    controllers (it can be turned off on the Controls page; not yet available for DirectInput
    controllers).
  - **Save states:** separate from the game's own save points, **F5** saves, **F7** loads and **F6**
    changes the slot (1–5; the files sit next to the memory card as `VagrantStory_durumN.vss`). The
    memory card is not part of a save state: loading a state does not undo saves made on the card.
    The Advanced page has two more options: **save the state on exit and continue where you left
    off**, and **rewind** (hold F8 for the last ~30 seconds; ~110 MB of memory).
  - **Cheat codes:** turned on from the Advanced page. Codes go in `VagrantStory_hileler.txt` next
    to the exe in GameShark / Action Replay format (a `[Cheat name]` line followed by codes such as
    `80012345 03E7`; `[Name] off` is a disabled cheat). Supported types: 80, 30, D0–D3, E0–E3. Codes
    are specific to the game's release (European release: SLES-02754). "Open cheat file…" creates
    the file with an explanation.
  - **Frame interpolation (Graphics page, experimental):** the game shows a new frame every 2
    vblanks (25 fps). When on, before each new frame an in-between frame is shown that draws every
    polygon exactly halfway between its place in the previous and the new frame, so the picture
    runs at 50 fps. The game itself does not change (its logic keeps running at 25 fps). The cost
    is that the new frame appears 1 vblank (20 ms) later. No in-between frame is made at scene
    changes, when the game slows down, in videos, or for polygons that cannot be found in both
    frames or jump too far; those polygons are drawn at their new place. Text and gauges (sprites)
    stay in place. Only works with the graphics card renderer at 2× internal resolution and up.
    Command line: `--interp 0/1`.
  - **Texture packs:** PNGs placed in the `dokular` folder next to the exe are drawn in place of the
    game's textures (at 2× internal resolution and up, with both renderers; the game itself and the
    1× picture do not change). To make a pack, turn on "Dump the game's textures" on the Advanced
    page: whenever the game draws a texture that has not been dumped yet, it is written to the
    `dokular\_dokum` folder as `<upload hash>-<palette hash>-<colour depth>.png`. The picture is the
    whole area the game uploaded to VRAM at once, drawn with that palette; transparent pixels have
    alpha 0. An upscaled or redrawn version is used once it is placed in the `dokular` folder under
    **the same name** (sub-folders are fine; folders starting with `_` are not read). Any size works
    (usually 2×–8×). It is filtered bilinearly when drawn; pixels with alpha below 50% are
    transparent. Semi-transparency comes from the game's own texture pixel. The name is the first
    three parts of the file name; a note such as `-sword` may follow. Known limits: while the game
    changes a texture's palette (colour fades) the pack texture is not used. Draws that use a
    texture window (E2) and textures spread over several uploads cannot be replaced either. The
    pack is read again at every start. Command line: `--texpack FOLDER`, `--texdump FOLDER`.
  - **Run-ahead (Advanced page):** on every vblank the real frame is run (buttons, audio) and its
    state is kept. Then 1–3 frames are run ahead and the last one is shown, after which the kept
    state is restored. The result of a button press shows up that many frames (20–60 ms) sooner.
    The frame shown is pixel for pixel the same as the matching later frame of a normal run.
    Emulation does 2–4 times the work per frame. On a Ryzen 9 8945HS + RTX 4060 at 4×, 72 real
    frames per second were measured with 1 frame and 50 with 2 frames (50 needed): 1 frame is
    recommended, 2–3 frames can slow the game in heavy scenes. Frame interpolation turns off while
    it is on. F5/F7, rewind and the exit save work on the real timeline. Command line:
    `--runahead N`.
  - **RetroAchievements (Achievements page):** sign in with your retroachievements.org account; the
    password is not stored, only the session token the server returns is written to the ini.
    Vagrant Story's achievement set (200 achievements) only recognises the **US release
    (SLUS-01040)**; on the European disc you are told there is no achievement set. Softcore only: a
    new emulator is not approved for hardcore; save states, rewind and cheats can be used. Unlocked
    achievements are shown in the window title and in `VagrantStory.log`. With run-ahead only the
    real frames are checked. The official
    [rcheevos](https://github.com/RetroAchievements/rcheevos) library (v12.5.0, MIT) is in
    `third_party/rcheevos`. `--ra-hash DISC` prints a disc's RA hash.
  - **US release:** the SLUS-01040 disc works too (60 Hz, no `.sbi` needed). The compiled game code
    is generated from the European release, so the US release runs on the interpreter: 84–103
    frames per second with the graphics card renderer at 4× (60 needed). Run-ahead cannot keep up
    with real time on this release.
  - **PAL 60 Hz (Display page):** the European release runs at 25 frames per second at 50 Hz and is
    therefore ~17% slower than the US release (60 Hz, 30 frames per second); that is the original
    behaviour. When on, the PAL disc runs at 60 Hz speed and the game moves at US release speed.
    Music and text speed are driven by a timer, so they do not change (measured). Command line:
    `--pal60 0/1`.
  - **Notices:** save state, rewind and achievement notices appear in a semi-transparent box at the
    bottom centre of the picture as well as in the window title; they also show in fullscreen.
  - **Skipping videos:** the game skips the intro video with START by itself; in other scenes hold
    **Tab** to fast-forward.
  - **Anti-aliasing (supersampling):** when the internal resolution is larger than the window, the
    picture is scaled down by averaging the pixels each screen pixel covers; jagged polygon edges
    become smooth. The 7× and 8× internal resolutions exist for this (recommended on a 1080p screen;
    graphics card renderer only, the processor is limited to 6×).
  - **3D texture filter:** Off, Smooth (bilinear) or **Sharp bilinear** (texture pixels stay sharp,
    only the border between them is smoothed).
  - **Advanced › CPU speed:** speeds the emulated PS1 processor up by 150%–300%; the game's own
    slowdowns in busy scenes shrink (the game itself does not run faster). Loading is shorter too.
  - **Controls:** choose which controller button triggers each PS1 button (×, ○, □, △, L1/R1,
    L2/R2, START, SELECT, L3/R3); "Reset to defaults" goes back to the position-based mapping. The
    d-pad and left stick are fixed; keyboard keys do not change.
- Settings are saved in `VagrantStory.ini` next to the exe. If "Show this window on every start" is
  turned off, the game opens directly; to get back to the settings, hold **Shift** while starting
  the game.
- The game's start-up and shutdown steps are written to `VagrantStory.log` next to the exe (if
  something goes wrong it shows where it stopped). If shutdown does not finish in 8 seconds the game
  ends itself, so no windowless process is left in the background.
- In game: **F11** / **Alt+Enter** fullscreen, **F4** filter, **F3** aspect ratio (in turn),
  **Shift+F3** 3D widening, **M** sound; these changes are saved too. **Esc** quits (it asks first).
- **Internal resolution** draws the 3D scene at 2×–4× resolution: polygon edges and texture mapping
  become sharp. The game's own VRAM stays at the original resolution (emulation accuracy is kept);
  the high-resolution copy is only shown on screen. This copy is drawn in 24-bit colour (true
  colour), so the hard colour bands the PS1's 15-bit colours make in dark scenes do not show.
  **3D texture smoothing** removes the blocks in low-resolution textures (it also softens 2D text a
  little).
- **Speed:** by default the graphics card draws the high-resolution picture (Direct3D 11; PS1
  textures, semi-transparency, the mask bit and perspective are done in the shader with the same
  integer maths as the PS1). On laptops with two graphics cards the powerful one (for example
  NVIDIA) is picked automatically. On this computer (RTX 4060 Laptop), in the heaviest part of the
  intro cutscene, 5× runs at ≈ 2.8× real time and 6× at ≈ 2.6×; with the processor's integrated
  graphics (Radeon 780M) 5× runs at ≈ 1.5×. With **Processor** the drawing is spread in strips over
  all processor cores; heavy scenes then run at ≈ 1.06× at 4× and ≈ 0.8× (slowdown) at 5×.
  `vs_host --perf` prints the speed every 250 frames (`--gpu-hr` / `--cpu-hr` picks the renderer;
  headless runs use the processor by default).
- For the sharpest picture use the **Sharp pixels** filter together with the internal resolution;
  the Smooth and CRT filters blur the picture on purpose.
- **Memory card:** saving in the game works. Card 1 is `saves\VagrantStory_kart1.mcr` next to the
  exe (it can be changed in the settings window). This is the standard 128 KB `.mcr` format other
  emulators (DuckStation, ePSXe, Mednafen…) read; if the file does not exist an empty, formatted
  card is created. Changes are written to disk about half a second after the game stops writing,
  and on exit.

File and folder names such as `VagrantStory_kart1.mcr`, `VagrantStory_hileler.txt` and `dokular`
stay in Turkish in every language, so existing setups keep working.

When started from a terminal, `VagrantStory.exe` writes its log to that terminal. For development
and debugging options use the console build `build/vs_host.exe`.

## Linux, Steam Deck and Android

The Linux and Android builds run the same game core with SDL3 (`tools/sdl_host.cpp`): window,
audio, gamepad, touch buttons and an in-game menu. The picture is drawn by the processor (no
Direct3D) and shown through SDL's OpenGL / Vulkan / GLES renderer. Emulation is exactly the same as
on Windows (the same save state gives the same frames, byte for byte).

- **Menu:** Esc, **L3 + R3** on a gamepad (press both sticks together) or the Guide button, and on
  Android the back button or the ≡ button at the top of the screen. From the menu: save / load
  state / slot, aspect ratio, 3D widening, sharp / smooth picture, internal resolution (1–4×), PAL
  60 Hz, sound, touch buttons, fullscreen, change disc, quit. The game pauses while the menu is
  open.
- **First start:** if no disc is set, a disc picker opens. On Linux / Steam Deck there is a file
  browser inside the window (it also works in Steam's Game Mode); on Android the system file picker
  opens.
- **Keyboard:** the same as on Windows (arrows, Z X A S, Q W E R, Enter, Backspace; F5 / F7 / F6
  states, hold F8 to rewind, F3 ratio, Shift+F3 3D widening, F4 picture, M sound, F11 fullscreen,
  Tab fast-forward).
- **Settings:** the same `VagrantStory.ini` format. On Linux it is in `~/.local/share/VagrantStory/`
  (if there is a `VagrantStory.ini` next to the program, that folder is used for portable use).
  Saves, the cheat file and texture packs (`dokular`) go in this folder too.
- **RetroAchievements (Linux):** works with the system's `libcurl`. Signing in is only possible in
  the Windows settings window for now: copy the `ra_user` / `ra_token` lines from there into the
  Linux ini. Achievements are not available on Android yet.

### Steam Deck

1. In Desktop Mode, extract `VagrantStory-Port-…-linux-x64.tar.gz` into a folder.
2. In Steam → Add a Game → Add a Non-Steam Game, add the `VagrantStory` file.
3. Start it in Game Mode. On the first start choose the disc image in the built-in file browser
   (`.cue` / `.bin` / `.iso`; for PAL, the `.sbi` with the same name must be in the same folder).

Fullscreen is the default on the Deck; the menu is **L3 + R3**. The Deck's controls arrive as a
gamepad through Steam Input (A = ✕, B = ○, X = □, Y = △).

### Android

Install and open the APK, then choose the `.bin` (and for PAL the `.sbi`) file with **Choose disc
image**; several files can be chosen at once. The files are copied once into the app's folder
(`Android/data/io.github.aligothenburg.vagrantstory/files/`; settings and saves live there too).
No `.cue` is needed. Touch the screen to show the touch buttons; they hide when a gamepad is used.
On phones the default internal resolution is 1× (the processor draws the picture); it can be raised
from the menu on powerful devices.

**Home screen icon:** on the first start the game captures Rood Inverse from the end of the intro
video, from your own disc, in the background (it takes about half a minute and does not slow the
game down), and the phone asks whether to add it to the home screen. If you agree, a shortcut with
this picture that opens the game appears on the home screen. The picture is saved as `kapak.png` in
the app folder; the shortcut can be requested again with **Home screen icon** in the menu.

## Building

```powershell
./build.ps1
```

**Linux** (CMake downloads the SDL3 source by itself; a local folder can be given with
`-DVSPORT_SDL_SOURCE_DIR=...`):

```bash
sudo apt install build-essential cmake ninja-build libasound2-dev libpulse-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libwayland-dev wayland-protocols libegl1-mesa-dev libdbus-1-dev libudev-dev libpipewire-0.3-dev libdecor-0-dev
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DVSPORT_LINK_AOT=OFF
cmake --build build/linux
```

`build/linux/VagrantStory` is the game itself (SDL3 and the C++ runtime are linked in; only
`glibc` 2.38+ is needed); `vs_host` is the headless test tool.

**Android** (needs JDK 17 and the Android SDK + NDK 29):

```powershell
cd android
./gradlew assembleRelease
```

APK: `android/app/build/outputs/apk/release/app-release.apk` (arm64 and x86_64).

### Adding translated text

Player-facing text is written at the call site as `txt("English", "Türkçe")` (see
`include/vsport/i18n.hpp`). Log lines, console output and comments are English only. The SDL
on-screen font is ASCII only, so the Turkish halves in `tools/sdl_host.cpp` are written without
Turkish letters.

## Disc image

The tools use your own Vagrant Story disc image (`.cue` + `.bin`). The commands below read the disc
path from the `$Disc` variable; set it to your own path in the PowerShell session first:

```powershell
$Disc = "D:\Games\Vagrant Story (Europe)\Vagrant Story (Europe).cue"
```

### Subchannel data (.sbi) — needed for the PAL release

The European release (SLES-02754) is protected by **LibCrypt**: on New Game it checks the
subchannel (Q) data of a few sectors. A `.bin` image does not contain this data; without the disc's
Redump `.sbi` file the game hangs at "NOW LOADING" after New Game. Put the `.sbi` file next to the
`.cue` with the same name and it is loaded automatically; if it is somewhere else, give it with
`--sbi`:

```powershell
$Sbi = "D:\Games\Vagrant Story (Europe)\Vagrant Story (Europe).sbi"
./build/vs_host.exe --disc $Disc --sbi $Sbi
```

The US release (SLUS-01040) has no such protection.

> **Note:** `generated_recomp/` is code generated from the disc and is kept out of the repository
> by `.gitignore` (it is a direct translation of the game's code, so it must not be shared). In a
> fresh clone, generate it first with `vs_recomp`, then build with `./build.ps1`; without the folder
> `vs_host` is built without AOT modules and runs on the interpreter only.

## Using the tools

1. **Inspecting the disc image and overlays (`vs_inspect`):**
   ```powershell
   ./build/vs_inspect.exe --disc $Disc --extract-dir extracted

   # Disassemble the core EXE or an overlay at its load address:
   ./build/vs_inspect.exe --disc $Disc --disasm 0x80010B14 64
   ./build/vs_inspect.exe --disc $Disc --prg BATTLE/BATTLE.PRG --disasm 0x800D51D0 40
   ```

2. **Static C++20 code generation (`vs_recomp`):**
   ```powershell
   ./build/vs_recomp.exe --disc $Disc --out-dir generated_recomp
   ```

3. **Native Windows host (`vs_host`):**
   ```powershell
   # Live hardware check demo (GTE + DMA6 OTC + DMA2 GPU + Win32):
   ./build/vs_host.exe --self-demo

   # Boot from the disc image (native C++20 AOT is active in window mode):
   ./build/vs_host.exe --disc $Disc --sbi $Sbi

   # Dump PS1 RAM (2 MB) to a file at the end of a headless run (debugging):
   ./build/vs_host.exe --disc $Disc --sbi $Sbi --headless --cycles 900000000 --dump-ram ram.bin

   # Press buttons on given vblanks in headless mode (BUTTON@VBLANK[:FRAMES], comma separated):
   ./build/vs_host.exe --disc $Disc --headless --cycles 470000000 --press START@370,START@430,DOWN@650:4 --dump-display menu.ppm

   # Save a screenshot every N frames (for example to compare 1× with 4× frame by frame);
   # --rim 0/1/2 picks the character rim light:
   ./build/vs_host.exe --disc $Disc --sbi $Sbi --headless --cycles 20000000000 --scale 4 --dump-series frames 50

   # Headless performance, 24-bit display / VRAM and 44.1 kHz WAV audio output test:
   ./build/vs_host.exe --disc $Disc --headless --cycles 1150000000 --dump-display fmv.ppm --dump-wav spu.wav

   # Unit tests; the second compares the graphics card renderer with the processor (every kind of
   # primitive, semi-transparency, mask, texture window, perspective, sprites textured from their
   # own area). It runs on Microsoft's WARP software device, no graphics card needed:
   ./build/vsport_tests.exe
   ./build/vsport_gpu_tests.exe
   ```
   The game runs at real-time speed for the video mode it picks (PAL 50 Hz / NTSC 60 Hz).

   **Controls:** keyboard and gamepad can be used together; a controller can be plugged in while
   the game is running. Xbox and XInput compatible controllers are read through XInput,
   DualShock 4 / DualSense and other generic gamepads through DirectInput. The left analogue stick
   acts like the d-pad.

   | PS1 | Keyboard | Xbox (XInput) | DualShock 4 / DualSense |
   |---|---|---|---|
   | D-pad | Arrow keys | D-pad / left stick | D-pad / left stick |
   | × | `Z` | A | × |
   | ○ | `X` | B | ○ |
   | □ | `A` | X | □ |
   | △ | `S` | Y | △ |
   | L1 / R1 | `Q` / `W` | LB / RB | L1 / R1 |
   | L2 / R2 | `E` / `R` | LT / RT | L2 / R2 |
   | START | `Enter` | Menu (Start) | Options |
   | SELECT | `Backspace` | View (Back) | Share / Create |
   | Free camera | `J` / `L` turn, `I` / `K` tilt | right stick | right stick |

   **Free camera:** the right stick turns the camera (left-right) and tilts it (up-down) to any
   angle, without being tied to the game's 45-degree L1/R1 steps. Walking direction follows the
   camera; L1/R1 still work and snap to the nearest 45-degree angle. The camera only changes when
   you touch the right stick and while the game is using its field camera (it does nothing on the
   title screen). It can be turned off on the **Controls** page (`free_camera=0`). Works on the
   European and US releases.

   Controller buttons can be changed on the **Controls** page of the settings window (stored in
   `VagrantStory.ini` as lines such as `pad_cross=circle`).

   *Window shortcuts:* `F11` / `Alt+Enter` fullscreen | `Tab` (hold) fast-forward | `F2` full 1024×512 VRAM | `F3` aspect ratio (4:3 → 10:7 → 3:2 → 16:10 → 16:9 → fill) | `Shift+F3` 3D widening (GTE) | `F4` HLSL shader mode (sharp / bilinear / Trinitron CRT) | `M` sound on/off | `F12` save PPM | `ESC` quit
