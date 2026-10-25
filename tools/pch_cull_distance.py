#!/usr/bin/env python3
"""Size-aware draw distance of small static meshes (run after every codegen).

UE3's InitViews (English edition sub_824F50B0) marks each static mesh element of a visible primitive in
View.StaticMeshVisibilityMap when MinDrawDistanceSquared (+272) <= distance^2 < MaxDrawDistanceSquared (+276), the
distance being from the view origin to the primitive's bounds origin (FPrimitiveSceneInfo +44, in r25). The designers
left CullDistance unset on all ~6,100 static meshes of the three areas checked (docs/draw-count-options.md #3), so
small props stay drawn however far away. This inserts, after the load of the element's MaxDrawDistanceSquared,

    ctx.f13.f64 = me_cull_max_distance_sq(ctx.f13.f64, r25.u32, base);

which lowers it for small primitives (src/native/me_cull_distance.cpp, masseffect_cull_small_*). With the feature off
the function returns its first argument. Only the static-mesh element loop is touched (dynamic primitives:
characters, particles, are not); large meshes are never culled.

The file is found by searching the generated tree. Idempotent; exit 1 if the site is not found exactly once.
Flags: see tools/pch_common.py (--gen, --dry-run, --check).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pch_common as c

NAME = 'pch_cull_distance'
F = 'sub_824F50B0'
MARK = 'me_cull_max_distance_sq'
args = c.Args()
path = args.file or c.search_definition(args.gen, F)
s = c.read(path)
start = s.find('DEFINE_REX_FUNC(%s) {' % F)
if start < 0:
    c.failure('%s: no %s definition in %s' % (NAME, F, path))
end = s.find('\n}\n', start)
if end < 0:
    c.failure('%s: end of %s not found' % (NAME, F))
body = s[start:end]
if MARK in body:
    c.finish(args, path, s, True, NAME)

SITE = ('\ttemp.u32 = REX_LOAD_D_U32(r11.u32, 276);\n'
        '\tctx.f13.f64 = double(temp.f32);\n'
        '\t// fcmpu cr6,f0,f13\n'
        '\tcr6.compare(f0.f64, ctx.f13.f64);\n')
if body.count(SITE) != 1:
    c.failure('%s: the MaxDrawDistanceSquared compare was found %d times in %s (expected 1)'
              % (NAME, body.count(SITE), F))
if 'PPCRegister r25{}' not in body or 'r29.s64 = r25.s64 + 44;' not in body:
    c.failure('%s: r25 is not the primitive scene info in %s' % (NAME, F))
PATCHED = SITE.replace('\tctx.f13.f64 = double(temp.f32);\n',
                       '\tctx.f13.f64 = double(temp.f32);\n'
                       '\tctx.f13.f64 = %s(ctx.f13.f64, r25.u32, base);  // tools/%s.py\n' % (MARK, NAME))
body2 = body.replace(SITE, PATCHED, 1)
decl = 'extern "C" double %s(double max_sq, uint32_t scene_info, uint8_t* base);\n\n' % MARK
s2 = s[:start] + decl + body2 + s[end:]
print('%s: patched %s in %s' % (NAME, F, os.path.basename(path)))
c.finish(args, path, s2, False, NAME)
