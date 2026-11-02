#include "graphics/presentation/dlss/rrGuestTrace.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

namespace Libs::Graphics::RRGuestTrace {
namespace {
uint64_t EnvironmentNumber(const char* name, uint64_t fallback) {
	const char* value = std::getenv(name);
	if (value == nullptr) return fallback;
	const std::string_view text(value);
	uint64_t result = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
	return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size()
	           ? result : fallback;
}
}

NativeView View(Common::SlotId id, const ImageViewInfo& view) {
	return {id.index, id.generation, static_cast<uint32_t>(view.aspect),
	        view.base_level, view.level_count, view.base_layer, view.layer_count};
}

static std::string NativeFields(NativeView view) {
	if (view.index == UINT32_MAX || view.generation == 0) return "native=unknown";
	return fmt::format("native={}:{} aspect={} mip={}:{} layer={}:{}", view.index,
	    view.generation, view.aspect, view.base_level, view.level_count, view.base_layer, view.layer_count);
}

bool SelectedFrame(uint64_t frame) {
	if (!Config::DLSSDiagnosticsEnabled()) return false;
	static const auto first = EnvironmentNumber("KYTY_RR_TRACE_FRAME", UINT64_MAX);
	static const auto count = std::clamp<uint64_t>(EnvironmentNumber("KYTY_RR_TRACE_COUNT", 1), 1, 8);
	return frame >= first && frame - first < count;
}

uint64_t Begin(uint64_t frame, std::string_view kind, uint64_t shader) {
	if (!SelectedFrame(frame)) return 0;
	static std::atomic<uint64_t> sequence {0};
	const auto pass = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	if (pass > 65536) return 0;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[RRTrace][Pass] id={} frame={} kind={} shader={:016X}\n", pass, frame, kind, shader));
	return pass;
}

void Stage(uint64_t pass, std::string_view stage,
           const ShaderRecompiler::IR::CompiledShaderInfo& program,
           const ShaderRecompiler::IR::ResourceSnapshot& resources,
           const PreparedBindings* bindings) {
	if (pass == 0) return;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[RRTrace][Stage] pass={} stage={} shader={:016X} bvh={} bda_writes={}\n",
	    pass, stage, program.shader_hash, program.info.uses_bvh, program.info.bda_writes));
	for (size_t i = 0; i < std::min(resources.images.size(), program.info.images.size()); ++i) {
		const auto r = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
		const auto& info = program.info.images[i];
		std::string words;
		for (uint32_t j = 0; j < std::min(resources.images[i].dword_count, 8u); ++j)
			words += fmt::format("{:08X},", resources.images[i].dwords[j]);
		const auto native = bindings != nullptr && i < bindings->images.size()
		    ? View(bindings->images[i].image_id, bindings->images[i].desc.view_info) : NativeView {};
		Log::WriteToConsoleAndLog(fmt::format(
		    "[RRTrace][Image] pass={} stage={} index={} source={} address=0x{:X} extent={}x{} "
		    "format={} read={} written={} atomic={} words={} {}\n", pass, stage, i, info.source,
		    r.Base40(), r.Width5() + 1, r.Height5() + 1, static_cast<uint32_t>(r.Format()),
		    info.read, info.written, info.atomic, words, NativeFields(native)));
	}
	for (size_t i = 0; i < std::min(resources.buffers.size(), program.info.buffers.size()); ++i) {
		const auto r = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto& info = program.info.buffers[i];
		Log::WriteToConsoleAndLog(fmt::format(
		    "[RRTrace][Buffer] pass={} stage={} index={} source={} address=0x{:X} stride={} "
		    "records={} read={} written={}\n", pass, stage, i, info.source, r.Base48(),
		    r.Stride(), r.NumRecords(), info.read, info.written));
	}
}

void End(uint64_t pass) {
	if (pass != 0)
		Log::WriteToConsoleAndLog(fmt::format("[RRTrace][End] pass={}\n", pass));
}

void Target(uint64_t pass, uint32_t slot, uint64_t address, uint32_t width,
            uint32_t height, std::string_view format, bool depth, bool written, NativeView native) {
	if (pass == 0) return;
	Log::WriteToConsoleAndLog(fmt::format(
	    "[RRTrace][Target] pass={} slot={} address=0x{:X} extent={}x{} format={} depth={} written={} {}\n",
	    pass, slot, address, width, height, format, depth, written, NativeFields(native)));
}
}
