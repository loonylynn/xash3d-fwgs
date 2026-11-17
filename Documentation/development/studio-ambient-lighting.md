# Studio ambient lighting migration

This document tracks the renderer-side migration to a Source-style studio lighting
model: a six-direction ambient cube sampled from existing BSP lightmaps and
consumed by the CPU per-vertex studio shading path. The existing GoldSrc
directional and local dynamic-light contributions remain active. It does not
require BSP recompilation or a map format change.

## Current status

### Phase 1 — directional sampling and cache

The renderer maintains a render-side ambient cube cache and exposes sampling
diagnostics. The cache is indexed by client entity, is updated only for visible
studio models, and uses a point-sample fallback before directional refinement
has completed. The cache is now consumed by the per-vertex shading path in
Phase 2 below.

The directional sampler in `ref/common/ref_light.c` reuses the recursive
style-aware BSP lightmap lookup used by `R_LightPoint`. `R_LightPoint` itself
is unchanged for existing callers. The new `R_LightSampleDir` API reports
whether the ray hit a usable lightmap and whether the surface uses an animated
light style.

The OpenGL studio renderer collects candidates only after its existing studio
bounding-box/frustum check succeeds. The scheduler runs after solid and
translucent studio entities and the viewmodel have been drawn. Invisible entities
therefore do not use the sampling budget or update their cache timestamps.

The cache is a fixed array indexed by client entity index, with a reserved slot
for the viewmodel. Each entry stores six linear-space RGB values, the last
sampled center, six per-axis sample times, a staggered next-axis index, validity
and animation masks, motion state, model identity, and the local
`renderGeneration` used to invalidate reused edict slots. Cache state resets on
renderer initialization and map changes. A backwards client-time jump invalidates
the temporal state so the next scheduled sample snaps instead of blending across
a demo rewind.

The per-frame scheduler uses a bounded top-K selection rather than a full sort.
Its score combines projected-size approximation, velocity, and sample age.
Incomplete cubes receive an age bonus until all six directions have been
attempted, so a point-only initial fallback continues to receive staggered
directional refinement even for stationary entities. A cache is eligible for
the static refresh interval only after all six axes have been attempted.
Far-LOD entries use one downward `R_LightPoint` sample for all six directions;
they return to directional refinement when they re-enter the near range.

Rays scale their offset and length from the entity's bounds. A solid start is
nudged toward the entity origin and uses a point-light fallback if it remains
solid. Sky, unlit surfaces, and misses are rejected as directional samples;
missing directions use the average of valid directions or the cached point
fallback. Samples are squared into linear space and blended with a time-based
exponential interpolation.

### Diagnostic cvars

| Cvar | Default | Purpose |
|---|---:|---|
| `r_studio_ambient_budget` | `12` | Maximum logical BSP light traces per rendered frame |
| `r_studio_ambient_far_dist` | `2048` | Distance beyond which one point sample is used for all six axes |
| `r_studio_ambient_static_interval` | `2.0` | Minimum refresh interval for completed stationary caches |
| `r_studio_ambient_cube` | `1` | Consume sampled ambient cubes in studio-model shading |
| `r_studio_lighting_debug` | `0` | Draw six sample rays and print scheduler/draw-lighting diagnostics |
| `r_studio_lighting_debug_entity` | `65` | Entity index for detailed draw diagnostics; `-1`" for all |

To inspect the sampler while the old studio shading is still active, set
`r_studio_lighting_debug 1`. The console line reports traces used/budget,
visible candidates, frustum-culled checks, static skips, far-LOD candidates, and
sampled caches. The ray colors show the cached cube values converted back from
linear space.

### Phase 2 — ambient-cube shading

The CPU per-vertex studio path now evaluates the six cached samples against a
world-space surface normal using Valve's squared-normal weighting. The cube
replaces the legacy scalar ambient term when its cache is valid. The existing
GoldSrc directional term (including its current Lambert response and light
colour) remains separate, and is combined with the cube in the renderer's
squared-gamma linear-light approximation. `R_LightLambert` continues to
apply local dynamic lights afterward. `STUDIO_NF_FULLBRIGHT` and
`STUDIO_NF_FLATSHADE` retain their legacy behavior; invalid caches and
`r_studio_ambient_cube 0` also fall back to the old shading path.

## Planned phases

- **Phase 3:** use final interpolated Jolt ragdoll bone transforms for the
  pelvis/center sample and optionally support per-body samples.
- **Phase 4:** add optional CPU light-warp ramp and rim-light controls.
- **Phase 5 (optional):** consider a shader implementation only after CPU output
  is stable and only if it can be kept in parity with the CPU path.

The GLSL compatibility shim exists, but it is not currently being used for the
mandatory lighting implementation. The CPU path remains the cross-backend
baseline.

## Verification status

The sampling/cache and CPU ambient-cube shading changes have been committed to
the engine fork. The source path has been inspected, but the engine has not yet
been built or runtime-tested in this work session. Verify both the initial point
fallback and subsequent six-axis refinement in-game before treating the shading
migration as complete. The next implementation phase is the ragdoll-specific
sample-position update using the final interpolated bone transforms.
