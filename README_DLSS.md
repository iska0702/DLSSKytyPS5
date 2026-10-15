# KytyPS5 — DLSS & Frame Generation (Experimental Fork)

An experimental fork of the **KytyPS5** PlayStation 5 emulator featuring native integration of **NVIDIA DLSS Super Resolution (DLSS 3.7+)**, **DLSS 3 Frame Generation**, and **NVIDIA Reflex Low Latency** via the NVIDIA Streamline SDK.

---

## Key Features

1. **NVIDIA DLSS Super Resolution (DLSS 3.7+)**:
   - High-fidelity AI upscaling from lower internal render resolutions up to target display resolutions (1080p, 1440p / 2K, 4K).
   - Dynamic resolution reconstruction accelerated by NVIDIA Tensor Cores.
   - Built-in sharpness slider (**DLSS Sharpness**) configurable directly from the launcher GUI.

2. **NVIDIA DLSS 3 Frame Generation (Experimental)**:
   - AI-powered intermediate frame generation leveraging Optical Flow Accelerators on **GeForce RTX 40-series GPUs**.
   - Accessible via the **"Enable Frame Generation (DLSS 3)"** toggle in the launcher settings (standard 2x FG).
   - *Advanced Multi-Frame Generation (MFG)* is preserved for power users via the CLI flag: `--dlss-mfg <2x|3x|4x|dynamic>`.

3. **NVIDIA Reflex Low Latency**:
   - Reduces system and presentation latency (Modes: *Off*, *On*, *On + Boost*).

4. **Game resolution patches**:
   - ASTRO BOT (`_Patches/PPSA21567.json`) has 30 profiles: Full HD, QHD/2K and 4K, separate DLSS/FSR modes, and Native AA, Quality, Balanced, Performance and Ultra Performance.
   - Select exactly one profile. See [the complete resolution table](ASTRO_BOT_RESOLUTIONS.md). NVIDIA model presets J/K/L/M are separate launcher options and do not define render resolution.
   - Astro's Playroom uses its separate `_Patches/PPSA01325.json` patch.

---

## Quick Start & Verification

### Current integration limits

The presentation path receives the guest game's final color image. Astro's
Playroom now supplies real depth and motion through verified G-buffer producers
(including PS hash `00B2AAA5E8E98F3F`). The captured textures are tied to each prepared
frame; motion direction and the game's invalid-vector sentinel are converted
before tagging. Other materials writing the verified resource pair refresh the
same inputs. Resource IDs include generation checks; guest addresses are not hard-coded.
Loading/menu frames and unsupported games use spatial presentation. Astro camera matrices
and jitter are decoded from the verified 496-byte view buffer; normalized motion
has the current/previous projection jitter removed. A separate HUD-free color pass
remains unimplemented. An NVIDIA indicator
confirms SDK activity, not reconstruction quality.

With native guides, SR retains history and resets it when guide availability,
resolution or validated camera continuity changes. Without native guides or camera,
DLSS evaluation is bypassed; logo/menu magnification uses clamped Catmull-Rom
reconstruction. Frame generation pauses and resumes with the native inputs. This remains
an experimental integration: the color input includes guest postprocessing and UI;
it does not replace the game's existing antialiasing pass.

Sharpness is applied once by the presentation CAS pass on DLSS game frames.
Zero disables sharpening; the logo/menu spatial path does not add CAS halos.
Frame-generation multipliers are limited to the device's reported capabilities.
The overlay uses observed presentation statistics rather than the requested multiplier;
it needs 120 application presents to establish an average. Dynamic MFG on Vulkan
falls back to a supported fixed multiplier. Actual image quality and frame pacing
must be verified in a running game.

For identifying the guest resources, `--dlss-diagnostics` now records a bounded
inventory (512 distinct pass signatures): producer VS/PS hashes, render-target
slots/formats/dimensions, depth state, and image/buffer descriptor words. These
are candidates for analysis, not automatic motion-vector detection. Set
`KYTY_DRAW_RUN=0` for a diagnostic capture to avoid the fast draw replay path,
and use `--graphics-debug-dump true --shader-log-direction File --shader-log-folder <capture-folder>` to
save the corresponding shaders. Diagnostics are disabled in normal runs.

### 1. Configure Launcher
1. Open **Launcher** -> **Settings** -> **Configuration**.
2. Under the **Graphics** tab:
   - Set **Screen Resolution** to your target display resolution (e.g., `2560x1440` or `3840x2160`).
   - Under **NVIDIA DLSS & Streamline (Experimental)**:
     - Check **Enable DLSS Super Resolution**.
     - (Optional, RTX 40 & 50 series): Check **Enable Frame Generation (DLSS 3 / MFG)** and pick your multiplier (2x, 3x, 4x, Dynamic).
     - Adjust **Sharpness** to preference (default: 0.15).
     - Select **NVIDIA Reflex Mode** (*On* or *Boost*).

### 2. Select Internal Render Resolution in Game Patches
1. Right-click the game (e.g., *Bloodborne / Demon's Souls / ASTRO BOT*) -> **Cheats (experimental)...**.
2. Select your desired internal resolution preset matching your target scale (e.g., *QHD - DLSS Performance (1280x720)* or *QHD - DLSS Quality (1708x960)*).
3. The game renders at the selected lower resolution and DLSS reconstructs the output. Clarity depends on valid temporal inputs and the game's existing antialiasing.

### 3. Verify DLSS with the NVIDIA On-Screen Indicator
To visually verify that DLSS and Streamline are actively processing frames:
- Double-click `Enable_NVIDIA_DLSS_Indicator.reg` to activate NVIDIA's official DLSS overlay.
- When launching the emulator, a watermark will appear in the lower-left corner displaying:
  - DLSS DLL version (e.g., `3.7.x`)
  - Real-time input render resolution vs. output display resolution
  - Active DLSS scaling preset
- To disable the overlay later, double-click `Disable_NVIDIA_DLSS_Indicator.reg`.

### 4. Enable Hardware-Accelerated GPU Scheduling (HAGS) for Frame Generation
DLSS Frame Generation strictly requires Windows Hardware-Accelerated GPU Scheduling:
- Double-click `Enable_HAGS_For_DLSS_MFG.reg` and reboot your PC (or enable in *Windows Settings -> System -> Display -> Graphics -> Hardware-accelerated GPU scheduling*).

---

## DirectStorage and crash diagnostics

DirectStorage currently serves the APR file I/O path only; GPU texture
streaming and GPU decompression are not integrated. Guest destinations are
copied from a bounded CPU staging buffer so GPU memory tracking remains active.
See [runtime findings](docs/DIRECTSTORAGE_RUNTIME_FINDINGS.md) for the measured
results and the separate Astro physics assertion investigation.

Unhandled faults save their original registers and code bytes under
`_HangTrace/guest-fault-*.txt`. Release run scripts also preserve the console log.

## System Requirements

- **OS**: Windows 10/11 x64 (Build 19041+) with HAGS enabled
- **GPU**:
  - **NVIDIA GeForce RTX 20 / 30 / 40 / 50 Series** (Required for DLSS Super Resolution & Reflex)
  - **NVIDIA GeForce RTX 40 / 50 Series** for Frame Generation; **RTX 50 Series** for Multi-Frame Generation
- **Driver**: Latest NVIDIA Game Ready / Studio Drivers.
- **Dependencies**: Included in release (`sl.*.dll`, `nvngx_*.dll`, `Qt6` runtime).

## Native Astro rendering with DLAA

Run `Run_Astro_DLAA_Native.cmd` from the Windows distribution (source:
`tools/Run_Astro_DLAA_Native.cmd`). It selects native 2560x1440 guest rendering,
DLAA and MFG 4x. This avoids the 1708x960 Quality profile reducing UI resolution,
but uses more GPU time. Native profiles are selected automatically for DLAA;
unsupported startup frames use spatial presentation. The regular
`Run_Astro_DLSS_MFG.cmd` keeps the faster Quality profile.

## AMD FidelityFX alternative

FSR 3.1 Super Resolution and Frame Generation are available through --fsr and --fsr-fg, or launcher settings. See [README_FSR.md](README_FSR.md) for setup and current limits.
