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
