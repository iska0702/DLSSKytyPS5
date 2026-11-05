# KytyPS5 (NVIDIA DLSS, Frame Generation & DirectStorage Fork)

[![Version](https://img.shields.io/badge/release-v1.6.5-orange.svg)](https://github.com/iska0702/DLSSKytyPS5/releases/tag/v1.6.5)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4.svg)](#system-requirements)
[![Base](https://img.shields.io/badge/upstream-Jetsku%2FKytyPS5%20int21-green.svg)](https://github.com/Jetsku/KytyPS5)
[![License](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

An advanced experimental fork of [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) (`u59 int21`), integrating **NVIDIA DLSS 3 / DLSS 4 Multi-Frame Generation (MFG)**, **AMD FSR 3.1**, **NVIDIA Reflex**, and **Microsoft DirectStorage** asset streaming for PlayStation 5 emulation on Windows x64.

---

## v1.6.5 — Frame Generation synchronization fixes

Fixes the reproduced MFG 4× and DLSS FG 1080p device losses caused by bypassing
Streamline's Vulkan idle synchronization. A 600-frame 1080p GPU test now confirms
the requested 4× multiplier. FSR FG, DLSS FG 2× and MFG 3× checks also pass.
See [release notes](RELEASE_NOTES_1.6.5.md) for validation scope and remaining limitations.

## Bundled Libraries & DLL Versions

| Component | Library / DLL | Version | Purpose |
|---|---|---|---|
| **NVIDIA Streamline** | `sl.interposer.dll` | `2.14.1` | Interposer framework coordinating graphics hooks |
| **DLSS Super Resolution** | `sl.dlss.dll`, `nvngx_dlss.dll` | `310.9.1.0` (NGX file version) | AI-accelerated spatial-temporal upscaling & DLAA |
| **DLSS Frame Generation** | `sl.dlss_g.dll`, `nvngx_dlssg.dll` | `310.9.1.0` (NGX file version) | 2x Frame Generation & 3x/4x/Dynamic Multi-Frame Gen |
| **NVIDIA Reflex** | `sl.reflex.dll` | `2.14.1` | Render queue and system latency reduction |
| **AMD FSR 3** | Built-in Vulkan backend | `3.1.2` | High-fidelity FidelityFX Super Resolution & Frame Gen |
| **DirectStorage** | `dstorage.dll`, `dstoragecore.dll` | `1.2.2407.1501` (file version) | Async file reads and mapped texture streaming; archive-reader fallback for `.zar` |

---

## Features Added in This Fork

### 1. NVIDIA DLSS Super Resolution & DLAA
- Integrates native depth buffers, motion vectors, and jitter offsets provided by guest rendering.
- Modes supported: `UltraPerformance`, `Performance`, `Balanced`, `Quality`, and native resolution `DLAA`.
- Tunable Contrast Adaptive Sharpening (CAS) slider directly in launcher settings.

### 2. Multi-Frame Generation (MFG 2x, 3x, 4x, Dynamic)
- **2x Multiplier (DLSS 3):** Generates 1 interpolated frame per rendered frame (supported on GeForce RTX 40 & 50 series).
- **3x / 4x Multipliers (DLSS 4 MFG):** Multi-frame generation synthesizing 2 or 3 frames per rendered frame.
- **Dynamic Multiplier:** Native dynamic mode is used only when the SDK/driver reports support; otherwise the application reports its fixed-multiplier fallback explicitly.

### 3. NVIDIA Reflex Low Latency
- Native Reflex integration (`On`, `On + Boost`, `Off`) tightly coupled with swapchain presentation and frame pacing.
- Automatically enforced whenever Frame Generation is activated to prevent input lag accumulation.

### 4. AMD FidelityFX Super Resolution 3.1 (FSR 3.1)
- Built-in Vulkan backend providing vendor-agnostic upscaling and 2x frame generation for AMD Radeon and Intel Arc graphics.

### 5. Microsoft DirectStorage Fast I/O
- Asynchronous APR/file reads and verified mapped Vulkan texture sources support asset streaming.
- ZArchive (`.zar`) members retain their ordinary archive-reader fallback; PS5 archive GPU decompression is not implemented. See [DirectStorage details](README_DIRECTSTORAGE.md).

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
Launch `kyty_launcher.exe`. Under game settings:
1. Navigate to the **Upscalers & Frame Generation** tab.
2. Check **Enable DLSS** and choose your desired mode and sharpening level.
3. Check **Enable Frame Generation (DLSS 3 / MFG)** and select your desired multiplier (2x, 3x, 4x, or Dynamic).
4. Configure **DirectStorage** under General settings if streaming from fast storage.

### Command Line Flags
```powershell
kyty_emulator.exe --game "D:\Games\ExampleGame.zar" --dlss --dlss-mode Quality --dlss-sharpness 0.35 --dlss-fg --dlss-mfg 2x --reflex on --directstorage
```

---

## Zero-Stutter & Performance Optimization Guide (Как убрать лаги и заикания)

If you experience frame rate drops, CPU lag, micro-stutters, or intermittent Frame Generation (MFG), follow these essential optimization steps:

### 1. Disable Windows Memory Integrity (HVCI / VBS) — Major CPU Speedup
- **The Problem:** PS5 emulation relies on guest memory write-tracking page faults (`VirtualProtect`). When Windows **Memory Integrity (HVCI)** / **Virtualization-Based Security (VBS)** is enabled, every page fault is intercepted by the Microsoft Hypervisor (Ring -1). This increases fault handling latency from **2.0 µs to 766.5 µs (over 380x slower)**, consuming **20–25 ms of CPU time per frame** and throttling maximum frame rates to ~35 FPS regardless of GPU power.
- **The Fix:**
  1. Open Windows Start menu and search for **Core Isolation** (Изоляция ядра).
  2. In **Device Security -> Core isolation details** (Безопасность устройства -> Сведения об изоляции ядра), turn **OFF** **Memory integrity** (Целостность памяти).
  3. Restart your PC.
  4. *Result:* Eliminates the 23 ms/frame CPU penalty and immediately restores full 60+ FPS base performance.

### 2. Precompile All Shaders via Right-Click Menu (Компиляция шейдеров)
- **The Problem:** Compiling shaders in real-time during gameplay produces hitches and micro-freezes whenever new effects, environments, or characters appear.
- **The Fix:**
  1. Open `kyty_launcher.exe`.
  2. **Right-click** on any game in the list (e.g. *ASTRO BOT* or *Astro's Playroom*).
  3. Select **"Compile shaders..."** (Скомпилировать шейдеры).
  4. The dedicated multi-threaded Vulkan compiler dialog will warm and precompile 100% of journaled compute/graphics pipelines directly to disk (`_PipelineCache/`).
  5. Once complete, start the game for completely stutter-free gameplay!

### 3. DLSS Frame Generation & Multi-Frame Gen (MFG) Best Practices
- **Keep the Game Window in Focus:** NVIDIA Streamline and the GeForce driver automatically pause frame generation whenever the emulator window is not in active foreground focus (`DLSS-G disabled: window not focused`). If you click on a second monitor or another application, FG will pause and instantly resume once you focus the window again.
- **Swapchain Pacing (Mailbox Mode):** DLSS Frame Generation requires unlocked presentation without vertical sync stalls. Since v1.6.0, the emulator automatically initializes Vulkan swapchains in `VK_PRESENT_MODE_MAILBOX_KHR` whenever Frame Generation is enabled, eliminating `SyncInterval=1` mutex timeouts. Avoid forcing external VSync in third-party software.
- **Recommended MFG Multiplier:**
  - **8 GB VRAM GPUs (e.g. RTX 4060 / RTX 5060 Laptop):** Use **MFG 2x** or **MFG 3x**. 4x mode generates 3 frames per real frame and requires substantial memory bandwidth; 2x/3x ensures maximum smoothness and rock-solid frame pacing.
  - **12 GB+ VRAM GPUs (RTX 4070 Ti / 4080 / 5080 / 5090):** Use **MFG 3x** or **MFG 4x** to reach 144–240 FPS at 1440p / 4K.

### 4. Automated u59-Preset Performance Fallback
- In v1.6.0, all 60+ core performance tuning flags from `u59-preset.json` (deferred write protections `KYTY_DEFER_PROTECTS=all`, fast DMA transfers, BDA sparse page tables, VRAM GC budgets, and shader prefetch) are compiled directly into the binary as an automated fallback. Games will always run with maximum speed and zero "preset not found" warnings, even if run standalone.

## License & Attribution

- KytyPS5 is licensed under the [GNU General Public License version 2 (GPL-2.0-only)](LICENSE).
- Based on [Jetsku/KytyPS5](https://github.com/Jetsku/KytyPS5) and the original [Kyty](https://github.com/InoriRus/Kyty) project by InoriRus.
- NVIDIA Streamline and DLSS binaries are proprietary components owned by NVIDIA Corporation.
