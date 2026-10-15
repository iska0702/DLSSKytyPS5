# NVIDIA DLSS Ray Reconstruction — experimental lighting backend

This work is on `feature/dlss-ray-reconstruction`, after the published `v1.5.0` release. The 1.5.0 ZIP is unchanged and does not include RR.

## What is implemented

- Load and check `sl::kFeatureDLSS_RR` through Streamline 2.14.1, with the matching signed `sl.dlss_d.dll` and `nvngx_dlssd.dll` runtimes.
- Use the documented custom-engine NGX project identity. An invented numeric application ID combined with an empty project GUID caused the initial RR capability query to fail; the corrected identity enables RR on the verification RTX 5060 Laptop GPU.
- Dispatch an offscreen HDR RR pass using noisy color, linear diffuse/specular albedo, packed float normal/roughness, depth, dense motion and world-space specular hit distance. Supply verified camera constants and inverse view transforms.
- Reject missing, incompatible or uncertified inputs. Do not pass the final SDR presentation image to RR.
- Preserve HDR output for the caller's subsequent composition/tone mapping. Reset history on mode/extent/camera changes and free SDK resources before Streamline shutdown.
- Request the backend with `--dlss-rr` or the launcher's explicitly labelled lighting-backend checkbox. GPU capability and input availability are separate states; requesting the backend does not imply RR is evaluating game frames.

## Current game limitation

**RR is not yet active in ASTRO BOT.** The existing presentation capture supplies final postprocessed color, depth, motion and camera for ordinary DLSS/FSR. It does not provide a certified noisy HDR lighting snapshot, unpacked linear diffuse/specular material guides and reflection hit distance before the guest denoisers. The local build therefore continues ordinary DLSS and reports `RR waiting for lighting inputs`.

Finishing ASTRO BOT integration requires identifying and replacing its relevant guest denoising pass, decoding its material G-buffer, snapshotting all matching lighting inputs, and returning the reconstructed HDR image before the game's composition/tone-mapping passes. Simply attaching G-buffer-shaped textures or enabling a checkbox would not establish correct RR semantics. No constant/fabricated game guides are used.

## Validation

```powershell
cmake --build build --target kyty_emulator launcher dlss_rr_input_tests directstorage_tests
.\build\dlss_rr_input_tests.exe
.\build\directstorage_tests.exe --dlss-rr-vulkan
ctest --test-dir build -R '^(dlss_rr_inputs|dlss_camera|directstorage|emulator_present_mode_cli)$' --output-on-failure
```

The optional GPU test uses controlled synthetic HDR lighting to check real `slEvaluateFeature(kFeatureDLSS_RR)` execution, resource lifetime and GPU readback against a constant-radiance reference over three frames. It passed on the verification GPU. It does not establish gameplay denoising quality or constitute ASTRO BOT RR support. The input-contract tests reject ordinary presentation frames, SDR/sRGB data, missing reflection distance, inconsistent grids, image aliasing, invalid output layouts and incorrect reflection transforms.

Authoritative API requirements are in the supplied SDK's `docs/ProgrammingGuideDLSS_RR.md` and `include/sl_dlss_d.h`.
