<div align="center">

<img src="extras/banner.png" alt="Mass Effect Cinnamon" width="60%">

# 🍂 Mass Effect Cinnamon

### BioWare's *Mass Effect* (Xbox 360, 2007), statically recompiled for the Nintendo Switch, and tuned for a steady 30 fps at stock clocks

[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-e60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#-building-from-source)
[![Clocks](https://img.shields.io/badge/clocks-stock%201224%20%2F%20460.8%20MHz-2ea44f?style=for-the-badge)](docs/optimisation.md)
[![Target](https://img.shields.io/badge/target-30%20fps-orange?style=for-the-badge)](docs/optimisation.md)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=for-the-badge)](LICENSE)

**A performance fork of [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by
[NebadasSwifty](https://github.com/nebadasSwifty)**: all the credit for the port itself goes there.

[What's new](#-whats-new-in-cinnamon) •
[Performance](#-performance) •
[Build it](#-building-from-source) •
[Controls](#-controls) •
[Optimisation deep dive](docs/optimisation.md) •
[Credits](#-credits)

</div>

---

## ✨ What's new in Cinnamon

Cinnamon keeps everything the original port does: native ARM64 code translated from the game's `default.xex`, a
Vulkan renderer on Mesa/NVK, and no emulation of the Xbox 360 CPU. On top of that it adds:

- 🎯 **Gyro aim assist.** Aim with motion controls while holding **ZL**, on top of the right stick. Sensitivity can be
  changed in game (**L + R + X** / **L + R + Y**); always-on mode and per-axis inversion are in `masseffect.toml`.
- ⚡ **Optimised gameplay at stock clocks.** It targets 30 fps on CPU 1224 MHz / GPU 460.8 MHz handheld, **with no
  overclock**. In the latest measured run 79 % of gameplay holds 29-30 fps and nothing drops below 20
  ([how](docs/optimisation.md)).
- 🎬 **Smooth cutscenes.** A **"PREPARING SHADERS" loader** builds the Vulkan pipelines once, before the game starts,
  so cutscenes and new areas no longer stutter through shader compiles.
- 🧠 **No wasted draws.** The game's **depth prepass is no longer built** (~430 draws per frame on the Normandy that
  the renderer threw away anyway), and the **2x MSAA tiling that replayed the whole scene twice is off**.
- 🌄 **Small-object distance culling.** Distant rocks, debris and props smaller than a few pixels are skipped,
  sized by each object; characters and large scenery are never touched.
- 🖼️ **Sharper 720x400 → 720p image.** Rendering at **720x400** keeps the GPU in budget, a new **FXAA pass** cleans
  the edges, and **FSR** upscales to the screen with HUD-friendly sharpening.
- 🔦 **Correct lens flares.** Flares and light halos are **occlusion-tested for real** again, so they no longer shine
  through walls, rocks or Shepard.
- 🌑 **Shadows kept, made cheaper.** Character shadows stay, at lower cost (256x256 maps, tiny far shadows skipped).
- 🧵 **No streaming stalls.** Texture streaming no longer splits frames when you turn around: the **180° turn drop
  to 15 fps is gone**.
- 🔋 **Official memory profile.** It requests Nintendo's own *GPU 460.8 MHz + memory 1600 MHz* handheld configuration.
  That is not an overclock, and it helps the cache-bound CPU threads.

> [!NOTE]
> Everything was measured on a real console at stock clocks, from the port's own profiler logs.
> **[docs/optimisation.md](docs/optimisation.md)** explains each change, why it was needed and what it gained.

## 📊 Performance

<div align="center">

10-second profile blocks during gameplay, handheld, CPU 1224 MHz / GPU 460.8 MHz

| Build | 29-30 fps | 25-29 fps | 20-25 fps | < 20 fps | Mean |
| :--- | :---: | :---: | :---: | :---: | :---: |
| Early Cinnamon build (800x448, before the draw-count work) | 43 % | 30 % | 21 % | 5 % | 26.9 |
| **Cinnamon** (latest measured) | **79 %** | 8 % | 13 % | **0 %** | **28.9** |

</div>

Cutscenes, interiors, dialogue and most exploration run locked at 30. The heaviest crowd and outdoor scenes (the Eden
Prime dig-site camp, the busy Normandy crew deck) can still dip into the low 20s, because all three CPU cores are full
there; the newest change, small-object culling, is aimed at exactly those scenes.

## 🛠️ Building from source

> [!IMPORTANT]
> **There is no installer and no download of the game.** Clone this repository and build the port yourself from your
> **own original Xbox 360 disc image** of *Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1)*. The repository contains no
> game files, and nothing made from the disc may ever be committed to it.

The full step-by-step guide is in **[docs/building.md](docs/building.md)** (macOS and Linux through Docker; native
Windows in its last section). In short:

```sh
# 0. Get the source
git clone https://github.com/jvankooo/masseffect-cinnamon.git
cd masseffect-cinnamon

# 1. Third-party libraries the repository does not carry
python3 tools/fetch_thirdparty.py

# 2. Your disc: extract the ISO into assets/game_root (check default.xex against docs/editions.md)
python3 tools/extract_iso.py "Mass Effect (USA, Europe) (En,Es,Pl) (Rev 1).iso"

# 3. The code generator, then translate the game's program to C++ (applies the Cinnamon patches too)
tools/build_host.sh
tools/codegen.sh

# 4. The Vulkan driver (Mesa/NVK for the Switch), once: an hour or more
mesa/build_mesa_docker.sh

# 5. The Switch program: out/nx/masseffect-nx.nro
tools/build_nro.sh              # Docker
# Windows without Docker:
# powershell -ExecutionPolicy Bypass -File tools\build_nro.ps1 -MesaSdk <...\opt\devkitpro\portlibs\switch> -Glslang <glslangValidator.exe>

# 6. The shader library from your disc (see docs/building.md, step 7)
shaders/tools/build_extra_tools.sh ue3_shader_scan
find assets/game_root -type f -iname '*.xxx' -print0 | xargs -0 out/tools/ue3_shader_scan run/shaders
cp shaders/runtime_containers/*.bin run/shaders/
ALLOW_FAILURES=1 shaders/tools/build_shader_spirv.sh run/shaders run/shaderlib
```

### 📁 SD card layout

```text
sdmc:/switch/masseffect-nx/
  ├── masseffect-nx.nro               ← out/nx/masseffect-nx.nro
  ├── masseffect.toml                 ← app/masseffect.toml   (the tuned Cinnamon settings)
  ├── masseffect_prewarm_list.bin     ← app/prewarm/masseffect_prewarm_list-en.bin
  ├── masseffect_shaders.mesp         ← run/shaderlib/
  ├── masseffect_shaders.mesp.idx     ← run/shaderlib/
  └── game_root/                      ← your extracted disc (assets/game_root)
```

Start it with full application memory: hold **R** while launching an installed game to open the homebrew menu in
title mode, or use a forwarder set to the 39-bit address space.

> [!TIP]
> The **first boot** shows the *PREPARING SHADERS* screen for a few minutes while it builds the pipeline cache on
> the SD card. Every boot after that is quick.

## 🎮 Controls

<div align="center">

| Switch | Action in Mass Effect | Xbox 360 |
| :--- | :--- | :--- |
| **A** | Interact / Confirm / Take Cover | A |
| **B** | Cancel / Exit / Storm (Sprint) | B |
| **X** | Draw & Holster Weapon / Reload | X |
| **Y** | First Aid (Medi-Gel) | Y |
| **ZR** | Fire Weapon / Vehicle Cannon | RT |
| **ZL** | Target Lock / Zoom / Vehicle Thruster, **+ gyro aiming while held** | LT |
| **R** | Weapon Wheel (Pause Combat) | RB |
| **L** | Biotic & Tech Power Wheel (Pause Combat) | LB |
| **+** | System Menu / Pause | Start |
| **−** | Codex & Journal / Equipment | Back |
| **Left Stick** | Shepard Movement / Mako Steering | Left Stick |
| **Right Stick** | Camera Look / Turret Aim | Right Stick |
| **L3** | Toggle Crouch | Left Stick Click |
| **R3** | Scope Zoom Toggle | Right Stick Click |
| **D-Pad** | Squad Orders (Attack, Move, Regroup) | D-Pad |

</div>

### 🎯 Gyro aim assist

<div align="center">

| Input | Effect |
| :--- | :--- |
| Hold **ZL** and move the console / controller | Fine aim with motion, added to the right stick |
| **L + R + X** | Gyro sensitivity +0.5 |
| **L + R + Y** | Gyro sensitivity −0.5 |

</div>

The settings live in `masseffect.toml`:
- `input_gyro`: 0 = off, 1 = while ZL is held (default), 2 = always.
- `input_gyro_sensitivity`: default 3.0.
- `input_gyro_invert_x` and `input_gyro_invert_y`: invert an axis.
- `input_gyro_deadzone`: hand-tremor filter.

Turning follows the real horizontal axis whichever way you hold the console, because the yaw is measured around
gravity.

Other options: `input_xbox_layout = true` for the Xbox button positions, `input_rumble = true` for rumble.
**L + R + Up / Right / Down / Left** open the SDK's debug overlay, settings, log console and achievements.

## ⚙️ Tuning

Every key in [`app/masseffect.toml`](app/masseffect.toml) is commented. The Cinnamon ones you may want to touch:

<div align="center">

| Key | Default | What it does |
| :--- | :---: | :--- |
| `masseffect_cull_small_factor` | `100` | Small-object draw distance (lower = faster, more pop-in; `0` = off) |
| `input_gyro_sensitivity` | `3.0` | Gyro aiming speed |
| `masseffect_native_output_fxaa_subpix` | `0.5` | FXAA softness (0 = sharpest) |
| `masseffect_flare_occlusion` | `true` | Real occlusion for lens flares and halos |
| `masseffect_guest_skip_prepass` | `true` | The game does not build its depth prepass |
| `masseffect_scene_width` / `_height` | `720` / `400` | Internal resolution (keep the width equal to `video_mode_width`) |

</div>

> [!WARNING]
> Do not enable `masseffect_cpu_boost`: while it is on, the system throttles the GPU and the game falls to a few fps.
> Cinnamon is tuned for, and measured at, **stock clocks**; overclocking is neither needed nor recommended.

## 📚 Documentation

- **[docs/optimisation.md](docs/optimisation.md)**: how Cinnamon reaches 30 fps at stock clocks.
- [docs/building.md](docs/building.md): the full build, step by step.
- [docs/best-config.md](docs/best-config.md): every console run behind the shipped settings.
- [docs/README.md](docs/README.md): the upstream guide to how the whole port works (start here to learn the internals).
- [docs/known-issues.md](docs/known-issues.md): what is not finished or not proven.

## 💛 Credits

- **[NebadasSwifty](https://github.com/nebadasSwifty)**: author of **[masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx)**,
  the Mass Effect Switch port this fork is built on: the recompilation, the native Vulkan renderer, the EDRAM
  emulation, the shader pipeline, the Switch platform layer and nearly all of the documentation. Cinnamon is a
  modified version of that work (see [LICENSE](LICENSE)).
- **BioWare and Electronic Arts**: creators of Mass Effect. This is an unofficial, non-commercial fan project with no
  affiliation.
- **[StevensND](https://github.com/StevensND)**: [NFSMW-NX](https://github.com/StevensND/NFSMW-NX), pioneering work on
  statically recompiled Xbox 360 games on the Switch (not affiliated with this project).
- **[madelrandel-blip](https://github.com/madelrandel-blip/NFSMW-Recompiled)**: NFSMW Recompiled, where the upstream
  port started.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the research of the
  **[Xenia](https://xenia.jp)** team.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp, the Xenos shader translator.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)**:
  mesa-switch (Mesa, NVK and NAK on Horizon OS).
- **[devkitPro](https://devkitpro.org)** and **[switchbrew](https://github.com/switchbrew/libnx)**: devkitA64 and libnx.
- All third-party libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## ⚖️ Legal

No affiliation with Electronic Arts or BioWare. "Mass Effect" is a trademark of Electronic Arts Inc. This repository
contains **no assets or program code from the original game**: you must build it from your own legally obtained disc.
Running homebrew requires custom firmware, which violates Nintendo's terms of service and can get a console banned.
That is your call.

Copyright (c) 2026 NebadasSwifty (the original port), with modifications for Mass Effect Cinnamon (2026). Source code
under the **GPL-3.0** ([LICENSE](LICENSE)), as inherited from the original project. The SDK changes are under the SDK's
BSD-3-Clause license, the shader translator and Mesa changes under MIT ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
