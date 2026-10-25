// Mass Effect - the scene depth prepass is not built by the game at all (docs/draw-count-options.md, "Drop the
// prepass at the source").
//
// WHAT THE 2007 ENGINE DOES (addresses of this edition)
//   FSceneRenderer::Render (sub_824F5FE8) calls, per depth priority group, RenderPrePass (sub_824F6890). For the
//   world group with occlusion testing on, that sets the depth-only state and calls RenderDPGPrePass
//   (sub_824F6DB0) twice: foreground group into a compressed depth range, then the world group. RenderDPGPrePass
//   draws the PositionOnlyDepthDrawList (+8) and DepthDrawList (+36) of Scene->DPGs[DPG] (196 bytes each) with
//   View.StaticMeshOccluderMap (View+540), then the dynamic occluders (View+564, count +568; bUseAsOccluder, bit
//   0x2000000 of the proxy's +40) through a TDynamicPrimitiveDrawer, and returns bDirty. RenderPrePass ORs the
//   results and returns them; Render ignores that return value. The occlusion queries (sub_827E2D10,
//   sub_82392670) come after the prepass inside RenderPrePass and are not touched here.
//
// WHY
//   The port throws the prepass draws away on the ring (masseffect_native_skip_prepass, the B1 black-shards fix):
//   EDRAM mode 5 draws at the scene pitch. Measured on the Normandy deck (2026-10-25, 1224 MHz): ~430 such draws
//   per frame of ~2,300, all built by the UE3 render thread and parsed by the ring thread (PM4 parsing, not the
//   draw translation, is most of the ring's ~45 ms per frame there), then dropped. Not building them saves that
//   work on both threads, with the same picture.
//
// masseffect_guest_skip_prepass = true (default): RenderDPGPrePass returns FALSE without drawing, but only while
//   masseffect_native_skip_prepass is on and masseffect_native_query_occlusion_depth is off (that mode records the
//   prepass draws into the occlusion depth, so it needs them).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_BOOL(masseffect_guest_skip_prepass, true, "Mass Effect",
                    "The game does not build the scene depth prepass (RenderDPGPrePass returns without drawing). "
                    "Only while masseffect_native_skip_prepass is on (the port drops those draws anyway) and "
                    "masseffect_native_query_occlusion_depth is off. Saves UE3 render thread and ring thread time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, masseffect_native_skip_prepass);           // masseffect_native_draws.cpp
REXCVAR_DECLARE(bool, masseffect_native_query_occlusion_depth);  // masseffect_native_draws.cpp

REX_EXTERN(__imp__sub_824F6DB0);  // FSceneRenderer::RenderDPGPrePass(UINT DPGIndex, const FViewInfo& View)

namespace {
std::atomic<uint64_t> g_prepass_skipped{0};
}  // namespace

REX_HOOK_RAW(sub_824F6DB0) {
  static const bool skip = REXCVAR_GET(masseffect_guest_skip_prepass) &&
                           REXCVAR_GET(masseffect_native_skip_prepass) &&
                           !REXCVAR_GET(masseffect_native_query_occlusion_depth);
  if (!skip) {
    __imp__sub_824F6DB0(ctx, base);
    return;
  }
  const uint64_t n = g_prepass_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || (n & 0xFFFF) == 0)
    REXLOG_INFO("[prepass] scene depth prepass not built by the game ({} RenderDPGPrePass calls skipped)", n);
  ctx.r3.u64 = 0;  // bDirty = FALSE (RenderPrePass ORs it into a result its caller ignores)
}
