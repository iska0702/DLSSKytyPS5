#ifndef KYTY_COMMON_GUEST_PROBE_H_
#define KYTY_COMMON_GUEST_PROBE_H_

#include <cstdint>

// KYTY_GUEST_PROBE (diagnostic, off by default; Windows only): software breakpoints in the
// main guest program that log registers and memory on their first hits.
//
//   KYTY_GUEST_PROBE=<va>[@<hits>][,<mem>]...[;<va>...]   or   KYTY_GUEST_PROBE=@<file>
//     <file>  one probe per line; lines starting with '#' are comments
//     <va>    hex address relative to the main program's base (an ELF virtual address)
//     <hits>  decimal number of matched hits before the breakpoint is removed (default 8);
//             each matched hit is logged, and every 1000th pass logs a count line
//     '^<va>' a deferred probe: armed only when ArmDeferred() is called (KYTY_AGC_CB_WATERMARK
//             calls it when a command buffer passes 75 % in one frame)
//     <mem>   <reg>[+-]<hexoff>[*<hexoff2>][:<hexlen>]: dump <len> bytes (default 0x40) at
//             reg+off, or with *off2 at (*(u64*)(reg+off))+off2
//             prefix '!': a condition; the hit counts and logs only if those bytes are not all 0
//             prefix '?' with '=<hexvalue>' (e.g. ?rsi+0=908d31d40): a condition; the 8 bytes there
//             must equal the value
//             prefix '=': an experiment; stores the 8-byte value <hexlen> at the address
//             prefix '&': at the first matched hit, a hardware write watch (ArmWriteWatch) on the
//             8 bytes at that address, on every thread including this one
//
// Each logged hit is one "GuestProbe:" line with the general registers, the return address at
// [rsp] (meaningful at a function entry) and the dumps. The instruction under the breakpoint is
// re-executed with a single step; a thread that passes while another thread is stepping may miss
// the probe.
namespace Common::GuestProbe {

// Records the main program image and arms the probes in it. Call once, after relocation and
// game patches, before the program runs.
void Arm(uint64_t base, uint64_t size);

// Arms the deferred ('^') probes that have not run yet. Any thread; reason goes to the log.
void ArmDeferred(const char* reason);

// Diagnostic: a hardware write watch (DR0, 8 bytes) on every thread that exists now; each hit
// logs one "GuestWatch:" line (pc relative to the main program, registers, the new value), at
// most 64 lines. Used by KYTY_SRT_WATCH_SHADER (SrtWalker.cpp).
void ArmWriteWatch(uint64_t address);

} // namespace Common::GuestProbe

#endif
