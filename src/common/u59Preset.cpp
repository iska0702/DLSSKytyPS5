#include "common/u59Preset.h"

#include "common/stringUtils.h"

#include <cstring>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <system_error>

namespace Common::U59Preset {

bool Parse(std::string_view text, std::vector<Entry>& entries, std::string& error) {
	entries.clear();
	const auto document = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
	if (document.is_discarded() || !document.is_object()) {
		error = "not a JSON object";
		return false;
	}
	for (const auto& [key, value]: document.items()) {
		if (!key.starts_with("KYTY_") && !key.starts_with("TRACY_")) {
			error = fmt::format("key \"{}\" does not start with KYTY_ or TRACY_", key);
			entries.clear();
			return false;
		}
		if (!value.is_string()) {
			error = fmt::format("the value of {} is not a string", key);
			entries.clear();
			return false;
		}
		entries.push_back({key, value.get<std::string>()});
	}
	return true;
}

LoadResult Load(const std::vector<std::filesystem::path>& dirs) {
	LoadResult result;
	for (const auto& dir: dirs) {
		result.searched.push_back(dir);
		const auto      path = dir / FileName;
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			continue;
		}
		result.path = path;
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			result.status = Status::Unreadable;
			result.error  = "the file cannot be opened";
			return result;
		}
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (file.bad()) {
			result.status = Status::Unreadable;
			result.error  = "reading the file failed";
			return result;
		}
		result.status = Parse(text, result.entries, result.error) ? Status::Loaded : Status::Invalid;
		return result;
	}
	result.status = Status::Missing;
	return result;
}

std::string Describe(const LoadResult& result) {
	static constexpr std::string_view consequence =
	    "Games start with the emulator's built-in defaults, which are slower and use more video memory "
	    "than the release settings. Put the u59-preset.json from the release archive next to the "
	    "launcher.";
	switch (result.status) {
		case Status::Loaded: return {};
		case Status::Missing: {
			std::string where;
			for (const auto& dir: result.searched) {
				where += where.empty() ? "" : "; ";
				where += PathToString(dir);
			}
			return fmt::format("{} was not found (looked in: {}). {}", FileName,
			                   where.empty() ? std::string("nowhere") : where, consequence);
		}
		case Status::Unreadable:
			return fmt::format("{} cannot be read ({}: {}). {}", FileName, PathToString(result.path),
			                   result.error, consequence);
		case Status::Invalid:
			return fmt::format("{} is not valid and was not applied ({}: {}). {}", FileName,
			                   PathToString(result.path), result.error, consequence);
	}
	return {};
}

const std::vector<std::string_view>& MarkerKeys() {
	static const std::vector<std::string_view> keys = {
	    "KYTY_CP_SEQ",
	    "KYTY_CP_RECORDER",
	    "KYTY_SUBMISSION_MODE",
	    "KYTY_PROGRAM_CACHE",
	    "KYTY_SRT_READ_RUNS",
	    "KYTY_TEXTURE_CLEAN_PROOFS",
	    "KYTY_FUNCTION_ARRAY_SHRINK",
	    "KYTY_SHADER_PRECOMPILE",
	    "KYTY_PIPELINE_JOURNAL",
	    // Missing from the int19.4 release preset (draw reuse off): an old file is named.
	    "KYTY_DRAW_RUN_RECALL",
	    "KYTY_DRAW_RUN_TARGETS",
	    "KYTY_DRAW_RUN_PIPESWAP",
	};
	return keys;
}

std::vector<std::string> StartupWarnings(const EnvLookup& env) {
	const auto is_set = [&](const char* name) {
		const char* value = env(name);
		return value != nullptr && value[0] != '\0';
	};
	const auto is_zero = [&](const char* name) {
		const char* value = env(name);
		return value != nullptr && std::strcmp(value, "0") == 0;
	};

	std::vector<std::string> lines;
	std::string              missing;
	size_t                   missing_count = 0;
	for (const auto key: MarkerKeys()) {
		if (!is_set(std::string(key).c_str())) {
			missing += missing.empty() ? "" : ", ";
			missing += key;
			missing_count++;
		}
	}
	if (missing_count == MarkerKeys().size()) {
		lines.push_back(fmt::format(
		    "Warning: {} not applied (none of its settings are in the environment): running with the "
		    "built-in defaults, which are slower and use more video memory. Start games from the "
		    "launcher with {} in the launcher's folder, or set the file's variables yourself.\n",
		    FileName, FileName));
	} else if (missing_count != 0) {
		lines.push_back(fmt::format(
		    "Warning: {} looks old or edited: {} not set (built-in defaults used). Use the file from "
		    "this release.\n",
		    FileName, missing));
	}
	if (is_zero("KYTY_SRT_VARIANT_READS")) {
		lines.emplace_back(
		    "Warning: KYTY_SRT_VARIANT_READS=0: the ray-traced lighting and GI kernels are skipped, so "
		    "lit geometry renders black unless the game's lighting patches are on.\n");
	}
	// KYTY_RT_STUB=1 keeps the passes running (every ray misses): lit, without ray-traced shadows.
	if (is_zero("KYTY_RT_SOFTWARE") && (!is_set("KYTY_RT_STUB") || is_zero("KYTY_RT_STUB"))) {
		lines.emplace_back(
		    "Warning: KYTY_RT_SOFTWARE=0: software ray tracing is off, so lit geometry renders black "
		    "unless the game's lighting patches are on.\n");
	}
	return lines;
}


std::string_view BundledFallbackPresetJson() {
	static constexpr std::string_view preset = R"JSON({
  "KYTY_ALIAS_CPU_WRITE_DIAGNOSTICS": "0",
  "KYTY_BDA_INCREMENTAL_SYNC": "1",
  "KYTY_CAPTURE_SHADER_DETAILS": "0",
  "KYTY_CP_RECORDER": "1",
  "KYTY_CP_RECORDER_VERIFY": "0",
  "KYTY_CP_SEQ": "1",
  "KYTY_CP_SEQ_PREFETCH": "1",
  "KYTY_CP_SEQ_VERIFY": "0",
  "KYTY_DCC_GPU": "1",
  "KYTY_DCC_GPU_REFRESH": "1",
  "KYTY_DESCRIPTOR_OFFSET_AUDIT": "0",
  "KYTY_DIRTY_IMAGE_QUERY": "direct",
  "KYTY_DRAW_PREP_BINDINGS": "1",
  "KYTY_FAST_BACKING_TRANSFERS": "1",
  "KYTY_GPU_OCCLUSION": "0",
  "KYTY_GPU_OP_COUNTERS": "0",
  "KYTY_GPU_OP_PROFILE": "0",
  "KYTY_HANG_TRACE": "0",
  "KYTY_HANG_WATCHDOG": "auto",
  "KYTY_HOST_INPUT_MIN_PRESS_MS": "0",
  "KYTY_HOST_INPUT_ONLY": "0",
  "KYTY_IMAGE_CACHE_POLICY": "legacy",
  "KYTY_IMAGE_LOOKUP": "fast",
  "KYTY_LABEL_MODE": "split",
  "KYTY_LOD_STATS_MODE": "gpu",
  "KYTY_MESH_RESTART": "1",
  "KYTY_NATIVE_IMAGE_POOL": "1",
  "KYTY_PROFILE_AGGREGATES": "0",
  "KYTY_PROFILE_DETAILS": "0",
  "KYTY_PROFILE_FRAMES_ONLY": "0",
  "KYTY_PROFILE_LOADING": "0",
  "KYTY_PROGRAM_CACHE": "1",
  "KYTY_RENDERER_BATCH": "1",
  "KYTY_RESOURCE_DEPENDENCY_CACHE": "0",
  "KYTY_RESOURCE_MATERIALIZATION": "optimized",
  "KYTY_RESOURCE_REUSE": "0",
  "KYTY_SHADER_METADATA_BACKING": "0",
  "KYTY_SHADER_WRITE_RETICK": "1",
  "KYTY_SRT_ARITHMETIC_TAPES": "1",
  "KYTY_SRT_COMPILED_RECIPES": "1",
  "KYTY_SRT_DECODED_ROOTS": "0",
  "KYTY_SRT_READ_RUNS": "1",
  "KYTY_SRT_SHARED_CLEAN_VALUES": "1",
  "KYTY_SUBMISSION_COALESCE": "1",
  "KYTY_SUBMISSION_MODE": "queued",
  "KYTY_TEXTURE_CLEAN_PROOFS": "1",
  "KYTY_VERTEX_FAILURE_DUMP": "",
  "TRACY_NO_SYS_TRACE": "1",
  "KYTY_SRT_VARIANT_READS": "1",
  "KYTY_CPU_RESERVE": "cp",
  "KYTY_DRAW_PREP_COLD_TOKEN": "1",
  "KYTY_CP_COMMIT": "all",
  "KYTY_RECORDER_DESCRIPTOR_SETS": "1",
  "KYTY_PUSH_SHADOW_FRESH_SKIP": "1",
  "KYTY_META_CLEAR_MEMO": "1",
  "KYTY_PENDING_REFRESH_US": "200",
  "KYTY_TEXTURE_SPARSE_RESIDENCY": "auto",
  "KYTY_NATIVE_IMAGE_POOL_IDLE_MS": "5000",
  "KYTY_BDA_PAGETABLE_SPARSE": "1",
  "KYTY_TILER_SCRATCH_POOL_IDLE_MS": "5000",
  "KYTY_VRAM_IDLE_FRAMES": "600",
  "KYTY_DRAW_RUN": "1",
  "KYTY_DRAW_RUN_ACQUIRE": "1",
  "KYTY_DRAW_RUN_PUSH": "1",
  "KYTY_DRAW_RUN_WRITES": "1",
  "KYTY_DRAW_PREP_CLEAN_SINCE": "1",
  "KYTY_DRAW_RUN_IMAGE_EPOCH": "1",
  "KYTY_DRAW_RUN_CONT_ELIGIBLE": "1",
  "KYTY_DRAW_RUN_RECALL": "1",
  "KYTY_DRAW_RUN_TARGETS": "1",
  "KYTY_DRAW_RUN_PIPESWAP": "1",
  "KYTY_CP_OWNED_COUNTERS": "1",
  "KYTY_FUNCTION_ARRAY_SHRINK": "1",
  "KYTY_VRAM_GC_BUDGET": "1",
  "KYTY_OCCLUSION_BATCH": "1",
  "KYTY_OCCLUSION_SLOTS": "16384",
  "KYTY_OCCLUSION_RESET_BATCH": "1",
  "KYTY_PIPELINE_PREFETCH": "1",
  "KYTY_PIPELINE_PREFETCH_PROGRAMS": "1",
  "KYTY_RAM_SMALL_UPLOAD_RING": "1",
  "KYTY_SHADER_PRECOMPILE": "1",
  "KYTY_PIPELINE_FAST_FIRST": "1",
  "KYTY_RT_SOFTWARE": "auto",
  "KYTY_BINDING_LEAN_MISS": "1",
  "KYTY_BINDING_MEMO_TOUCH": "1",
  "KYTY_PIPELINE_JOURNAL": "1",
  "KYTY_PIPELINE_JOURNAL_RETARGET": "0",
  "KYTY_PIPELINE_KNOWN": "1",
  "KYTY_PIPELINE_PREWARM_COMPUTE": "1",
  "KYTY_CB_SHADER_MASK_EXPORTS": "1",
  "KYTY_DEFER_PROTECTS": "all",
  "KYTY_EOP_TIMESTAMPS": "gpu",
  "KYTY_CP_WAIT_JOURNAL": "1",
  "KYTY_PIPELINE_SKIP": "1",
  "KYTY_READBACK_FLUSH_SIDE": "1",
  "KYTY_STREAM_COPY_DEFER": "1",
  "KYTY_UNIFORM_BUFFERS": "1",
  "KYTY_NATIVE_FMINMAX": "1",
  "KYTY_EOP_FLUSH_BATCH": "64",
  "KYTY_UPLOAD_DMA_WRITE_DRAIN": "1",
  "KYTY_GUEST_TEB_STACK_RANGE": "1",
  "KYTY_CLEAN_UNTOUCHED": "1",
  "KYTY_DEVICE_FAULT_EXT": "1",
  "KYTY_REBIND_AFTER_JOIN": "1",
  "KYTY_STENCIL_TARGETS_APART": "1",
  "KYTY_PAD_STATE_COALESCE": "1"
})JSON";
	return preset;
}

} // namespace Common::U59Preset
