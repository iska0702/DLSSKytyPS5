# DirectStorage and Astro crash investigation (2026-10-12)

## User crash

The independent user run (PID 8076) terminated after 103 seconds / 3174 flips.
`_HangTrace/watchdog-1791824528746-pid8076/watchdog.txt` records PC
`eboot.bin + 0x19097f0`. The unencrypted SELF contains `CD 41` at that address:
an intentional `int 0x41` following the game's assertion path.
The nearby string references identify `PhysicsRigidBody.cpp`, line 95,
assertion `!pos.IsNan()`. This establishes a NaN physics position, but does
not establish which earlier operation generated it. There is no evidence
in this report attributing it to DLSS, MFG, or DirectStorage.

The crashing release had an enabled experimental elapsed-time gameplay patch.
The user reports an immediate crash while descending a windy slope. The
injected clock admits frame steps up to 0.25 seconds, unlike the original
game clock. The patch is now disabled by default in both source and release
`_Patches/PPSA01325.json` to restore original gameplay timing. The previous
JSON is preserved as `PPSA01325.json.before-physics-timing-fix`.
This is a stability mitigation; its involvement in the NaN remains unproven.
DLSS/MFG do not require that gameplay clock override. No game assertion or
original eboot was modified. The old watchdog did not preserve the original floating-point
registers. Unhandled faults now additionally save GPRs, XMM registers,
code bytes and the original call chain to `_HangTrace/guest-fault-*.txt`,
including when launched through the launcher with no persistent console.

## File I/O corrections

- APR reads use a 4 MiB CPU staging buffer followed by an emulator-thread
  copy into guest memory. DirectStorage worker threads must not write into
  guest pages that GPU tracking may protect.
- Requests beyond the configured 32 MiB staging capacity fall back instead
  of narrowing a 64-bit byte count to 32 bits.
- Completion waits are checked; a failed wait closes the queue before a
  destination can be reused. Shutdown closes queues before releasing them.
- Error records are queried only after the queue error event is signaled.
- EOF, short reads and standard-path fallback retain the APR semantics.
- Successful request/byte counters are visible even with general logging
  set to Silent. The launcher describes the implemented file I/O feature.

Hardware tests passed: 16 MiB byte-for-byte integrity, EOF bounds, empty
EOF read, missing-file fallback, initialization and shutdown. One measured
run took 10.80 ms through DirectStorage versus 7.19 ms through ReadFile;
this synchronous implementation does not demonstrate a loading speedup.

Actual Astro runtime with `--directstorage` confirmed 5120 completed file
requests / 3,431,564,480 bytes with zero recorded errors. A preceding
DLSS/MFG + DirectStorage run exceeded 8400 presented source frames without
reproducing the user's assertion. This is not a movement/gameplay stress
test or proof that the NaN crash is fixed.

## GPU path limitation

`LoadFileToSharedTexture2D` has no renderer callers. GPU texture streaming
and GPU decompression are not integrated with PS5 texture decoding and
upload. Enabling DirectStorage currently changes APR host file reads only.

Release scripts save stdout/stderr to `_TempData/Astro-last-run.log` and
`_TempData/Astro-DirectStorage-last-run.log`; a nonzero exit keeps the
console open. The second script explicitly enables DirectStorage.
