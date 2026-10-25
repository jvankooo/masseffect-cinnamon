# How Mass Effect Cinnamon runs at 30 fps on stock clocks

Mass Effect Cinnamon is a fork of [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by **NebadasSwifty**.
This page covers what the fork changes so the game holds its 30 fps cap on an unmodified Switch, in handheld mode, at
the **stock clocks: CPU 1224 MHz and GPU 460.8 MHz**, with no overclock. For how the port itself works (static
recompilation, the native Vulkan renderer, EDRAM emulation), see the upstream documentation in this folder,
starting with [native-renderer.md](native-renderer.md) and [optimization-paths.md](optimization-paths.md).

Every change below came from a console log: the port writes a profile every 10 seconds (`logs/rex/rex_profile.log`)
and a renderer report (`logs/masseffect_001.log`), and each fix targets a cost those logs showed.

## Where the time goes

A frame on this port passes through four workers, and the slowest one sets the frame rate:

| Worker | What it does | Typical share in a heavy view |
|---|---|---|
| Game thread | Unreal Engine 3 gameplay: actors, animation, physics, scripts | 80-90 % of a core |
| UE3 render thread | Builds the draw calls and Direct3D commands of the frame | 40-55 % of a core |
| Ring thread | Reads the Xbox 360 GPU command stream (PM4), translates every draw to Vulkan, emulates EDRAM | 75-90 % of a core |
| GPU (460.8 MHz) | Draws the frame | 15-35 ms per frame |

The process gets **3 CPU cores**, so the three threads plus audio and the driver share them. At 1224 MHz the heavy
views were CPU-bound on all three cores at once: the GPU sat idle 20+ ms per frame waiting for commands. Making one
thread faster only hands the bottleneck to the next. The approach that works is **removing work that reaches every
thread at once: fewer and cheaper draws**, then making each remaining draw cheaper on the ring.

## Results

10-second profile blocks during gameplay (prologue, Normandy, Eden Prime), stock clocks, handheld:

| Build | Blocks at 29-30 fps | 25-29 fps | 20-25 fps | Below 20 fps | Mean |
|---|---|---|---|---|---|
| Early build (run 6, 800x448; sections 1, 4, 6, 7 and 10 already in, not yet 2, 3, 5 and 9) | 43 % | 30 % | 21 % | 5 % | 26.9 |
| After 3 and 5, then 2 (runs 7-8, short heavy routes) | 43-49 % | 32-36 % | 20 % | 0 % | 27.6-27.9 |
| Latest measured (run 11; everything except section 9) | **79 %** | 8 % | 13 % | **0 %** | **28.9** |

Cutscenes, interiors, dialogue and most exploration run locked at 30. The remaining dips (21-24 fps) are the heaviest
crowd and outdoor scenes, such as the Eden Prime dig-site camp and the busy Normandy crew deck, where all three cores
are full. The last item below (small-object distance culling) targets exactly those scenes and has not been measured
on a console yet.

## The changes, in order of impact

### 1. The game's own 2x MSAA and predicated tiling are off

On the Xbox 360, Mass Effect renders with 2x MSAA and predicated tiling: the scene is split into two EDRAM tiles and
**the whole command stream of the scene is replayed once per tile**. On the Normandy deck that meant ~3,300 draws per
frame for the ring thread and 11 fps. With `masseffect_game_msaa = 0` (a one-byte patch, the same as Xenia's "Black
Shading Fix") the scene is drawn once, without the 2x-to-1x depth and colour conversions. FXAA replaces the lost edge
smoothing (section 6).

### 2. The game no longer builds its depth prepass

UE3 draws every occluder twice: once depth-only (the prepass), then with materials. The port has to drop the prepass
(its 2x depth disagreed with the material pass and caused black shards), but the game still built it and the ring
still parsed it: **~430 of ~2,300 draws per frame** on the Normandy deck, for nothing. Cinnamon hooks UE3's
`RenderDPGPrePass` (`sub_824F6DB0`) so the game never builds it (`masseffect_guest_skip_prepass`,
[me_guest_prepass.cpp](../app/src/native/me_guest_prepass.cpp)). The picture is unchanged, and the work disappears from
the render thread, the ring thread and the game thread's waits. Heavy views went from 20-26 fps to 27-30 fps.

### 3. Texture streaming no longer stalls the GPU

Turning 180 degrees caused 15 fps bursts. Each texture streamed in for a newly visible area split the frame into a new
GPU submission, so there were more submissions than the 3 work slots in a frame. The CPU then waited ~32 ms per frame
for the GPU. `masseffect_native_inline_texture_updates` records the upload in order inside the frame's command
buffer (pass closed, barrier, copy, barrier) instead of splitting the frame.

### 4. A shader prewarm loader instead of stutter

The first run of a cutscene or area compiled Vulkan pipelines on the fly (~240 ms each on a cold cache): cutscenes
fell to 8-13 fps. With `masseffect_native_pipelines_prewarm_blocking`, the port compiles the known pipeline list once,
behind a **"PREPARING SHADERS" progress screen**, while the game is held in its Direct3D start-up (so no logo sound
plays early). It uses 3 threads and saves the result to the SD card. The first boot takes a few minutes; every boot
after that is quick, and cutscenes play smoothly.

### 5. 720x400 internal resolution

The scene renders at 720x400 instead of 800x448 (20 % fewer pixels for the 460.8 MHz GPU). 720 is a whole number of
the Xbox 360's 80-pixel EDRAM tiles and stays clear of the 560-pixel shadow-map pitch, so the EDRAM emulation stays
exact.

### 6. FXAA, then FSR, so 400 lines do not look pixelated

A new output pass runs FXAA 3.11 on the finished 720x400 frame
([me_output_fxaa.frag](../app/src/native/masseffect/shaders/me_output_fxaa.frag), `masseffect_native_output_fxaa`),
before FSR upscales it to the screen. FSR then enlarges smooth edges instead of stair steps. The sharpening is tuned
for legible HUD text (`present_fsr_sharpness_reduction = 0.1`, `masseffect_native_output_fxaa_subpix = 0.5`).

### 7. Cheaper shadows, kept

Character shadows stay, through the game's own config (`masseffect_ini_override`, no engine `SET` command):
each composite shadow map is at most 256x256 instead of 432x432 (about a third of the pixels), and shadows of
characters smaller than 64 texels on screen are not drawn. The Normandy crew scenes drew ~12 character shadows per
frame.

### 8. Lighter game thread

The same game-config route also changes the following:
- BioWare's combat VFX budget is 50 instead of 100.
- Dead bodies are cleaned up after 2 s instead of 5, while covering up to 30 % of the screen.
- Garbage collection runs every 60 s instead of 30, so its main-thread spikes happen half as often.
- The audio device updates its voices every 30 ms instead of 15.

In code, `masseffect_dle_visible_every = 2` updates each character's dynamic light environment every second frame.

### 9. Small-object distance culling

The designers set no draw distance on Mass Effect's static meshes (`CullDistance` is unset on ~6,100 of them in the
three areas checked), so distant rocks, debris and props are drawn however small they get. Outdoors they are most of
the draws. Cinnamon gives small static meshes a size-based draw distance: beyond 100 times their bounds radius
(at least 2,000 units) they are skipped, which at 400 lines is about 5 pixels across. A post-codegen patch
([tools/pch_cull_distance.py](../tools/pch_cull_distance.py)) puts the check into UE3's own per-mesh visibility test in
`InitViews`, so it costs nothing per frame
([me_cull_distance.cpp](../app/src/native/me_cull_distance.cpp)). Characters, effects and large meshes are never
affected. Tune it with `masseffect_cull_small_factor` (lower = more culling, 0 = off).

### 10. Platform settings

- `masseffect_switch_ram_1600`: the official handheld profile "GPU 460.8 MHz + memory 1600 MHz" (`0x92220007`)
  instead of memory at 1331 MHz. This is not an overclock; it is one of Nintendo's own configurations, and the game,
  render and ring threads spend much of their time in cache misses.
- `masseffect_exclusive_core = 22`: the UE3 render thread runs at a higher priority, with no core reserved for the
  main thread. At 1224 MHz the pinned main thread left ~40 % of a core idle while the other cores were full.

These build on the many CPU and renderer optimizations of the upstream port (native hot functions, deferred
command recording, EDRAM fast paths, texture coherency and more); see [optimization-paths.md](optimization-paths.md).

## Correctness fixes that came with it

A faster build is only useful if it still looks right. These were found on the way:

- **Lens flares and light halos through walls.** The port answers the game's occlusion queries "visible" without
  drawing them (real queries cost too much). Flares and glows therefore shone through rocks and characters. Cinnamon
  measures only those few queries for real: they are issued after the base pass, so they are tested against the full
  scene depth, and Vulkan occlusion queries count the pixels. The game never waits for the answer; each flare uses
  its latest measured value ([me_flare_occlusion.cpp](../app/src/native/me_flare_occlusion.cpp),
  `masseffect_flare_occlusion`).
- **Black cutscenes at 720x400.** The motion-blur pass reads a velocity buffer whose backing is 736 pixels wide at this
  resolution. Cropping it failed for its format, the pass was dropped every frame, and cutscenes, where that pass
  writes the final picture, stayed black. The crop now supports every plain colour format.
- **Cutscenes and engine `SET` commands.** UE3's `SET` re-attaches every component of a class, including Matinee
  cutscene actors, which froze or blacked out cutscenes. Cinnamon changes no engine state through `SET`; everything
  goes through the game config or code hooks.

## What was tried and left out

- **Skipping pose updates of off-screen characters** (`masseffect_skel_skip_unrendered`): no measurable gain, and a
  stale pose can show for one frame on a cutscene camera cut. Off.
- **Real occlusion queries for everything** (query modes 2 and 3): the game waits for them, or the answers follow
  pooled query objects wrongly. Mode 0 plus real flare queries is the best balance.
- **Overclocking:** not used and not needed for these results. Everything here is measured at 1224 / 460.8 MHz.

## Measuring it yourself

Profiles and reports are written next to the NRO in `logs/`. The useful lines are listed below; see
[measuring.md](measuring.md) for more.

| Line | Shows |
|---|---|
| `rex_profile.log` blocks | fps and the CPU share of each thread every 10 s |
| `[native] 10.0 s:` | Swaps (frames), draws, packets, rejected draws |
| `[native] GPU per Swap` | GPU time per frame and the `gap` (GPU waiting for the CPU) |
| `[native] ring partition` | where the ring thread's time goes (`masseffect_native_ring_partition = true`) |
| `[flare]`, `[cull]`, `[prepass]` | the Cinnamon features at work |
