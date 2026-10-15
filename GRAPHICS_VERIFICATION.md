# Graphics and storage verification — 2026-10-15

Hardware: NVIDIA GeForce RTX 5060 Laptop GPU, NVIDIA driver 617.42, Windows, Vulkan. Results describe synthetic integration tests, not a guarantee for every game.

| Feature | Observed result |
| --- | --- |
| DLSS SR + FG, 1280×720 output | Production DLSSManager evaluated SR and presented generated frames through the Streamline replacement swapchain. 130 application frames completed; reported effective FG multiplier was 2×. |
| DLSS SR + FG, 1920×1080 output | Failed with `ErrorDeviceLost`, including the repeat after correcting the test blit extent. Unresolved; do not treat 1080p FG as verified. |
| FSR SR + FG | GPU readback matched the constant-color reference. Real replacement swapchain produced six presents from three application frames. Both context variants were destroyed successfully. |
| DirectStorage | 16 MiB host read matched the reference byte for byte. EOF, short read, missing file, disabled/re-enabled initialization and empty shared-texture cleanup passed. |
| Regression checks | CTest `directstorage`, `dlss_camera`, `emulator_present_mode_cli`: 3/3 passed. |

## Fixes included

- Package all Streamline dependencies beside both executables, including DLSS-G, Reflex, PCL, NIS and NVIDIA low-latency runtime. Use the SDK path inherited from the root build.
- Require supported and loaded plugins before enabling features; require Reflex for FG and validate the reserved Vulkan queue count.
- Enable supported Vulkan descriptor indexing, maintenance4 and the timeline-semaphore/maintenance4 extension aliases required by the bundled Streamline SDK.
- Improve launcher executable discovery and report native startup failures instead of treating them as a missing emulator.
- Fix DirectStorage reinitialization, HRESULT handling, shared-image cleanup order, memory-type selection, size validation and fence error handling.
- Integrate FSR temporal upscaling and replacement-swapchain FG with input validation, history resets, queue reservation and spatial fallback.

## Practical limits

The original ASTRO BOT run reached the spatial fallback because certified temporal guides were unavailable. The subsequent game-specific implementation recognizes its captured RGBA16F MRT4 motion producer and 576-byte camera block. In actual ASTRO BOT runs, FSR SR and DLSS SR both evaluated with a 1920×1080 input and 2560×1440 output. FSR FG produced approximately 240 actual presents per 120 application presents across repeated sample windows. See `build/astrobot-fsr-integrated.log`, `build/astrobot-dlss-integrated.log` and `build/astrobot-fsr-fg-integrated.log`.

The subsequent DLSS FG game run also reported SDK status `0x0 (OK)`, two actual presents since the preceding query, and an effective 2× multiplier in the title after the averaging window (for example, 36 application FPS / 72 estimated display FPS). Its log is `build/astrobot-dlss-fg-final.log`. No `undefined format` or device-loss error was observed in that run. This is a short game integration check; it does not resolve or replace the separate historical synthetic 1080p failure above.

The prepared-color path now converts A2R10G10B10 to the supported A2B10G10R10 format with an actual channel-converting blit, and Streamline resource creation flags are explicitly populated. The CLI `--dlss-fg` now also enables DLSS, consistent with `--dlss-mfg` and `--fsr-fg`. Rebuilding the emulator refreshes its copy beside the launcher even when launcher sources have not changed.

This is a compatibility implementation: it normalizes guest-upscaled video-out color to the verified motion/depth grid, then runs host reconstruction. It retains guest postprocessing and HUD and does not replace guest TAA/upscaling or promise reduced guest rendering cost. The selected experimental 1708×960 patch still yielded an actual 1920×1080 camera/guide grid in these runs. General image quality, other shader variants and long-session stability need further validation. Menus and frames without valid inputs still use spatial fallback. A captured ASTRO BOT camera fixture passes the camera regression test.

Resolution patch correction: the preceding 1920×1080 results predate the lookup-table fix. The patch now changes all three out-of-line 1080p setter copies and the three width/height lookup-table pairs used by inlined game code. In `build/astrobot-resolution-tables.log`, the selected 1708×960 profile produces actual 1708×960 camera dimensions, RGBA16F motion and D32S8 depth snapshots; FSR creates a 1708×960 → 2560×1440 context and evaluates frames. `tools/Verify_AstroBot_Resolution.py` verifies 78 byte guards across six profiles against the original SELF and checks agreement between setter instructions and table entries. Existing profile selections are preserved in each deployed patch file.

The corresponding DLSS game run also evaluates 1708×960 → 2560×1440 (`build/astrobot-resolution-dlss-final.log`). The title reports `DLSS: Active (Quality 1708x960->2560x1440)`. FG presentation depends separately on foreground focus; it does not determine the internal render resolution.

DirectStorage currently accelerates the implemented APR host-file-read path. GPU texture streaming and GPU decompression are not integrated. The cached ReadFile comparison was faster in this test, so no storage speedup is claimed.

The 1080p DLSS-G device loss remains an open issue. The successful 720p test is a short smoke test, not a long-session stability result. Its log is `build/dlss-final-audit.log`; the failed corrected 1080p run is `build/dlss-1080p-corrected.log` plus `.err`.

## Reproduce

```powershell
cmake --build build --target kyty_emulator launcher directstorage_tests fsr_vulkan_tests
.\build\directstorage_tests.exe
.\build\directstorage_tests.exe --dlss-requirements
.\build\directstorage_tests.exe --dlss-vulkan
.\build\directstorage_tests.exe --dlss-vulkan-1080p
.\build\fsr_vulkan_tests.exe --present
ctest --test-dir build -R '^(directstorage|dlss_camera|emulator_present_mode_cli)$' --output-on-failure
```

The DLSS presentation tests require keeping the visible test window in the foreground. NVIDIA disables frame generation for an unfocused window. Keep unsaved GPU application work closed when reproducing the unresolved 1080p device-loss test.
