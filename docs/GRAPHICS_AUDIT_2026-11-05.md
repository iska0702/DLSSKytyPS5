# GPU graphics audit — 2026-11-05

**Follow-up:** the device-loss failures recorded below are superseded by the
[Frame Generation fix](FRAME_GENERATION_FIX_2026-11-05.md). MFG 4× at 1080p now
passes a 600-frame check; native Dynamic MFG remains unavailable on this driver.

Local hardware: NVIDIA GeForce RTX 5060 Laptop GPU, Windows driver
`32.0.16.2101`, Vulkan. `HwSchMode=2` in the Windows registry. No registry,
driver or global GPU settings were changed.

These are synthetic GPU integration checks, not new gameplay or long-session
verification. Presentation tests use the production DLSSManager. The FSR smoke
exercises the supplied SDK's upscaling/interpolation and replacement swapchain.

| Check | Observed result | Log in `build/` |
| --- | --- | --- |
| FSR SR + FG 2× | PASS; GPU readback matched RGB reference; 3 application frames produced 6 actual presents | `audit-fsr-20261105.log` |
| DLSS SR + FG 2×, 1280×720 | PASS; 130 application frames completed, SDK-derived observed multiplier 2×; repeated after the camera fix | `audit-dlss-fg2-final-20261105.log` |
| DLSS SR + MFG 3×, 1280×720 | PASS; 130 application frames completed, observed multiplier 3× | `audit-dlss-mfg3-20261105.log` |
| DLSS SR + MFG 4×, 1280×720 | FAIL; device lost on submit after initial generated presents; not verified as stable | `audit-dlss-mfg4-20261105.log` |
| DLSS SR + FG 2×, 1920×1080 | FAIL; `ErrorDeviceLost` at wait-idle; the historical 1080p problem remains reproducible | `audit-dlss-fg1080-20261105.log` |
| Dynamic MFG | SDK reports unsupported: required driver components unavailable | `audit-dlss-requirements-20261105.log` |
| Camera, presentation CLI, U59 preset | PASS, 3/3 after the camera correction | `audit-regressions-20261105.log` |

The SDK reports a maximum of five generated frames on this configuration. That
capability alone does not establish stable presentation at any particular multiplier.
The cause of the 4×/1080p device loss was not established by this audit. No driver
issue or synchronization fix is claimed. MFG 3× required the user to focus the test
window; loss of focus is separate from the recorded device-loss failures.

## Changes made during verification

- Add explicit `--dlss-mfg3-vulkan` and `--dlss-mfg4-vulkan` GPU test modes. Select
  the requested production setting and require that exact observed multiplier;
  silently falling back to a lower multiplier cannot pass the test.
- Correct camera jitter consistency. The existing camera policy accepted offsets
  up to four render pixels but clipped only the SDK pixel offset to two while
  retaining the original projection and motion jitter. Preserve the accepted
  displacement and keep the existing four-pixel rejection boundary. Tests cover
  two-pixel acceptance, larger unclipped offsets and rejection outside the range.
- Rebuild the emulator and refresh its copy beside the launcher. Release ZIPs
  were not changed or published.

## Reproduce

After building `directstorage_tests`, `fsr_vulkan_tests` and `dlss_camera_tests`:

```powershell
.\build\fsr_vulkan_tests.exe --present
.\build\directstorage_tests.exe --dlss-requirements
.\build\directstorage_tests.exe --dlss-vulkan
.\build\directstorage_tests.exe --dlss-mfg3-vulkan
.\build\directstorage_tests.exe --dlss-mfg4-vulkan
.\build\directstorage_tests.exe --dlss-vulkan-1080p
ctest --test-dir build -R '^(dlss_camera|emulator_present_mode_cli|u59_preset)$' --output-on-failure
```

Run presentation tests sequentially and keep each DLSS test window focused.
The failing modes above can reset the GPU; they remain failure reproducers.
