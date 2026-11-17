# Studio ambient lighting migration

This document tracks the renderer-side migration to a Source-style studio lighting
model: a six-direction ambient cube sampled from existing BSP lightmaps, with
Half-Lambert directional and dynamic lighting to be added in the later shading
phase. It does not require BSP recompilation or a map format change.

## Current status

### Phase 1 — directional sampling and cache

The existing studio shading path is intentionally still active. Phase 1 only
maintains a render-side ambient cube cache and exposes sampling diagnostics; it
does not alter the color submitted for any studio mesh.

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
| `r_studio_lighting_debug` | `0` | Draw six sample rays per visible cache and print scheduler counters |

To inspect the sampler while the old studio shading is still active, set
`r_studio_lighting_debug 1`. The console line reports traces used/budget,
visible candidates, frustum-culled checks, static skips, far-LOD candidates, and
sampled caches. The ray colors show the cached cube values converted back from
linear space.

## Planned phases

- **Phase 2:** consume the ambient cube in the CPU per-vertex studio lighting
  path, add Half-Lambert directional/dynamic-light contributions without
  double-counting, then remove the superseded studio lighting helpers.
- **Phase 3:** use the final interpolated Jolt ragdoll bone transforms for the
  pelvis/center sample and optionally support per-body samples.
- **Phase 4:** add the optional CPU light-warp ramp and rim-light controls.
- **Phase 5 (optional):** consider a shader implementation only after CPU output
  is stable and only if it can be kept in parity with the CPU path.

The GLSL compatibility shim exists, but it is not currently being used for the
mandatory lighting implementation. The CPU path remains the cross-backend
baseline.

## Verification status

The Phase 1 changes have been committed to the engine fork. Static source checks
have been performed, but the engine has not been built and the debug rays have
not been observed in-game in this work session. Phase 1 is therefore implemented
but not yet runtime-verified. It must be built and inspected before moving to the
shading replacement in Phase 2.
