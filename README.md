# KytyPS5 (NVIDIA DLSS, Frame Generation & DirectStorage Fork)

[![Version](https://img.shields.io/badge/release-v1.5.6-orange.svg)](https://github.com/iska0702/DLSSKytyPS5/releases/tag/v1.5.6)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](#system-requirements)
[![Base](https://img.shields.io/badge/upstream-Jetsku%2FKytyPS5%20int21-green.svg)](https://github.com/Jetsku/KytyPS5)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

An advanced experimental fork of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) (`u59 int21`), integrating **NVIDIA DLSS 3 / DLSS 4 Multi-Frame Generation (MFG)**, **AMD FSR 3.1**, **NVIDIA Reflex**, and **Microsoft DirectStorage** asset streaming for PlayStation 5 emulation on Windows x64.

---

## Bundled Libraries & DLL Versions

| Component | Library / DLL | Version | Purpose |
|---|---|---|---|
| **NVIDIA Streamline** | `sl.interposer.dll` | `2.14.1` | Interposer framework coordinating graphics hooks |
| **DLSS Super Resolution** | `sl.dlss.dll`, `nvngx_dlss.dll` | `3.10.6` | AI-accelerated spatial-temporal upscaling & DLAA |
| **DLSS Frame Generation** | `sl.dlss_g.dll`, `nvngx_dlssg.dll` | `3.10.6` | 2x Frame Generation & 3x/4x/Dynamic Multi-Frame Gen |
| **NVIDIA Reflex** | `sl.reflex.dll` | `2.14.1` | Render queue and system latency reduction |
| **AMD FSR 3** | Built-in Vulkan backend | `3.1.2` | High-fidelity FidelityFX Super Resolution & Frame Gen |
| **DirectStorage** | `dstorage.dll`, `dstoragecore.dll` | `1.2.3` | Direct NVMe bypass streaming for game packages & `.zar` |

---

## Features Added in This Fork

### 1. NVIDIA DLSS Super Resolution & DLAA
- Integrates native depth buffers, motion vectors, and jitter offsets provided by guest rendering.
- Modes supported: `UltraPerformance`, `Performance`, `Balanced`, `Quality`, and native resolution `DLAA`.
- Tunable Contrast Adaptive Sharpening (CAS) slider directly in launcher settings.

### 2. Multi-Frame Generation (MFG 2x, 3x, 4x, Dynamic)
- **2x Multiplier (DLSS 3):** Generates 1 interpolated frame per rendered frame (supported on GeForce RTX 40 & 50 series).
- **3x / 4x Multipliers (DLSS 4 MFG):** Multi-frame generation synthesizing 2 or 3 frames per rendered frame.
- **Dynamic Multiplier:** Real-time adaptive frame generation based on display refresh rate headroom.

### 3. NVIDIA Reflex Low Latency
- Native Reflex integration (`On`, `On + Boost`, `Off`) tightly coupled with swapchain presentation and frame pacing.
- Automatically enforced whenever Frame Generation is activated to prevent input lag accumulation.

### 4. AMD FidelityFX Super Resolution 3.1 (FSR 3.1)
- Built-in Vulkan backend providing vendor-agnostic upscaling and 2x frame generation for AMD Radeon and Intel Arc graphics.

### 5. Microsoft DirectStorage Fast I/O
- Asynchronous GPU/CPU asset decompression and streaming bypasses conventional Win32 file bottlenecks.
- Accelerated package streaming from raw game directories and compressed ZArchive (`.zar`) containers.

### 6. Full ZArchive Cheat & Patch Support
- Transparent SHA-256 calculation for virtual files loaded from `.zar` containers (e.g. `PPSA21567.zar!\eboot.bin`), resolving cheat errors in *Astro Bot* and other games.
- Built-in fallback flags `KYTY_IGNORE_CHEAT_HASH=1` and `KYTY_IGNORE_CHEAT_ERROR=1`.

### 7. Upstream Sync: Jetsku/KytyPS5 int21
- Fully synchronized with upstream `Jetsku/KytyPS5` `main` branch through commit `744705ee` (`u59 int21`).
- Preserves all upstream fixes: CPU placement optimizations, audio object mixing, VR/OpenXR, new shader codegen pipelines, and enhanced memory barriers.

---

## Advantages & Disadvantages

### Advantages (Pros)
- **Massive Performance Boost:** Up to 2x–4x frame rates when utilizing DLSS Frame Generation and Multi-Frame Gen.
- **Superior Reconstruction:** Tensor Core AI temporal anti-aliasing yields cleaner image edges than bilinear or spatial upscalers.
- **Reduced Input Latency:** Reflex offsets latency penalty introduced by frame generation buffers.
- **Fast Asset Streaming:** DirectStorage drastically shortens loading screen times and virtually eliminates stutter in streaming open-world titles.
- **Intuitive GUI Configuration:** Dedicated unified **Upscalers & Frame Generation** tab (DLSS + FSR 3.1) and DirectStorage panels in the graphical launcher.

### Disadvantages & Limitations (Cons)
- **Hardware Requirement:** NVIDIA DLSS requires an NVIDIA GeForce RTX GPU; Frame Generation (2x/3x/4x) requires RTX 40 or 50 series cards.
- **HUD Ghosting / Artifacts:** Titles that lack separate HUD-free render passes may display minor UI motion blur or ghosting under extreme motion.
- **Experimental Ray Reconstruction:** Certified HDR lighting buffers are required for full Ray Reconstruction; games without exposed denoising inputs fallback to standard DLSS SR.
- **DirectStorage NVMe Recommendation:** Maximum throughput requires a PCIe Gen 3/4/5 NVMe SSD running on Windows 10/11.

---

## Requirements

- **Operating System:** Windows 10/11 64-bit (version 21H2 or later recommended).
- **Processor:** x86-64 CPU with AVX2 and FMA (Intel 10th Gen+ or AMD Ryzen 3000+).
- **Graphics Card:**
  - *For DLSS SR:* NVIDIA GeForce RTX 20 / 30 / 40 / 50 Series.
  - *For DLSS Frame Generation:* NVIDIA GeForce RTX 40 / 50 Series.
  - *For FSR 3.1:* Vulkan 1.3 capable GPU (AMD Radeon RX 6000+, Intel Arc A-Series, NVIDIA GTX/RTX).
- **Storage:** NVMe SSD recommended for DirectStorage streaming.

---

## Usage

### Graphical Launcher
Launch `launcher.exe` (or `kyty_launcher.exe`). Under game settings:
1. Navigate to **NVIDIA DLSS** group.
2. Check **Enable DLSS** and choose your desired mode and sharpening level.
3. Check **Enable Frame Generation (DLSS 3 / MFG)** and select your desired multiplier (2x, 3x, 4x, or Dynamic).
4. Configure **DirectStorage** under General settings if streaming from fast storage.

### Command Line Flags
```powershell
kyty_emulator.exe --game "D:\Games\ExampleGame.zar" --dlss --dlss-mode Quality --dlss-sharpness 0.35 --dlss-fg --dlss-mfg 2x --reflex on --directstorage
```

---

## License & Attribution

- KytyPS5 is licensed under the [GNU General Public License version 2 (GPL-2.0-only)](LICENSE).
- Based on [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) and the original [Kyty](https://github.com/InoriRus/Kyty) project by InoriRus.
- NVIDIA Streamline and DLSS binaries are proprietary components owned by NVIDIA Corporation.
