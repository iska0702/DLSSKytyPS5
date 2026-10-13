# Runtime findings: PPSA01325, 2026-10-12

Tested `D:/Programs/PPSA01325` with the rebuilt emulator and packaged Streamline
2.14.1 libraries, RTX 5060 Laptop GPU / driver 617.14, 2560x1440 output,
DLSS Quality and fixed MFG 4x. Captures are in the distribution's
`_TempData/dlss-integration-capture` and `dlss-integration-capture-2` folders.

## Confirmed MFG repair

The original integration intercepted swapchain creation, acquire and present,
but enumerated images through native Vulkan. Routing `vkGetSwapchainImagesKHR`
through the Streamline interposer repaired the missing hook. The second capture
reports `DLSS-G Runtime Status: 0x0 (OK), frames presented since last query: 4`.
There are occasional counts of 1 around scene/loading transitions, followed by 4.
This confirms generated presentations, not an independently measured FPS or
image-quality result. Test process was stopped after collecting the capture.

## Confirmed SR execution

During gameplay the presentation source is 1708x960, output 2560x1440, effective
Quality. The new capture reports successful DLSS evaluation. The old failure
using DLAA at mismatched 2560x1440 -> 2560x1600 dimensions is absent here.

## Native guest guides

The guest deferred geometry passes write five render targets, including slot 4
at 1708x960 in R16G16_SFLOAT and depth at the same dimensions in
D32_SFLOAT_S8_UINT. Addresses observed were 0x50A490000 and 0x50AB90000;
they are observations, not stable addresses to hard-code.

Captured PS hash 00B2AAA5E8E98F3F exports to target 4. Its disassembly loads
two 4x4 matrices at offsets 0 and 320 of the same descriptor, performs two
perspective divisions, differences the projected positions with 0.5 factors,
and exports the packed pair. This is evidence of a native motion-vector producer.
Its conditional branch supplies 128 in Y as an invalid-vector sentinel.
The integration now identifies the producer and generation-checked resource IDs,
refreshes the pair when other materials write those same G-buffer attachments,
and converts current-minus-previous UV to current-to-previous displacement.
Nonfinite vectors and components outside [-1,1] become zero. Depth is copied
through a depth-only sampled view into R32_FLOAT. Both outputs are recorded with
the color snapshot and owned by the prepared-frame pool until its presentation
fence permits reuse. They are tagged for both SR and FG. Loading and unsupported
frames cannot reuse a candidate from an earlier flip.

Capture 5 confirms successful native-guide tagging and SR evaluation, and exposed
the initial material-only tracking gap repaired by tracking the verified resource
pair. Capture 6 verifies normal draw optimizations: native guides remain selected
through more than 4,000 scene presentations, with successful SR evaluation and
no evaluation failures. MFG reports four presented frames while interpolation is
enabled; it correctly drops to one when the window loses focus. A separate
`dlss-sr-native-capture` verifies production mode without diagnostic inventory,
native guides and CAS disabled with explicit sharpness zero, over thousands of
scene presentations. The test processes were stopped after collecting logs.
Camera matrices, jitter, native antialiasing interaction and HUD separation
still need validation; no claim of fully correct temporal reconstruction or
visually confirmed improvement is made. Vulkan validation layers are not installed
on this machine.

## Camera and motion corrections, 2026-10-13

The new captures identify the 496-byte Astro view block: current view-projection
at byte 0, view at 64, projection at 128, inverse view at 192, inverse projection
at 256, previous view-projection at 320, normalized projection jitter at 384,
near/far at 424/428 and render dimensions at 432/436. Moving-camera captures
were essential: projection and view-projection are equal in the opening scene.
The decoder validates perspective structure, viewport, matrix products and finite
values, and uses coherent backing reads. It does not depend on guest addresses.

Projection jitter is removed from matrices sent to Streamline. The raster shift
is the negative of the buffer's jitter and is converted to render-pixel units.
Normalized motion is negated and has the current/previous jitter difference
removed, so the SDK's motion-vectors-jittered flag remains consistently false.
Gameplay doubles the opening scene's jitter amplitude; the decoder preserves
the actual sample positions instead of rejecting half the samples and repeatedly
resetting history. Projection changes, missing guides and broken continuity reset
history. Camera constants travel with the same prepared frame as depth and motion.

Verified producer variants include 00B2AAA5E8E98F3F, 4077CAA2B4017744,
1F603955EAA7C161 and AB73801B6EF3E1D5. Other materials can refresh a previously
verified resource pair, including passes with fewer active color attachments.
The guest's invalid-motion sentinel is resolved using native depth and camera
reprojection when camera data is valid, rather than forcing static surfaces to
zero screen motion during camera movement. CAS supplies sharpening once;
the DLSS sharpening option is zero to avoid applying the setting twice.

`dlss_camera_tests` covers captured projection values, raster jitter sign and
units, moving cameras, reprojection of a static world point, history continuity,
the larger gameplay jitter range and invalid/singular inputs. These tests pass.
Captures 6 and 8 show successful 1708x960 to 2560x1440 SR evaluation with parsed
camera data; capture 6 includes user-controlled gameplay. Capture 8 includes the
depth-based sentinel resolve. Runtime logs and shader compilation do not prove
visual quality, and Vulkan validation layers remain unavailable.

The presentation input remains the guest's final postprocessed color with UI.
Separating HUD and replacing guest antialiasing are not implemented by these
camera/motion fixes. Those limitations must not be described as completed.

After the camera/motion corrections, the user reported that gameplay was no
longer blurry but text and the Sony startup screen remained blurry. The revised
presentation path now bypasses DLSS when camera or native guides are missing.
Those frames use clamped Catmull-Rom spatial magnification instead of temporal
reconstruction with fabricated guides and constantly reset history. Frame
generation is suspended on those frames and reconfigured when native inputs
return. CAS is applied only once on reconstructed game frames; the spatial
logo/menu path avoids an additional sharpening pass.

Capture 9 verifies the final transition on hardware: startup frames report
`DLSS Evaluated: NO`, then native scene frames report `YES`; DLSS-G reports
status 0x0 and four presented frames after native inputs resume. The installed
emulator matches the rebuilt executable (SHA-256
`28699495B3E7BB50CA3A91CBB7190867819170606FC4819929AC30333E01F69E`).

The user still reported blurry startup text after spatial filtering. The Quality
patch renders the entire guest frame, including UI, at 1708x960. A new optional
`tools/Run_Astro_DLAA_Native.cmd` uses 2560x1440 guest rendering and DLAA at the
same output size. Native HD/FHD/QHD/4K profiles are disabled by default and
selected automatically for DLAA. Resolution parsing now skips the letter x in
"Fixed" and reads the actual dimensions. Capture 11 confirms automatic QHD Native
selection, 2560x1440 camera buffers, NGX DLAA creation and successful evaluation;
DLSS-G reports status 0x0 with four presented frames. Native rendering costs more
GPU time. Visual improvement of the remaining startup blur has not yet been
confirmed by the user, and low-detail source assets cannot be repaired by DLAA.
