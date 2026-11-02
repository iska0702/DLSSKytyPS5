#ifndef KYTY_COMMON_SPIN_GATE_H_
#define KYTY_COMMON_SPIN_GATE_H_

// KYTY_SPIN_GATE=1 (default off): helper threads that spin for new work before they block (the
// command recorder's idle wait for work, the CP sequencer's wait for its resolver) spin only while
// the PC keeps CPUs idle. A spin spares the producer a wake-up system call per hand-off, but on a
// PC with few cores it takes CPU time the game's own threads need (chenxiao07/KytyPS5 de192903c:
// held to 4 cores, 43-50 fps with 1% lows of 3-5 fps spinning, 57-63 fps without). The gate
// measures the system's idle CPUs (GetSystemTimes, spinning included) at most every 250 ms: it
// closes below an average of three idle CPUs and opens again from five. Closed, those waits block
// at once. Windows only (always open elsewhere). Adapted from chenxiao07/KytyPS5 de192903c.

#include <cstdint>

namespace Common::SpinGate {

[[nodiscard]] bool Enabled();
// Whether a helper thread may spin for new work now (always true with the switch off).
[[nodiscard]] bool SpinAllowed();
// Times the gate closed since start-up.
[[nodiscard]] uint64_t Shuts();

} // namespace Common::SpinGate

#endif /* KYTY_COMMON_SPIN_GATE_H_ */
