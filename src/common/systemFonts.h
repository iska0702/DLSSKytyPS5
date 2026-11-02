#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Common {

// Host fonts that cover CJK (Simplified/Traditional Chinese, Japanese, Korean) glyphs.
// KYTY_CJK_FONT, when set, is tried first. Only existing regular files are returned,
// ordered by preference. TrueType collections (.ttc) are included; use face index 0.
std::vector<std::filesystem::path> FindCjkFontFiles();

// Maps a file read-only for the rest of the process (it is never unmapped). Nothing is read up
// front: pages are faulted in only when touched, so mapping a ~20 MB font collection costs an
// open and a map instead of a full read. Returns an empty span on failure.
std::span<const uint8_t> MapFileReadOnly(const std::filesystem::path& path);

} // namespace Common
