// Native graphics system (see me_native_system.cpp).
#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace me::native {

// true when MASSEFFECT_NATIVE=1: the app installs the native system instead of Xenos emulation.
bool Enabled();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

// Called on the game thread when Direct3D binds a shader object (sub_8222F850), before it patches the
// vertex shader: records which library entry the microcode at that physical address belongs to, so the
// ring can identify IM_LOADs by address No-op without the native system.
void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex);

// True when the shader object was matched to a library entry (diagnostics).
bool HasEntry(uint32_t object);

// Called on the game thread by the four Direct3D draw functions (src/me_d3d_trace.cpp): the bound VS
// and PS objects and the call's primitive type and counts. The ring pairs each DRAW_INDX with these
// records in order, because the ring's own shader loads can be immediate
// copies Direct3D patched. No-op without the native system.
void NoteDrawCall(const uint8_t* base, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7);

// Called by the ring thread after it writes the read pointer back: wakes the game's D3D GPU wait
// (src/native/me_ring_wait.cpp, sub_8222FA98).
void NotifyRingProgress();

// Flare occlusion (src/native/me_flare_occlusion.cpp, masseffect_flare_occlusion with query mode 0).
// Active: the feature is on. TakeMeasured (ring thread, at a query's BEGIN, with the end structure's physical
// address): true once for a flare query, which the ring then measures for real. Answer (render thread, GetData of an
// occlusion query; ready/count: what the original GetData would return): true for a flare query, with the count to give.
bool FlareOcclusionActive();
bool FlareTakeMeasured(uint32_t end_address);
// Ring thread, at a query's END: the element (key) the guest registered for this issue of the end structure, 0 if none.
uint32_t FlareTakeKey(uint32_t end_address);
// Ring thread, when the GPU count of a flare query is read (masseffect_native_targets.cpp ReadOcclusion).
void FlareResult(uint32_t key, uint32_t samples);
bool FlareAnswer(uint32_t query, bool ready, uint32_t& count);

// Hang watchdog (src/native/me_hang_watchdog.cpp), nothing unless masseffect_hang_watchdog = true.
// Start: once the guest image is loaded. Interval: the Swaps and draws of each 10-second renderer report.
void StartHangWatchdog();
void HangWatchdogInterval(uint64_t swaps, uint64_t draws);

}  // namespace me::native
