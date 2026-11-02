// KYTY_EOP_TIMESTAMPS unit tests (eopTimestampClock.h): the conversion of device timestamps to
// the guest reference clock, and the life cycle of the timestamp query slots. Also the
// KYTY_GPU_TIMESTAMP_SCALE stretch (gpuTimestampScale.h).
#include "graphics/host_gpu/renderer/eopTimestampClock.h"
#include "graphics/guest_gpu/gpuTimestampScale.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "FAILED: %s\n", text);
		g_failures++;
	}
}

uint64_t Distance(uint64_t a, uint64_t b) {
	return a > b ? a - b : b - a;
}

void TestDeviceDelta() {
	Check(EopTimestamps::DeviceDelta(150, 100, 64) == 50, "64-bit forward delta");
	Check(EopTimestamps::DeviceDelta(100, 150, 64) == -50, "64-bit backward delta");
	const uint64_t top = (uint64_t {1} << 36u) - 100u;
	Check(EopTimestamps::DeviceDelta(50, top, 36) == 150, "36-bit counter wraps forward");
	Check(EopTimestamps::DeviceDelta(top, 50, 36) == -150, "36-bit counter wraps backward");
	Check(EopTimestamps::DeviceDelta(0x123456789ull, 0x123456789ull, 36) == 0, "equal values");
}

// A 3 GHz TSC, a 10 MHz QPC (Windows) and a 1 GHz device clock, all at 500 s.
EopTimestamps::ClockMap SampleMap() {
	EopTimestamps::ClockMap map;
	map.device_raw    = 7'000'000'000ull;
	map.device_qpc    = 5'000'000'000ull; // 500 s at 10 MHz
	map.qpc_frequency = 10'000'000ull;
	map.period_ns     = 1.0;
	map.valid_bits    = 64;
	map.pair_qpc      = 5'000'000'000ull;
	map.pair_tsc      = 1'500'000'000'000ull; // 500 s at 3 GHz
	map.tsc_frequency = 3'000'000'000ull;
	return map;
}

void TestToReference() {
	auto     map       = SampleMap();
	uint64_t reference = 0;
	uint64_t expected  = 0;
	Check(Sync::ScaleReferenceClock(map.pair_tsc, map.tsc_frequency, expected) &&
	          expected == 50'000'000'000ull,
	      "500 s is 5e10 reference ticks");
	Check(EopTimestamps::ToReference(map, map.device_raw, reference) && reference == expected,
	      "the calibrated device time maps to the paired TSC time");
	// 1 ms later and earlier on the device: 100,000 reference ticks (10 ns each).
	Check(EopTimestamps::ToReference(map, map.device_raw + 1'000'000u, reference) &&
	          Distance(reference, expected + 100'000u) <= 1u,
	      "a device millisecond is 100,000 reference ticks");
	Check(EopTimestamps::ToReference(map, map.device_raw - 1'000'000u, reference) &&
	          Distance(reference, expected - 100'000u) <= 1u,
	      "earlier device times map backwards");
	// The (QPC, TSC) pair read 1 ms after the device calibration: the device sample stays at
	// 500 s.
	auto late          = map;
	late.pair_qpc      = map.device_qpc + 10'000u;
	late.pair_tsc      = map.pair_tsc + 3'000'000u;
	Check(EopTimestamps::ToReference(late, late.device_raw, reference) &&
	          Distance(reference, expected) <= 1u,
	      "a later (QPC, TSC) pair is accounted for");
	// Another device tick period (a 100 MHz device clock, 10 ns ticks).
	auto slow      = map;
	slow.period_ns = 10.0;
	Check(EopTimestamps::ToReference(slow, slow.device_raw + 100'000u, reference) &&
	          Distance(reference, expected + 100'000u) <= 1u,
	      "the device period scales deltas");
	// A narrow device counter that wrapped since the calibration.
	auto narrow       = map;
	narrow.valid_bits = 36;
	narrow.device_raw = (uint64_t {1} << 36u) - 500u;
	Check(EopTimestamps::ToReference(narrow, 500u, reference) &&
	      Distance(reference, expected + 100u) <= 1u,
	      "a wrapped 36-bit device counter converts forward");
	// Invalid maps and results before the clock's start.
	auto invalid          = map;
	invalid.tsc_frequency = 0;
	Check(!EopTimestamps::ToReference(invalid, map.device_raw, reference),
	      "no TSC frequency: no conversion");
	invalid           = map;
	invalid.period_ns = 0.0;
	Check(!EopTimestamps::ToReference(invalid, map.device_raw, reference),
	      "no device period: no conversion");
	auto early     = map;
	early.pair_tsc = 1000u;
	Check(!EopTimestamps::ToReference(early, early.device_raw - 1'000'000u, reference),
	      "a time before the TSC's start is refused");
}

// ResetConsumed runs recorded by the fake command buffer.
struct Resets {
	std::vector<std::pair<uint32_t, uint32_t>> runs;
	auto operator()() {
		return [this](uint32_t first, uint32_t count) { runs.emplace_back(first, count); };
	}
};

void TestQueryRingOrder() {
	EopTimestamps::QueryRing ring(4);
	Check(ring.Allocate() == EopTimestamps::QueryRing::NoSlot, "nothing is free before a reset");
	Resets resets;
	Check(ring.ResetConsumed(resets()) == 4 && resets.runs.size() == 1 &&
	          resets.runs[0] == std::make_pair(0u, 4u),
	      "the first command buffer resets every slot in one run");
	Check(ring.Allocate() == 0 && ring.Allocate() == 1 && ring.Allocate() == 2 &&
	          ring.Allocate() == 3,
	      "slots are allocated in ring order");
	Check(ring.Allocate() == EopTimestamps::QueryRing::NoSlot, "a full ring allocates nothing");
	resets.runs.clear();
	Check(ring.ResetConsumed(resets()) == 0 && resets.runs.empty(),
	      "pending slots are never reset");
	ring.Consume(1);
	Check(ring.ResetConsumed(resets()) == 0, "a later slot waits for the earlier ones");
	ring.Consume(0);
	Check(ring.ResetConsumed(resets()) == 2 && resets.runs.size() == 1 &&
	          resets.runs[0] == std::make_pair(0u, 2u),
	      "consumed slots are reset in ring order");
	Check(ring.Allocate() == 0 && ring.Allocate() == 1, "reset slots are reused");
	ring.Consume(3);
	ring.Consume(2);
	ring.Consume(0);
	resets.runs.clear();
	// Slots 2, 3 then 0 (the wrap): two runs.
	Check(ring.ResetConsumed(resets()) == 3 && resets.runs.size() == 2 &&
	          resets.runs[0] == std::make_pair(2u, 2u) && resets.runs[1] == std::make_pair(0u, 1u),
	      "a reset run is split at the end of the ring");
	Check(ring.Available() == 3, "three slots are free again");
}

// A producer allocates slots and hands them to a consumer thread that consumes them late and out
// of order within small batches. The producer resets consumed slots at "command buffer begins".
// A slot is never reset while pending, and never allocated twice without a reset in between.
void TestQueryRingConcurrent() {
	constexpr uint32_t Capacity = 64;
	EopTimestamps::QueryRing ring(Capacity);
	std::vector<std::atomic<int>> state(Capacity); // 0 free, 1 pending, 2 consumed
	std::vector<std::atomic<uint32_t>> handoff(Capacity * 4);
	std::atomic<uint64_t> produced {0};
	std::atomic<bool>     done {false};
	std::atomic<uint32_t> errors {0};
	std::thread consumer([&] {
		std::mt19937 rng(7);
		uint64_t     consumed = 0;
		std::vector<uint32_t> batch;
		while (!done.load(std::memory_order_acquire) ||
		       consumed < produced.load(std::memory_order_acquire)) {
			const auto available = produced.load(std::memory_order_acquire);
			while (consumed < available && batch.size() < 8) {
				batch.push_back(handoff[consumed % handoff.size()].load(std::memory_order_acquire));
				consumed++;
			}
			std::shuffle(batch.begin(), batch.end(), rng);
			for (const auto slot: batch) {
				int expected = 1;
				if (!state[slot].compare_exchange_strong(expected, 2)) {
					errors.fetch_add(1);
				}
				ring.Consume(slot);
			}
			batch.clear();
			std::this_thread::yield();
		}
	});
	constexpr uint64_t Target      = 200000;
	uint64_t           allocations = 0;
	const auto         deadline    = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	// Command-buffer begins until enough slots went through the ring (a stalled recycle ends at
	// the deadline and fails the count check).
	while (allocations < Target && std::chrono::steady_clock::now() < deadline) {
		const auto before = allocations;
		(void)ring.ResetConsumed([&](uint32_t first, uint32_t count) {
			for (uint32_t i = 0; i < count; i++) {
				int expected = 2;
				// Consumed (or never used): only such slots may be reset.
				if (!state[first + i].compare_exchange_strong(expected, 0) && expected != 0) {
					errors.fetch_add(1);
				}
			}
		});
		for (uint32_t i = 0; i < 5; i++) {
			const auto slot = ring.Allocate();
			if (slot == EopTimestamps::QueryRing::NoSlot) {
				break;
			}
			int expected = 0;
			if (!state[slot].compare_exchange_strong(expected, 1)) {
				errors.fetch_add(1);
			}
			// At most Capacity slots are outstanding (allocation needs a consumed slot), so the
			// handoff ring of 4 x Capacity entries never overruns the consumer.
			handoff[produced.load(std::memory_order_relaxed) % handoff.size()].store(
			    slot, std::memory_order_release);
			produced.fetch_add(1, std::memory_order_release);
			allocations++;
		}
		if (allocations == before) {
			std::this_thread::yield(); // every slot is pending: wait for the consumer
		}
	}
	done.store(true, std::memory_order_release);
	consumer.join();
	Check(errors.load() == 0, "slots are never reset while pending nor allocated twice");
	Check(allocations >= Target, "slots are recycled many times"); // up to 4 past the target
	std::printf("  query ring: %llu allocations through %u slots\n",
	            static_cast<unsigned long long>(allocations), Capacity);
}

} // namespace

void TestGpuTimestampScale() {
	namespace S = Libs::Graphics::GpuTimestampScale;
	Check(S::ParsePercent(nullptr) == S::Off, "scale: unset is off");
	Check(S::ParsePercent("") == S::Off, "scale: empty is off");
	Check(S::ParsePercent("0") == S::Off, "scale: 0 is off");
	Check(S::ParsePercent("100") == 100, "scale: 100");
	Check(S::ParsePercent("115") == 115, "scale: 115");
	Check(S::ParsePercent("200") == 200, "scale: 200");
	Check(S::ParsePercent("99") == 99, "scale: 99 (compression)");
	Check(S::ParsePercent("25") == 25, "scale: 25");
	Check(S::ParsePercent("24") == S::Off, "scale: below range is off");
	Check(S::ParsePercent("201") == S::Off, "scale: above range is off");
	Check(S::ParsePercent("115x") == S::Off, "scale: trailing text is off");
	Check(S::ParsePercent("-150") == S::Off, "scale: negative is off");

	constexpr uint64_t anchor = 5'000'000'000ull; // 50 s on the 100 MHz reference clock
	Check(S::Stretch(anchor + 1'000'000, anchor, 100) == anchor + 1'000'000, "scale 100: identity");
	Check(S::Stretch(anchor + 1'000'000, anchor, 115) == anchor + 1'150'000, "scale 115: 10 ms -> 11.5 ms");
	Check(S::Stretch(anchor + 1'000'000, anchor, 200) == anchor + 2'000'000, "scale 200: doubled");
	Check(S::Stretch(anchor + 1'000'000, anchor, 50) == anchor + 500'000, "scale 50: 10 ms -> 5 ms");
	Check(S::Stretch(anchor + 1'000'000, anchor, 20) == anchor + 1'000'000, "scale 20: invalid, real clock");
	Check(S::Stretch(anchor + 1'000'000, 0, 150) == anchor + 1'000'000, "no flip yet: real clock");
	Check(S::Stretch(anchor, anchor, 150) == anchor, "at the flip: real clock");
	Check(S::Stretch(anchor - 10, anchor, 150) == anchor - 10, "before the anchor: real clock");
	Check(S::Stretch(anchor + 1'000'000, anchor, 300) == anchor + 1'000'000, "invalid percent: real clock");
	// A pair of timestamps inside one frame measures the stretched duration.
	const auto begin = S::Stretch(anchor + 200'000, anchor, 115);
	const auto end   = S::Stretch(anchor + 1'200'000, anchor, 115);
	Check(end - begin == 1'150'000, "scale 115: a 10 ms span inside a frame reads 11.5 ms");
	// Monotonic within a frame; never behind the real clock.
	uint64_t previous = 0;
	bool     monotonic = true, ahead = true;
	for (uint64_t t = anchor; t < anchor + 2'000'000; t += 997) {
		const auto v = S::Stretch(t, anchor, 133);
		monotonic    = monotonic && v >= previous;
		ahead        = ahead && v >= t;
		previous     = v;
	}
	Check(monotonic, "scale: monotonic between flips");
	Check(ahead, "scale: never behind the real clock");
	// No overflow for a long time without a flip (one hour).
	const uint64_t hour = 360'000'000'000ull;
	Check(S::Stretch(anchor + hour, anchor, 200) == anchor + 2 * hour, "scale: one hour without a flip");
}

int main() {
	TestDeviceDelta();
	TestToReference();
	TestQueryRingOrder();
	TestQueryRingConcurrent();
	TestGpuTimestampScale();
	if (g_failures != 0) {
		std::fprintf(stderr, "EopTimestampTests: %d failure(s)\n", g_failures);
		return 1;
	}
	std::puts("EopTimestampTests: all cases passed");
	return 0;
}
