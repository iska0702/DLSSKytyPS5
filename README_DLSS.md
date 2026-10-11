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

4. **Engineered Resolution Patches (Game Patches)**:
   - Pre-configured render resolution tiers in `_Patches/PPSA01325.json` and `_Patches/PPSA21567.json` (ASTRO BOT):
     - **HD (720p Output)**: Ultra Perf (`428x240`), Perf (`640x360`), Balanced (`744x432`), Quality (`854x480`)
     - **FHD (1080p Output)**: Ultra Perf (`640x360`), Perf (`960x540`), Balanced (`1114x648`), Quality (`1280x720`)
     - **QHD / 2K (1440p Output)**: Ultra Perf (`854x480`), Perf (`1280x720`), Balanced (`1486x840`), Quality (`1708x960`)
     - **4K (2160p Output)**: Ultra Perf (`1280x720`), Perf (`1920x1080`), Balanced (`2228x1252`), Quality (`2560x1440`)
   - All render dimensions are strictly aligned to even integers (multiples of 2/4), completely preventing the NVIDIA NGX `0xbad00005` (`NVSDK_NGX_Result_FAIL_InvalidParameter`) error.

---

## Quick Start & Verification

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
3. The game will render at this lower base resolution, and DLSS will upscale it to crisp 1440p/4K with Tensor Core anti-aliasing.

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

## System Requirements

- **OS**: Windows 10/11 x64 (Build 19041+) with HAGS enabled
- **GPU**:
  - **NVIDIA GeForce RTX 20 / 30 / 40 / 50 Series** (Required for DLSS Super Resolution & Reflex)
  - **NVIDIA GeForce RTX 40 / 50 Series** (Required for DLSS Frame Generation & Multi-Frame Generation)
- **Driver**: Latest NVIDIA Game Ready / Studio Drivers.
- **Dependencies**: Included in release (`sl.*.dll`, `nvngx_*.dll`, `Qt6` runtime).
