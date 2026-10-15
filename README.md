# KytyPS5 (NVIDIA DLSS, AMD FSR & Frame Generation Experimental Fork)

> [!TIP]
> **Experimental Graphics Integration:** This fork explores NVIDIA DLSS Super Resolution, DLAA, DLSS Frame Generation, AMD FidelityFX Super Resolution (FSR), FSR Frame Generation, image sharpening, and additional graphics features through experimental renderer and Streamline integration work.
>
> **Documentation and Setup:** [README_DLSS.md](README_DLSS.md)
>
> **Windows Releases:** [Download from GitHub Releases](https://github.com/iska0702/DLSSKytyPS5/releases)

This project is an independent fork of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5). It explores advanced graphics reconstruction, temporal upscaling, frame generation, DirectStorage file I/O, and game-specific compatibility patches.

All additional graphics features are experimental. Availability, compatibility, image quality, and performance depend on the game, renderer integration, GPU hardware, drivers, and available rendering data.

## v1.0.0 — Graphics Integration Update (2026-10-13)

- **Astro's Playroom:** Tested with game ID `PPSA01325`, version `01.905.000`. Native depth and motion-vector inputs have been verified for prepared gameplay frames. Camera matrices, raster jitter, and motion reprojection are integrated into the experimental DLSS rendering path.
- **Astro Bot:** Added an executable-specific resolution patch for the supplied executable under game ID `PPSA23060`. The patch uses an exact executable SHA-256 match because the APP_VER is unknown. Native-resolution DLAA profiles are available through the patch configuration, but runtime DLSS compatibility and image quality have not been verified.
- **DLSS Frame Generation:** Streamline swapchain image queries and resource layouts were corrected. Fixed 4x Multi Frame Generation was observed during runtime testing. Unsupported multipliers are clamped, and Vulkan Dynamic mode uses a supported fixed multiplier.
- **Native QHD + DLAA:** Added an optional Astro's Playroom launch script for native 2560×1440 rendering with DLAA. This preserves the native render resolution at a higher GPU cost.
- **AMD FSR:** The project documentation and graphics configuration distinguish AMD FSR upscaling and frame-generation experiments from NVIDIA DLSS. Each feature requires its own compatible implementation and appropriate rendering inputs.
- **DirectStorage:** APR file reads use bounded CPU staging before copying data into protected guest memory. File integrity and EOF tests passed, and more than 5,000 game-loading requests completed without recorded errors. A loading-speed improvement has not been established.
- **Diagnostics:** Unhandled guest faults preserve registers, code bytes, and call chains in `_HangTrace/guest-fault-*.txt`.

See [DLSS Runtime Findings](docs/DLSS_RUNTIME_FINDINGS.md), [DirectStorage Runtime Findings](docs/DIRECTSTORAGE_RUNTIME_FINDINGS.md), and [Astro Bot Patch](docs/ASTRO_BOT_PATCH.md).

## Graphics Upscaling and Frame Generation

| Feature | Description | Status |
|---|---|---|
| NVIDIA DLSS Super Resolution | AI-assisted image upscaling | Experimental |
| NVIDIA DLAA | Deep learning anti-aliasing at native resolution | Experimental |
| NVIDIA DLSS Frame Generation | AI-generated intermediate frames | Experimental |
| NVIDIA Multi Frame Generation options | 2x, 3x, 4x, and Dynamic launcher options | Experimental |
| AMD FidelityFX Super Resolution (FSR) | Spatial or temporal upscaling, depending on the FSR implementation | Experimental |
| AMD FSR Frame Generation | Intermediate frame generation where supported by the integrated implementation | Experimental; runtime support must be verified |
| NVIDIA Image Scaling (NIS) | Image scaling and sharpening | Experimental |
| FidelityFX Contrast Adaptive Sharpening (CAS) | Contrast-adaptive image sharpening | Experimental |
| NVIDIA Streamline | Framework for integrating supported graphics features | Experimentally integrated |

**Important:** Listing a feature does not mean that it is fully implemented or operational in every game. FSR upscaling and FSR Frame Generation are separate technologies and must be evaluated independently. FSR Frame Generation must not be confused with AMD Fluid Motion Frames (AFMF), which is a separate driver-level technology.

## Game Testing and Compatibility

### Astro's Playroom

- **Game ID:** `PPSA01325`
- **Tested version:** `01.905.000`
- **Native temporal inputs:** Depth and motion vectors were verified for prepared gameplay frames.
- **DLSS integration:** Camera and projection data, raster jitter, motion-vector processing, and temporal-history handling have been integrated experimentally.
- **DLAA:** An optional native 2560×1440 rendering profile is provided.
- **Frame Generation:** Fixed 4x Multi Frame Generation was observed during runtime testing.
- **Limitations:** A separate HUD-free color pass is not implemented. Presented color still includes the game's postprocessing and UI. Image quality and overall compatibility remain experimental.

Frames without verified temporal inputs use a spatial presentation path instead of DLSS. Logo and menu frames bypass DLSS and frame generation where the required rendering data is unavailable.

### Astro Bot

- **Game ID:** `PPSA23060`
- **Executable-specific patch:** Added in [ASTRO_BOT_PATCH.md](docs/ASTRO_BOT_PATCH.md).
- **Executable identification:** The loader requires an exact executable SHA-256 match because the APP_VER is unknown.
- **Resolution profiles:** Native-resolution profiles support DLAA selection in the patch configuration.
- **Optional renderer/GI modifications:** Disabled.
- **Runtime verification:** Full DLSS compatibility, frame-generation behavior, image quality, and game stability have not been verified.

The Astro Bot patch must not be interpreted as proof that DLSS or FSR works correctly throughout the game. Compatibility must be verified against the exact executable and runtime configuration.

### FSR and FSR Frame Generation Testing

FSR upscaling and FSR Frame Generation require separate validation. Successful initialization or the appearance of a graphics setting is not sufficient evidence that either feature works correctly.

For each tested game and feature, record:

- The exact game ID, executable hash, and game version.
- The selected upscaling or frame-generation mode.
- The internal rendering resolution and output resolution.
- Whether valid depth, motion vectors, and frame timing are supplied.
- Whether generated frames are actually presented.
- Visual artifacts, UI reconstruction issues, crashes, and frame-pacing behavior.
- GPU model, driver version, and relevant emulator logs.

Do not mark FSR or FSR Frame Generation as fully working for Astro's Playroom or Astro Bot unless that specific feature has been tested and verified in the corresponding build.

## Additional Graphics Features

### Image Sharpening

The experimental configuration includes:

- **NVIDIA Image Scaling (NIS):** Optional image scaling and sharpening.
- **FidelityFX CAS:** Contrast Adaptive Sharpening.
- **DLSS sharpening controls:** Configurable sharpening behavior where supported by the active rendering path.

Zero sharpness disables the CAS pass in the relevant implementation. Excessive sharpening can introduce halos, noise, and other artifacts.

### DirectStorage and Vulkan

DirectStorage is connected to the APR host file-read path. Data is read into a bounded CPU staging buffer and copied into guest memory on the emulator thread. Standard file reads provide a fallback when DirectStorage is unavailable or a request fails.

The shared D3D12/Vulkan texture helper currently has no renderer callers. GPU texture streaming and GPU decompression are not integrated. Faster loading times have not been established.

### Hardware Requirements

- Windows x64.
- A compatible build of this KytyPS5 fork.
- Compatible GPU hardware and drivers for the selected graphics feature.
- NVIDIA hardware and supported drivers for applicable DLSS features.
- Hardware and software meeting the requirements of the selected FSR implementation.
- A legally obtained game copy.

Official NVIDIA Multi Frame Generation support is associated with compatible GeForce RTX 50-series GPUs. This does not imply that every experimental frame-generation mode in this fork requires or officially supports the same hardware.

## Current Development Status

This fork is an experimental graphics research project built on the existing KytyPS5 emulator codebase.

Current development areas include:

- NVIDIA DLSS Super Resolution and DLAA.
- DLSS Frame Generation and Multi Frame Generation experiments.
- AMD FSR upscaling and FSR Frame Generation experiments.
- Image sharpening and reconstruction quality.
- Native resolution profiles and launcher configuration.
- Experimental DirectStorage file I/O.
- Game-specific compatibility and rendering patches.
- Validation of temporal inputs, frame pacing, and generated-frame presentation.

**Experimental status:** A feature being present in the code or launcher does not guarantee that it works in every game. Verified results should be documented separately from planned work and unverified implementations.

## Reporting Issues

When reporting a problem, include:

- KytyPS5 fork version or release tag.
- Game ID, version, and executable hash when relevant.
- GPU model and driver version.
- Upscaling and frame-generation settings.
- Internal and output resolutions.
- Relevant emulator and graphics logs.
- Whether the issue occurs with experimental features disabled.

Please distinguish emulator compatibility issues from problems caused by the experimental graphics integration.

## Credits and Licensing

This project is based on [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5). Credit for the original emulator, upstream changes, and existing contributions belongs to their respective authors.

NVIDIA DLSS, NVIDIA Streamline, AMD FidelityFX, Microsoft DirectStorage, Vulkan, PlayStation, and the referenced game titles are technologies or trademarks belonging to their respective owners.

This fork is independently developed and is not affiliated with or endorsed by Sony Interactive Entertainment, NVIDIA, AMD, or the referenced game developers. Third-party components remain subject to their respective licenses.

See [LICENSE](LICENSE) and the applicable third-party license files for details. No proprietary games or PlayStation system firmware are distributed with this project.
