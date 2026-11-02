// KYTY_GUEST_PROBE (common/guestProbe.h) on a small generated function called by many threads.
// 1. --retire (default): a probe with few hits on a function that 8 threads call in a loop. The probe
//    retires (restores the original byte) while other threads may already have executed its int3;
//    their late breakpoint exceptions must run the restored instruction, not end the process
//    (bugc8-17 and lblc-3/4/10/13 on 2026-10-16 exited silently right after a probe's last hit).
// 2. --shared: a normal and a deferred probe at the same address. The deferred one must record the
//    function's own byte, not the normal probe's int3: after both retire the function still works.
// 3. --equals: a probe with an equality condition (?rcx+0=<value>) counts only the calls whose
//    argument points at that value, and retires after its hits on matching calls only.
// Windows only; elsewhere the probes are compiled out and the test passes.

#include "common/guestProbe.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GuestProbeTests: failed: %s\n", text);
		std::fflush(stderr);
		std::abort();
	}
}

#if defined(_WIN32)

using Fn = int (*)(int);

// int f(int x) { return x + 1; } with the probed instruction first (lea eax, [rcx + 1]; ret),
// padded with int3 so that only the probe's byte is ever patched.
Fn MakeFunction(uint8_t** code_out) {
	auto* code = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	Check(code != nullptr, "VirtualAlloc");
	std::memset(code, 0xcc, 0x1000);
	const uint8_t body[] = {0x8d, 0x41, 0x01, 0xc3};
	std::memcpy(code, body, sizeof(body));
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(body));
	*code_out = code;
	return reinterpret_cast<Fn>(code);
}

void Hammer(Fn fn, int threads, int calls) {
	std::atomic<int>         wrong {0};
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; t++) {
		pool.emplace_back([&, t] {
			for (int i = 0; i < calls; i++) {
				if (fn(i + t) != i + t + 1) {
					wrong++;
				}
			}
		});
	}
	for (auto& th: pool) {
		th.join();
	}
	Check(wrong.load() == 0, "the probed function returned a wrong value");
}

void TestRetire() {
	uint8_t* code = nullptr;
	Fn       fn   = MakeFunction(&code);
	(void)_putenv_s("KYTY_GUEST_PROBE", "0@3,rcx+0:8");
	Common::GuestProbe::Arm(reinterpret_cast<uint64_t>(code), 0x1000);
	Check(code[0] == 0xcc, "the probe is armed");
	for (int round = 0; round < 20; round++) {
		Hammer(fn, 8, 20000);
	}
	Check(code[0] == 0x8d, "the retired probe restored the original byte");
}

void TestShared() {
	uint8_t* code = nullptr;
	Fn       fn   = MakeFunction(&code);
	(void)_putenv_s("KYTY_GUEST_PROBE", "0@2;^0@2");
	Common::GuestProbe::Arm(reinterpret_cast<uint64_t>(code), 0x1000);
	Hammer(fn, 1, 10);
	Check(code[0] == 0x8d, "the normal probe retired");
	Common::GuestProbe::ArmDeferred("test");
	Check(code[0] == 0xcc, "the deferred probe is armed");
	Hammer(fn, 1, 10);
	Check(code[0] == 0x8d, "the deferred probe restored the function's byte, not an int3");
	Hammer(fn, 4, 20000);
}

void TestEquals() {
	auto* code = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	Check(code != nullptr, "VirtualAlloc");
	std::memset(code, 0xcc, 0x1000);
	// int g(const uint64_t* p) { return int(*p) + 1; }: mov eax, [rcx]; inc eax; ret
	const uint8_t body[] = {0x8b, 0x01, 0xff, 0xc0, 0xc3};
	std::memcpy(code, body, sizeof(body));
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(body));
	using G = int (*)(const uint64_t*);
	auto g  = reinterpret_cast<G>(code);
	(void)_putenv_s("KYTY_GUEST_PROBE", "0@2,?rcx+0=1234");
	Common::GuestProbe::Arm(reinterpret_cast<uint64_t>(code), 0x1000);
	const uint64_t other = 5;
	const uint64_t match = 0x1234;
	for (int i = 0; i < 100; i++) {
		Check(g(&other) == 6, "a non-matching call returns its value");
	}
	Check(code[0] == 0xcc, "non-matching calls do not count as hits");
	Check(g(&match) == 0x1235, "the first matching call");
	Check(code[0] == 0xcc, "one matching hit left");
	Check(g(&match) == 0x1235, "the second matching call");
	Check(code[0] == 0x8b, "the probe retired after its matching hits");
}

#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
	const char* mode = argc > 1 ? argv[1] : "--retire";
	if (std::strcmp(mode, "--shared") == 0) {
		TestShared();
	} else if (std::strcmp(mode, "--equals") == 0) {
		TestEquals();
	} else {
		TestRetire();
	}
	std::printf("GuestProbeTests: %s passed\n", mode);
#else
	(void)argc;
	(void)argv;
	std::printf("GuestProbeTests: not on this platform\n");
#endif
	return 0;
}
