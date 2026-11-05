# Frame Generation fix — 2026-11-05

The previously failing MFG 4× and DLSS FG 1080p tests now complete without
`ErrorDeviceLost`. The combined stress check completed **600 application frames
at 1920×1080 with an observed 4× multiplier**, without lowering the requested mode.
These are bounded synthetic GPU checks, not long-session gameplay validation.

## Cause and changes

The manual Streamline integration intercepted presentation and swapchain functions,
but left `vkDeviceWaitIdle` pointing at the native Vulkan implementation. That
function requires external synchronization with submissions on every device queue.
Streamline was submitting on its dedicated generation/pacing worker queues at the
same time. Validation recorded `UNASSIGNED-Threading-MultipleThreads-Write` for
the application's `vkDeviceWaitIdle` racing those submissions.

The supplied SDK's `source/core/sl.interposer/vulkan/wrapper.cpp` has a dedicated
`vkDeviceWaitIdle` wrapper. Its before-hooks coordinate the plugin workers before
the native wait. The fix installs that wrapper in the application's Vulkan
dispatcher after successful Streamline initialization and restores the original
function during shutdown. GPU tests verify restoration explicitly.

Other changes:

- The presentation smoke uses an application submission fence per frame, rather
  than waiting all SDK queues on every frame.
- Resource release drains the coordinated queues and turns off/releases DLSS-G
  before deleting tagged images. Swapchain destruction also uses the coordinated
  wait. Shutdown no longer frees inputs ahead of that drain.
- The full-frame path clears the optional explicit Backbuffer tag; the interposer
  already owns the full replacement backbuffer. An extent is retained on the null tag.
- Output barriers cover NGX transfer clears as well as shader writes, including
  the transfer-write source dependency after evaluation.
- The Dynamic MFG fallback message now reports the SDK/driver capability result,
  instead of asserting that the feature is categorically D3D12-only. The observed
  driver still reports native Dynamic MFG unavailable; no capability is fabricated.

## Evidence

Hardware: RTX 5060 Laptop GPU, Windows driver `32.0.16.2101`, Vulkan.

| Check | Result | Log in `build/` |
| --- | --- | --- |
| MFG 4×, 720p, synchronization validation | PASS, observed 4×; no queue-threading or destroy-swapchain errors | `fg-fix-mfg4-final-validation-20261105.log` |
| MFG 4×, 1080p, 600 frames | PASS, observed 4×; shutdown restores native dispatcher | `fg-fix-final-stress-20261105.log` |
| DLSS FG 2×, 1080p | PASS, observed 2× | `fg-fix-final-fg1080-20261105.log` |
| MFG 3×, 720p | PASS, observed 3× | `fg-fix-final-mfg3-20261105.log` |
| FSR SR/FG | PASS, reference readback and 6 actual presents from 3 application frames | `fg-fix-final-fsr-20261105.log` |
| Camera, presentation CLI, U59 preset | PASS, 3/3 | `fg-fix-final-regressions-20261105.log` |

Validation is **not completely clean**: the final validation run still logged
36 synchronization/layout/semaphore diagnostics in Streamline/NGX command
sequences. Their elimination is not established by these passing tests. The
specific application DeviceWaitIdle threading race and outstanding-use error
at swapchain destruction are absent from the final validation log.

The isolated Khronos validation package is under `build/validation-20261105`;
no global layer registration or driver settings were changed. Downloaded archive
SHA-256: `328fbb1bfdc4b0583da034628af535e5d12e02581fd09bfd70bf31aa1f2cc700`.

Both build executables were refreshed and their hashes match. The installed
`D:\Programs\KytyPS5-U59-Windows-x64\kyty_emulator.exe` was updated as prescribed
by the repository's deployment script. Its previous executable is preserved as
`kyty_emulator.pre-fg-fix-20261105.bak`. Release ZIPs were not changed.

## Reproduce the stress check

```powershell
$env:KYTY_DLSS_TEST_FRAMES = '600' # accepted range: 130..3600
.\build\directstorage_tests.exe --dlss-mfg4-vulkan-1080p
```

Keep the test window focused. Passing requires the requested observed multiplier,
not a lower fallback. Validation can be enabled for this process only with
`VK_LAYER_PATH`, `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, and
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
