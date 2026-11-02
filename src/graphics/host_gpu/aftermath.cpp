#include "graphics/host_gpu/aftermath.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32) && defined(KYTY_HAVE_AFTERMATH)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GFSDK_Aftermath_GpuCrashDump.h>
#endif

namespace Libs::Graphics::Aftermath {

bool Requested() {
	static const bool requested = [] {
		const auto* value = std::getenv("KYTY_AFTERMATH");
		return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
	}();
	return requested;
}

#if defined(_WIN32) && defined(KYTY_HAVE_AFTERMATH)

namespace {

std::atomic<bool>                        g_active {false};
std::atomic<uint32_t>                    g_dumps {0};
PFN_GFSDK_Aftermath_GetCrashDumpStatus   g_status = nullptr;
std::mutex                               g_write_mutex;

std::wstring DumpFolder() {
	std::wstring path(MAX_PATH, L'\0');
	const auto   length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	const auto slash = path.find_last_of(L"\\/");
	path             = (slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash)) +
	       L"\\_AftermathDumps";
	CreateDirectoryW(path.c_str(), nullptr);
	return path;
}

void WriteFile(const wchar_t* prefix, const wchar_t* extension, const void* data, uint32_t size) {
	std::lock_guard lock(g_write_mutex);
	SYSTEMTIME      time {};
	GetLocalTime(&time);
	wchar_t name[128];
	std::swprintf(name, std::size(name), L"\\%ls-%04u%02u%02u-%02u%02u%02u-%lu-%u%ls", prefix, time.wYear,
	              time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, GetCurrentProcessId(),
	              g_dumps.fetch_add(1), extension);
	const auto path = DumpFolder() + name;
	if (auto* file = _wfopen(path.c_str(), L"wb"); file != nullptr) {
		std::fwrite(data, 1, size, file);
		std::fclose(file);
		std::fprintf(stderr, "Nsight Aftermath: wrote %ls (%u bytes)\n", path.c_str(), size);
		std::printf("Nsight Aftermath: wrote %ls (%u bytes)\n", path.c_str(), size);
		std::fflush(stdout);
	}
}

void GFSDK_AFTERMATH_CALL OnCrashDump(const void* dump, const uint32_t size, void* /*user*/) {
	WriteFile(L"kyty", L".nv-gpudmp", dump, size);
}

void GFSDK_AFTERMATH_CALL OnShaderDebugInfo(const void* info, const uint32_t size, void* /*user*/) {
	WriteFile(L"shader", L".nvdbg", info, size);
}

void GFSDK_AFTERMATH_CALL OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription add, void* /*user*/) {
	add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "Kyty");
}

} // namespace

bool Initialize() {
	if (!Requested() || g_active.load()) {
		return g_active.load();
	}
	std::wstring path(MAX_PATH, L'\0');
	const auto   length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	const auto slash = path.find_last_of(L"\\/");
	path = (slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash)) +
	       L"\\GFSDK_Aftermath_Lib.x64.dll";
	HMODULE library = LoadLibraryW(path.c_str());
	if (library == nullptr) {
		std::printf("Nsight Aftermath (KYTY_AFTERMATH): GFSDK_Aftermath_Lib.x64.dll not found next to the "
		            "emulator; GPU crash dumps are off (copy it from the Nsight Aftermath SDK, lib\\x64)\n");
		return false;
	}
	const auto enable = reinterpret_cast<PFN_GFSDK_Aftermath_EnableGpuCrashDumps>(
	    GetProcAddress(library, "GFSDK_Aftermath_EnableGpuCrashDumps"));
	g_status = reinterpret_cast<PFN_GFSDK_Aftermath_GetCrashDumpStatus>(
	    GetProcAddress(library, "GFSDK_Aftermath_GetCrashDumpStatus"));
	if (enable == nullptr || g_status == nullptr) {
		std::printf("Nsight Aftermath (KYTY_AFTERMATH): the library lacks the crash dump API; off\n");
		return false;
	}
	const auto result = enable(GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
	                           GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks, OnCrashDump,
	                           OnShaderDebugInfo, OnDescription, nullptr, nullptr);
	if (!GFSDK_Aftermath_SUCCEED(result)) {
		std::printf("Nsight Aftermath (KYTY_AFTERMATH): enabling GPU crash dumps failed (0x%x); off\n",
		            static_cast<unsigned>(result));
		return false;
	}
	g_active.store(true);
	std::printf("Nsight Aftermath (KYTY_AFTERMATH): GPU crash dumps on, written to _AftermathDumps\n");
	return true;
}

bool Active() {
	return g_active.load();
}

void WaitForDump() {
	if (!g_active.load() || g_status == nullptr) {
		return;
	}
	static std::atomic_flag waited = ATOMIC_FLAG_INIT;
	if (waited.test_and_set()) {
		std::this_thread::sleep_for(std::chrono::seconds(3)); // the first caller waits for the dump
		return;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	auto       status   = GFSDK_Aftermath_CrashDump_Status_Unknown;
	while (std::chrono::steady_clock::now() < deadline) {
		if (!GFSDK_Aftermath_SUCCEED(g_status(&status)) || status == GFSDK_Aftermath_CrashDump_Status_Finished ||
		    status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::printf("Nsight Aftermath: crash dump status %d\n", static_cast<int>(status));
	std::fflush(stdout);
}

#else

bool Initialize() {
	if (Requested()) {
		std::printf("Nsight Aftermath (KYTY_AFTERMATH): this build has no Aftermath support "
		            "(configured without KYTY_AFTERMATH_SDK)\n");
	}
	return false;
}

bool Active() {
	return false;
}

void WaitForDump() {}

#endif

} // namespace Libs::Graphics::Aftermath
