# FidelityFX FSR 3.1 Vulkan integration

FSR Super Resolution and 2x Frame Generation use the matching headers and Windows x64 libraries in `../FidelityFX-Vulkan-Source-1.1.4`. The other SDK folders are not mixed into this build: their API and binary versions may differ.

## Run

In Launcher configuration, enable **AMD FidelityFX FSR 3.1 (Experimental)**. The controls include Super Resolution, Frame Generation and sharpness. FSR takes precedence over DLSS; their frame-generation swapchains are mutually exclusive.

CLI examples (from the build directory):

```powershell
.\kyty_emulator.exe --game "D:\Games\PPSA01325" --screen-width 2560 --screen-height 1440 --fsr --fsr-sharpness 0.35 --playgo-hack
.\kyty_emulator.exe --game "D:\Games\PPSA01325" --screen-width 2560 --screen-height 1440 --fsr-fg --fsr-sharpness 0.35 --playgo-hack
```

Select the game's internal resolution through the existing game patches. FSR uses the actual input and display sizes; selecting a mode does not reduce the guest rendering cost automatically. `--no-fsr` disables FSR and its FG. Sharpening is applied once inside FSR; the shared presentation CAS pass is skipped on those frames.

## Inputs and limits

Temporal reconstruction uses the same certified Astro depth/motion producers and validated camera capture as DLSS. Motion is converted to normalized current-to-previous displacement without jitter; FSR receives render-pixel scales, actual jitter and camera clipping planes. No fabricated guides are used. Unsupported games, menus and frames without valid temporal inputs use the spatial presentation fallback. History resets on input/output resize, missing guides and camera discontinuities. FG pauses on these transitions and resumes once temporal history exists.

ASTRO BOT 01.018 now has an experimental compatibility path: the captured pixel shader `92E45C84A2FF97DA` exports motion in MRT4.rg of an RGBA16F target. Its 576-byte view descriptor extends the validated 496-byte camera prefix. The snapshot uses the matching D32S8 depth target and validated viewport. When guest video-out is larger than this grid, color is resampled to the guide size before host reconstruction. This retains guest TAA/upscaling, postprocessing and HUD; it does not replace the guest upscaler or promise a rendering speedup. The initial resolution patch missed the inlined lookup tables and left the actual grid at 1920×1080. The corrected patch updates three setter copies and three pairs of width/height tables. With the 1708×960 profile selected, the camera, depth and motion snapshots now actually use 1708×960 and FSR reconstructs to 2560×1440. The patch label alone does not verify the actual rendering size.

The same snapshot serves DLSS and FSR. Presentation diagnostics report their evaluated states separately. For FSR FG, `--dlss-diagnostics` also logs application presents and the SDK's actual present count over a sample window.

FG uses FidelityFX's Vulkan replacement swapchain and all of its create/acquire/get-images/present/destroy functions. The proxy receives its required shader-read backbuffer layout. Two queues in the graphics/present family are reserved exclusively for FidelityFX presentation and acquisition; the host side-copy queue is disabled when needed to leave these available. If insufficient queues exist, SR remains available and a log explains why FG is disabled.

The guest color already contains postprocessing, antialiasing and HUD. A separate guest HUD-free pass and reactive/transparency masks are not available; HUD and transparent surfaces can show artifacts. The emulator overlay is also composed before interpolation. The title identifies when FG is enabled and explicitly labels 2x as requested, not as measured display FPS. This is an experimental integration; gameplay quality, pacing, resize and long-session stability require validation on the target GPU.

## Build

Configure the project with Clang/clang-cl and the usual project dependencies:

```powershell
cmake -S . -B build -G Ninja -DKYTY_ENABLE_FSR=ON -DFIDELITYFX_SDK_DIR=D:/DLSSEmulatorPS5/FidelityFX-Vulkan-Source-1.1.4
cmake --build build --target kyty_emulator launcher
```

The build copies all five matching FidelityFX DLLs beside the emulator and installs them beside the executable. `KYTY_ENABLE_FSR=OFF` builds the spatial fallback without linking FidelityFX. The supplied binaries support Windows x64; other platforms require disabling this option or providing a future source-build integration. The AMD SDK license is included under `LICENSES/FidelityFX-SDK.txt`.

## GPU verification

```powershell
cmake --build build --target fsr_vulkan_tests dlss_camera_tests
.\build\fsr_vulkan_tests.exe
.\build\fsr_vulkan_tests.exe --present
.\build\dlss_camera_tests.exe
```

The GPU test executes three SR frames and three optical-flow/interpolation frames, reads the output back and verifies a constant-color reference. It also destroys both kinds of contexts. The default mode runs without a presentation window. `--present` additionally exercises the real replacement swapchain and checks that generated frames increase the actual present count (three application frames produced six presents on the verification GPU). Neither mode confirms gameplay quality or long-session frame pacing. The standalone SR API owns its three external shared textures, avoiding the SDK 1.1.4 combined wrapper's double backend destruction in upscaling-only mode. The Vulkan device explicitly enables the memory-requirements extension alias resolved by this SDK.
