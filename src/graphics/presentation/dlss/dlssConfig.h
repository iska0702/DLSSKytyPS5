#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSCONFIG_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSCONFIG_H_

#include <cstdint>
#include <string>

namespace Libs::Graphics {

enum class DLSSMode : uint32_t {
	Off = 0,
	UltraPerformance = 1,
	Performance = 2,
	Balanced = 3,
	Quality = 4,
	DLAA = 5,
};

enum class DLSSPreset : uint32_t {
	Default = 0,
	PresetJ = 1,
	PresetK = 2,
	PresetL = 3,
	PresetM = 4,
};

enum class DLSSStatus : uint32_t {
	Disabled = 0,
	Initializing,
	Available,
	Active,
	Unsupported,
	Error,
};

struct DLSSSettings {
	bool       enabled     = false;
	DLSSMode   mode        = DLSSMode::Quality;
	float      sharpness   = 0.0f;
	DLSSPreset preset      = DLSSPreset::Default;
	bool       diagnostics = false;
};

[[nodiscard]] constexpr const char* DLSSModeToString(DLSSMode mode) noexcept {
	switch (mode) {
		case DLSSMode::UltraPerformance: return "Ultra Performance";
		case DLSSMode::Performance: return "Performance";
		case DLSSMode::Balanced: return "Balanced";
		case DLSSMode::Quality: return "Quality";
		case DLSSMode::DLAA: return "DLAA";
		case DLSSMode::Off:
		default: return "Off";
	}
}

[[nodiscard]] constexpr const char* DLSSPresetToString(DLSSPreset preset) noexcept {
	switch (preset) {
		case DLSSPreset::PresetJ: return "Preset J (Ghosting Reduction)";
		case DLSSPreset::PresetK: return "Preset K (Max Image Quality)";
		case DLSSPreset::PresetL: return "Preset L (Ultra Performance Tuned)";
		case DLSSPreset::PresetM: return "Preset M (Performance Tuned)";
		case DLSSPreset::Default:
		default: return "Default";
	}
}

[[nodiscard]] constexpr const char* DLSSStatusToString(DLSSStatus status) noexcept {
	switch (status) {
		case DLSSStatus::Disabled: return "DLSS Disabled";
		case DLSSStatus::Initializing: return "DLSS Initializing";
		case DLSSStatus::Available: return "DLSS Available";
		case DLSSStatus::Active: return "DLSS Active";
		case DLSSStatus::Unsupported: return "DLSS Unsupported";
		case DLSSStatus::Error: return "DLSS Initialization Failed";
		default: return "Unknown";
	}
}

[[nodiscard]] constexpr float DLSSGetScaleRatio(DLSSMode mode) noexcept {
	switch (mode) {
		case DLSSMode::DLAA: return 1.0f;            // 1.0x native render
		case DLSSMode::Quality: return 1.5f;         // 66.7% per dimension
		case DLSSMode::Balanced: return 1.7f;        // 58.8% per dimension
		case DLSSMode::Performance: return 2.0f;     // 50.0% per dimension
		case DLSSMode::UltraPerformance: return 3.0f;// 33.3% per dimension
		case DLSSMode::Off:
		default: return 1.0f;
	}
}

inline void DLSSCalculateRenderResolution(uint32_t target_width, uint32_t target_height,
                                          DLSSMode mode, uint32_t& out_render_width,
                                          uint32_t& out_render_height) noexcept {
	const float scale = DLSSGetScaleRatio(mode);
	if (scale <= 1.0f || mode == DLSSMode::DLAA || mode == DLSSMode::Off) {
		out_render_width  = target_width;
		out_render_height = target_height;
	} else {
		out_render_width  = static_cast<uint32_t>(static_cast<float>(target_width) / scale);
		out_render_height = static_cast<uint32_t>(static_cast<float>(target_height) / scale);
		// Round to even numbers for optimal hardware tiling
		out_render_width  = (out_render_width + 1) & ~1u;
		out_render_height = (out_render_height + 1) & ~1u;
	}
}

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSCONFIG_H_
