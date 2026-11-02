#pragma once

#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

struct EmbeddedFetchLoad {
	uint32_t              pc         = 0;
	int                   attrib_id  = -1;
	uint32_t              components = 0;
};

struct EmbeddedFetchPlan {
	std::vector<EmbeddedFetchLoad> loads;
	int32_t                        vertex_offset_sgpr   = -1;
	int32_t                        instance_offset_sgpr = -1;
};

struct TranslateOptions {
	ShaderType                    stage               = ShaderType::Unknown;
	uint32_t                      wave_size           = 64;
	uint64_t                      shader_hash         = 0;
	uint32_t                      user_data_base      = 0;
	uint32_t                      user_data_count     = 64;
	ShaderStageInputInfo          input_info;
	const EmbeddedFetchPlan*      embedded_fetch = nullptr;
};

IR::Program TranslateProgram(const Decoder::Program& decoded, const CFG::Graph& cfg,
                             const TranslateOptions& options);

// KYTY_LANE_LOCAL (LaneLocal.cpp, chenxiao07/KytyPS5 9907780e1 + 9bb47e15d): a wave64 program whose
// results in each lane depend on no lane outside its 32-lane half. On a host with 32-wide subgroups it
// runs one guest lane per invocation instead of two. reason: why not. uniform_loads: proven only with
// KYTY_LANE_LOCAL_UNIFORM_LOADS=1 (buffer loads taken as uniform; not used here: such a program keeps
// two lanes per invocation).
bool IsWaveLaneLocal(const Decoder::Program& program, std::string* reason = nullptr,
                     bool* uniform_loads = nullptr);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
