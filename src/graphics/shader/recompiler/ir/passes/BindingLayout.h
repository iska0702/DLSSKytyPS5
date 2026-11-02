#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

void AllocateBindings(Program& program, uint32_t push_data_start_dword = 0,
                      bool lds_storage = false);

struct SharedMemoryResources {
	bool lds = false;
	bool gds = false;
};

// Whether the shader accesses LDS (DS operations) and GDS. The Buffers group binds every tracked
// buffer but the UniformBuffers ones (memory_offset_count == info.buffers.size() still).
SharedMemoryResources CollectSharedMemory(const Program& program);

// KYTY_UNIFORM_BUFFERS (CodegenOptions::uniform_buffers, EmK530/KytyPS5 407de5105): the buffers
// bound as uniform buffers (DescriptorBindingKind::UniformBuffers) rather than storage buffers.
// A buffer qualifies when every access to it is a ReadConstBuffer at an immediate offset inside
// UniformBufferBytes minus the base slack, and it is never written, formatted, aliased by an
// image or part of an indirect table. Needs a device that returns zero for out-of-range uniform
// dwords (HostBufferRobustness::uniform_dword_loads_return_zero). Ascending resource order.
[[nodiscard]] std::vector<uint32_t> UniformBufferResources(const Program& program);

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind);

// Pixel shaders that sample images record, per GET_LOD_STATS counter, the finest mip level
// sampled and a sample count (KYTY_LOD_STATS_MODE=gpu, the default).
[[nodiscard]] bool UsesMipStats(const Program& program);

// Programs whose BVH node tests are counted (BvhIntersectRay with KYTY_RT_NODE_BUDGET or
// KYTY_RT_NODE_STATS). The count reports through GDS, so such a program binds GDS.
[[nodiscard]] bool UsesBvhNodeCount(const Program& program);
[[nodiscard]] bool UsesRayGuide(const Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_ */
