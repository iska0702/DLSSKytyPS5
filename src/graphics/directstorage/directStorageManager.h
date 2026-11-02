// MIT License
// Copyright (c) 2022 BeastLe9enD (direct_storage_vk_example)
// Integrated into KytyPS5 for DirectStorage + Vulkan interoperability.

#pragma once

#include "common/common.h"
#include "graphics/directstorage/directStorageConfig.h"

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
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
	uint64_t async_requests = 0;
	uint64_t async_bytes_requested = 0;
	uint64_t async_budget_fallbacks = 0;
	uint64_t async_completed = 0;
	uint64_t async_completed_bytes = 0;
	uint64_t async_errors = 0;
	uint64_t texture_streams = 0;
	uint64_t texture_stream_bytes = 0;
	uint64_t texture_changed_fallbacks = 0;
	uint64_t mapped_reads = 0;
	uint64_t published_reads = 0;
	uint64_t texture_lookups = 0;
};

struct DirectStorageAsyncCounters {
	std::atomic<uint64_t> completed {0}, bytes {0}, errors {0};
};

// Owns the destination until I/O completes, including when abandoned after an APR error.
// No DirectStorage worker writes into guest pages or a reused texture staging reservation.
class DirectStorageRead {
public:
	~DirectStorageRead();
	// Wait finalizes status/flushes. Bytes() becomes available after a successful Wait().
	bool Wait();
	[[nodiscard]] bool IsComplete() const;
	[[nodiscard]] std::span<const uint8_t> Bytes() const;
	[[nodiscard]] vk::Buffer NativeBuffer() const;
private:
	friend class DirectStorageManager;
	struct State;
	explicit DirectStorageRead(std::unique_ptr<State> state);
	std::unique_ptr<State> m_state;
	void ReleaseNativeStorage(); // device drained; retain host bytes for external ticket owners
};

struct DirectStorageTextureSource {
	std::shared_ptr<DirectStorageRead> owner;
	vk::Buffer buffer {};
	uint64_t offset = 0;
	uint64_t size = 0;
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
	[[nodiscard]] DirectStorageStats GetStats() const;

	/// Loads binary file content asynchronously/directly into host memory using DirectStorage memory queue.
	bool LoadFileToHostMemory(const std::filesystem::path& path, uint64_t file_offset,
	                          void* destination, uint64_t size, uint64_t* bytes_read);

	// Starts I/O immediately; independent tickets overlap with guest publication/processing.
	// <=32 MiB per ticket, <=128 MiB and 64 retained tickets per active queue session.
	// Null means fallback. Completed externally owned tickets can outlive queue shutdown.
	std::shared_ptr<DirectStorageRead> EnqueueFileRead(const std::filesystem::path& path,
	                                                uint64_t file_offset, uint64_t size);
	// Link a completed file range to the guest bytes actually published by APR. Texture use
	// verifies the full current guest range (not an edge hash) before consuming this buffer.
	void PublishRead(const std::shared_ptr<DirectStorageRead>& read, uint64_t guest_address);
	DirectStorageTextureSource FindTextureSource(uint64_t guest_address, uint64_t size);

	/// Creates a shared 2D texture backed by D3D12 and imported into Vulkan via VK_KHR_external_memory_win32,
	/// and enqueues a DirectStorage file copy into the D3D12 resource with GPU fence synchronization.
	bool LoadFileToSharedTexture2D(const std::filesystem::path& path, uint32_t width, uint32_t height,
	                              vk::Format format, DirectStorageSharedTexture& out_texture);

	void DestroySharedTexture(DirectStorageSharedTexture& texture);

private:
	bool m_initialized = false;
	std::atomic<bool> m_available {false};
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

	mutable Common::Mutex m_mutex;
	std::vector<std::weak_ptr<DirectStorageRead>> m_reads;
	std::shared_ptr<DirectStorageAsyncCounters> m_async_counters;
	struct PublishedRead {
		uint64_t address;
		uint64_t size;
		std::shared_ptr<DirectStorageRead> read;
	};
	std::vector<PublishedRead> m_published_reads;
};

} // namespace Libs::Graphics::DirectStorageIntegration
