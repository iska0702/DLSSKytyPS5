#pragma once
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::Frontend {
// Exact shaders from PPSA01325 01.905.000. PCs precede the first destructive
// use of the final closest-ray parameter (v10); v12 is -1 for a miss.
constexpr uint32_t PlayroomReflectionCapturePC(uint64_t hash) {
	switch (hash) {
		case 0x5DF28D2058CAF728ull: return 0x1A18;
		case 0x55D28021295DC816ull: return 0x1D5C;
		case 0xC18F4AD08601D141ull: return 0x1FD4;
		case 0xD65C9DF1AD5E3AF5ull: return 0x22A0;
		default: return UINT32_MAX;
	}
}
}
