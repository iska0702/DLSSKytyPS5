#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_READFAULTDIAG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_READFAULTDIAG_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Read-fault diagnostic (KYTY_READ_FAULT_DIAG=0x<guest page address>, default off).
//
// For guest-thread read faults on that one 4 KiB tracker page, BufferCache::ReadMemory prints
// one "ReadFaultDiag" line per fault (the first KYTY_READ_FAULT_DIAG_LINES, default 4000) with:
//   - the path taken (eager wait, side copy issued, duplicate wait, drain) and the tick waited for;
//   - where the wait went: the GPU-thread round trip (SendCommandSync), waiting until that tick was
//     handed to the driver, GPU execution until it completed, the side copy, and the publication
//     (backing write, protection update, locks). In this mode the waits poll instead of blocking;
//   - the newest GPU write recorded into the page before the fault (record time, tick, caller);
//   - end-of-pipe label / WRITE_DATA writes the command processor made after that GPU write was
//     recorded and before the fault (record time in KYTY_LABEL_MODE=record): how many, and the
//     first and the last one (address, value, age). On hardware such a label is written only
//     after the GPU write, so a guest that waited for it would read the new contents;
//   - whether the faulting byte is one the GPU wrote (a dirty range of the readback).
// Publications of the page print "ReadFaultDiag publish" lines with the published ranges and
// their first bytes (the first 16 publications).
namespace Libs::Graphics::ReadFaultDiag {

// KYTY_READ_FAULT_DIAG=auto: the page of the first guest read fault that takes longer than 2 ms
// becomes the diagnosed page (AutoChoose).
// KYTY_READ_FAULT_DIAG=0x<address>:0x<size> diagnoses a range of pages instead of one.
[[nodiscard]] inline uint64_t& RangeSize() noexcept {
	static uint64_t size = 4096;
	return size;
}
inline std::atomic<uint64_t> g_page {UINT64_MAX};
inline std::atomic<bool>     g_auto {false};
[[nodiscard]] inline uint64_t Page() noexcept {
	auto page = g_page.load(std::memory_order_acquire);
	if (page == UINT64_MAX) [[unlikely]] {
		const auto* value = std::getenv("KYTY_READ_FAULT_DIAG");
		page              = 0;
		if (value != nullptr && std::strcmp(value, "auto") == 0) {
			g_auto.store(true, std::memory_order_release);
		} else if (value != nullptr) {
			char* end = nullptr;
			page      = std::strtoull(value, &end, 0) & ~uint64_t {0xfff};
			if (end != nullptr && *end == ':') {
				RangeSize() = std::max<uint64_t>(4096, std::strtoull(end + 1, nullptr, 0));
			}
		}
		uint64_t expected = UINT64_MAX;
		g_page.compare_exchange_strong(expected, page, std::memory_order_acq_rel);
		page = g_page.load(std::memory_order_acquire);
	}
	return page;
}
[[nodiscard]] inline bool Enabled() noexcept {
	return Page() != 0 || g_auto.load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool AutoPending() noexcept {
	return Page() == 0 && g_auto.load(std::memory_order_relaxed);
}
inline void AutoChoose(uint64_t vaddr) noexcept {
	uint64_t expected = 0;
	if (g_page.compare_exchange_strong(expected, vaddr & ~uint64_t {0xfff},
	                                   std::memory_order_acq_rel)) {
		std::printf("ReadFaultDiag: diagnosing page 0x%llx\n",
		            static_cast<unsigned long long>(vaddr & ~uint64_t {0xfff}));
	}
}
[[nodiscard]] inline bool Covers(uint64_t vaddr, uint64_t size) noexcept {
	const auto page = Page();
	return page != 0 && vaddr < page + RangeSize() && page < vaddr + size;
}
[[nodiscard]] inline uint64_t NowNs() noexcept {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

struct Label {
	uint64_t    ns      = 0;
	uint64_t    address = 0;
	uint64_t    value   = 0;
	const char* kind    = "";
};
struct State {
	std::mutex                 mutex;
	std::array<Label, 1024>    labels {};
	uint64_t                   label_count = 0;
	std::atomic<uint64_t>      write_ns {0};
	std::atomic<uint64_t>      write_tick {0};
	std::atomic<uint64_t>      write_caller {0};
	std::atomic<uint64_t>      writes {0};
	std::atomic<uint32_t>      lines {0};
	std::atomic<uint32_t>      publishes {0};
};
[[nodiscard]] inline State& Get() noexcept {
	static State state;
	return state;
}

// Command processor: a label or WRITE_DATA written now (record time) or a deferred label landing.
inline void NoteLabel(const char* kind, uint64_t address, uint64_t value) noexcept {
	if (!Enabled()) {
		return;
	}
	auto&           state = Get();
	std::lock_guard lock(state.mutex);
	state.labels[state.label_count % state.labels.size()] = {NowNs(), address, value, kind};
	state.label_count++;
}

// GPU thread: a GPU write into [vaddr, vaddr + size) was recorded at `tick`.
inline void NoteGpuWrite(uint64_t vaddr, uint64_t size, uint64_t tick, uint64_t caller) noexcept {
	if (!Covers(vaddr, size)) {
		return;
	}
	auto& state = Get();
	state.write_tick.store(tick, std::memory_order_relaxed);
	state.write_caller.store(caller, std::memory_order_relaxed);
	state.writes.fetch_add(1, std::memory_order_relaxed);
	state.write_ns.store(NowNs(), std::memory_order_release);
}

struct LabelWindow {
	uint64_t count       = 0;
	Label    first       = {};
	Label    last        = {};
	uint64_t last_any_ns = 0; // the newest label at all (even before the GPU write)
};
// Labels written in (after_ns, before_ns].
[[nodiscard]] inline LabelWindow LabelsBetween(uint64_t after_ns, uint64_t before_ns) noexcept {
	LabelWindow window;
	auto&       state = Get();
	std::lock_guard lock(state.mutex);
	const auto  size  = static_cast<uint64_t>(state.labels.size());
	const auto  count = state.label_count;
	for (uint64_t i = count > size ? count - size : 0; i < count; i++) {
		const auto& label = state.labels[i % size];
		if (label.ns > before_ns) {
			continue;
		}
		window.last_any_ns = std::max(window.last_any_ns, label.ns);
		if (label.ns <= after_ns) {
			continue;
		}
		if (window.count == 0 || label.ns < window.first.ns) {
			window.first = label;
		}
		if (window.count == 0 || label.ns >= window.last.ns) {
			window.last = label;
		}
		window.count++;
	}
	return window;
}

// One fault being resolved on this thread (set by ReadMemory for the diagnosed page).
struct Fault {
	uint64_t start_ns      = 0;
	uint64_t sendsync_ns   = 0; // GPU-thread round trip
	uint64_t dispatch_ns   = 0; // until the waited tick was handed to the driver
	uint64_t gpu_ns        = 0; // until the GPU completed it
	uint64_t copy_ns       = 0; // side copy after the producer
	uint64_t phases_end_ns = 0; // end of the polled phases (publication follows)
	uint64_t wait_tick     = 0;
	bool     waited        = false;
	bool     byte_dirty    = false;
	bool     byte_known    = false;
	uint64_t fault_vaddr   = 0;
};
inline thread_local Fault* t_fault = nullptr;

} // namespace Libs::Graphics::ReadFaultDiag

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_READFAULTDIAG_H_
