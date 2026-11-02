#include "common/systemFonts.h"

#include <cstdlib>
#include <limits>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Common {

namespace {

void AppendIfFile(std::vector<std::filesystem::path>& out, const std::filesystem::path& path) {
	if (path.empty()) {
		return;
	}
	std::error_code ec;
	if (std::filesystem::is_regular_file(path, ec) && !ec) {
		out.push_back(path);
	}
}

} // namespace

std::vector<std::filesystem::path> FindCjkFontFiles() {
	std::vector<std::filesystem::path> result;

	if (const char* env = std::getenv("KYTY_CJK_FONT"); env != nullptr && *env != '\0') {
		AppendIfFile(result, std::filesystem::path(env));
	}

#if defined(_WIN32)
	std::filesystem::path fonts_dir = "C:\\Windows\\Fonts";
	if (const char* windir = std::getenv("WINDIR"); windir != nullptr && *windir != '\0') {
		fonts_dir = std::filesystem::path(windir) / "Fonts";
	}
	for (const char* name: {"msyh.ttc", "msyh.ttf", "Deng.ttf", "simhei.ttf", "simsun.ttc",
	                        "msjh.ttc", "malgun.ttf", "YuGothM.ttc", "meiryo.ttc"}) {
		AppendIfFile(result, fonts_dir / name);
	}
#elif defined(__APPLE__)
	for (const char* path: {"/System/Library/Fonts/PingFang.ttc",
	                        "/System/Library/Fonts/Hiragino Sans GB.ttc",
	                        "/System/Library/Fonts/STHeiti Medium.ttc",
	                        "/System/Library/Fonts/STHeiti Light.ttc",
	                        "/Library/Fonts/Arial Unicode.ttf"}) {
		AppendIfFile(result, path);
	}
#else
	for (const char* path: {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
	                        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
	                        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
	                        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
	                        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
	                        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"}) {
		AppendIfFile(result, path);
	}
#endif

	return result;
}

std::span<const uint8_t> MapFileReadOnly(const std::filesystem::path& path) {
#if defined(_WIN32)
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
	                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return {};
	}
	LARGE_INTEGER size {};
	if (GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0 ||
	    static_cast<uint64_t>(size.QuadPart) > std::numeric_limits<uint32_t>::max()) {
		CloseHandle(file);
		return {};
	}
	HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	CloseHandle(file);
	if (mapping == nullptr) {
		return {};
	}
	void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(mapping);
	if (view == nullptr) {
		return {};
	}
	return {static_cast<const uint8_t*>(view), static_cast<size_t>(size.QuadPart)};
#else
	const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return {};
	}
	struct stat st {};
	if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
	    static_cast<uint64_t>(st.st_size) > std::numeric_limits<uint32_t>::max()) {
		close(fd);
		return {};
	}
	void* view = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (view == MAP_FAILED) {
		return {};
	}
	return {static_cast<const uint8_t*>(view), static_cast<size_t>(st.st_size)};
#endif
}

} // namespace Common
