// Mass Effect - lens flares and light glows hidden behind geometry again, with the occlusion queries still in
// mode 0 for everything else (docs/occlusion-queries.md, section F).
//
// WHAT THE 2007 ENGINE DOES (addresses of this edition)
//   InitViews (sub_824F50B0) puts every visible primitive with relevance bit 0x10000000 (flares, glows) into a
//   per-view, per-DPG list (View + 996 + 12 * DPG). RenderPrePass (sub_824F6890), after the depth prepass, calls
//   sub_827E2D10(list, RHI, View) for the world DPG: for each element it takes a pooled query (sub_82391F78),
//   Issue(BEGIN) (sub_82228F58, flags 2), draws the element's occlusion sprite (sub_827E2B48(RHI, element + 44), color
//   and depth writes off, depth test on), Issue(END) (flags 1), and keeps the query in the view state. The light
//   and primitive visibility pass (sub_82392F28) reads it back with GetData (sub_82229158); the flare reads the
//   magnitude of the count.
//
// WHY IT SHOWED THROUGH ROCKS
//   Query mode 0 answers every query "1000 samples" (real queries cost too much on the ring, and the prepass the
//   boxes were tested against is not drawn: masseffect_native_skip_prepass, masseffect_guest_skip_prepass). So a
//   flare is always fully visible, behind rocks and characters too (Eden Prime camp fire, BIOA_PRO10_08).
//
// WHAT THIS DOES (masseffect_flare_occlusion = true, only with masseffect_native_query_mode = 0)
//   1. The flare list is not issued inside RenderPrePass but right after the base pass of the same DPG
//      (sub_824F7020): the scene depth then holds every opaque surface, as the console's prepass depth did.
//   2. At Issue(BEGIN) the query's end structure (physical address) is registered, at Issue(END) the element it
//      measures (key: the element's sprite data pointer). The ring measures registered queries with real Vulkan
//      occlusion queries (the mode 2 machinery, only for these few) and, when it reads the GPU count, hands it to
//      that element's history (FlareResult). All other queries stay "visible" at no cost.
//   3. GetData of a flare query never waits: the real count if the GPU result is already in the end structure,
//      otherwise the element's latest measured count, "visible" for an element never measured.
//   Run 9 (2026-10-25) showed why step 2 feeds the history from the ring: the game reads a flare query about a frame
//   after issuing it, before the GPU result exists, so a history fed only by GetData was rarely refreshed (27 real
//   answers in 10 s against 1,700 issues) and the halo stayed visible. Hidden flares fade two to four frames late.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

REXCVAR_DEFINE_BOOL(masseffect_flare_occlusion, true, "Mass Effect",
                    "Lens flares and light glows are occlusion-tested for real (only those queries; query mode 0 "
                    "keeps answering 'visible' for the rest): their queries are issued after the base pass and "
                    "measured by the ring with Vulkan occlusion queries. Fixes flares and halos showing through "
                    "geometry")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(int32_t, masseffect_native_query_mode);  // me_native_system.cpp

REX_EXTERN(__imp__sub_827E2D10);  // issues the occlusion queries of a view's flare list (list, RHI, View)
REX_EXTERN(__imp__sub_824F7020);  // FSceneRenderer::RenderBasePass(DPG)
REX_EXTERN(__imp__sub_827E2B48);  // draws one flare element's occlusion sprite (RHI, element + 44)
REX_EXTERN(__imp__sub_82228F58);  // D3D Issue(query, flags): 2 = BEGIN, 1 = END

namespace me::native {
namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

bool Active() {
  static const bool active = Enabled() && REXCVAR_GET(masseffect_flare_occlusion) &&
                             REXCVAR_GET(masseffect_native_query_mode) == 0;
  return active;
}

// --- Render thread state (the hooks below and GetData all run on the UE3 render thread) ---
struct Deferred {
  uint32_t list, rhi, view;
};
std::array<Deferred, 8> g_deferred;
uint32_t g_deferred_count = 0;
bool g_issuing = false;      // inside the deferred sub_827E2D10
uint32_t g_query = 0;        // query object between its BEGIN and END
uint32_t g_key = 0;          // element of that query (its sprite data pointer)
std::unordered_map<uint32_t, uint32_t> g_query_key;  // issued flare query object -> element

// --- Shared with the ring thread (g_ring_mutex) ---
std::mutex g_ring_mutex;
std::unordered_map<uint32_t, uint32_t> g_ring_begins;             // end structure -> BEGINs not yet parsed
std::unordered_map<uint32_t, std::deque<uint32_t>> g_ring_keys;  // end structure -> elements of ENDs not yet parsed
std::unordered_map<uint32_t, uint32_t> g_last;                    // element -> latest measured count
// The address form the ring sees in RB_SAMPLE_COUNT_ADDR: 0 not known yet (both registered), 1 physical,
// 2 physical + the uncached view's 4 KB step.
std::atomic<int> g_form{0};

struct Stats {
  std::atomic<uint64_t> deferred{0}, issued{0}, measured{0}, fed{0}, fed_zero{0}, real{0}, real_zero{0}, history{0},
      unknown{0};
  int64_t next_ms = 0;  // render thread
} g_stats;

void Report() {
  using namespace std::chrono;
  const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  if (now < g_stats.next_ms) return;
  const bool first = g_stats.next_ms == 0;
  g_stats.next_ms = now + 10000;
  if (first) return;
  size_t known = 0;
  {
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    known = g_last.size();
  }
  REXLOG_INFO("[flare] 10 s: {} flare lists issued after the base pass, {} queries, {} measured by the ring, {} GPU "
              "counts fed to the elements ({} of them 0 = hidden); answers: {} real ({} of them 0), {} from the "
              "element's latest count, {} never measured (visible); {} elements known, address form {}",
              g_stats.deferred.exchange(0), g_stats.issued.exchange(0), g_stats.measured.exchange(0),
              g_stats.fed.exchange(0), g_stats.fed_zero.exchange(0), g_stats.real.exchange(0),
              g_stats.real_zero.exchange(0), g_stats.history.exchange(0), g_stats.unknown.exchange(0), known,
              g_form.load(std::memory_order_relaxed));
}

// The end structure's physical address, from the query's first tile entry as GetData computes its read address.
// Both forms until the ring has matched one (FlareTakeMeasured), then only that one.
template <typename F>
void ForEachForm(const uint8_t* base, uint32_t query, F&& f) {
  const uint32_t e = Load32(base, query + 24);
  if (!e) return;
  const uint32_t physical = e & 0x1FFFFFFFu;
  const uint32_t stepped = physical + ((((e >> 20) & 0xFFFu) + 512u) & 0x1000u);
  const int form = g_form.load(std::memory_order_relaxed);
  if (form != 2) f(physical);
  if (form != 1 && stepped != physical) f(stepped);
}

// While the form is not known: which form each registered address is, for queries whose two forms differ.
std::unordered_map<uint32_t, int> g_form_of;  // g_ring_mutex

void RegisterBegin(const uint8_t* base, uint32_t query) {
  std::lock_guard<std::mutex> lock(g_ring_mutex);
  if (g_ring_begins.size() > 4096) g_ring_begins.clear();  // a form the ring never sees
  ForEachForm(base, query, [](uint32_t address) { ++g_ring_begins[address]; });
  if (g_form.load(std::memory_order_relaxed) == 0) {
    const uint32_t e = Load32(base, query + 24);
    const uint32_t physical = e & 0x1FFFFFFFu;
    const uint32_t stepped = physical + ((((e >> 20) & 0xFFFu) + 512u) & 0x1000u);
    if (e && stepped != physical) {
      if (g_form_of.size() > 4096) g_form_of.clear();
      g_form_of[physical] = 1;
      g_form_of[stepped] = 2;
    }
  }
}

void RegisterEnd(const uint8_t* base, uint32_t query, uint32_t key) {
  std::lock_guard<std::mutex> lock(g_ring_mutex);
  if (g_ring_keys.size() > 4096) g_ring_keys.clear();
  ForEachForm(base, query, [key](uint32_t address) {
    auto& queue = g_ring_keys[address];
    if (queue.size() >= 64) queue.pop_front();
    queue.push_back(key);
  });
}

}  // namespace

bool FlareOcclusionActive() { return Active(); }

bool FlareTakeMeasured(uint32_t end_address) {
  if (!Active()) return false;
  end_address &= 0x1FFFFFFFu;
  std::lock_guard<std::mutex> lock(g_ring_mutex);
  if (g_ring_begins.empty()) return false;
  const auto it = g_ring_begins.find(end_address);
  if (it == g_ring_begins.end()) return false;
  if (--it->second == 0) g_ring_begins.erase(it);
  g_stats.measured.fetch_add(1, std::memory_order_relaxed);
  if (g_form.load(std::memory_order_relaxed) == 0) {
    // The first match of a query whose two forms differ decides which one the ring sees; the other is no longer
    // registered (its leftover entries are dropped by the size caps).
    if (const auto f = g_form_of.find(end_address); f != g_form_of.end()) {
      g_form.store(f->second, std::memory_order_relaxed);
      g_form_of.clear();
      REXLOG_INFO("[flare] the ring sees the end structures in form {} ({:08X})", f->second, end_address);
    }
  }
  return true;
}

uint32_t FlareTakeKey(uint32_t end_address) {
  if (!Active()) return 0;
  std::lock_guard<std::mutex> lock(g_ring_mutex);
  if (g_ring_keys.empty()) return 0;
  const auto it = g_ring_keys.find(end_address & 0x1FFFFFFFu);
  if (it == g_ring_keys.end()) return 0;
  const uint32_t key = it->second.front();
  it->second.pop_front();
  if (it->second.empty()) g_ring_keys.erase(it);
  return key;
}

void FlareResult(uint32_t key, uint32_t samples) {
  if (!key) return;
  {
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    if (g_last.size() > 4096) g_last.clear();
    g_last[key] = samples;
  }
  g_stats.fed.fetch_add(1, std::memory_order_relaxed);
  if (!samples) g_stats.fed_zero.fetch_add(1, std::memory_order_relaxed);
}

bool FlareAnswer(uint32_t query, bool ready, uint32_t& count) {
  if (!Active()) return false;
  const auto it = g_query_key.find(query);
  if (it == g_query_key.end()) return false;
  const uint32_t key = it->second;
  g_query_key.erase(it);
  {
    std::lock_guard<std::mutex> lock(g_ring_mutex);
    if (ready) {
      g_last[key] = count;
      g_stats.real.fetch_add(1, std::memory_order_relaxed);
      if (!count) g_stats.real_zero.fetch_add(1, std::memory_order_relaxed);
    } else if (const auto last = g_last.find(key); last != g_last.end()) {
      count = last->second;
      g_stats.history.fetch_add(1, std::memory_order_relaxed);
    } else {
      count = 1000;  // never measured: visible, as mode 0
      g_stats.unknown.fetch_add(1, std::memory_order_relaxed);
    }
  }
  Report();
  return true;
}

}  // namespace me::native

using namespace me::native;

// RenderPrePass: the flare list is remembered and issued after the base pass instead.
REX_HOOK_RAW(sub_827E2D10) {
  if (!Active() || g_issuing) {
    __imp__sub_827E2D10(ctx, base);
    return;
  }
  if (g_deferred_count < g_deferred.size()) {
    g_deferred[g_deferred_count++] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32};
  } else {
    __imp__sub_827E2D10(ctx, base);  // more views than expected: as before
    return;
  }
  ctx.r3.u64 = 0;
}

// RenderBasePass: then the flare lists of this frame, against the complete scene depth.
REX_HOOK_RAW(sub_824F7020) {
  __imp__sub_824F7020(ctx, base);
  if (!g_deferred_count) return;
  const uint64_t result = ctx.r3.u64;
  const uint32_t count = g_deferred_count;
  g_deferred_count = 0;
  g_issuing = true;
  for (uint32_t i = 0; i < count; ++i) {
    ctx.r3.u64 = g_deferred[i].list;
    ctx.r4.u64 = g_deferred[i].rhi;
    ctx.r5.u64 = g_deferred[i].view;
    __imp__sub_827E2D10(ctx, base);
  }
  g_issuing = false;
  g_query = g_key = 0;
  g_stats.deferred.fetch_add(count, std::memory_order_relaxed);
  if (g_query_key.size() > 4096) g_query_key.clear();  // queries the game never read back
  ctx.r3.u64 = result;
}

REX_HOOK_RAW(sub_827E2B48) {
  if (g_issuing) g_key = ctx.r4.u32;
  __imp__sub_827E2B48(ctx, base);
}

REX_HOOK_RAW(sub_82228F58) {
  if (g_issuing && ctx.r3.u32) {
    const uint32_t query = ctx.r3.u32;
    if (ctx.r4.u32 == 2) {  // BEGIN: registered before D3D writes the packet
      g_query = query;
      g_key = 0;
      RegisterBegin(base, query);
      g_stats.issued.fetch_add(1, std::memory_order_relaxed);
    } else if (ctx.r4.u32 == 1 && query == g_query) {  // END: registered before D3D writes the packet
      const uint32_t key = g_key ? g_key : query;
      g_query_key[query] = key;
      RegisterEnd(base, query, key);
      g_query = 0;
    }
  }
  __imp__sub_82228F58(ctx, base);
}
