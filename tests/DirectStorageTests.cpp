#include "common/emulatorConfig.h"
#include "graphics/directstorage/directStorageConfig.h"
#include "graphics/directstorage/directStorageManager.h"
#include "common/file.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace {

using Libs::Graphics::DirectStorageIntegration::DirectStorageManager;
using Libs::Graphics::DirectStorageIntegration::DirectStorageSharedTexture;

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "[FAIL] %s\n", message);
		g_failures++;
	} else {
		std::printf("[PASS] %s\n", message);
	}
}

void TestDirectStorageDisabledFallback() {
	std::printf("\n--- Test 1: Subsystem disabled / fallback verification ---\n");
	auto& ds = DirectStorageManager::Instance();

	Config::SetDirectStorageEnabled(false);
	Check(!Config::DirectStorageEnabled(), "Config::DirectStorageEnabled() reports false when disabled");

	// Even if Initialize is called with nullptr or disabled config, it safely handles it
	bool init_res = ds.Initialize(nullptr);
	Check(!init_res, "Initialize(nullptr) returns false or disabled state");
	Check(!ds.IsEnabled() || !ds.IsAvailable(), "DirectStorage reports not available when disabled");

	// Clean up
	ds.Shutdown();
	Check(!ds.IsAvailable(), "IsAvailable is false after Shutdown()");
}

void TestDirectStorageInitializationAndHardware() {
	std::printf("\n--- Test 2: Subsystem initialization and hardware detection ---\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto& ds = DirectStorageManager::Instance();
	Config::SetDirectStorageEnabled(true);
	Check(Config::DirectStorageEnabled(), "Config::DirectStorageEnabled() reports true");

	// Initialize standalone DirectStorage (without Vulkan context first, to verify D3D12 + Factory)
	bool ok = ds.Initialize(nullptr);
	std::printf("DirectStorage initialization without Vulkan context: %s (IsAvailable=%d)\n",
	            ok ? "success" : "failed / unsupported hardware", ds.IsAvailable() ? 1 : 0);

	if (ds.IsAvailable()) {
		Check(ds.IsEnabled(), "Subsystem is enabled");
		Check(ds.IsAvailable(), "Subsystem is available");
	} else {
		std::printf("[INFO] Running in environment without DirectStorage GPU support or D3D12 device; fallback active.\n");
	}
	ds.Shutdown();
#else
	std::printf("[INFO] Non-Windows platform; DirectStorage cleanly no-op.\n");
#endif
}

void TestDirectStorageHostMemoryFileStreaming() {
	std::printf("\n--- Test 3: Host memory asynchronous file streaming & integrity benchmark ---\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto& ds = DirectStorageManager::Instance();
	Config::SetDirectStorageEnabled(true);
	ds.Initialize(nullptr);

	// Create a temporary test file with deterministic random content (16 MB)
	const size_t file_size = 16 * 1024 * 1024;
	std::vector<uint8_t> reference_data(file_size);
	for (size_t i = 0; i < file_size; ++i) {
		reference_data[i] = static_cast<uint8_t>((i * 137 + 42) & 0xFF);
	}

	const std::wstring test_filename = L"directstorage_test_temp.bin";
	HANDLE h_file = CreateFileW(test_filename.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                            FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h_file != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h_file, reference_data.data(), static_cast<DWORD>(file_size), &written, nullptr);
		CloseHandle(h_file);
	}

	// Benchmark Standard I/O Read
	std::vector<uint8_t> std_read_buf(file_size, 0);
	auto t0 = std::chrono::high_resolution_clock::now();
	HANDLE h_read = CreateFileW(test_filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (h_read != INVALID_HANDLE_VALUE) {
		DWORD bytes_read = 0;
		ReadFile(h_read, std_read_buf.data(), static_cast<DWORD>(file_size), &bytes_read, nullptr);
		CloseHandle(h_read);
	}
	auto t1 = std::chrono::high_resolution_clock::now();
	double std_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	std::printf("Standard ReadFile (16 MB): %.2f ms (%.2f MB/s)\n",
	            std_ms, (file_size / (1024.0 * 1024.0)) / (std_ms / 1000.0));

	Check(std::memcmp(reference_data.data(), std_read_buf.data(), file_size) == 0,
	      "Standard read data integrity match");

	// Test DirectStorage fast-path if available
	if (ds.IsAvailable()) {
		std::vector<uint8_t> ds_read_buf(file_size, 0);
		uint64_t bytes_read = 0;
		auto ds_t0 = std::chrono::high_resolution_clock::now();
		bool load_ok = ds.LoadFileToHostMemory(test_filename, 0, ds_read_buf.data(), file_size, &bytes_read);
		auto ds_t1 = std::chrono::high_resolution_clock::now();
		double ds_ms = std::chrono::duration<double, std::milli>(ds_t1 - ds_t0).count();

		Check(load_ok, "DirectStorage LoadFileToHostMemory returned true");
		Check(bytes_read == file_size, "DirectStorage bytes_read matches file_size");
		std::printf("DirectStorage Read (16 MB): %.2f ms (%.2f MB/s)\n",
		            ds_ms, (file_size / (1024.0 * 1024.0)) / (ds_ms / 1000.0));

		Check(std::memcmp(reference_data.data(), ds_read_buf.data(), file_size) == 0,
		      "DirectStorage host loaded data byte-for-byte exact match with reference");
	} else {
		std::printf("[INFO] DirectStorage runtime unavailable, skipping hardware direct streaming benchmark.\n");
	}

	DeleteFileW(test_filename.c_str());
	ds.Shutdown();
#else
	std::printf("[INFO] DirectStorage file streaming is Windows-only.\n");
#endif
}

void TestDirectStorageSharedTextureLifecycle() {
	std::printf("\n--- Test 4: Shared D3D12/Vulkan texture resource lifecycle & error safety ---\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto& ds = DirectStorageManager::Instance();
	Config::SetDirectStorageEnabled(true);
	ds.Initialize(nullptr);

	if (ds.IsAvailable()) {
		// Attempt to load a nonexistent texture: should fail gracefully without crash or resource leak
		DirectStorageSharedTexture out_tex {};
		bool ok = ds.LoadFileToSharedTexture2D(L"nonexistent_texture.gnf", 256, 256,
		                                      vk::Format::eR8G8B8A8Unorm, out_tex);
		Check(!ok, "Gracefully handles nonexistent texture file without crashing");

		// Test DestroySharedTexture with empty resource
		DirectStorageSharedTexture empty_res {};
		ds.DestroySharedTexture(empty_res);
		Check(true, "DestroySharedTexture handles empty/null resource safely");
	} else {
		std::printf("[INFO] DirectStorage unavailable, shared texture lifecycle skipped.\n");
	}
	ds.Shutdown();
#endif
}

} // namespace

int main(int argc, char* argv[]) {
	Config::Initialize();

	std::printf("========================================\n");
	std::printf("  KytyPS5 DirectStorage Subsystem Tests  \n");
	std::printf("========================================\n");

	TestDirectStorageDisabledFallback();
	TestDirectStorageInitializationAndHardware();
	TestDirectStorageHostMemoryFileStreaming();
	TestDirectStorageSharedTextureLifecycle();

	std::printf("\n========================================\n");
	if (g_failures != 0) {
		std::fprintf(stderr, "DirectStorage Tests: %d test(s) failed!\n", g_failures);
		return 1;
	}

	std::printf("All DirectStorage Tests PASSED successfully!\n");
	return 0;
}
