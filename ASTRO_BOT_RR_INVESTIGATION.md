# ASTRO BOT pre-denoise lighting investigation

Tested game: PPSA21567, 01.018.000. This is investigation evidence, **not working game RR**.

## Captured native paths

The previous `[DLSS][GuestCompute]` inventory observed only direct dispatches and deduplicated
each shader for the whole process. ASTRO BOT's relevant BVH shaders execute through
`dispatchIndirect`; consequently that inventory missed the lighting producers entirely.

The opt-in RR trace now observes direct and indirect dispatches at native emission, plus draw
resources and attachments at the common acquisition/emission path. It records repeated passes,
frame/pass IDs, shader hashes, BVH/BDA flags, descriptor source IDs and original image words.
It reads existing immutable resource snapshots, without GPU readback or changing shaders.
An explicit end record detects truncated pass descriptions. The frame window is capped at eight
frames and the process at 65,536 pass descriptions. Normal launches produce no RR trace.

Actual two-frame capture at 1708x960 -> 2560x1440:

- 150 pass descriptions, 160 candidate image dependencies, eight submitted BVH dispatches.
- Four BVH shader variants per frame: `78AF8E269B528B5C`, `7BD68261B1BDFB68`,
  `F3F4E1671B30C1F4`, `530DCD964F29983C`.
- All four target one render-sized RGBA16F image and a separate R16F image. These are distinct
  outputs; the R16F image must not be assumed to be reflection distance.
- Subsequent draw shaders include `0A394C16BC0935C9`, `4F4D1E114A9F9AAE`,
  `70B33A567DCBA0D6`, `EE4A30DD74F51F5F`, `ABA5381E7B4D42BC`,
  `F47C0C097C598A36`, `18CED3FC6FBA62AC`, then `2936E5ECA99D75E7`.
  Several draws modify the same HDR attachment. Replacing a single last-writer entry would
  incorrectly discard preserved pixels, blend contributions and partial tile writes.

Guest addresses in the local capture are deliberately not used as integration identifiers.
They change with resolution and allocation lifetime.

## Shader evidence and remaining semantic checks

- `147B9B98574ECB8D`, PCs `0x84..0xEC`, samples RGB from the RGBA16F normal target,
  explicitly normalizes it, and encodes its direction to two channels. It samples G from the
  RGBA8 material target into a separate scalar output. This establishes the channel mapping;
  it does not establish normal coordinate space or the scalar's roughness convention.
- `78AF8E269B528B5C`, PCs `0x3250..0x32F4`, stores clamped nonnegative RGB plus a
  length computed from `v28/v29/v27` and camera-block offset 400 into RGBA16F.
  Those coordinates were constructed at `0x668..0x760` using primary depth, inverse projection
  and inverse view **before** BVH traversal. Therefore this alpha must not be certified as the
  reflected ray's hit distance merely because it contains a length.
- The separate R16F output is generated at `0x3304..0x3334` using luminance weights
  approximately `0.2126, 0.7152, 0.0722`, multiplied by `0.18`, with a background mask.
  It likewise cannot serve as RR reflection hit distance.
- `70B33A567DCBA0D6`, PCs `0x20..0xD8`, copies HDR RGB and changes alpha using
  primary depth reconstruction and a minimum with the existing alpha. This short pass is not
  proof of a denoiser; it cannot simply be skipped as the RR replacement point.

Still required: certify diffuse/specular albedo decoding, normal space and linear roughness;
capture actual reflected-ray distance (or valid specular motion); identify guest temporal/spatial
denoising boundaries; insert RR and restore HDR to the guest composition pipeline with correct
resource lifetime and synchronization. No candidate is automatically promoted to `RRInputs`.

## Reproduce

In PowerShell, before launching the emulator with its existing game/patch arguments:

```powershell
$env:KYTY_RR_TRACE_FRAME = '180'
$env:KYTY_RR_TRACE_COUNT = '2'
# Add --dlss-rr --dlss-diagnostics to the existing emulator launch.
# Redirect its standard output to build/rr-lighting-trace-complete.log.
```

After the selected frames have been recorded:

```powershell
python tools/Analyze_RR_Guest_Trace.py build/rr-lighting-trace-complete.log --output build/rr-lighting-flow-complete
python tests/RRGuestTraceTests.py
Remove-Item Env:KYTY_RR_TRACE_FRAME, Env:KYTY_RR_TRACE_COUNT
```

The analyzer emits JSON and Markdown. Its graph records base-address candidates and conservative
attachment preservation edges in host recording order. It does not resolve memory aliases,
allocation generations, asynchronous queue scheduling or buffer/BDA dependencies. These limits
must be resolved in the native resource adapter before changing game rendering.
