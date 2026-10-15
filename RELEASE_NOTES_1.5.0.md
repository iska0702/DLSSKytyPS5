# DLSSKytyPS5 1.5.0

- Added FidelityFX FSR 3.1 Vulkan Super Resolution and 2? Frame Generation, launcher settings and CLI flags.
- Added ASTRO BOT 01.018 camera, depth and motion capture for DLSS/FSR. Verified 1708?960 ? 2560?1440 reconstruction in actual game runs. DLSS FG and FSR FG separately produced additional presents in short GPU checks.
- Fixed ASTRO BOT resolution patches, including inlined lookup tables. Added 30 profiles: Full HD, QHD and 4K ? DLSS/FSR ? Native AA, Quality, Balanced, Performance and Ultra Performance. See ASTRO_BOT_RESOLUTIONS.md.
- Fixed launcher emulator discovery, runtime DLL packaging and stale executable deployment.
- Added Vulkan feature/queue validation, captured-camera tests and optional GPU integration tests.
- Hardened DirectStorage host reads, reinitialization and resource cleanup.

## Limits

Experimental compatibility integration: guest postprocessing/TAA/upscaling and HUD remain in the color path. No universal game support or rendering speedup is promised. DirectStorage GPU streaming/decompression is not integrated. The earlier synthetic DLSS FG 1080p device-loss issue remains recorded in GRAPHICS_VERIFICATION.md. Other resolutions and long sessions need further validation.

Ray Reconstruction is not included in 1.5.0; it is subsequent work and requires verified noisy lighting, albedo, normal/roughness and reflection inputs before guest denoising/tone mapping.

## Windows package

Extract the complete ZIP into one folder and run launcher.exe. Configure your own legally obtained game path. No game executable, game assets, saves or personal configuration are distributed. Source builds require the matching external Streamline 2.14.1 and FidelityFX Vulkan 1.1.4 SDKs documented in README_FSR.md.
