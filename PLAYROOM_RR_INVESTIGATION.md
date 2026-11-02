# ASTRO's PLAYROOM RR integration investigation

Game: `D:\Eboot\PPSA01325`, 01.905.000, matching the patch's source SHA-256.
Date: 2026-10-16. **Game RR is not implemented by this investigation.**

## Actual scene evidence

For the diagnostic launches only, `build/PPSA01325-rr-investigation.json` disables
`Select the existing non-tiled deferred-lighting renderer` and
`Select the common GI-unavailable state`. The source/deployed game patches were not changed.
The elapsed-time gameplay patch remains disabled and QHD Quality remains selected.

The initial frame-180/181 window captured 65 passes and 57 candidate image dependencies.
The later scene window at frames 600/601 captured 93 passes, 75 candidate dependencies
and eight submitted BVH dispatches: four kernels per frame:

- `5DF28D2058CAF728`
- `55D28021295DC816`
- `C18F4AD08601D141`
- `D65C9DF1AD5E3AF5`

All four write the same render-sized RGBA16F lighting image (1708x960). Its native identity
must follow allocation lifetime; the observed address is not a safe integration identifier.
Following consumers include `113ACD3E87A31CF0`, `D15E00A777D250B5`,
`102D2A9E3D8AA477` and `EB87B8B7C526169D`. Attachment preservation edges do not prove a
denoising operation or justify skipping any of these draws.

In the captured RDNA2 of `5DF28D2058CAF728`, PCs `0xDC8`, `0xDD0`, `0xDD8` subtract the
reconstructed primary position (`v40/v41/v42`) from camera-block offset-400 coordinates.
At `0x1F9C`, `0x1FD8`, `0x1FEC`, `0x1FF0` their squared length becomes output alpha.
It is not certified as reflected-ray hit distance. `D15E00A777D250B5` is a short RGBA
texture sample/export pass; it is not evidence of temporal/spatial denoising.

## Backend work verified

The backend now accepts either world-space specular hit distance or dense reflected-geometry
motion, as documented in the supplied NVIDIA `ProgrammingGuideDLSS_RR.md` sections 4.1.8/9.
The reflection guide is distinct from ordinary primary-surface motion. Exactly one guide is
required, with matching extent and scalar/RG float format as appropriate. The unused SDK tag
is explicitly removed, and changing guide kind resets temporal history.

`build/rr-reflection-guides-gpu.log` records seven real RR evaluations and HDR readbacks:
three hit-distance frames, three reflected-motion frames, then hit distance again. All match
the synthetic constant-radiance reference. Fourteen input-contract cases, three selected
CTest checks and five provenance-analyzer tests also passed. This proves backend execution,
not game integration.

## Required before enabling game RR

The native material prepass described below now implements linear diffuse/specular
material decoding, world-space normals and linear roughness. Obtain actual reflected-ray
distance or motion from an active RT producer; identify the complete denoising
boundary and compose reconstructed HDR back into the game. Coordinate RR's upscale with
guest TAA/tone mapping and host SR to avoid repeated reconstruction. Preserve resources
through GPU completion, reset on scene/extent changes, and validate moving scenes.

The diagnostic build correctly reports `RR waiting for lighting inputs`; no synthetic game
guides, primary-depth-as-reflection-distance substitution or forced `RR active` flag were added.

## Native resource and reflection-parameter follow-up

The renderer trace now records the actual texture-cache image slot **and generation**, plus
aspect, mip range and layer range, after resource acquisition. The analyzer matches overlapping
views of the same native generation, rejects recycled slots/disjoint subresources, and preserves
producers of untouched mip levels. Guest addresses alone are retained only for older captures.
This still does not resolve copies or aliases between distinct native images, GPU queue ordering,
per-pixel coverage, blending, or the semantic meaning of an image.

`build/rr-playroom-native-flow.json/.md` captured 92 passes at frames 600/601, eight BVH
dispatches and 58 candidate dependencies. All 52 sampled-image dependencies use native identity
and subresource overlap; six are conservative attachment-preservation dependencies. The four
lighting kernels reference the same native normal/depth/color/material set and HDR destination
within this capture. No game resource was replaced by this diagnostic change.

The closest-ray parameter is still in VGPR `v10` at the following shader-specific PCs, with
the hit identifier in `v12` (`-1` means no hit):

| Shader | PC before hit handling |
| --- | --- |
| `5DF28D2058CAF728` | `0x1A18` |
| `55D28021295DC816` | `0x1D5C` |
| `C18F4AD08601D141` | `0x1FD4` |
| `D65C9DF1AD5E3AF5` | `0x22A0` |

The subsequent instructions construct hit positions using this parameter and a ray direction.
This is a candidate instrumentation point, not yet a certified dense hit-distance texture:
miss semantics, world units, origin bias, skipped tiles and per-pixel ownership must be preserved.
It must be exported to a separate owned resource without overwriting the HDR alpha or guest
registers. Original shaders already use the HDR alpha for primary-surface distance.

In the first kernel, material RGB is blended toward dielectric F0 `0.05` at `0x2D4..0x2E4`,
and material G is used as roughness at `0xB44..0xB8C` (squared and then squared again for the
BRDF). Raw F0 is not automatically the RR specular-albedo guide: NVIDIA's supplied guide
section 4.2.1 describes the roughness/NdotV-dependent environment-BRDF conversion. The native
adapter must reproduce the corresponding game decoding before certifying the inputs.

The native-provenance build succeeded. Eleven analyzer regressions and the three selected
RR-input/camera/CLI CTest checks passed. **There is still no game call to RR evaluation.**

## Raw GPU export and empty-tile finding

An opt-in `StoreRayGuide` IR operation now copies the raw closest-ray parameter and hit id
at those PCs into a dedicated host-owned storage buffer. It does not modify guest registers,
guest descriptors, the HDR alpha, or guest memory. A separate coverage mode at the lighting
store distinguishes a pixel written without a ray from a pixel that never executed the pass.
The operation honors EXEC and bounds checks both coordinates and the descriptor's actual
`OpArrayLength`. Its distinct binding and opt-in flag are included in the codegen fingerprint.

The native owner initializes a fresh capture allocation for each selected frame, flushes host
writes before GPU use, retains the allocation through the scheduler timeline and invalidates
it only after completion. It also snapshots actual GPU indirect arguments and first tile-list
entries. Captures are capped at eight frames and 128 MiB per frame; unsupported/allocation-failed
captures bind a zero-extent dummy instead of altering game rendering. This remains diagnostic
data, not certified RR inputs.

The dedicated GPU test checks an actual hit and coverage-only pixel, preserved inactive lanes,
coordinate overflow, oversized dimensions, a shortened descriptor range and untouched canaries.
It passed, followed by seven RR HDR dispatch/readbacks using both reflection-guide kinds.
See `build/rr-export-and-reconstruction-gpu.log`.

The current game was moved to `D:\Eboot\PPSA01325.zar`; subsequent launches use the emulator's
archive loader. The normal PLAYROOM streaming script now falls back to that archive when the
extracted directory is absent. Archive-member reads retain the ordinary reader fallback.

In `build/rr-playroom-tiles.log` and its two raw frame captures (600/601), **all four** indirect
dispatches have groups `1,1,1` and first tile-list words `FFFFFFFF`. The shader's entry loads
this word and branches to its return on the empty sentinel, before either lighting storage or
ray traversal. Both frames consequently have zero lighting coverage and zero ray exports.
These submitted BVH-capable programs did not produce noisy RT lighting in the captured scene.
Descriptor inventory alone had not established that distinction. This observation does not
prove that other levels have no active RT path.

Full integration still requires a populated game RT lighting producer,
dense valid reflection guides and HDR integration around the actual guest denoisers
and temporal upscaler. No captured empty image is passed to RR or reported as active game RR.
The user cannot perform a gameplay transition now, so that additional scene check is pending.

Reproduce the raw export by setting `KYTY_RR_GUIDE_CAPTURE=1`, `KYTY_RR_TRACE_FRAME=600`,
`KYTY_RR_TRACE_COUNT=2` and `KYTY_RR_GUIDE_FILE=<absolute output prefix>` in the diagnostic
process environment, then launching the matching game with the investigation patch. Analyze:

```powershell
python tools/Analyze_RR_Reflection_Capture.py build/rr-playroom-tiles.600.bin --output build/rr-playroom-tiles.600.json
python tests/RRReflectionCaptureTests.py
```

The little-endian binary starts with four uint32 words: width, height, low frame number and
schema version. Each pixel has raw float32 parameter bits, raw uint32 hit id and write marker
(0 untouched, 1 parameter captured, 2 lighting coverage without a captured parameter).
Version 3 adds four uint3 indirect argument snapshots; version 4 additionally appends four
uint32 first-tile words. `FFFFFFFF` is a miss only in a captured hit-id field, not in an
unwritten pixel. Header/schema/size/markers are validated before analyzing the payload.

Version 5 changes each pixel to four DWORDs by appending an atomic ray-export count;
the 64-byte dispatch/tile tail is unchanged. The first kernel contains a probe loop:
`s43` is incremented at `0x1D4C`, and the loop tests it against `s42` at `0xF00`.
Each visit can run the traversal and reach the export point for the same pixel.
Consequently, even complete pixel coverage does not prove a unique reflected ray:
the raw parameter/hit fields contain the last export, while `repeated_pixels` records
pixels with more than one visit. Such pixels cannot be certified as unique reflection
guides without identifying the lighting contribution and combining its guides correctly.
The counter includes misses and ignores inactive EXEC lanes and coverage-only stores.
The parser retains support for versions 1-4 but cannot recover their missing counts.

`build/rr-counted-export-build.log` records the successful rebuild. Nine parser tests
passed, and `build/rr-counted-export-gpu.log` records a real GPU test of repeated-ray
counting, bounds and canaries, followed by seven successful synthetic RR HDR frames.
The examined consumers also rule out treating the first draws as identified denoisers:
`113ACD3E87A31CF0` samples a 1x1 texture and writes the preserved lighting attachment;
`D15E00A777D250B5` samples/exports RGBA; `102D2A9E3D8AA477` has no sampled input.
`EB87B8B7C526169D` does include neighbourhood reads and motion reprojection, but its
complete temporal role and safe replacement boundary still require verification.

An additional regression exposed a diagnostic call to `GetGpu()` in standalone render contexts
that do not own a guest GPU. Trace entry points now check both diagnostics and `HasGpu()` before
accessing the game frame counter. The raw-export test uses its own native GPU resources.

After this fix, `program_cache_gpu` passed with process-local
`KYTY_GUEST_BACKING_LAZY_COMMIT=1` (`build/rr-export-cache-gpu-fixed.log`). The normal
full-commit allocation could not be tested under the available Windows commit budget;
no global memory settings were changed. BVH GPU regression tests also passed
(`build/rr-export-bvh-regression.log`). The raw-export GPU bounds/canary checks and
seven synthetic RR HDR evaluations passed (`build/rr-export-and-reconstruction-gpu.log`).
These validate the instrumentation and backend, not completed game RR integration.

## Native material preparation and later perspective scene

`RRPlayroomMaterials` now records a native compute prepass from the matching
base-color, metal/roughness, view-normal and depth images to owned RGBA16F diffuse,
specular-albedo and world-normal/linear-roughness images. The first lighting kernel
establishes diffuse `base * (1 - metallic)` at `0x1F90..0x1FC8`, dielectric F0 `0.05`
at `0x2D4..0x2E4`, and normal transformation through view-block offset 192 at
`0x1A0..0x248`. The prepass applies the supplied NVIDIA 4.2.1 EnvBRDFApprox2 to F0,
linear roughness squared and NdotV. Unused packed/flag channels are not treated as
material data. Invalid pixels produce zero guides and an invalid albedo-alpha mask.

The exact first shader uses initial `s14:s15` as its table pointer and loads the
camera V# at table+336. Scalar camera loads can be flattened SRT values: their
descriptor is not necessarily an entry in `info.buffers`. The capture therefore
reads this exact table path before native bindings become immutable, and validates
viewport, projection/inverse, affine view/inverse, orthonormal basis and camera
position. The material prepass supports perspective and orthographic projection;
orthographic ray direction is derived from inverse projection, including shear.
This does not extend the RR backend to orthographic game rendering.

Each output frame owns its images, uniform upload, descriptor pool and shared pipeline
until GPU completion. The diagnostic readback restores shader-readable layouts;
the renderer restores its previously cached compute pipeline and invalidates descriptor
and push-constant shadows after recording the helper. Normal launches do not run
this optional capture. Output allocation is capped at 256 MiB per converted frame.

Add `KYTY_RR_MATERIAL_CAPTURE=1` to the existing diagnostic environment to record the
prepass in the selected RR trace window. Camera captures use the existing
`KYTY_RR_GUIDE_FILE` prefix and `.FRAME.camera.bin` suffix. Output validity summaries
are reported only after the scheduler confirms GPU completion.

The early frame-180 capture had an orthographic camera and all 1,639,680 material
pixels masked. Longer unattended runs automatically reached a perspective scene
with ordinary DLSS active after roughly frame 1200. The later capture in
`build/rr-playroom-perspective.log` at frames 1800/1801 produced **1,639,680 valid
material pixels per frame**, zero masked/invalid pixels, and roughness 0..0.537109375.
The first lighting kernel wrote 146,944 / 147,200 pixels, but no pixel reached the
ray export. Thus populated lighting and materials alone do not establish RT execution.
The trace contains 271 passes and 612 candidate dependencies; new original shaders
are in `build/PlayroomRRPerspectiveShaders/original`.

The subsequent `build/rr-playroom-lighting-gate.log` confirms both frames have
`probe_count=1`, **`ray_gate=0`**, and a mapped BVH reference. This is the exact
lighting-block DWORD at offset 28: PCs `0xE14..0xEC0` test it and construct the
ray-enabled EXEC mask. The kernel intentionally skips its traversal when that
value is zero. Its lighting coverage was 161,024 / 160,256 pixels; all material
pixels were valid, while both ray-export counts remained zero. A mapped reference
does not prove a complete/valid acceleration structure. The gate is not forced on.
These scene observations do not establish the RT behavior of other levels.

`build/rr-material-final-gpu.log` verifies 81 pixels against an independent double
precision reference: metals/roughness endpoints, perspective/rotated/jittered and
sheared orthographic cameras, invalid masks, ignored flag channels and canaries.
The same GPU test now feeds the material-prepass outputs into seven real RR HDR
evaluations/readbacks, using both reflection-guide types. All passed. Six selected
CTest checks also passed (`build/rr-material-ctest.log`); the GPU-cache check used
process-local lazy guest backing commitment.

**Game RR is still not evaluating.** Material conversion is implemented and has run
on real game resources, but a populated RT reflection producer and its guides,
the complete denoiser/TAA replacement boundary and reconstructed-HDR insertion
remain required. There is no forced certification or fabricated reflection input.

## Local evidence

`build/rr-playroom-scene.log`, `build/rr-playroom-scene-flow.json/.md`,
`build/PlayroomRRShaders/original`, `build/rr-reflection-guides-gpu.log`.

Analyze a bounded capture with:

```powershell
python tools/Analyze_RR_Guest_Trace.py build/rr-playroom-scene.log --game "ASTRO's PLAYROOM" --output build/rr-playroom-scene-flow
```
