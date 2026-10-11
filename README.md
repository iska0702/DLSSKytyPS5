# KytyPS5 (NVIDIA DLSS & Frame Generation Experimental Fork)

> [!TIP]
> **Experimental NVIDIA DLSS Integration:** This fork explores NVIDIA DLSS Super Resolution, DLAA, Frame Generation, image sharpening, and additional graphics features through NVIDIA Streamline.
>
> **Documentation and Setup:** [README_DLSS.md](README_DLSS.md)
>
> **Pre-built Windows Releases:** [Download from GitHub Releases](https://github.com/iska0702/DLSSKytyPS5/releases)

This project is a fork of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5), with experimental graphics enhancements and additional launcher configuration options.

The fork explores DLSS integration, frame generation, Vulkan-based DirectStorage, game-specific compatibility patches, and image sharpening. These features are experimental and may not work as intended in every game.

- [NVIDIA DLSS & Frame Generation Guide](README_DLSS.md)
- [Experimental Features and Known Limitations](docs/EXPERIMENTAL.md)
- [Release Downloads](https://github.com/iska0702/DLSSKytyPS5/releases)
- [Original Jetsku/KytyPS5 Repository](https://github.com/Jetsku/KytyPS5)

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](#system-requirements)
[![Status](https://img.shields.io/badge/status-experimental-orange.svg)](#current-status)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

KytyPS5 is a PlayStation 5 emulator written in C++. This fork builds upon the existing Jetsku/KytyPS5 codebase and investigates additional graphics technologies for Windows x64.

> [!IMPORTANT]
> This is an independent experimental fork. It is not affiliated with or endorsed by Sony Interactive Entertainment or NVIDIA. No games or proprietary PlayStation system software are distributed with this project.

## NVIDIA DLSS Integration

The project includes experimental integration work involving the following NVIDIA technologies.

| Feature | Description | Status |
|---|---|---|
| DLSS Super Resolution | AI-assisted image upscaling | Experimental |
| DLAA | Deep Learning Anti-Aliasing | Experimental |
| DLSS Frame Generation | Intermediate frame generation | Experimental |
| Multi-Frame Generation options | 2x, 3x, 4x, and Dynamic launcher options | Experimental |
| NVIDIA NIS | Image sharpening after upscaling | Experimental |
| FidelityFX CAS | Adaptive contrast sharpening | Experimental |
| NVIDIA Streamline | Framework for graphics feature integration | Integrated experimentally |

Feature availability and results depend on the implementation, hardware, drivers, and the rendering information available to the emulator.

## NVIDIA DLL Versions

The project configuration specifies the following NVIDIA libraries and SDK version.

| Component | Version | Purpose |
|---|---|---|
| `nvngx_dlss.dll` | 3.10.9.1 | DLSS Super Resolution |
| `nvngx_dlssg.dll` | 3.10.9.1 | Frame Generation |
| `nvngx_dlssd.dll` | 3.10.9.1 | Ray Reconstruction |
| NVIDIA Streamline SDK | 2.14.1 | Integration framework |

The presence of these libraries does not mean every feature is fully operational. Each technology requires compatible integration and appropriate input data.

**Hardware note:** Official NVIDIA Multi Frame Generation support is associated with compatible GeForce RTX 50-series GPUs. Available features depend on NVIDIA's hardware, driver, and software requirements.

## Additional Graphics Features

### Image Sharpening

Two additional sharpening approaches are included in the experimental configuration:

- **NVIDIA Image Scaling (NIS):** Optional sharpening following upscaling.
- **FidelityFX CAS:** Contrast Adaptive Sharpening for additional image detail enhancement.

Sharpening can improve perceived clarity, but excessive sharpening may introduce halos, noise, and other artifacts.

### DirectStorage and Vulkan

The fork explores experimental Microsoft DirectStorage integration with Vulkan-based GPU streaming.

The intended goals include:

- Exploring alternative asset-loading paths.
- Reducing unnecessary data-transfer overhead.
- Investigating GPU-assisted asset streaming.
- Providing a launcher option for the experimental feature.

Performance improvements are not guaranteed. Actual results depend on the implementation, storage device, driver support, and the emulator's asset-loading pipeline.

### Game-Specific Compatibility Patches

Experimental DLSS-related patches are included for selected game identifiers:

- **Bloodborne / Demon's Souls:** `PPSA01325`
- **ASTRO BOT:** `PPSA21567`

Compatibility depends on the specific game version and patch implementation. These patches are not guaranteed to work across all versions or configurations.

### Windows Configuration

The project includes registry scripts related to:

- Hardware-Accelerated GPU Scheduling (HAGS).
- DLSS status indicator configuration.

Review registry scripts before running them. Changes to Windows graphics settings may require a restart.

## Current Status

**Development status: Experimental.**

The project explores additional graphics features while retaining the underlying KytyPS5 emulator functionality.

Current areas of interest include:

- DLSS Super Resolution integration.
- DLAA configuration.
- Frame Generation experiments.
- Image sharpening.
- Vulkan-based DirectStorage experiments.
- Game-specific compatibility testing.
- Launcher graphics configuration.

Compatibility, image quality, and performance may vary significantly between games and builds.

## Known Limitations

### Missing Motion Vectors and Depth Data

A major limitation of the current experimental DLSS approach is the lack of reliable game-generated motion vectors and scene depth data.

The emulator captures the final color buffer through the `VideoOutSubmitFlip` presentation path, using `layers[0]`. This is not equivalent to accessing the game's complete internal rendering pipeline.

The described implementation in `dlssManager.cpp` uses fallback resources:

- **Motion vectors:** `R16G16_SFLOAT`, cleared to `(0.0, 0.0)`.
- **Depth buffer:** `D32_SFLOAT`, cleared to `1.0`.

These values do not contain the actual motion of scene objects or the scene's real depth.

Temporal DLSS reconstruction normally relies on information such as:

1. Subpixel camera jitter.
2. Accurate motion vectors.
3. A valid depth buffer.
4. Temporal history from previous frames.
5. Appropriate exposure and other integration-specific inputs.

When these inputs are missing or invalid, reconstruction quality can be substantially reduced. Potential results include blur, ghosting, unstable details, and limited improvement over basic spatial upscaling.

A more complete implementation would require obtaining reliable rendering data from the emulated game's pipeline or developing a suitable method to reconstruct equivalent information.

### Frame Generation

Frame Generation may introduce:

- Ghosting and visual artifacts.
- Incorrectly reconstructed moving objects.
- UI artifacts.
- Additional GPU overhead.
- Increased latency or inconsistent frame pacing.

Increasing the displayed frame rate does not necessarily increase the game's internally rendered frame rate or improve input responsiveness.

### Ray Reconstruction

The presence of `nvngx_dlssd.dll` does not automatically enable Ray Reconstruction.

A compatible ray-tracing pipeline and suitable input data are required. Without these, the library cannot provide the intended functionality.

### DirectStorage

DirectStorage support remains experimental. Faster loading times and improved streaming performance must be demonstrated through testing rather than assumed.

### Game Compatibility

- Not every PS5 game is compatible with KytyPS5.
- Game-specific patches may depend on particular versions.
- Graphics features may behave differently between games.
- Crashes, visual artifacts, and regressions are possible.

## Advantages

- Experimental NVIDIA graphics technology integration.
- Configurable upscaling and frame generation options.
- Additional image sharpening controls.
- Experimental Vulkan-based asset streaming.
- Selected game-specific patches.
- A launcher-oriented approach to graphics configuration.
- A foundation for further experimentation with emulator graphics reconstruction.

## System Requirements

- Windows x64.
- A compatible build of this KytyPS5 fork.
- Compatible NVIDIA hardware and drivers for the relevant DLSS features.
- A legally obtained game copy.
- Additional requirements may apply to individual NVIDIA technologies.

## Releases

Pre-built Windows packages are published on the project's GitHub Releases page.

**[View Releases and Downloads](https://github.com/iska0702/DLSSKytyPS5/releases)**

Read the release notes before upgrading. Experimental builds may contain regressions, incomplete features, or changes to configuration files.

## Reporting Issues

When reporting a problem, include:

- The KytyPS5 fork version or release tag.
- The affected game and its version.
- GPU model and driver version.
- Relevant emulator logs.
- The graphics settings used.
- Whether the problem occurs with experimental features disabled.

Please distinguish between an emulator compatibility issue and an issue specifically caused by the experimental graphics integration.

## Credits and Licensing

This project is based on [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5). Credit for the original emulator, upstream changes, and existing contributions belongs to their respective authors.

NVIDIA DLSS, NVIDIA Streamline, Microsoft DirectStorage, Vulkan, FidelityFX, PlayStation, and the referenced game titles are technologies or trademarks belonging to their respective owners.

This fork is independently developed and is not affiliated with or endorsed by Sony Interactive Entertainment, NVIDIA, or the referenced game developers.

Third-party components remain subject to their respective licenses. See [LICENSE](LICENSE) and the applicable third-party license files for details.

No proprietary games or PlayStation system firmware are distributed with this project.
