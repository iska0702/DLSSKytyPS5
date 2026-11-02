#ifndef EMULATOR_SRC_COMMON_GUESTLOAD_H_
#define EMULATOR_SRC_COMMON_GUESTLOAD_H_

#include <atomic>
#include <cstdint>

// The guest load instruction that faulted on this thread, for fault handlers that may complete it
// themselves instead of letting it run again (KYTY_READ_FAULT_PUBLISHED, BufferCache::ReadMemory).
// The loader's access-fault handler publishes the exception's native context for the duration of
// the fault and registers the decoder (Loader::X64InstructionEmulator), so the graphics code needs
// no loader dependency. Only plain register loads with one memory source are decoded (MOV, MOVZX,
// MOVSX, MOVSXD, MOVSS/MOVSD, 128-bit MOVUPS/MOVAPS/MOVDQU/MOVDQA/MOVUPD/MOVAPD/LDDQU, MOVD/MOVQ,
// and their VEX.128 forms); anything else is left to run again.
namespace GuestLoad {

struct Load {
	uint64_t address = 0; // the bytes the instruction reads: [address, address + size)
	uint32_t size    = 0;
	// Decoder state for Apply.
	uint8_t  length     = 0;
	uint8_t  kind       = 0;
	uint8_t  dest       = 0;
	uint8_t  dest_bytes = 0;
	bool     vex        = false;
};
// Decodes the faulting instruction of `native_context`; false unless it is a supported load whose
// memory source contains `fault_vaddr`.
using DecodeFn = bool (*)(void* native_context, uint64_t fault_vaddr, Load& load);
// Completes it with `bytes` (load.size bytes) as the loaded memory and advances the instruction
// pointer past it.
using ApplyFn = bool (*)(void* native_context, const Load& load, const void* bytes);

inline std::atomic<DecodeFn> g_decode {nullptr};
inline std::atomic<ApplyFn>  g_apply {nullptr};
inline thread_local void*    t_native_context = nullptr;

inline void Register(DecodeFn decode, ApplyFn apply) noexcept {
	g_apply.store(apply, std::memory_order_release);
	g_decode.store(decode, std::memory_order_release);
}

// Native context of the access fault being resolved on this thread (nullptr outside one).
class FaultScope {
public:
	explicit FaultScope(void* native_context) noexcept: m_previous(t_native_context) {
		t_native_context = native_context;
	}
	~FaultScope() { t_native_context = m_previous; }
	FaultScope(const FaultScope&)            = delete;
	FaultScope& operator=(const FaultScope&) = delete;

private:
	void* m_previous;
};

[[nodiscard]] inline bool Decode(uint64_t fault_vaddr, Load& load) noexcept {
	const auto decode = g_decode.load(std::memory_order_acquire);
	return decode != nullptr && t_native_context != nullptr &&
	       decode(t_native_context, fault_vaddr, load);
}
[[nodiscard]] inline bool Apply(const Load& load, const void* bytes) noexcept {
	const auto apply = g_apply.load(std::memory_order_acquire);
	return apply != nullptr && t_native_context != nullptr && apply(t_native_context, load, bytes);
}

} // namespace GuestLoad

#endif // EMULATOR_SRC_COMMON_GUESTLOAD_H_
