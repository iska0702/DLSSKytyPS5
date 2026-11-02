#include "graphics/directstorage/directStorageManager.h"
#include "graphics/directstorage/directStorageFileIO.h"
#include "kernel/memory.h"

#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <array>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <wrl/client.h>
#endif

namespace Libs::Graphics::DirectStorageIntegration {
bool TryReadFile(const std::filesystem::path& path, uint64_t offset, void* destination,
                 uint64_t size, uint64_t* bytes_read) {
	auto& storage = DirectStorageManager::Instance();
	return storage.IsEnabled() && storage.IsAvailable() &&
	       storage.LoadFileToHostMemory(path, offset, destination, size, bytes_read);
}

struct DirectStorageRead::State {
	std::vector<uint8_t> bytes;
	uint64_t byte_size = 0;
	VmaAllocator allocator = nullptr;
	VmaAllocation allocation = nullptr;
	VkBuffer buffer = VK_NULL_HANDLE;
	uint8_t* mapped = nullptr;
	uint8_t* Data() { return mapped != nullptr ? mapped : bytes.data(); }
	const uint8_t* Data() const { return mapped != nullptr ? mapped : bytes.data(); }
	mutable std::mutex mutex;
	bool finished = false;
	bool success = false;
	std::shared_ptr<DirectStorageAsyncCounters> counters;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	Microsoft::WRL::ComPtr<IDStorageFile> file;
	Microsoft::WRL::ComPtr<IDStorageStatusArray> status;
	Microsoft::WRL::ComPtr<IDStorageQueue1> queue;
	HANDLE event = nullptr;
	bool submitted = false;
	~State() {
		// Also cover allocation failure while constructing the public ticket after Submit.
		// Its destructor cannot drain us if that ticket was never successfully allocated.
		if (submitted && !finished && WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
			queue->Close();
		if (event != nullptr) CloseHandle(event);
		if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, buffer, allocation);
	}
#endif
};

DirectStorageRead::DirectStorageRead(std::unique_ptr<State> state): m_state(std::move(state)) {}
DirectStorageRead::~DirectStorageRead() { Wait(); }

bool DirectStorageRead::IsComplete() const {
	std::scoped_lock lock(m_state->mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return m_state->finished || (m_state->submitted && m_state->status->IsComplete(0));
#else
	return true;
#endif
}

bool DirectStorageRead::Wait() {
	std::scoped_lock lock(m_state->mutex);
	if (m_state->finished) return m_state->success;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (m_state->submitted) {
		// Wait for the event as well as the preceding status. Releasing the event as soon
		// as IsComplete becomes true would race the queue's following SetEvent operation.
		const auto wait = WaitForSingleObject(m_state->event, INFINITE);
		if (wait != WAIT_OBJECT_0) m_state->queue->Close(); // drain before releasing storage
		m_state->success = wait == WAIT_OBJECT_0 && SUCCEEDED(m_state->status->GetHResult(0));
		if (m_state->success && m_state->allocation != nullptr)
			m_state->success = vmaFlushAllocation(m_state->allocator, m_state->allocation, 0, m_state->byte_size) == VK_SUCCESS;
	}
	// Published texture bytes may stay cached long after I/O. They no longer need the
	// source handle; retaining it could interfere with guest saves/renames and waste handles.
	m_state->file.Reset();
#endif
	m_state->finished = true;
	if (m_state->counters) {
		if (m_state->success) {
			m_state->counters->bytes.fetch_add(m_state->byte_size, std::memory_order_relaxed);
			const auto completed = m_state->counters->completed.fetch_add(1, std::memory_order_relaxed) + 1;
			if (completed == 1 || completed % 1024 == 0) {
				std::printf("[DirectStorage] Streaming completed: reads=%llu bytes=%llu errors=%llu\n",
				            static_cast<unsigned long long>(completed),
				            static_cast<unsigned long long>(m_state->counters->bytes.load()),
				            static_cast<unsigned long long>(m_state->counters->errors.load()));
				std::fflush(stdout);
			}
		} else {
			m_state->counters->errors.fetch_add(1, std::memory_order_relaxed);
		}
	}
	return m_state->success;
}

DirectStorageStats DirectStorageManager::GetStats() const {
	Common::LockGuard lock(m_mutex);
	auto result = m_stats;
	if (m_async_counters) {
		result.async_completed = m_async_counters->completed.load();
		result.async_completed_bytes = m_async_counters->bytes.load();
		result.async_errors = m_async_counters->errors.load();
		result.total_bytes_loaded += result.async_completed_bytes;
	}
	return result;
}

std::span<const uint8_t> DirectStorageRead::Bytes() const {
	std::scoped_lock lock(m_state->mutex);
	return m_state->finished && m_state->success ? std::span<const uint8_t>(m_state->Data(), static_cast<size_t>(m_state->byte_size))
	                                          : std::span<const uint8_t> {};
}

vk::Buffer DirectStorageRead::NativeBuffer() const {
	std::scoped_lock lock(m_state->mutex);
	return m_state->finished && m_state->success ? vk::Buffer(m_state->buffer) : vk::Buffer {};
}

void DirectStorageRead::ReleaseNativeStorage() {
	std::scoped_lock lock(m_state->mutex);
	if (m_state->buffer == VK_NULL_HANDLE) return;
	m_state->bytes.assign(m_state->mapped, m_state->mapped + m_state->byte_size);
	vmaDestroyBuffer(m_state->allocator, m_state->buffer, m_state->allocation);
	m_state->buffer = VK_NULL_HANDLE;
	m_state->allocation = nullptr;
	m_state->mapped = nullptr;
}

void DirectStorageManager::PublishRead(const std::shared_ptr<DirectStorageRead>& read, uint64_t address) {
	if (!read || !read->NativeBuffer()) return;
	const auto size = read->Bytes().size();
	if (size == 0 || address > UINT64_MAX - size) return;
	Common::LockGuard lock(m_mutex);
	if (!m_available) return;
	std::erase_if(m_published_reads, [&](const auto& entry) {
		return entry.address < address + size && address < entry.address + entry.size;
	});
	if (m_published_reads.size() >= 32) m_published_reads.erase(m_published_reads.begin());
	m_published_reads.push_back({address, size, read});
	++m_stats.published_reads;
}

DirectStorageTextureSource DirectStorageManager::FindTextureSource(uint64_t address, uint64_t size) {
	DirectStorageTextureSource source;
	if (!IsEnabled() || size == 0 || address > UINT64_MAX - size) return source;
	{
		Common::LockGuard lock(m_mutex);
		if (!m_available) return source;
		++m_stats.texture_lookups;
		for (auto it = m_published_reads.rbegin(); it != m_published_reads.rend(); ++it) {
			if (address >= it->address && address - it->address <= it->size &&
			    size <= it->size - (address - it->address)) {
				source = {it->read, it->read->NativeBuffer(), address - it->address, size};
				break;
			}
		}
	}
	if (!source.buffer || source.offset % 256 != 0) return {};
	// Guest CPU decompression/writes can change data before any texture starts watching
	// these pages. Comparing every byte avoids relying on unobserved writes or stale VAs.
	const auto bytes = source.owner->Bytes();
	// Called with TextureCache's region lock held. CompareGpuCleanBacking would query
	// that cache again and recursively acquire its region lock. The caller already uses
	// the ordinary plain-guest-source eligibility checks; inspect backing without reentry.
	std::array<uint8_t, 64 * 1024> scratch;
	for (uint64_t done = 0; done < size;) {
		const auto part = (std::min<uint64_t>)(scratch.size(), size - done);
		if ((!LibKernel::Memory::TryReadBacking(address + done, scratch.data(), part) &&
		     !LibKernel::Memory::TryReadPrtBacking(address + done, scratch.data(), part)) ||
		    std::memcmp(scratch.data(), bytes.data() + source.offset + done, part) != 0) {
			Common::LockGuard lock(m_mutex);
			++m_stats.texture_changed_fallbacks;
			return {};
		}
		done += part;
	}
	{
		Common::LockGuard lock(m_mutex);
		++m_stats.texture_streams;
		m_stats.texture_stream_bytes += size;
		if (m_stats.texture_streams == 1 || m_stats.texture_streams % 256 == 0) {
			std::printf("[DirectStorage] Vulkan texture source: textures=%llu bytes=%llu changed=%llu\n",
			            static_cast<unsigned long long>(m_stats.texture_streams),
			            static_cast<unsigned long long>(m_stats.texture_stream_bytes),
			            static_cast<unsigned long long>(m_stats.texture_changed_fallbacks));
			std::fflush(stdout);
		}
	}
	return source;
}

std::shared_ptr<DirectStorageRead> DirectStorageManager::EnqueueFileRead(
    const std::filesystem::path& path, uint64_t file_offset, uint64_t size) {
	if (!IsEnabled() || size == 0 || size > 32ull * 1024 * 1024) return {};
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return {};
#else
	Common::LockGuard lock(m_mutex);
	if (!m_available || !m_memory_queue || !m_dstorage_factory) return {};
	++m_stats.total_requests;
	++m_stats.file_to_memory_requests;
	auto retained = [&] {
		std::erase_if(m_reads, [](const auto& item) { return item.expired(); });
		uint64_t total = 0;
		for (const auto& weak : m_reads)
			if (const auto read = weak.lock()) total += read->m_state->byte_size;
		return total;
	};
	uint64_t retained_bytes = retained();
	while (!m_published_reads.empty() && (m_reads.size() >= 64 ||
	       size > 128ull * 1024 * 1024 - retained_bytes)) {
		m_published_reads.erase(m_published_reads.begin());
		retained_bytes = retained();
	}
	if (m_reads.size() >= 64 || size > 128ull * 1024 * 1024 - retained_bytes) {
		++m_stats.async_budget_fallbacks;
		return {};
	}
	auto state = std::make_unique<DirectStorageRead::State>();
	if (!m_async_counters) m_async_counters = std::make_shared<DirectStorageAsyncCounters>();
	state->counters = m_async_counters;
	const auto wide_path = path.wstring();
	if (FAILED(m_dstorage_factory->OpenFile(wide_path.c_str(), IID_PPV_ARGS(&state->file)))) {
		++m_stats.error_count;
		return {};
	}
	BY_HANDLE_FILE_INFORMATION info {};
	if (FAILED(state->file->GetFileInformation(&info))) return {};
	const uint64_t file_size = uint64_t {info.nFileSizeHigh} << 32 | info.nFileSizeLow;
	const auto readable = file_offset < file_size ? (std::min)(size, file_size - file_offset) : 0;
	state->byte_size = readable;
	if (readable >= 64 * 1024 && readable <= 8 * 1024 * 1024 && m_graphics && m_graphics->allocator) {
		// DirectStorage's memory destination writes directly into Vulkan's mapped upload
		// storage. Guest tiled layouts are decoded by the existing Vulkan GPU tiler.
		VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer.size = readable;
		buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation {};
		allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
		                   VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
		allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		VmaAllocationInfo result {};
		if (vmaCreateBuffer(m_graphics->allocator, &buffer, &allocation, &state->buffer,
		                    &state->allocation, &result) == VK_SUCCESS) {
			state->allocator = m_graphics->allocator;
			state->mapped = static_cast<uint8_t*>(result.pMappedData);
			++m_stats.mapped_reads;
		}
	}
	if (state->mapped == nullptr) state->bytes.resize(static_cast<size_t>(readable));
	if (readable != 0) {
		if (FAILED(m_dstorage_factory->CreateStatusArray(1, "Kyty streaming read",
		                                               IID_PPV_ARGS(&state->status)))) return {};
		state->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (state->event == nullptr) return {};
		state->queue = m_memory_queue;
		DSTORAGE_REQUEST request {};
		request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
		request.Source.File.Source = state->file.Get();
		request.Source.File.Offset = file_offset;
		request.Source.File.Size = static_cast<uint32_t>(readable);
		request.Destination.Memory.Buffer = state->Data();
		request.Destination.Memory.Size = static_cast<uint32_t>(readable);
		request.UncompressedSize = static_cast<uint32_t>(readable);
		m_memory_queue->EnqueueRequest(&request);
		m_memory_queue->EnqueueStatus(state->status.Get(), 0);
		m_memory_queue->EnqueueSetEvent(state->event);
		state->submitted = true;
		m_memory_queue->Submit();
	} else {
		state->finished = state->success = true;
		state->file.Reset();
	}
	++m_stats.async_requests;
	m_stats.async_bytes_requested += readable;
	auto read = std::shared_ptr<DirectStorageRead>(new DirectStorageRead(std::move(state)));
	m_reads.push_back(read);
	return read;
#endif
}
}
