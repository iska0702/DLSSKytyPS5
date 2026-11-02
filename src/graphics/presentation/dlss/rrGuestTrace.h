#pragma once

#include <cstdint>
#include <string_view>

namespace Common { struct SlotId; }
namespace Libs::Graphics { struct PreparedBindings; struct ImageViewInfo; }

namespace Libs::Graphics::ShaderRecompiler::IR {
struct CompiledShaderInfo;
struct ResourceSnapshot;
}

namespace Libs::Graphics::RRGuestTrace {
struct NativeView {
	uint32_t index = UINT32_MAX, generation = 0, aspect = 0;
	uint32_t base_level = 0, level_count = 0, base_layer = 0, layer_count = 0;
};
NativeView View(Common::SlotId id, const ImageViewInfo& view);
bool SelectedFrame(uint64_t frame);
// Opt-in descriptor provenance, not an RR input certification. No guest memory readback.
// KYTY_RR_TRACE_FRAME selects the first frame; KYTY_RR_TRACE_COUNT defaults to 1 (max 8).
uint64_t Begin(uint64_t frame, std::string_view kind, uint64_t shader);
void End(uint64_t pass);
void Stage(uint64_t pass, std::string_view stage,
           const ShaderRecompiler::IR::CompiledShaderInfo& program,
           const ShaderRecompiler::IR::ResourceSnapshot& resources,
           const PreparedBindings* bindings = nullptr);
void Target(uint64_t pass, uint32_t slot, uint64_t address, uint32_t width,
            uint32_t height, std::string_view format, bool depth = false,
            bool written = true, NativeView native = {});
}
