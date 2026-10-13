#pragma once

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace Loader::GamePatch {

inline bool IsPatchSHA256(const std::string& hash) {
	if (hash.size() != 64) return false;
	for (const unsigned char c: hash) {
		if (!std::isxdigit(c)) return false;
	}
	return true;
}

inline bool PatchExecutableMatches(const std::filesystem::path& path, const std::string& expected) {
	if (!IsPatchSHA256(expected)) return false;
	std::ifstream file(path, std::ios::binary);
	if (!file) return false;
	AVSHA* sha = av_sha_alloc();
	if (sha == nullptr) return false;
	if (av_sha_init(sha, 256) != 0) {
		av_free(sha);
		return false;
	}
	std::array<uint8_t, 65536> buffer {};
	while (file.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || file.gcount() > 0) {
		av_sha_update(sha, buffer.data(), static_cast<size_t>(file.gcount()));
	}
	const bool read_ok = file.eof() && !file.bad();
	std::array<uint8_t, 32> digest {};
	av_sha_final(sha, digest.data());
	av_free(sha);
	if (!read_ok) return false;
	constexpr char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < digest.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(expected[2 * i])) != hex[digest[i] >> 4] ||
		    std::tolower(static_cast<unsigned char>(expected[2 * i + 1])) != hex[digest[i] & 15]) return false;
	}
	return true;
}

} // namespace Loader::GamePatch
