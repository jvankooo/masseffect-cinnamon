// Mass Effect - size-aware draw distance for small static meshes (docs/draw-count-options.md, #3 and section 4).
//
// The designers left PrimitiveComponent.CullDistance unset on every static mesh checked (~6,100 in three areas), so
// small rocks, debris and props are drawn however far away; outdoors they are most of the draws (t240: 65-70 %).
// Each draw costs the UE3 render thread, the ring thread and the GPU, and in the heavy views all three CPU cores are
// full. Here a small primitive gets a draw distance proportional to its size: beyond masseffect_cull_small_factor x
// its bounds radius (at least masseffect_cull_small_min_distance) it is not drawn. At the 400-line scene and the
// game's field of view a primitive at 100 x its radius is ~5 pixels across, so what disappears is a speck.
// Primitives with a radius above masseffect_cull_small_max_radius (buildings, cliffs, terrain, large props) keep
// their own distance; dynamic primitives (characters, particles, effects) are not on this path at all.
//
// Where: UE3's InitViews (sub_824F50B0) tests each static mesh element of a visible primitive against its
// MinDrawDistanceSquared (+272) and MaxDrawDistanceSquared (+276) with the squared distance from the view origin to
// the primitive's bounds origin; tools/pch_cull_distance.py makes it read the maximum through
// me_cull_max_distance_sq(max, scene info, base). The bounds (FBoxSphereBounds) sit at FPrimitiveSceneInfo + 44:
// origin +44, box extent +56 (both passed to the frustum test there), sphere radius +68.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(masseffect_cull_small_factor, 100, "Mass Effect",
                     "Small static meshes are not drawn beyond this many times their bounds radius (0 = off; 100 "
                     "= about 5 pixels across at the 400-line scene). Saves UE3 render thread, ring and GPU time "
                     "outdoors")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_cull_small_max_radius, 256, "Mass Effect",
                     "masseffect_cull_small_factor applies only to static meshes with a bounds radius up to this "
                     "(Unreal units); larger ones keep their own draw distance")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_cull_small_min_distance, 2000, "Mass Effect",
                     "masseffect_cull_small_factor never culls closer than this (Unreal units)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

struct Settings {
  bool on = false;
  double factor = 0.0, max_radius = 0.0, min_distance = 0.0;
};

const Settings& Get() {
  static const Settings s = [] {
    Settings v;
    v.factor = double(REXCVAR_GET(masseffect_cull_small_factor));
    v.max_radius = double(REXCVAR_GET(masseffect_cull_small_max_radius));
    v.min_distance = double(REXCVAR_GET(masseffect_cull_small_min_distance));
    v.on = v.factor > 0.0;
    REXLOG_INFO("[cull] small static meshes: {} (beyond {} x radius, at least {} units, radius up to {})",
                v.on ? "on" : "off", v.factor, v.min_distance, v.max_radius);
    return v;
  }();
  return s;
}

// Render thread only (InitViews).
uint64_t g_calls = 0, g_clamped = 0, g_logged = 0;
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

}  // namespace

extern "C" double me_cull_max_distance_sq(double max_sq, uint32_t scene_info, uint8_t* base) {
  const Settings& s = Get();
  if (!s.on || !scene_info) return max_sq;
  uint32_t bits;
  std::memcpy(&bits, base + scene_info + 68, sizeof(bits));
  bits = __builtin_bswap32(bits);
  float radius;
  std::memcpy(&radius, &bits, sizeof(radius));
  ++g_calls;
  if (g_logged < 8) {
    ++g_logged;
    REXLOG_INFO("[cull] sample primitive {:08X}: bounds radius {:.1f}, own max draw distance {:.0f}", scene_info,
                radius, max_sq > 0.0 && max_sq < 1e30 ? std::sqrt(max_sq) : -1.0);
  }
  if ((g_calls & 0xFFFF) == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last >= std::chrono::seconds(10)) {
      REXLOG_INFO("[cull] last ~10 s: {} static mesh element tests, {} with a size-based draw distance", g_calls,
                  g_clamped);
      g_calls = g_clamped = 0;
      g_last = now;
    }
  }
  if (!(radius > 0.0f) || radius > s.max_radius) return max_sq;
  const double d = std::max(s.min_distance, double(radius) * s.factor);
  const double d2 = d * d;
  if (d2 >= max_sq) return max_sq;
  ++g_clamped;
  return d2;
}
