// MIT License
// Copyright (c) 2022 BeastLe9enD (direct_storage_vk_example)
// Integrated into KytyPS5 for DirectStorage + Vulkan interoperability.

#pragma once

#include "common/common.h"
#include "graphics/directstorage/directStorageConfig.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectStorage/dstorage.h>
#endif

#include "graphics/host_gpu/graphicContext.h"
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics::DirectStorageIntegration {

struct DirectStorageSharedTexture {
	vk::Image        image       = nullptr;
	vk::DeviceMemory memory      = nullptr;
	vk::ImageView    image_view  = nullptr;
	uint32_t         width       = 0;
	uint32_t         height      = 0;
	vk::Format       format      = vk::Format::eUndefined;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ID3D12Resource*  d3d_resource = nullptr;
#endif
};

struct DirectStorageStats {
	uint64_t total_requests = 0;
	uint64_t file_to_gpu_requests = 0;
	uint64_t file_to_memory_requests = 0;
	uint64_t total_bytes_loaded = 0;
	uint64_t fallback_count = 0;
	uint64_t error_count = 0;
	double   last_read_duration_ms = 0.0;
	double   total_read_time_ms = 0.0;
};

class DirectStorageManager {
public:
	static DirectStorageManager& Instance();

	DirectStorageManager();
	~DirectStorageManager();

	KYTY_CLASS_NO_COPY(DirectStorageManager);

	/// Initializes D3D12 device matching Vulkan physical device LUID and creates DirectStorage factory and queues.
	/// If graphics is nullptr, initializes in standalone mode for file/memory operations.
	bool Initialize(GraphicContext* graphics = nullptr);

	/// Shuts down DirectStorage, releases queues, factories, and D3D12 devices.
	void Shutdown();

	[[nodiscard]] bool IsAvailable() const { return m_available; }
	[[nodiscard]] bool IsEnabled() const;
	[[nodiscard]] const std::string& GetStatusMessage() const { return m_status_message; }
	[[nodiscard]] const DirectStorageStats& GetStats() const { return m_stats; }

	/// Loads binary file content asynchronously/directly into host memory using DirectStorage memory queue.
	bool LoadFileToHostMemory(const std::filesystem::path& path, uint64_t file_offset,
	                          void* destination, uint64_t size, uint64_t* bytes_read);

	/// Creates a shared 2D texture backed by D3D12 and imported into Vulkan via VK_KHR_external_memory_win32,
	/// and enqueues a DirectStorage file copy into the D3D12 resource with GPU fence synchronization.
	bool LoadFileToSharedTexture2D(const std::filesystem::path& path, uint32_t width, uint32_t height,
	                              vk::Format format, DirectStorageSharedTexture& out_texture);

	void DestroySharedTexture(DirectStorageSharedTexture& texture);

private:
	bool m_initialized = false;
	bool m_available = false;
	std::string m_status_message = "DirectStorage not initialized";
	DirectStorageStats m_stats;

	GraphicContext* m_graphics = nullptr;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	ID3D12Device*       m_d3d12_device = nullptr;
	IDStorageFactory*   m_dstorage_factory = nullptr;
	IDStorageQueue1*    m_gpu_queue = nullptr;
	IDStorageQueue1*    m_memory_queue = nullptr;
	ID3D12Fence*        m_sync_fence = nullptr;
	HANDLE              m_fence_event = nullptr;
	uint64_t            m_fence_value = 0;
#endif

	Common::Mutex      m_mutex;
};

} // namespace Libs::Graphics::DirectStorageIntegration
