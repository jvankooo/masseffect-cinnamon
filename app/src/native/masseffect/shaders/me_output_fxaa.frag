#version 450
// Output pass with FXAA (masseffect_native_output_fxaa): the front buffer drawn into the presenter's guest output,
// texel for texel, with FXAA 3.11 "quality" (preset 12: 5 edge-search steps, 1.0 1.5 2.0 4.0 12.0 texels) and then
// the game's gamma ramp. It replaces the exact-texel variant of masseffect_output_ramp_gamma_ps: same bindings, same
// ramp lookup, and a pixel that is not on an edge reads exactly its own texel, so only edge pixels change.
//
// Why here: at an internal resolution below 720p the presenter's FSR upscale stretches stair-stepped edges to 1.5x
// their size; anti-aliasing the frame before the upscale gives FSR smooth edges to work from. The pass runs on the
// small (WxH) image, after the HUD is composed, like the Xbox 360's own FXAA titles.
//
// With an internal resolution the source is the 1280x720 front buffer and only its WxH corner holds this frame
// (me_resolution.cpp, masseffect_scene_front_full): every tap is clamped to uv_max, the centre of the last valid
// texel, so no pixel outside the corner (stale or never written) can bleed into the right and bottom edges.
layout(set = 0, binding = 0) uniform texture2D source_image;
layout(set = 0, binding = 1) uniform sampler source_sampler;  // linear, clamp to edge (immutable)
layout(set = 0, binding = 2, std140) uniform Ramp {
  vec4 entries[256];  // the game's ramp per channel, 0-1
  vec4 mixing;        // grading of the generic output shader, unused here (neutral in this port)
  vec4 effects;       // vignette and scanlines of the generic output shader, unused here (off)
} ramp;
layout(push_constant) uniform Constants {
  layout(offset = 16) ivec2 offset;    // output pixel -> source texel
  layout(offset = 24) vec2 inverse;    // 1 / source image size
  layout(offset = 32) vec2 uv_max;     // centre of the last texel of the valid corner
  layout(offset = 40) float subpix;    // FXAA subpixel aliasing removal, 0 (off) - 1 (softest)
  layout(offset = 44) float threshold; // FXAA minimum local contrast for an edge, relative to the local maximum
} constants;
layout(location = 0) out vec4 color;

// Edges darker than this are not processed (FXAA's fxaaQualityEdgeThresholdMin, "high quality" value).
const float kThresholdMin = 0.0625;
const float kSteps[5] = float[5](1.0, 1.5, 2.0, 4.0, 12.0);

vec3 Fetch(vec2 uv) {
  return textureLod(sampler2D(source_image, source_sampler), min(uv, constants.uv_max), 0.0).rgb;
}

// Luma of the gamma-encoded colour (Rec. 601 weights, as the generic output shader's FXAA).
float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float LumaAt(vec2 uv) { return Luma(Fetch(uv)); }

vec3 Fxaa(vec2 pos) {
  const vec2 rcp = constants.inverse;
  const vec3 rgb_m = Fetch(pos);
  const float luma_m = Luma(rgb_m);
  float luma_s = LumaAt(pos + vec2(0.0, rcp.y));
  const float luma_e = LumaAt(pos + vec2(rcp.x, 0.0));
  float luma_n = LumaAt(pos - vec2(0.0, rcp.y));
  const float luma_w = LumaAt(pos - vec2(rcp.x, 0.0));
  const float range_max = max(max(luma_n, luma_w), max(luma_e, max(luma_s, luma_m)));
  const float range_min = min(min(luma_n, luma_w), min(luma_e, min(luma_s, luma_m)));
  const float range = range_max - range_min;
  if (range < max(kThresholdMin, range_max * constants.threshold)) return rgb_m;  // not an edge: exact texel

  const float luma_nw = LumaAt(pos - rcp);
  const float luma_se = LumaAt(pos + rcp);
  const float luma_ne = LumaAt(pos + vec2(rcp.x, -rcp.y));
  const float luma_sw = LumaAt(pos + vec2(-rcp.x, rcp.y));

  // Horizontal or vertical edge, from the second differences of the 3x3 neighbourhood.
  const float luma_ns = luma_n + luma_s;
  const float luma_we = luma_w + luma_e;
  const float edge_horz = abs(-2.0 * luma_w + luma_nw + luma_sw) + abs(-2.0 * luma_m + luma_ns) * 2.0 +
                          abs(-2.0 * luma_e + luma_ne + luma_se);
  const float edge_vert = abs(-2.0 * luma_s + luma_sw + luma_se) + abs(-2.0 * luma_m + luma_we) * 2.0 +
                          abs(-2.0 * luma_n + luma_nw + luma_ne);
  const bool horz_span = edge_horz >= edge_vert;
  // Subpixel blend amount: how much the centre differs from the 3x3 average, relative to the local contrast.
  const float subpix_a = (luma_ns + luma_we) * 2.0 + (luma_nw + luma_sw + luma_ne + luma_se);
  const float subpix_c = clamp(abs(subpix_a * (1.0 / 12.0) - luma_m) / range, 0.0, 1.0);
  const float subpix_f = (-2.0 * subpix_c + 3.0) * subpix_c * subpix_c;
  const float subpix_h = subpix_f * subpix_f * constants.subpix;

  // The side of the edge with the steeper gradient; the search runs half a texel towards it, along the edge.
  if (!horz_span) {
    luma_n = luma_w;
    luma_s = luma_e;
  }
  float length_sign = horz_span ? rcp.y : rcp.x;
  const float gradient_n = luma_n - luma_m;
  const float gradient_s = luma_s - luma_m;
  const bool pair_n = abs(gradient_n) >= abs(gradient_s);
  const float gradient_scaled = max(abs(gradient_n), abs(gradient_s)) * 0.25;
  if (pair_n) length_sign = -length_sign;
  const float luma_local_average = 0.5 * (pair_n ? luma_n + luma_m : luma_s + luma_m);
  const bool luma_m_below = luma_m - luma_local_average < 0.0;

  vec2 pos_b = pos;
  if (horz_span) pos_b.y += length_sign * 0.5;
  else pos_b.x += length_sign * 0.5;
  const vec2 step_np = horz_span ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);

  // Walk both ends of the edge until the luma leaves the edge's pair average (or the steps run out).
  vec2 pos_n = pos_b - step_np * kSteps[0];
  vec2 pos_p = pos_b + step_np * kSteps[0];
  float luma_end_n = LumaAt(pos_n) - luma_local_average;
  float luma_end_p = LumaAt(pos_p) - luma_local_average;
  bool done_n = abs(luma_end_n) >= gradient_scaled;
  bool done_p = abs(luma_end_p) >= gradient_scaled;
  if (!done_n) pos_n -= step_np * kSteps[1];
  if (!done_p) pos_p += step_np * kSteps[1];
  for (int i = 2; i < 5 && !(done_n && done_p); ++i) {
    if (!done_n) luma_end_n = LumaAt(pos_n) - luma_local_average;
    if (!done_p) luma_end_p = LumaAt(pos_p) - luma_local_average;
    done_n = abs(luma_end_n) >= gradient_scaled;
    done_p = abs(luma_end_p) >= gradient_scaled;
    if (!done_n) pos_n -= step_np * kSteps[i];
    if (!done_p) pos_p += step_np * kSteps[i];
  }

  // Blend across the edge by how close this pixel is to the nearer end of the span.
  const float dst_n = horz_span ? pos.x - pos_n.x : pos.y - pos_n.y;
  const float dst_p = horz_span ? pos_p.x - pos.x : pos_p.y - pos.y;
  const bool direction_n = dst_n < dst_p;
  const bool good_span = ((direction_n ? luma_end_n : luma_end_p) < 0.0) != luma_m_below;
  const float pixel_offset = 0.5 - min(dst_n, dst_p) / (dst_n + dst_p);
  const float offset = max(good_span ? pixel_offset : 0.0, subpix_h);
  vec2 pos_f = pos;
  if (horz_span) pos_f.y += offset * length_sign;
  else pos_f.x += offset * length_sign;
  return Fetch(pos_f);
}

void main() {
  const ivec2 pixel = ivec2(gl_FragCoord.xy) - constants.offset;
  const vec3 texel = Fxaa((vec2(pixel) + 0.5) * constants.inverse);
  // The game's gamma ramp, per channel, as masseffect_output_ramp_gamma_ps (256 entries, nearest).
  const uvec3 i = uvec3(clamp(texel, 0.0, 1.0) * 255.0 + 0.5);
  color = vec4(ramp.entries[i.r].r, ramp.entries[i.g].g, ramp.entries[i.b].b, 1.0);
}
