#pragma once

#include "common/file.h"

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

inline bool ComputeExecutableSHA256(const std::filesystem::path& path, std::string* out_sha256) {
	if (out_sha256 == nullptr) return false;
	out_sha256->clear();

	AVSHA* sha = av_sha_alloc();
	if (sha == nullptr) return false;
	if (av_sha_init(sha, 256) != 0) {
		av_free(sha);
		return false;
	}

	std::array<uint8_t, 65536> buffer {};

	Common::File file(path, Common::File::Mode::Read);
	if (!file.IsInvalid()) {
		uint32_t bytes_read = 0;
		while (!file.IsEOF()) {
			file.Read(buffer.data(), static_cast<uint32_t>(buffer.size()), &bytes_read);
			if (bytes_read == 0) break;
			av_sha_update(sha, buffer.data(), bytes_read);
		}
	} else {
		std::ifstream fallback(path, std::ios::binary);
		if (!fallback) {
			av_free(sha);
			return false;
		}
		while (fallback.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || fallback.gcount() > 0) {
			av_sha_update(sha, buffer.data(), static_cast<size_t>(fallback.gcount()));
		}
		if (!fallback.eof() || fallback.bad()) {
			av_free(sha);
			return false;
		}
	}

	std::array<uint8_t, 32> digest {};
	av_sha_final(sha, digest.data());
	av_free(sha);

	constexpr char hex[] = "0123456789abcdef";
	out_sha256->resize(64);
	for (size_t i = 0; i < digest.size(); ++i) {
		(*out_sha256)[2 * i]     = hex[digest[i] >> 4];
		(*out_sha256)[2 * i + 1] = hex[digest[i] & 15];
	}
	return true;
}

inline bool PatchExecutableMatches(const std::filesystem::path& path, const std::string& expected,
                                   std::string* observed = nullptr) {
	if (!IsPatchSHA256(expected)) return false;
	std::string computed;
	if (!ComputeExecutableSHA256(path, &computed)) return false;
	if (observed != nullptr) {
		*observed = computed;
	}
	for (size_t i = 0; i < 64; ++i) {
		if (std::tolower(static_cast<unsigned char>(expected[i])) !=
		    std::tolower(static_cast<unsigned char>(computed[i]))) return false;
	}
	return true;
}

} // namespace Loader::GamePatch
