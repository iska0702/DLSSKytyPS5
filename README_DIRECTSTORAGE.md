# DirectStorage asset and texture streaming

Enable `--directstorage`, or **DirectStorage Asset Streaming (Experimental)** in the launcher.
`tools/Run_Astro_Streaming.cmd` launches the local ASTRO BOT installation with this path and
DLSS Quality at 2560x1440, preserving the selected game resolution patch.
`tools/Run_Playroom_Streaming.cmd` does the same for ASTRO's PLAYROOM at
`D:\Eboot\PPSA01325`, using its separate `PPSA01325.json` patch.

## Data path

1. APR starts bounded asynchronous host reads ahead within consecutive read runs. Large reads
   keep four 1 MiB blocks in flight while the calling guest thread publishes earlier bytes.
   Speculation stops at map, sync, event and write-address commands. Overlapping guest writes,
   errors and completion notifications remain in recorded order.
2. Read-only kernel `pread` (also used by kernel AIO) can use the same storage backend. It
   preserves descriptor position and falls back to the existing reader, including archive members.
3. With Vulkan initialized, eligible 64 KiB–8 MiB requests target mapped Vulkan storage buffers.
   DirectStorage completes and the allocation is flushed before APR publishes guest bytes.
4. The texture cache can consume that original buffer directly for a sampled, single-sample,
   tiled color texture with plain guest backing. It checks the **entire** current guest range,
   rejects modified/unmapped data, and runs the existing Vulkan GPU tiler into the native image
   (or its linear scratch fallback). It retains the source through GPU completion.
5. Other textures keep their normal upload paths: CPU-decoded data, GPU-owned/cache-buffer data,
   unmatched ranges, incompatible layouts and exhausted streaming budgets.

The DirectStorage destination used here is **memory**, backed where possible by a mapped Vulkan
buffer. This is not D3D12 GPU decompression or a claim that original PS5 archives use GDeflate.
Guest compression/decoding remains with the game. The old standalone shared-D3D12-texture API
is not used for PS5 tiled texture data.

## Ownership and limits

Each ticket owns its destination, source file, status entry and completion event. Discarding it
waits for completion before freeing storage. Waiting uses the event after the status entry;
checking the status alone would release the event before the queue signals it. One failed
request cannot provide success for another ticket.
Completion releases the source file handle even when the immutable GPU payload stays cached;
the Vulkan regression test checks that the original file can then be renamed and written.

There are at most 64 retained tickets and 128 MiB of source payload per active queue session,
and 32 published recent ranges. Cache retirement cannot destroy a source retained by a recorded
GPU operation. Shutdown must follow renderer/scheduler draining, as in WindowContext teardown;
it drains storage tickets and releases mapped allocations before the Vulkan allocator is destroyed.
Externally retained completed tickets keep a host copy after shutdown.

The full-byte texture comparison runs under the texture cache's lock using backing reads. It
must not call `CompareGpuCleanBacking`, whose coherence query reacquires that same cache lock.
An initial ASTRO BOT transition exposed this recursion; that implementation was replaced.

DirectStorage also enables the existing device-budget collector by default. It retires clean,
unused sampled images by frame age, trims idle native image allocations under pressure and
defers destruction until GPU completion. Bound, GPU-dirty and render-target images remain
protected. Explicit `KYTY_VRAM_GC_BUDGET=0` or `1` overrides this default. Faster storage cannot
remove stalls caused by an active texture set exceeding available video memory.

## Diagnostics and validation

`[DirectStorage] Streaming completed` counts successful I/O completions, not texture uploads.
`[DirectStorage] Vulkan texture source` counts accepted native texture sources. Shutdown prints
both figures plus mapped/published ranges and modified-data fallbacks. These distinguish a loaded
runtime from actual asset I/O and actual reuse by the renderer.

```powershell
.\build\directstorage_tests.exe
.\build\directstorage_tests.exe --directstorage-vulkan
ctest --test-dir build -R '^(kernel_file_system|kernel_file_system_directstorage|gpu_tiler|gpu_tiler_directstorage|texture_cache_alias_bytes|texture_cache_texel_sync_gpu_dirty|directstorage|vram_budget|image_pressure_retirement)$' --output-on-failure
```

The storage test covers multiple outstanding unaligned reads, EOF, missing/oversized reads,
ticket limits, abandoned requests and shutdown/reinitialization. The optional Vulkan test checks
file -> mapped source -> GPU image -> readback against 65,536 reference bytes, and checks guest
source subranges, a changed middle byte, unmapping and retained-ticket lifetime.

ASTRO BOT and ASTRO's PLAYROOM have both been launched with the streaming backend.
Reduced transition stalls require gameplay validation; storage
completion counts alone do not prove smooth frame times or eliminate shader compilation stalls.

The corrected ASTRO BOT run reached over 37,500 presented frames, completed over 31,000 reads
and 15 GB without reported storage errors, and logged a native Vulkan texture source of
5,636,096 bytes. The previous recursive cache-lock failure was not observed in that run.
The user has not yet repeated the failing transition, so transition stability is still unverified.

The final selected regression set passed all 16 tests, including filesystem/DirectStorage,
texture cache, GPU tiler, camera/RR input contracts and VRAM retirement. Two tests initially
could not reserve guest memory while ASTRO BOT was running; both passed when repeated after
the game exited. The harness now conditionally enables optional rasterization capabilities
and prefers the discrete adapter, matching the game on this machine. Earlier harness failures
and an AMD integrated-driver tiler crash are not used as passing evidence.

The GPU tiler check covers 338 cases across 244 format/mode pairs. Its DirectStorage variant
additionally verifies 16 file-backed mapped-buffer detile cases against the CPU reference.
The standalone Vulkan image test also passed with 65,536 bytes exactly matching readback.

The installation is now `D:\Eboot\PPSA21567`. A run before the budget-policy change completed
48,128 reads (24,326,317,187 bytes), reported zero storage errors and accepted 256 native
texture sources (410,845,184 bytes). Its video-memory fallback warnings motivated enabling
the existing budget collector. The subsequent budget-policy run completed at least 26,624
reads (13,871,259,900 bytes) without storage errors and retired 1,262 images / 3,996.8 MiB
cumulatively under pressure. This is reclaimed allocation traffic, not a measurement of
simultaneously saved VRAM or proof of smooth teleport frame times. The exact user-reported
transition still requires gameplay verification.

## ASTRO's PLAYROOM check (2026-10-16)

The extracted `D:\Eboot\PPSA01325` executable matches the existing 01.905.000 patch SHA-256
(`8572cc581114dd71c75058554f5c26932d37e194f0b99d460ec661ffe15f0a41`). All 346 nonzero
byte guards across its 23 mods match the original SELF. No game executable was changed.
The selected profile is QHD DLSS Quality (1708x960); the experimental elapsed-time gameplay
override remains disabled, preserving the prior physics-crash mitigation.

`build/directstorage-playroom.log` confirms asynchronous streaming, frame-aged budget collection,
8,192 successful reads / 4,012,697,239 bytes with zero storage errors, and an accepted native
Vulkan texture source of 577,536 bytes. At frames 1,801 and 2,101 DLSS evaluated with a
1708x960 input and 2560x1440 output. Earlier loading frames did not have temporal guides and
did not evaluate DLSS. This is a startup/scene integration check, not certification of a
particular transition or a loading-time improvement. The new launch script passes the game
and patch paths explicitly and leaves graphics options overridable through additional arguments.

Microsoft's API guidance: https://github.com/microsoft/DirectStorage/blob/main/Docs/DeveloperGuidance.md
