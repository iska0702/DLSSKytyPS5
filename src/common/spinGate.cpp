#include "common/spinGate.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // IWYU pragma: keep
#endif

namespace Common::SpinGate {

static std::atomic<bool>     g_open {true};
static std::atomic<int64_t>  g_next_ms {0};
static std::atomic<uint64_t> g_shuts {0};

bool Enabled() {
	static const bool on = [] {
		const char* value = std::getenv("KYTY_SPIN_GATE");
		const bool  set   = value != nullptr && value[0] == '1';
		if (set) {
			std::printf("SpinGate: helper threads spin for work only while 3+ CPUs idle (KYTY_SPIN_GATE)\n");
		}
		return set;
	}();
	return on;
}

#if defined(_WIN32)
// The refreshing thread only (the g_next_ms exchange orders consecutive refreshes).
static uint64_t g_last_idle  = 0;
static uint64_t g_last_total = 0;

static uint64_t FileTimeValue(const FILETIME& time) {
	return (static_cast<uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
}

static void Refresh() {
	FILETIME idle {};
	FILETIME kernel {};
	FILETIME user {};
	if (GetSystemTimes(&idle, &kernel, &user) == 0) {
		return;
	}
	const uint64_t idle_value  = FileTimeValue(idle);
	const uint64_t total_value = FileTimeValue(kernel) + FileTimeValue(user); // kernel includes idle
	const uint64_t idle_delta  = idle_value - g_last_idle;
	const uint64_t total_delta = total_value - g_last_total;
	const bool     first       = g_last_total == 0;
	g_last_idle                = idle_value;
	g_last_total               = total_value;
	if (first || total_delta == 0) {
		return;
	}
	static const uint32_t cpus = [] {
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		return info.dwNumberOfProcessors == 0 ? 1u
		                                      : static_cast<uint32_t>(info.dwNumberOfProcessors);
	}();
	// Average idle CPUs over the interval: the idle share of all CPUs' time times their count.
	const double idle_cpus =
	    static_cast<double>(idle_delta) * cpus / static_cast<double>(total_delta);
	const bool open = g_open.load(std::memory_order_relaxed);
	if (open && idle_cpus < 3.0) {
		g_open.store(false, std::memory_order_relaxed);
		g_shuts.fetch_add(1, std::memory_order_relaxed);
	} else if (!open && idle_cpus >= 5.0) {
		g_open.store(true, std::memory_order_relaxed);
	}
}
#endif

bool SpinAllowed() {
	if (!Enabled()) {
		return true;
	}
#if defined(_WIN32)
	const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
	                        std::chrono::steady_clock::now().time_since_epoch())
	                        .count();
	int64_t next = g_next_ms.load(std::memory_order_acquire);
	if (now >= next &&
	    g_next_ms.compare_exchange_strong(next, now + 250, std::memory_order_acq_rel)) {
		Refresh();
	}
#endif
	return g_open.load(std::memory_order_relaxed);
}

uint64_t Shuts() {
	return g_shuts.load(std::memory_order_relaxed);
}

} // namespace Common::SpinGate
