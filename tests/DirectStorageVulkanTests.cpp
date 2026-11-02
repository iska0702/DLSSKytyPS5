#define NOMINMAX
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/directstorage/directStorageManager.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

int RunDirectStorageVulkanSmoke() {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
	return 1;
#else
	using namespace Libs::Graphics;
	using namespace DirectStorageIntegration;
	Common::InitializeThreads();
	Config::SetDirectStorageEnabled(true);
	Log::Initialize();
	GraphicContext g;
	auto& storage = DirectStorageManager::Instance();
	auto check = [](vk::Result result) {
		if (result != vk::Result::eSuccess) throw std::runtime_error(vk::to_string(result));
	};
	const auto path = std::filesystem::temp_directory_path() /
	                  ("kyty-dstorage-vulkan-" + std::to_string(GetCurrentProcessId()) + ".bin");
	vk::CommandPool pool;
	vk::Image image;
	VmaAllocation image_allocation = nullptr;
	VkBuffer readback = VK_NULL_HANDLE;
	VmaAllocation readback_allocation = nullptr;
	int result = 1;
	bool memory_initialized = false;
	try {
		vk::detail::DynamicLoader loader;
		VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
		vk::ApplicationInfo app {};
		app.pApplicationName = "Kyty DirectStorage Vulkan streaming";
		app.apiVersion = VK_API_VERSION_1_3;
		vk::InstanceCreateInfo instance {};
		instance.pApplicationInfo = &app;
		check(vk::createInstance(&instance, nullptr, &g.instance));
		VULKAN_HPP_DEFAULT_DISPATCHER.init(g.instance);
		auto devices = EnumerateVulkan<vk::PhysicalDevice>("DS GPU test devices", [&](uint32_t* n, vk::PhysicalDevice* p) {
			return g.instance.enumeratePhysicalDevices(n, p);
		});
		if (devices.empty()) throw std::runtime_error("No Vulkan GPU");
		g.physical_device = devices.front();
		for (auto device : devices)
			if (device.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) g.physical_device = device;
		g.physical_device_properties = g.physical_device.getProperties();
		g.physical_device_memory_properties = g.physical_device.getMemoryProperties();
		uint32_t count = 0;
		g.physical_device.getQueueFamilyProperties(&count, nullptr);
		std::vector<vk::QueueFamilyProperties> families(count);
		g.physical_device.getQueueFamilyProperties(&count, families.data());
		for (uint32_t i = 0; i < count; ++i)
			if (families[i].queueFlags & vk::QueueFlagBits::eGraphics) { g.queue_family = i; break; }
		float priority = 1;
		vk::DeviceQueueCreateInfo queue {};
		queue.queueFamilyIndex = g.queue_family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
		vk::PhysicalDeviceVulkan12Features features;
		features.bufferDeviceAddress = true;
		vk::DeviceCreateInfo device {};
		device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
		device.pNext = &features;
		check(g.physical_device.createDevice(&device, nullptr, &g.device));
		VULKAN_HPP_DEFAULT_DISPATCHER.init(g.device);
		g.device.getQueue(g.queue_family, 0, &g.queue);
		if (!g.CreateAllocator() || !storage.Initialize(&g)) throw std::runtime_error("DirectStorage/Vulkan initialization failed");

		constexpr size_t bytes = 128 * 128 * 4;
		std::vector<uint8_t> expected(bytes);
		for (size_t i = 0; i < bytes; ++i) expected[i] = static_cast<uint8_t>((i * 137 + i / 251 + 42) & 255);
		{ std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(expected.data()), bytes); }
		auto source = storage.EnqueueFileRead(path, 0, bytes);
		if (!source || !source->Wait() || !source->NativeBuffer()) throw std::runtime_error("Native mapped source unavailable");
		if (source->Bytes().size() != bytes || std::memcmp(source->Bytes().data(), expected.data(), bytes))
			throw std::runtime_error("DirectStorage source mismatch");
		const auto renamed = path.string() + ".renamed";
		std::filesystem::rename(path, renamed);
		std::filesystem::rename(renamed, path);
		{
			std::fstream changed_file(path, std::ios::in | std::ios::out | std::ios::binary);
			changed_file.put(static_cast<char>(expected[0] ^ 0xff));
			if (!changed_file) throw std::runtime_error("Completed cached source keeps file locked");
		}
		std::puts("DirectStorage completed source: rename/write allowed while immutable GPU payload is retained: PASS");
		Libs::LibKernel::Memory::Initialize();
		memory_initialized = true;
		void* guest = nullptr;
		if (Libs::LibKernel::Memory::KernelMapNamedFlexibleMemory(&guest, bytes, 3, 0, "DS texture provenance") != 0)
			throw std::runtime_error("Guest mapping for source validation");
		const auto guest_address = reinterpret_cast<uint64_t>(guest);
		std::memcpy(guest, expected.data(), bytes);
		storage.PublishRead(source, guest_address);
		if (!storage.FindTextureSource(guest_address, bytes).buffer ||
		    storage.FindTextureSource(guest_address + 256, 512).offset != 256)
			throw std::runtime_error("Published source/subrange not found");
		static_cast<uint8_t*>(guest)[bytes / 2] ^= 1;
		if (storage.FindTextureSource(guest_address, bytes).buffer)
			throw std::runtime_error("Changed middle byte accepted as immutable file data");
		if (Libs::LibKernel::Memory::KernelMunmap(guest_address, bytes) != 0 ||
		    storage.FindTextureSource(guest_address, bytes).buffer)
			throw std::runtime_error("Unmapped guest data accepted as a texture source");
		std::puts("DirectStorage provenance: exact ranges/subranges accepted, changed middle/unmapped ranges rejected: PASS");

		vk::ImageCreateInfo image_info;
		image_info.imageType = vk::ImageType::e2D;
		image_info.format = vk::Format::eR8G8B8A8Unorm;
		image_info.extent = vk::Extent3D {128, 128, 1};
		image_info.mipLevels = image_info.arrayLayers = 1;
		image_info.samples = vk::SampleCountFlagBits::e1;
		image_info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
		VmaAllocationCreateInfo allocation {};
		allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		VkImage raw_image;
		if (vmaCreateImage(g.allocator, reinterpret_cast<const VkImageCreateInfo*>(&image_info), &allocation,
		                   &raw_image, &image_allocation, nullptr) != VK_SUCCESS) throw std::runtime_error("Image allocation");
		image = raw_image;
		VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer.size = bytes;
		buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
		allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		VmaAllocationInfo mapped {};
		if (vmaCreateBuffer(g.allocator, &buffer, &allocation, &readback, &readback_allocation, &mapped) != VK_SUCCESS)
			throw std::runtime_error("Readback allocation");
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.queueFamilyIndex = g.queue_family;
		check(g.device.createCommandPool(&pool_info, nullptr, &pool));
		vk::CommandBuffer command;
		vk::CommandBufferAllocateInfo commands {};
		commands.commandPool = pool; commands.level = vk::CommandBufferLevel::ePrimary;
		commands.commandBufferCount = 1;
		check(g.device.allocateCommandBuffers(&commands, &command));
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		check(command.begin(begin));
		vk::ImageMemoryBarrier barrier;
		barrier.image = image;
		barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
		                        {}, 0, nullptr, 0, nullptr, 1, &barrier);
		vk::BufferImageCopy copy;
		copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
		copy.imageExtent = {128, 128, 1};
		command.copyBufferToImage(source->NativeBuffer(), image, vk::ImageLayout::eTransferDstOptimal, 1, &copy);
		barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
		barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
		                        {}, 0, nullptr, 0, nullptr, 1, &barrier);
		command.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(readback), 1, &copy);
		check(command.end());
		vk::SubmitInfo submit;
		submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
		check(g.queue.submit(1, &submit, nullptr));
		check(g.device.waitIdle());
		vmaInvalidateAllocation(g.allocator, readback_allocation, 0, bytes);
		if (std::memcmp(mapped.pMappedData, expected.data(), bytes)) throw std::runtime_error("GPU image readback mismatch");
		storage.Shutdown();
		if (source->NativeBuffer() || source->Bytes().size() != bytes ||
		    std::memcmp(source->Bytes().data(), expected.data(), bytes)) throw std::runtime_error("Shutdown ticket lifetime mismatch");
		std::puts("DirectStorage -> mapped Vulkan source -> GPU image -> readback: PASS (65536 bytes exact)");
		result = 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "DirectStorage GPU test: %s\n", error.what());
	}
	if (g.device) (void)g.device.waitIdle();
	storage.Shutdown();
	if (pool) g.device.destroyCommandPool(pool, nullptr);
	if (image) vmaDestroyImage(g.allocator, image, image_allocation);
	if (readback) vmaDestroyBuffer(g.allocator, readback, readback_allocation);
	g.DestroyAllocator();
	if (g.device) g.device.destroy(nullptr);
	if (g.instance) g.instance.destroy(nullptr);
	if (memory_initialized) Libs::LibKernel::Memory::Shutdown();
	std::filesystem::remove(path);
	return result;
#endif
}
