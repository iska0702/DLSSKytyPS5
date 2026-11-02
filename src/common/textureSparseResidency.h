#ifndef EMULATOR_SRC_COMMON_TEXTURESPARSERESIDENCY_H_
#define EMULATOR_SRC_COMMON_TEXTURESPARSERESIDENCY_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// KYTY_TEXTURE_SPARSE_RESIDENCY=0|1|auto: partially resident textures as sparse residency images
// (GraphicContext::sparse_residency_image_enabled). It only saves VRAM (the fps is unchanged), and
// two RTX 4090 device losses had a sparse growth bind in flight, so `auto` (the u59-preset.json
// value) turns it on only for cards below the 16 GB class. Resolved once, at device creation.
namespace Common::TextureSparseResidency {

enum class Mode : uint8_t { Off, On, Auto };

// The VRAM figure is the largest DEVICE_LOCAL heap: fixed per card and always available. The driver
// budget (VK_EXT_memory_budget) is not used: it moves with what other programs hold (browser,
// overlays, the desktop), so the same card could flip between runs. Drivers report a little less
// than the nominal size (a 16 GB card shows about 15.6-16.0 GiB), hence the 15 GiB cut for the
// 16 GB class; 12 GB cards report about 11.7 GiB.
inline constexpr uint64_t AutoOffMinHeapBytes = 15ull << 30u;

// "1"/"on": On; "auto": Auto; anything else, or unset: Off (the code default, as before).
[[nodiscard]] inline Mode Parse(const char* value) {
	if (value == nullptr) {
		return Mode::Off;
	}
	if (std::strcmp(value, "1") == 0 || std::strcmp(value, "on") == 0) {
		return Mode::On;
	}
	if (std::strcmp(value, "auto") == 0) {
		return Mode::Auto;
	}
	return Mode::Off;
}

// Whether sparse residency is requested (device support is checked separately).
[[nodiscard]] inline bool Requested(Mode mode, uint64_t vram_heap_bytes) {
	return mode == Mode::On || (mode == Mode::Auto && vram_heap_bytes < AutoOffMinHeapBytes);
}

// The startup line: "Texture sparse residency (KYTY_TEXTURE_SPARSE_RESIDENCY=auto): off, 23.7 GiB
// VRAM >= 16 GiB class". `available`: the device supports it (only matters when requested).
[[nodiscard]] inline std::string Describe(const char* value, Mode mode, uint64_t vram_heap_bytes,
                                          bool available) {
	const bool requested = Requested(mode, vram_heap_bytes);
	const char* state    = !requested ? "off" : available ? "on" : "unavailable (device support)";
	char        text[256];
	const double gib = static_cast<double>(vram_heap_bytes) / static_cast<double>(1ull << 30u);
	if (mode == Mode::Auto) {
		std::snprintf(text, sizeof(text),
		              "Texture sparse residency (KYTY_TEXTURE_SPARSE_RESIDENCY=auto): %s, %.1f GiB VRAM "
		              "%s 16 GiB class\n",
		              state, gib, requested ? "<" : ">=");
	} else {
		std::snprintf(text, sizeof(text),
		              "Texture sparse residency (KYTY_TEXTURE_SPARSE_RESIDENCY=%s): %s, %.1f GiB VRAM\n",
		              value == nullptr ? "unset" : value, state, gib);
	}
	return text;
}

[[nodiscard]] inline const char* ResolvedName(Mode mode, bool enabled) {
	if (mode == Mode::Auto) {
		return enabled ? "auto:on" : "auto:off";
	}
	return enabled ? "on" : "off";
}

} // namespace Common::TextureSparseResidency

#endif // EMULATOR_SRC_COMMON_TEXTURESPARSERESIDENCY_H_
