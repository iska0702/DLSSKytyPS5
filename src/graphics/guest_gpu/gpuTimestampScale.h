#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_GPUTIMESTAMPSCALE_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_GPUTIMESTAMPSCALE_H_

#include <cstdint>
#include <cstdlib>

// KYTY_GPU_TIMESTAMP_SCALE=<percent> (live; unset, 0 or 100: off; accepted 25-200; after
// BryanKAdams/BryKytyPS5 b44c4ed "--gpu-timestamp-scale").
//
// Guest GPU timestamps (EOP/RELEASE_MEM clock writes, COPY_DATA of the reference clock) hold the
// reference clock when the command processor executes the packet, which includes the emulator's
// own stalls. Above 100 the time since the last guest flip is stretched by that percentage, so a
// game that sizes its dynamic resolution from GPU timestamps measures more GPU time and leaves
// headroom (lower internal resolution). Below 100 (a diagnostic) the time is compressed: the game
// measures less, as if the command processor were that much faster (a test of KYTY_EOP_TIMESTAMPS
// =gpu as a guard against dynamic-resolution increases that follow CP time instead of GPU time). Each flip returns to the real clock: a value is never more
// than one frame's stretch ahead of the CPU clock. Values written after a flip can be smaller than
// values written just before it (the stretch restarts), as in the original.
// KYTY_EOP_TIMESTAMPS=gpu rewrites clock labels with GPU query times later; those rewrites are
// not stretched.
namespace Libs::Graphics::GpuTimestampScale {

inline constexpr int64_t Off        = 100;
inline constexpr int64_t MinPercent = 25;
inline constexpr int64_t MaxPercent = 200;

// Live::Switch parser: the percentage, Off for unset, empty, "0" or anything outside 25-200.
[[nodiscard]] inline int64_t ParsePercent(const char* value) noexcept {
	if (value == nullptr || value[0] == '\0') {
		return Off;
	}
	char*      end    = nullptr;
	const auto parsed = std::strtoll(value, &end, 10);
	if (end == value || *end != '\0' || parsed < MinPercent || parsed > MaxPercent) {
		return Off;
	}
	return parsed;
}

// The stretched clock: `anchor` is the reference clock at the last guest flip (0: none yet).
[[nodiscard]] constexpr uint64_t Stretch(uint64_t now, uint64_t anchor, int64_t percent) noexcept {
	if (percent == Off || percent < MinPercent || percent > MaxPercent || anchor == 0 ||
	    now <= anchor) {
		return now;
	}
	const uint64_t elapsed = now - anchor;
	if (percent < Off) {
		return now - elapsed * static_cast<uint64_t>(Off - percent) / 100u;
	}
	return now + elapsed * static_cast<uint64_t>(percent - Off) / 100u;
}

} // namespace Libs::Graphics::GpuTimestampScale

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_GPUTIMESTAMPSCALE_H_ */
