// MIT License
// Copyright (c) 2022 BeastLe9enD (direct_storage_vk_example)
// Portions Copyright (c) KytyPS5 Project
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

#include "graphics/directstorage/directStorageManager.h"
#include "graphics/directstorage/directStorageConfig.h"

#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"

#include <chrono>
#include <cstdio>
#include <system_error>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Libs::Graphics::DirectStorageIntegration {

DirectStorageManager& DirectStorageManager::Instance() {
	static DirectStorageManager s_instance;
	return s_instance;
}

DirectStorageManager::DirectStorageManager() = default;

DirectStorageManager::~DirectStorageManager() {
	Shutdown();
}

bool DirectStorageManager::IsEnabled() const {
	return Config::DirectStorageEnabled();
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static DXGI_FORMAT VulkanToDxgiFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		case vk::Format::eR8Unorm:
			return DXGI_FORMAT_R8_UNORM;
		case vk::Format::eR8G8Unorm:
			return DXGI_FORMAT_R8G8_UNORM;
		case vk::Format::eR16G16B16A16Sfloat:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case vk::Format::eR32G32B32A32Sfloat:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case vk::Format::eBc1RgbaUnormBlock:
		case vk::Format::eBc1RgbaSrgbBlock:
			return DXGI_FORMAT_BC1_UNORM;
		case vk::Format::eBc2UnormBlock:
		case vk::Format::eBc2SrgbBlock:
			return DXGI_FORMAT_BC2_UNORM;
		case vk::Format::eBc3UnormBlock:
		case vk::Format::eBc3SrgbBlock:
			return DXGI_FORMAT_BC3_UNORM;
		case vk::Format::eBc4UnormBlock:
			return DXGI_FORMAT_BC4_UNORM;
		case vk::Format::eBc5UnormBlock:
			return DXGI_FORMAT_BC5_UNORM;
		case vk::Format::eBc7UnormBlock:
		case vk::Format::eBc7SrgbBlock:
			return DXGI_FORMAT_BC7_UNORM;
		default:
			return DXGI_FORMAT_UNKNOWN;
	}
}

static std::string HrToString(HRESULT hr) {
	return std::system_category().message(hr);
}

#endif

bool DirectStorageManager::Initialize(GraphicContext* graphics) {
	Common::LockGuard lock(m_mutex);
	if (!IsEnabled()) {
		m_status_message = "DirectStorage is disabled in configuration";
		m_available = false;
		return false;
	}
	if (m_initialized) {
		return m_available;
	}

	m_graphics = graphics;
	m_initialized = true;
	m_available = false;

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	m_status_message = "DirectStorage is only supported on Windows";
	LOGF("[DirectStorage] %s\n", m_status_message.c_str());
	return false;
#else

	LOGF("[DirectStorage] Initializing DirectStorage + Vulkan subsystem...\n");

	ComPtr<IDXGIFactory4> dxgi_factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
	if (FAILED(hr)) {
		m_status_message = fmt::format("CreateDXGIFactory1 failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		return false;
	}

	ComPtr<IDXGIAdapter1> matched_adapter;

	if (graphics != nullptr && graphics->physical_device != nullptr) {
		// 1. Check Vulkan extensions: VK_KHR_external_memory and VK_KHR_external_memory_win32
		auto extensions = EnumerateVulkan<vk::ExtensionProperties>(
			"vkEnumerateDeviceExtensionProperties",
			[&](uint32_t* count, vk::ExtensionProperties* values) {
				return graphics->physical_device.enumerateDeviceExtensionProperties(nullptr, count, values);
			});

		bool has_ext_mem = false;
		bool has_ext_mem_win32 = false;
		for (const auto& ext : extensions) {
			std::string_view name(ext.extensionName.data());
			if (name == VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) {
				has_ext_mem = true;
			} else if (name == "VK_KHR_external_memory_win32") {
				has_ext_mem_win32 = true;
			}
		}

		if (!has_ext_mem || !has_ext_mem_win32) {
			m_status_message = "Vulkan missing VK_KHR_external_memory or VK_KHR_external_memory_win32";
			LOGF("[DirectStorage] %s\n", m_status_message.c_str());
			return false;
		}

		// 2. Query Vulkan Physical Device LUID to match DXGI Adapter
		vk::PhysicalDeviceIDProperties id_props {};
		vk::PhysicalDeviceProperties2 props2 {};
		props2.pNext = &id_props;
		graphics->physical_device.getProperties2(&props2);

		if (id_props.deviceLUIDValid) {
			LUID vk_luid {};
			std::memcpy(&vk_luid, id_props.deviceLUID.data(), sizeof(LUID));

			ComPtr<IDXGIAdapter1> adapter;
			for (UINT i = 0; dxgi_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
				DXGI_ADAPTER_DESC1 desc {};
				adapter->GetDesc1(&desc);
				if (desc.AdapterLuid.LowPart == vk_luid.LowPart && desc.AdapterLuid.HighPart == vk_luid.HighPart) {
					matched_adapter = adapter;
					char desc_str[256] = {};
					WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, desc_str, sizeof(desc_str) - 1, nullptr, nullptr);
					LOGF("[DirectStorage] Matched DXGI adapter to Vulkan GPU: %s (LUID: %08lx:%08lx)\n",
					     desc_str, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
					break;
				}
			}
		}
	} else {
		// Standalone / test mode: pick default primary adapter
		dxgi_factory->EnumAdapters1(0, &matched_adapter);
	}

	if (!matched_adapter) {
		m_status_message = "Could not find suitable DXGI adapter for DirectStorage";
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		return false;
	}

	// 4. Create D3D12 Device on matched adapter
	hr = D3D12CreateDevice(matched_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_d3d12_device));
	if (FAILED(hr)) {
		m_status_message = fmt::format("D3D12CreateDevice failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		return false;
	}

	// 5. Create D3D12 Fence and Event for DirectStorage synchronization
	hr = m_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_sync_fence));
	if (FAILED(hr)) {
		m_status_message = fmt::format("D3D12 CreateFence failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		Shutdown();
		return false;
	}

	m_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!m_fence_event) {
		m_status_message = "CreateEvent for DirectStorage fence failed";
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		Shutdown();
		return false;
	}

	// 6. Get DirectStorage Factory
	hr = DStorageGetFactory(IID_PPV_ARGS(&m_dstorage_factory));
	if (FAILED(hr)) {
		m_status_message = fmt::format("DStorageGetFactory failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		Shutdown();
		return false;
	}

	// Configure DirectStorage staging buffer size and settings if needed
	m_dstorage_factory->SetStagingBufferSize(DSTORAGE_STAGING_BUFFER_SIZE_32MB);

	// 7. Create GPU Destination Queue
	DSTORAGE_QUEUE_DESC gpu_queue_desc {};
	gpu_queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	gpu_queue_desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
	gpu_queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
	gpu_queue_desc.Name = "KytyPS5_DS_GpuQueue";
	gpu_queue_desc.Device = m_d3d12_device;

	ComPtr<IDStorageQueue> raw_gpu_queue;
	hr = m_dstorage_factory->CreateQueue(&gpu_queue_desc, IID_PPV_ARGS(&raw_gpu_queue));
	if (FAILED(hr) || FAILED(raw_gpu_queue->QueryInterface(IID_PPV_ARGS(&m_gpu_queue)))) {
		m_status_message = fmt::format("CreateQueue (GPU) failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		Shutdown();
		return false;
	}

	// 8. Create Memory Destination Queue for CPU host staging / general file I/O
	DSTORAGE_QUEUE_DESC mem_queue_desc {};
	mem_queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	mem_queue_desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
	mem_queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
	mem_queue_desc.Name = "KytyPS5_DS_MemQueue";
	mem_queue_desc.Device = nullptr;

	ComPtr<IDStorageQueue> raw_mem_queue;
	hr = m_dstorage_factory->CreateQueue(&mem_queue_desc, IID_PPV_ARGS(&raw_mem_queue));
	if (FAILED(hr) || FAILED(raw_mem_queue->QueryInterface(IID_PPV_ARGS(&m_memory_queue)))) {
		m_status_message = fmt::format("CreateQueue (Memory) failed: {}", HrToString(hr));
		LOGF("[DirectStorage] %s\n", m_status_message.c_str());
		Shutdown();
		return false;
	}

	m_available = true;
	m_status_message = "DirectStorage + Vulkan initialized successfully";
	std::printf("[DirectStorage] File I/O ready (GPU streaming is not integrated).\n");
	return true;
#endif
}

void DirectStorageManager::Shutdown() {
	Common::LockGuard lock(m_mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_gpu_queue) {
		m_gpu_queue->Close();
		m_gpu_queue->Release();
		m_gpu_queue = nullptr;
	}
	if (m_memory_queue) {
		m_memory_queue->Close();
		m_memory_queue->Release();
		m_memory_queue = nullptr;
	}
	if (m_dstorage_factory) {
		m_dstorage_factory->Release();
		m_dstorage_factory = nullptr;
	}
	if (m_sync_fence) {
		m_sync_fence->Release();
		m_sync_fence = nullptr;
	}
	if (m_fence_event) {
		CloseHandle(m_fence_event);
		m_fence_event = nullptr;
	}
	if (m_d3d12_device) {
		m_d3d12_device->Release();
		m_d3d12_device = nullptr;
	}
#endif
	m_graphics = nullptr;
	m_available = false;
	m_initialized = false;
	m_status_message = "DirectStorage shut down";
}

bool DirectStorageManager::LoadFileToHostMemory(const std::filesystem::path& path, uint64_t file_offset,
                                               void* destination, uint64_t size, uint64_t* bytes_read) {
	if (bytes_read) {
		*bytes_read = 0;
	}

	if (!IsEnabled()) {
		return false;
	}

	if (!destination || size == 0) {
		return false;
	}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return false;
#else
	Common::LockGuard lock(m_mutex);
	if (!m_available || !m_memory_queue || !m_dstorage_factory) return false;
	m_stats.total_requests++;
	m_stats.file_to_memory_requests++;

	const auto start_time = std::chrono::high_resolution_clock::now();

	std::wstring wpath = path.wstring();
	ComPtr<IDStorageFile> dstorage_file;
	HRESULT hr = m_dstorage_factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&dstorage_file));
	if (FAILED(hr)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] OpenFile failed for %s: %s\n", Common::PathToString(path).c_str(), HrToString(hr).c_str());
		return false;
	}

	BY_HANDLE_FILE_INFORMATION file_info {};
	hr = dstorage_file->GetFileInformation(&file_info);
	if (FAILED(hr)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] GetFileInformation failed: %s\n", HrToString(hr).c_str());
		return false;
	}

	const uint64_t file_size = (static_cast<uint64_t>(file_info.nFileSizeHigh) << 32) | file_info.nFileSizeLow;
	if (file_offset >= file_size) {
		return true; // EOF, 0 bytes read
	}

	const uint64_t readable_bytes = (std::min)(size, file_size - file_offset);
	if (readable_bytes > DSTORAGE_STAGING_BUFFER_SIZE_32MB) {
		++m_stats.fallback_count;
		return false; // callers chunk large reads; never truncate to UINT32
	}
	if (readable_bytes == 0) {
		return true;
	}

	HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!event_handle) {
		m_stats.error_count++;
		return false;
	}

	DSTORAGE_REQUEST request {};
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
	request.Source.File.Source = dstorage_file.Get();
	request.Source.File.Offset = file_offset;
	request.Source.File.Size = static_cast<uint32_t>(readable_bytes);
	request.Destination.Memory.Buffer = destination;
	request.Destination.Memory.Size = static_cast<uint32_t>(readable_bytes);
	request.UncompressedSize = static_cast<uint32_t>(readable_bytes);

	m_memory_queue->EnqueueRequest(&request);
	m_memory_queue->EnqueueSetEvent(event_handle);
	m_memory_queue->Submit();

	const DWORD wait_result = WaitForSingleObject(event_handle, INFINITE);
	if (wait_result != WAIT_OBJECT_0) {
		// Stop all writes before the caller frees or reuses the destination buffer.
		m_memory_queue->Close();
		m_available = false;
		++m_stats.error_count;
		CloseHandle(event_handle);
		LOGF("[DirectStorage] Completion wait failed: %lu\n", wait_result);
		return false;
	}
	CloseHandle(event_handle);

	DSTORAGE_ERROR_RECORD error_record {};
	if (WaitForSingleObject(m_memory_queue->GetErrorEvent(), 0) == WAIT_OBJECT_0)
		m_memory_queue->RetrieveErrorRecord(&error_record);
	if (FAILED(error_record.FirstFailure.HResult)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] Memory request failed: %s\n", HrToString(error_record.FirstFailure.HResult).c_str());
		return false;
	}

	const auto end_time = std::chrono::high_resolution_clock::now();
	const double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
	m_stats.last_read_duration_ms = elapsed_ms;
	m_stats.total_read_time_ms += elapsed_ms;
	m_stats.total_bytes_loaded += readable_bytes;
	if (m_stats.file_to_memory_requests == 1 || m_stats.file_to_memory_requests % 1024 == 0) {
		std::printf("[DirectStorage] Host I/O completed: requests=%llu bytes=%llu errors=%llu\n",
		     m_stats.file_to_memory_requests, m_stats.total_bytes_loaded, m_stats.error_count);
		std::fflush(stdout);
	}

	if (bytes_read) {
		*bytes_read = readable_bytes;
	}

	return true;
#endif
}

bool DirectStorageManager::LoadFileToSharedTexture2D(const std::filesystem::path& path, uint32_t width,
                                                    uint32_t height, vk::Format format,
                                                    DirectStorageSharedTexture& out_texture) {
	out_texture = {};

	if (!IsEnabled() || !m_available || !m_graphics) {
		return false;
	}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return false;
#else
	Common::LockGuard lock(m_mutex);
	m_stats.total_requests++;
	m_stats.file_to_gpu_requests++;

	const auto start_time = std::chrono::high_resolution_clock::now();

	DXGI_FORMAT dxgi_fmt = VulkanToDxgiFormat(format);
	if (dxgi_fmt == DXGI_FORMAT_UNKNOWN) {
		m_stats.fallback_count++;
		LOGF("[DirectStorage] Unsupported format for D3D12/Vulkan sharing: %s\n", vk::to_string(format).c_str());
		return false;
	}

	std::wstring wpath = path.wstring();
	ComPtr<IDStorageFile> dstorage_file;
	HRESULT hr = m_dstorage_factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&dstorage_file));
	if (FAILED(hr)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] OpenFile failed for %s: %s\n", Common::PathToString(path).c_str(), HrToString(hr).c_str());
		return false;
	}

	BY_HANDLE_FILE_INFORMATION file_info {};
	hr = dstorage_file->GetFileInformation(&file_info);
	if (FAILED(hr)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] GetFileInformation failed: %s\n", HrToString(hr).c_str());
		return false;
	}

	const uint64_t file_size = (static_cast<uint64_t>(file_info.nFileSizeHigh) << 32) | file_info.nFileSizeLow;

	// 1. Create D3D12 shared committed texture
	D3D12_RESOURCE_DESC res_desc {};
	res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	res_desc.Alignment = 0;
	res_desc.Width = width;
	res_desc.Height = height;
	res_desc.DepthOrArraySize = 1;
	res_desc.MipLevels = 1;
	res_desc.Format = dxgi_fmt;
	res_desc.SampleDesc.Count = 1;
	res_desc.SampleDesc.Quality = 0;
	res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	res_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heap_props {};
	heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

	ID3D12Resource* d3d_res = nullptr;
	hr = m_d3d12_device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_SHARED,
	                                            &res_desc, D3D12_RESOURCE_STATE_COMMON,
	                                            nullptr, IID_PPV_ARGS(&d3d_res));
	if (FAILED(hr)) {
		m_stats.error_count++;
		LOGF("[DirectStorage] CreateCommittedResource failed: %s\n", HrToString(hr).c_str());
		return false;
	}

	// 2. Export shared Win32 HANDLE from D3D12 resource
	HANDLE shared_handle = nullptr;
	hr = m_d3d12_device->CreateSharedHandle(d3d_res, nullptr, GENERIC_ALL, nullptr, &shared_handle);
	if (FAILED(hr)) {
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] CreateSharedHandle failed: %s\n", HrToString(hr).c_str());
		return false;
	}

	// 3. Create Vulkan Image with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT
	vk::ExternalMemoryImageCreateInfo ext_image_info {};
	ext_image_info.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D12Resource;

	vk::ImageCreateInfo image_info {};
	image_info.pNext = &ext_image_info;
	image_info.imageType = vk::ImageType::e2D;
	image_info.format = format;
	image_info.extent = vk::Extent3D { width, height, 1 };
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = vk::SampleCountFlagBits::e1;
	image_info.tiling = vk::ImageTiling::eOptimal;
	image_info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	image_info.sharingMode = vk::SharingMode::eExclusive;
	image_info.initialLayout = vk::ImageLayout::eUndefined;

	vk::Image vk_image = nullptr;
	auto res = m_graphics->device.createImage(&image_info, nullptr, &vk_image);
	if (res != vk::Result::eSuccess) {
		CloseHandle(shared_handle);
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] vkCreateImage with external memory failed: %s\n", vk::to_string(res).c_str());
		return false;
	}

	// 4. Import D3D12 shared handle into Vulkan device memory using Win32 C struct
	VkImportMemoryWin32HandleInfoKHR import_info {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	import_info.pNext = nullptr;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
	import_info.handle = shared_handle;
	import_info.name = nullptr;

	vk::MemoryDedicatedAllocateInfo dedicated_info {};
	dedicated_info.pNext = &import_info;
	dedicated_info.image = vk_image;

	vk::MemoryAllocateInfo alloc_info {};
	alloc_info.pNext = &dedicated_info;

	// Query memory requirements
	const auto mem_reqs = m_graphics->device.getImageMemoryRequirements(vk_image);
	alloc_info.allocationSize = mem_reqs.size;

	// Find suitable memory type with device local
	uint32_t mem_type_index = UINT32_MAX;
	for (uint32_t i = 0; i < m_graphics->physical_device_memory_properties.memoryTypeCount; ++i) {
		if ((mem_reqs.memoryTypeBits & (1u << i)) &&
		    (m_graphics->physical_device_memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
			mem_type_index = i;
			break;
		}
	}
	if (mem_type_index == UINT32_MAX) {
		mem_type_index = 0;
	}
	alloc_info.memoryTypeIndex = mem_type_index;

	vk::DeviceMemory vk_mem = nullptr;
	res = m_graphics->device.allocateMemory(&alloc_info, nullptr, &vk_mem);
	CloseHandle(shared_handle); // Win32 handle can safely be closed after import

	if (res != vk::Result::eSuccess) {
		m_graphics->device.destroyImage(vk_image, nullptr);
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] vkAllocateMemory (Import D3D12 handle) failed: %s\n", vk::to_string(res).c_str());
		return false;
	}

	res = m_graphics->device.bindImageMemory(vk_image, vk_mem, 0);
	if (res != vk::Result::eSuccess) {
		m_graphics->device.freeMemory(vk_mem, nullptr);
		m_graphics->device.destroyImage(vk_image, nullptr);
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] vkBindImageMemory failed: %s\n", vk::to_string(res).c_str());
		return false;
	}

	// 5. Enqueue DirectStorage request into D3D12 resource
	m_fence_value++;
	DSTORAGE_REQUEST request {};
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
	request.Source.File.Source = dstorage_file.Get();
	request.Source.File.Offset = 0;
	request.Source.File.Size = static_cast<uint32_t>(file_size);
	request.Destination.Texture.Resource = d3d_res;
	request.Destination.Texture.SubresourceIndex = 0;
	request.Destination.Texture.Region.left = 0;
	request.Destination.Texture.Region.top = 0;
	request.Destination.Texture.Region.front = 0;
	request.Destination.Texture.Region.right = width;
	request.Destination.Texture.Region.bottom = height;
	request.Destination.Texture.Region.back = 1;
	request.UncompressedSize = static_cast<uint32_t>(file_size);

	m_gpu_queue->EnqueueRequest(&request);
	m_gpu_queue->EnqueueSignal(m_sync_fence, m_fence_value);
	m_gpu_queue->Submit();

	// 6. Synchronize DirectStorage completion before accessing image in Vulkan
	m_sync_fence->SetEventOnCompletion(m_fence_value, m_fence_event);
	if (m_sync_fence->GetCompletedValue() < m_fence_value) {
		WaitForSingleObject(m_fence_event, INFINITE);
	}

	DSTORAGE_ERROR_RECORD error_record {};
	m_gpu_queue->RetrieveErrorRecord(&error_record);
	if (FAILED(error_record.FirstFailure.HResult)) {
		m_graphics->device.freeMemory(vk_mem, nullptr);
		m_graphics->device.destroyImage(vk_image, nullptr);
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] DirectStorage texture request failed: %s\n",
		     HrToString(error_record.FirstFailure.HResult).c_str());
		return false;
	}

	// 7. Create Vulkan Image View
	vk::ImageViewCreateInfo view_info {};
	view_info.image = vk_image;
	view_info.viewType = vk::ImageViewType::e2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	view_info.subresourceRange.baseMipLevel = 0;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount = 1;

	vk::ImageView vk_view = nullptr;
	res = m_graphics->device.createImageView(&view_info, nullptr, &vk_view);
	if (res != vk::Result::eSuccess) {
		m_graphics->device.freeMemory(vk_mem, nullptr);
		m_graphics->device.destroyImage(vk_image, nullptr);
		d3d_res->Release();
		m_stats.error_count++;
		LOGF("[DirectStorage] vkCreateImageView failed: %s\n", vk::to_string(res).c_str());
		return false;
	}

	const auto end_time = std::chrono::high_resolution_clock::now();
	const double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
	m_stats.last_read_duration_ms = elapsed_ms;
	m_stats.total_read_time_ms += elapsed_ms;
	m_stats.total_bytes_loaded += file_size;

	out_texture.image = vk_image;
	out_texture.memory = vk_mem;
	out_texture.image_view = vk_view;
	out_texture.width = width;
	out_texture.height = height;
	out_texture.format = format;
	out_texture.d3d_resource = d3d_res;

	LOGF("[DirectStorage] Successfully loaded %s to shared D3D12/Vulkan texture (%ux%u) in %.2f ms\n",
	     Common::PathToString(path).c_str(), width, height, elapsed_ms);

	return true;
#endif
}

void DirectStorageManager::DestroySharedTexture(DirectStorageSharedTexture& texture) {
	if (!m_graphics || !m_graphics->device) {
		return;
	}

	if (texture.image_view) {
		m_graphics->device.destroyImageView(texture.image_view, nullptr);
		texture.image_view = nullptr;
	}
	if (texture.image) {
		m_graphics->device.destroyImage(texture.image, nullptr);
		texture.image = nullptr;
	}
	if (texture.memory) {
		m_graphics->device.freeMemory(texture.memory, nullptr);
		texture.memory = nullptr;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (texture.d3d_resource) {
		texture.d3d_resource->Release();
		texture.d3d_resource = nullptr;
	}
#endif
}

} // namespace Libs::Graphics::DirectStorageIntegration
