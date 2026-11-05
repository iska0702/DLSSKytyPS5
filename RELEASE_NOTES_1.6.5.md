# DLSSKytyPS5 1.6.5

## Fixes

- Fix reproduced `ErrorDeviceLost` failures in MFG 4× and DLSS FG 1080p tests.
  Vulkan idle waits now use Streamline's interposer to coordinate its background
  generation and presentation queues.
- Drain FG work before releasing tagged inputs and destroying the swapchain;
  restore the original Vulkan idle handler after Streamline shutdown.
- Cover NGX transfer clears and compute writes in output-image barriers.
- Keep accepted camera jitter consistent with projection and motion data instead
  of clipping only the SDK's jitter offset.
- Report the SDK/driver's native Dynamic MFG capability accurately and explicitly
  identify the fixed-multiplier fallback when dynamic mode is unavailable.

## Verification

Tested on an RTX 5060 Laptop GPU with Windows driver `32.0.16.2101`:

| Mode | GPU test result |
| --- | --- |
| DLSS SR + MFG 4×, 1080p | 600 application frames, observed 4× multiplier, no device loss |
| DLSS SR + FG 2×, 1080p | Passed, observed 2× multiplier |
| DLSS SR + MFG 3×, 720p | Passed, observed 3× multiplier |
| FSR SR + FG 2× | Reference GPU readback; 6 actual presents from 3 application frames |
| Camera, CLI, U59 preset | 3/3 regression checks passed |

New GPU test modes cover MFG 3×/4× and longer runs with
`KYTY_DLSS_TEST_FRAMES=130..3600`. A lower fallback multiplier cannot pass a test
that requests a higher multiplier.

## Known limitations

- These are bounded synthetic checks, not a guarantee of long-session stability
  in every game. Some Vulkan Validation diagnostics remain in Streamline/NGX
  sequences; a completely clean validation run is not claimed.
- The tested driver does not expose native Dynamic MFG. Support is not fabricated.
- Game DLSS Ray Reconstruction integration in ASTRO BOT/PLAYROOM remains incomplete.
- Keep the application window focused for FG/MFG.

Details: [Frame Generation fix report](docs/FRAME_GENERATION_FIX_2026-11-05.md).

## Installation

Extract the Windows x64 archive into a new folder and run `kyty_launcher.exe`.
The package includes runtime DLLs, Qt plugins, the U59 preset, patches and licenses.
Local saves, shader caches and settings are excluded. Microsoft Visual C++
Redistributable x64 may be required. A SHA-256 checksum accompanies the ZIP.
