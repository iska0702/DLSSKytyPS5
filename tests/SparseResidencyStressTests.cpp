// Sparse residency stress test (Bug A, KYTY_TEXTURE_SPARSE_RESIDENCY / KYTY_BDA_PAGETABLE_SPARSE).
//
// The emulator's own GraphicContext::CreateSparseImage / BindSparseImageLevels / DeleteImage manage
// many partially resident textures while compute dispatches on the main queue sample them, in
// flight, the way TextureCache does: levels grow (memory bound behind finer levels, then uploaded),
// images are retired once their last submission completed and new ones are created. Next to it,
// like the emulator: side-queue copies that wait on the main queue's timeline (side readbacks) and
// a sparse residency storage buffer that gains zero-filled blocks over time (the BDA page table),
// read by the same dispatches.
//
//   sparse_residency_stress_tests [--iterations N] [--validation off|core|sync|gpuav]
//                                 [--views clamped|all] [--barriers whole|bound] [--work N] [--seed S]
//
// --views clamped (default) samples each texture through a view of the whole chain whose minimum LOD
// is its first resident level (VK_EXT_image_view_min_lod, as TextureCache's views). --views all
// samples every level, non-resident ones included (they read zero).
//
// --barriers whole (default) transitions the whole mip chain, non-resident levels included, as
// Image::GetBarriers does: the views span them, so they must be in the view's layout too. --barriers
// bound transitions only levels with memory, newly bound ones from UNDEFINED: core validation
// rejects that (every subresource of a descriptor's view must be in its layout), which is why the
// Bug A fix orders the binds on the GPU instead: each growth waits for the submitted dispatches and
// each dispatch waits for the binds before it (GraphicContext::BindSparseImageLevels).
//
// GLSL of the dispatch (SparseResidencyStressShader.inc):
//   layout(local_size_x = 64) in;
//   layout(set = 0, binding = 0) uniform sampler2D textures[64];
//   layout(set = 0, binding = 1) buffer Output { vec4 values[]; } result;
//   layout(set = 0, binding = 2) readonly buffer Table { uint words[]; } table;
//   layout(push_constant) uniform Params { uint iterations, texture_count, table_words; float max_lod; };
//   each workgroup samples textures[group % count] with textureLod at LODs 0..max_lod and xors
//   table.words at pseudo-random indices below table_words.

#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "SparseResidencyStressShader.inc"

using namespace Libs::Graphics;

namespace {

constexpr uint32_t Slots       = 64; // textures sampled per dispatch (the descriptor array)
constexpr uint32_t InFlight    = 3;  // main-queue submissions in flight
constexpr uint64_t TableBlocks = 512;

std::atomic<uint32_t> g_validation_errors {0};

[[noreturn]] void Fail(const char* what) {
	std::fprintf(stderr, "sparse_residency_stress: FAILED: %s\n", what);
	std::fflush(stderr);
	std::exit(1);
}

void Check(vk::Result result, const char* what) {
	if (result != vk::Result::eSuccess) {
		std::fprintf(stderr, "sparse_residency_stress: %s failed: %s\n", what, vk::to_string(result).c_str());
		Fail(what);
	}
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void* /*user*/) {
	const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
	if (error) {
		g_validation_errors.fetch_add(1, std::memory_order_relaxed);
	}
	static std::atomic<uint32_t> printed {0};
	if (printed.fetch_add(1, std::memory_order_relaxed) < 40) {
		std::fprintf(stderr, "[validation %s] %s\n", error ? "error" : "warning",
		             data->pMessage != nullptr ? data->pMessage : "");
	}
	return VK_FALSE;
}

struct Options {
	uint32_t    iterations = 300;
	std::string validation = "off";
	bool        all_levels = false;
	bool        bound_barriers = false;
	uint32_t    work           = 1536; // texture samples per invocation and dispatch (GPU time in flight)
	uint32_t    seed       = 1;
};

struct Texture {
	bool        live        = false;
	std::unique_ptr<VulkanImage> image = std::make_unique<VulkanImage>();
	vk::Format  format      = vk::Format::eUndefined;
	uint32_t    block       = 1; // texel block width/height (4 for BC)
	uint32_t    block_bytes = 4;
	uint32_t    first       = 0;     // first resident level
	uint32_t    uploaded    = UINT32_MAX; // levels >= this hold uploaded texels
	vk::ImageLayout layout  = vk::ImageLayout::eUndefined;
	vk::ImageView   view    = nullptr;
	uint64_t    last_use    = 0;
	uint32_t    grows       = 0;
};

struct Retired {
	uint64_t      tick = 0;
	vk::ImageView view = nullptr;
	VulkanImage*  image = nullptr; // owned
};

class Stress {
public:
	explicit Stress(const Options& options): m_options(options), m_random(options.seed) {}

	int Run() {
		Init();
		CreatePipeline();
		CreateBuffers();
		for (uint32_t slot = 0; slot < Slots; slot++) {
			CreateTexture(slot);
		}
		std::thread side([this] { SideReadbacks(); });
		std::thread table([this] { TableGrowth(); });
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t iteration = 0; iteration < m_options.iterations; iteration++) {
			Iterate(iteration);
		}
		WaitTick(m_tick);
		m_stop.store(true);
		side.join();
		table.join();
		Check(m_graphics.device.waitIdle(), "vkDeviceWaitIdle");
		const auto seconds =
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		std::printf("sparse_residency_stress: %u iterations in %.1f s: %" PRIu64 " sparse images created, %" PRIu64
		            " level growths, %" PRIu64 " retired, %" PRIu64 " side readbacks, %" PRIu64
		            " table blocks bound; views=%s barriers=%s validation=%s errors=%u\n",
		            m_options.iterations, seconds, m_created, m_growths, m_retired_count,
		            m_side_copies.load(), m_table_bound.load(), m_options.all_levels ? "all" : "clamped", m_options.bound_barriers ? "bound" : "whole",
		            m_options.validation.c_str(), g_validation_errors.load());
		Shutdown();
		if (g_validation_errors.load() != 0) {
			Fail("validation errors");
		}
		std::puts("sparse_residency_stress: passed");
		return 0;
	}

private:
	void Init() {
		static vk::detail::DynamicLoader loader;
		VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
		vk::ApplicationInfo app {};
		app.pApplicationName = "sparse_residency_stress";
		app.apiVersion       = VK_API_VERSION_1_3;
		vk::InstanceCreateInfo instance_info {};
		instance_info.pApplicationInfo = &app;
		const std::array<const char*, 1> layers {"VK_LAYER_KHRONOS_validation"};
		const std::array<const char*, 2> extensions {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		                                             VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
		std::vector<vk::ValidationFeatureEnableEXT> enables;
		if (m_options.validation == "sync") {
			enables.push_back(vk::ValidationFeatureEnableEXT::eSynchronizationValidation);
		} else if (m_options.validation == "gpuav") {
			enables.push_back(vk::ValidationFeatureEnableEXT::eGpuAssisted);
		}
		vk::ValidationFeaturesEXT features {};
		features.enabledValidationFeatureCount = static_cast<uint32_t>(enables.size());
		features.pEnabledValidationFeatures    = enables.data();
		vk::DebugUtilsMessengerCreateInfoEXT messenger {};
		messenger.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
		                            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
		messenger.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
		                        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation;
		messenger.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&OnValidation);
		const bool validation = m_options.validation != "off";
		if (validation) {
			messenger.pNext                       = &features;
			instance_info.pNext                   = &messenger;
			instance_info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
			instance_info.ppEnabledLayerNames     = layers.data();
			instance_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
			instance_info.ppEnabledExtensionNames = extensions.data();
		}
		Check(vk::createInstance(&instance_info, nullptr, &m_graphics.instance), "vkCreateInstance");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(m_graphics.instance);
		if (validation) {
			messenger.pNext = nullptr;
			Check(m_graphics.instance.createDebugUtilsMessengerEXT(&messenger, nullptr, &m_messenger),
			      "vkCreateDebugUtilsMessengerEXT");
		}
		uint32_t count = 0;
		Check(m_graphics.instance.enumeratePhysicalDevices(&count, nullptr), "vkEnumeratePhysicalDevices");
		std::vector<vk::PhysicalDevice> devices(count);
		Check(m_graphics.instance.enumeratePhysicalDevices(&count, devices.data()), "vkEnumeratePhysicalDevices");
		for (auto physical: devices) {
			const auto features10 = physical.getFeatures();
			if (!features10.sparseBinding || !features10.sparseResidencyImage2D ||
			    !features10.sparseResidencyBuffer ||
			    !physical.getProperties().sparseProperties.residencyNonResidentStrict) {
				continue;
			}
			const auto families = physical.getQueueFamilyProperties();
			for (uint32_t family = 0; family < families.size(); family++) {
				const auto need = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
				                  vk::QueueFlagBits::eSparseBinding;
				if ((families[family].queueFlags & need) == need && families[family].queueCount >= 2) {
					m_graphics.physical_device = physical;
					m_graphics.queue_family    = family;
					break;
				}
			}
			if (m_graphics.physical_device) {
				break;
			}
		}
		if (!m_graphics.physical_device) {
			std::puts("sparse_residency_stress: skipped (no device with sparse residency and two universal queues)");
			std::exit(0);
		}
		m_graphics.physical_device.getProperties(&m_graphics.physical_device_properties);
		m_graphics.physical_device_memory_properties = m_graphics.physical_device.getMemoryProperties();
		std::printf("sparse_residency_stress: %s, driver 0x%x, family %u\n",
		            m_graphics.physical_device_properties.deviceName.data(),
		            m_graphics.physical_device_properties.driverVersion, m_graphics.queue_family);

		const std::array<float, 2> priorities {1.0f, 1.0f};
		vk::DeviceQueueCreateInfo  queue_info {};
		queue_info.queueFamilyIndex = m_graphics.queue_family;
		queue_info.queueCount       = 2;
		queue_info.pQueuePriorities = priorities.data();
		vk::PhysicalDeviceImageViewMinLodFeaturesEXT min_lod {};
		min_lod.minLod = VK_TRUE;
		vk::PhysicalDeviceVulkan12Features features12 {};
		features12.pNext                                    = &min_lod;
		features12.timelineSemaphore                        = VK_TRUE;
		features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		features12.bufferDeviceAddress                      = VK_TRUE;
		vk::PhysicalDeviceVulkan13Features features13 {};
		features13.pNext            = &features12;
		features13.synchronization2 = VK_TRUE;
		vk::PhysicalDeviceFeatures2 features2 {};
		features2.pNext                           = &features13;
		features2.features.sparseBinding          = VK_TRUE;
		features2.features.sparseResidencyImage2D = VK_TRUE;
		features2.features.sparseResidencyBuffer  = VK_TRUE;
		const std::array<const char*, 1> device_extensions {VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME};
		vk::DeviceCreateInfo device_info {};
		device_info.pNext                   = &features2;
		device_info.queueCreateInfoCount    = 1;
		device_info.pQueueCreateInfos       = &queue_info;
		device_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
		device_info.ppEnabledExtensionNames = device_extensions.data();
		Check(m_graphics.physical_device.createDevice(&device_info, nullptr, &m_graphics.device), "vkCreateDevice");
		VULKAN_HPP_DEFAULT_DISPATCHER.init(m_graphics.device);
		m_graphics.device.getQueue(m_graphics.queue_family, 0, &m_graphics.queue);
		m_graphics.device.getQueue(m_graphics.queue_family, 1, &m_graphics.side_queue);
		m_graphics.side_queue_index                = 1;
		m_graphics.sparse_residency_image_enabled  = true;
		m_graphics.sparse_residency_buffer_enabled = true;
		if (!m_graphics.CreateAllocator()) {
			Fail("vmaCreateAllocator");
		}

		vk::SemaphoreTypeCreateInfo timeline_type {};
		timeline_type.semaphoreType = vk::SemaphoreType::eTimeline;
		vk::SemaphoreCreateInfo semaphore_info {};
		semaphore_info.pNext = &timeline_type;
		Check(m_graphics.device.createSemaphore(&semaphore_info, nullptr, &m_timeline), "create timeline");
		Check(m_graphics.device.createSemaphore(&semaphore_info, nullptr, &m_side_timeline), "create side timeline");
		vk::CommandPoolCreateInfo pool_info {};
		pool_info.flags            = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
		pool_info.queueFamilyIndex = m_graphics.queue_family;
		Check(m_graphics.device.createCommandPool(&pool_info, nullptr, &m_pool), "create command pool");
		Check(m_graphics.device.createCommandPool(&pool_info, nullptr, &m_side_pool), "create side pool");
		Check(m_graphics.device.createCommandPool(&pool_info, nullptr, &m_table_pool), "create table pool");
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = m_pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = InFlight;
		Check(m_graphics.device.allocateCommandBuffers(&allocate, m_commands.data()), "allocate command buffers");
	}

	void CreatePipeline() {
		vk::SamplerCreateInfo sampler_info {};
		sampler_info.magFilter    = vk::Filter::eLinear;
		sampler_info.minFilter    = vk::Filter::eLinear;
		sampler_info.mipmapMode   = vk::SamplerMipmapMode::eLinear;
		sampler_info.addressModeU = vk::SamplerAddressMode::eRepeat;
		sampler_info.addressModeV = vk::SamplerAddressMode::eRepeat;
		sampler_info.maxLod       = VK_LOD_CLAMP_NONE;
		Check(m_graphics.device.createSampler(&sampler_info, nullptr, &m_sampler), "create sampler");
		std::array<vk::DescriptorSetLayoutBinding, 3> bindings {};
		bindings[0].binding         = 0;
		bindings[0].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		bindings[0].descriptorCount = Slots;
		bindings[0].stageFlags      = vk::ShaderStageFlagBits::eCompute;
		for (uint32_t i = 1; i < 3; i++) {
			bindings[i].binding         = i;
			bindings[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
			bindings[i].descriptorCount = 1;
			bindings[i].stageFlags      = vk::ShaderStageFlagBits::eCompute;
		}
		vk::DescriptorSetLayoutCreateInfo layout_info {};
		layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
		layout_info.pBindings    = bindings.data();
		Check(m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_set_layout), "set layout");
		vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, 16};
		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.setLayoutCount         = 1;
		pipeline_layout_info.pSetLayouts            = &m_set_layout;
		pipeline_layout_info.pushConstantRangeCount = 1;
		pipeline_layout_info.pPushConstantRanges    = &push;
		Check(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr, &m_pipeline_layout),
		      "pipeline layout");
		vk::ShaderModuleCreateInfo module_info {};
		module_info.codeSize = sizeof(kStressSampleSpirv);
		module_info.pCode    = kStressSampleSpirv;
		vk::ShaderModule module = nullptr;
		Check(m_graphics.device.createShaderModule(&module_info, nullptr, &module), "shader module");
		vk::ComputePipelineCreateInfo pipeline_info {};
		pipeline_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
		pipeline_info.stage.module = module;
		pipeline_info.stage.pName  = "main";
		pipeline_info.layout       = m_pipeline_layout;
		Check(m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline),
		      "compute pipeline");
		m_graphics.device.destroyShaderModule(module, nullptr);
		std::array<vk::DescriptorPoolSize, 2> sizes {
		    vk::DescriptorPoolSize {vk::DescriptorType::eCombinedImageSampler, Slots * InFlight},
		    vk::DescriptorPoolSize {vk::DescriptorType::eStorageBuffer, 2 * InFlight}};
		vk::DescriptorPoolCreateInfo pool_info {};
		pool_info.maxSets       = InFlight;
		pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
		pool_info.pPoolSizes    = sizes.data();
		Check(m_graphics.device.createDescriptorPool(&pool_info, nullptr, &m_descriptor_pool), "descriptor pool");
		std::array<vk::DescriptorSetLayout, InFlight> layouts {};
		layouts.fill(m_set_layout);
		vk::DescriptorSetAllocateInfo set_info {};
		set_info.descriptorPool     = m_descriptor_pool;
		set_info.descriptorSetCount = InFlight;
		set_info.pSetLayouts        = layouts.data();
		Check(m_graphics.device.allocateDescriptorSets(&set_info, m_sets.data()), "descriptor sets");
	}

	struct HostBuffer {
		vk::Buffer    buffer     = nullptr;
		VmaAllocation allocation = nullptr;
		void*         mapped     = nullptr;
	};

	HostBuffer MakeBuffer(uint64_t size, vk::BufferUsageFlags usage, bool host) {
		vk::BufferCreateInfo info {};
		info.size  = size;
		info.usage = usage;
		VmaAllocationCreateInfo create {};
		create.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		if (host) {
			create.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		}
		HostBuffer        buffer;
		VmaAllocationInfo result {};
		VkBuffer          native = VK_NULL_HANDLE;
		Check(static_cast<vk::Result>(vmaCreateBuffer(m_graphics.allocator, &static_cast<const VkBufferCreateInfo&>(info),
		                                              &create, &native, &buffer.allocation, &result)),
		      "vmaCreateBuffer");
		buffer.buffer = native;
		buffer.mapped = result.pMappedData;
		return buffer;
	}

	void CreateBuffers() {
		m_staging = MakeBuffer(StagingBytes, vk::BufferUsageFlagBits::eTransferSrc, true);
		auto* bytes = static_cast<uint8_t*>(m_staging.mapped);
		for (uint64_t i = 0; i < StagingBytes; i++) {
			bytes[i] = static_cast<uint8_t>((i * 2654435761u) >> 13u);
		}
		vmaFlushAllocation(m_graphics.allocator, m_staging.allocation, 0, VK_WHOLE_SIZE);
		for (auto& output: m_outputs) {
			output = MakeBuffer(OutputBytes, vk::BufferUsageFlagBits::eStorageBuffer, false);
		}
		m_side_target = MakeBuffer(SideBytes, vk::BufferUsageFlagBits::eTransferDst, true);
		// The BDA page table's shape: a sparse residency storage buffer, unbound ranges read zero.
		vk::BufferCreateInfo table_info {};
		table_info.size  = TableBlocks * 65536;
		table_info.usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
		table_info.flags = vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
		Check(m_graphics.device.createBuffer(&table_info, nullptr, &m_table), "create sparse table");
		vk::MemoryRequirements requirements {};
		m_graphics.device.getBufferMemoryRequirements(m_table, &requirements);
		m_table_block      = requirements.alignment;
		m_table_type_bits  = requirements.memoryTypeBits;
		if (table_info.size % m_table_block != 0) {
			Fail("sparse table block size");
		}
		vk::FenceCreateInfo fence_info {};
		Check(m_graphics.device.createFence(&fence_info, nullptr, &m_table_fence), "table fence");
		BindTableBlocks(1); // block 0 before the first dispatch reads it
	}

	// One texture: a sparse residency image created by the emulator's own code with memory behind
	// levels >= first only, its format and usage as Image::Image picks them for a sampled texture.
	void CreateTexture(uint32_t slot) {
		auto& texture = m_textures[slot];
		static const struct {
			vk::Format format;
			uint32_t   block;
			uint32_t   bytes;
		} formats[] = {{vk::Format::eR8G8B8A8Unorm, 1, 4},
		               {vk::Format::eR16G16B16A16Sfloat, 1, 8},
		               {vk::Format::eBc7UnormBlock, 4, 16},
		               {vk::Format::eBc1RgbaUnormBlock, 4, 8},
		               {vk::Format::eR8Unorm, 1, 1}};
		static const uint32_t sizes[] = {128, 256, 512, 1024, 2048, 300, 1000};
		for (;;) {
			const auto& format = formats[m_random() % std::size(formats)];
			const auto  width  = sizes[m_random() % std::size(sizes)];
			const auto  height = sizes[m_random() % std::size(sizes)];
			uint32_t    levels = 1;
			while ((width >> levels) != 0 || (height >> levels) != 0) {
				levels++;
			}
			vk::ImageCreateInfo info {};
			info.imageType     = vk::ImageType::e2D;
			info.format        = format.format;
			info.extent        = vk::Extent3D {width, height, 1};
			info.mipLevels     = levels;
			info.arrayLayers   = 1;
			info.samples       = vk::SampleCountFlagBits::e1;
			info.tiling        = vk::ImageTiling::eOptimal;
			info.initialLayout = vk::ImageLayout::eUndefined;
			info.sharingMode   = vk::SharingMode::eExclusive;
			info.flags         = vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage;
			info.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
			             vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
			if (format.block == 1) {
				info.usage |= vk::ImageUsageFlagBits::eColorAttachment;
			} else {
				info.flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
			}
			const uint32_t first = 1 + static_cast<uint32_t>(m_random() % std::min(levels - 1, 4u));
			texture       = Texture {};
			if (!m_graphics.CreateSparseImage(info, first, *texture.image)) {
				continue; // this format/usage cannot be sparse here
			}
			texture.live        = true;
			texture.format      = format.format;
			texture.block       = format.block;
			texture.block_bytes = format.bytes;
			texture.first       = first;
			texture.layout      = vk::ImageLayout::eUndefined;
			texture.view        = MakeView(texture);
			m_created++;
			return;
		}
	}

	vk::ImageView MakeView(const Texture& texture) {
		vk::ImageViewMinLodCreateInfoEXT min_lod {};
		min_lod.minLod = static_cast<float>(texture.first);
		vk::ImageViewCreateInfo info {};
		if (!m_options.all_levels) {
			info.pNext = &min_lod;
		}
		vk::ImageViewUsageCreateInfo usage {};
		usage.usage = vk::ImageUsageFlagBits::eSampled;
		min_lod.pNext = &usage;
		if (m_options.all_levels) {
			info.pNext = &usage;
		}
		info.image            = texture.image->image;
		info.viewType         = vk::ImageViewType::e2D;
		info.format           = texture.format;
		info.subresourceRange = vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0,
		                                                   texture.image->mip_levels, 0, 1};
		vk::ImageView view = nullptr;
		Check(m_graphics.device.createImageView(&info, nullptr, &view), "create image view");
		return view;
	}

	void Retire(Texture& texture) {
		auto* image = texture.image.release();
		texture.image = std::make_unique<VulkanImage>();
		m_retired.push_back({texture.last_use, texture.view, image});
		texture.view = nullptr;
		texture.live = false;
		m_retired_count++;
	}

	void CollectRetired(uint64_t completed) {
		size_t kept = 0;
		for (auto& retired: m_retired) {
			if (retired.tick > completed) {
				m_retired[kept++] = retired;
				continue;
			}
			if (retired.view) {
				m_graphics.device.destroyImageView(retired.view, nullptr);
			}
			if (retired.image != nullptr) {
				m_graphics.DeleteImage(*retired.image); // destroys the image, frees its memory
				delete retired.image;
			}
		}
		m_retired.resize(kept);
	}

	uint64_t Completed() {
		uint64_t value = 0;
		Check(m_graphics.device.getSemaphoreCounterValue(m_timeline, &value), "timeline counter");
		return value;
	}

	void WaitTick(uint64_t tick) {
		vk::SemaphoreWaitInfo wait {};
		wait.semaphoreCount = 1;
		wait.pSemaphores    = &m_timeline;
		wait.pValues        = &tick;
		Check(m_graphics.device.waitSemaphores(&wait, UINT64_MAX), "wait timeline");
	}

	void Iterate(uint32_t iteration) {
		const auto slot_index = iteration % InFlight;
		if (m_tick >= InFlight) {
			WaitTick(m_tick - InFlight + 1);
		}
		CollectRetired(Completed());
		// While the previous InFlight-1 submissions run: grow some textures (TextureCache::
		// EnsureResidency -> BindSparseImageLevels on the side queue, ordered after the submitted
		// ticks and before the next one on the GPU),
		// retire others whose last submission may still run (deleted once it completed) and
		// create replacements (CreateSparseImage).
		const uint32_t changes = 2 + static_cast<uint32_t>(m_random() % 6);
		for (uint32_t change = 0; change < changes; change++) {
			auto& texture = m_textures[m_random() % Slots];
			if (!texture.live) {
				continue;
			}
			if (texture.first > 0 && (m_random() % 3) != 0) {
				const auto first = texture.first - 1 - static_cast<uint32_t>(m_random() % texture.first);
				// TextureCache::BindSparseLevelsAfterSubmitted: after every submitted tick (all are).
				m_graphics.BindSparseImageLevels(*texture.image, first, m_tick != 0 ? m_timeline : vk::Semaphore {}, m_tick);
				texture.first = first;
				// The new levels need their view: the old one retires with the submissions using it.
				m_retired.push_back({texture.last_use, texture.view, nullptr});
				texture.view = MakeView(texture);
				texture.grows++;
				m_growths++;
			} else {
				Retire(texture);
			}
		}
		for (uint32_t slot = 0; slot < Slots; slot++) {
			if (!m_textures[slot].live) {
				CreateTexture(slot);
			}
		}
		Record(slot_index);
		Submit(slot_index);
	}

	void Record(uint32_t slot_index) {
		auto command = m_commands[slot_index];
		Check(command.reset(), "reset command buffer");
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		Check(command.begin(&begin), "begin");
		// Uploads of every resident level that has no texels yet (new images, new levels): the
		// whole chain goes to TRANSFER_DST and back, as TextureCache's refresh barriers do.
		std::vector<vk::ImageMemoryBarrier2> before;
		std::vector<vk::ImageMemoryBarrier2> after;
		std::vector<uint32_t>                uploads;
		for (uint32_t slot = 0; slot < Slots; slot++) {
			auto& texture = m_textures[slot];
			if (texture.uploaded <= texture.first) {
				continue;
			}
			uploads.push_back(slot);
			const auto levels = texture.image->mip_levels;
			vk::ImageMemoryBarrier2 barrier {};
			barrier.srcStageMask     = vk::PipelineStageFlagBits2::eComputeShader;
			barrier.srcAccessMask    = vk::AccessFlagBits2::eNone;
			barrier.dstStageMask     = vk::PipelineStageFlagBits2::eCopy;
			barrier.dstAccessMask    = vk::AccessFlagBits2::eTransferWrite;
			barrier.oldLayout        = texture.layout;
			barrier.newLayout        = vk::ImageLayout::eTransferDstOptimal;
			barrier.image            = texture.image->image;
			barrier.subresourceRange =
			    vk::ImageSubresourceRange {vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1};
			if (m_options.bound_barriers) {
				// Only levels with memory: never-bound levels are untouched, newly bound ones start
				// UNDEFINED, the ones uploaded before keep their layout.
				const auto previous = std::min(texture.uploaded, levels);
				barrier.subresourceRange.baseMipLevel = texture.first;
				barrier.subresourceRange.levelCount   = previous - texture.first;
				barrier.oldLayout                     = vk::ImageLayout::eUndefined;
				before.push_back(barrier);
				if (previous < levels && texture.layout != vk::ImageLayout::eUndefined) {
					barrier.subresourceRange.baseMipLevel = previous;
					barrier.subresourceRange.levelCount   = levels - previous;
					barrier.oldLayout                     = texture.layout;
					before.push_back(barrier);
				}
				barrier.subresourceRange.baseMipLevel = texture.first;
				barrier.subresourceRange.levelCount   = levels - texture.first;
			} else {
				before.push_back(barrier);
			}
			barrier.srcStageMask  = vk::PipelineStageFlagBits2::eCopy;
			barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
			barrier.dstStageMask  = vk::PipelineStageFlagBits2::eComputeShader;
			barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
			barrier.oldLayout     = vk::ImageLayout::eTransferDstOptimal;
			barrier.newLayout     = vk::ImageLayout::eShaderReadOnlyOptimal;
			after.push_back(barrier);
		}
		Barriers(command, before);
		for (const auto slot: uploads) {
			auto& texture = m_textures[slot];
			const auto end = std::min(texture.uploaded, texture.image->mip_levels);
			std::vector<vk::BufferImageCopy> regions;
			for (uint32_t level = texture.first; level < end; level++) {
				const auto width  = std::max(texture.image->extent.width >> level, 1u);
				const auto height = std::max(texture.image->extent.height >> level, 1u);
				const uint64_t bytes = uint64_t {(width + texture.block - 1) / texture.block} *
				                       ((height + texture.block - 1) / texture.block) * texture.block_bytes;
				if (bytes > StagingBytes) {
					Fail("staging buffer too small");
				}
				vk::BufferImageCopy region {};
				region.imageSubresource = vk::ImageSubresourceLayers {vk::ImageAspectFlagBits::eColor, level, 0, 1};
				region.imageExtent      = vk::Extent3D {width, height, 1};
				regions.push_back(region);
			}
			command.copyBufferToImage(m_staging.buffer, texture.image->image, vk::ImageLayout::eTransferDstOptimal,
			                          static_cast<uint32_t>(regions.size()), regions.data());
			texture.uploaded = texture.first;
			texture.layout   = vk::ImageLayout::eShaderReadOnlyOptimal;
		}
		Barriers(command, after);
		// The dispatch: every texture, every level its view allows, and the table's bound blocks.
		std::array<vk::DescriptorImageInfo, Slots> images {};
		for (uint32_t slot = 0; slot < Slots; slot++) {
			images[slot] = vk::DescriptorImageInfo {m_sampler, m_textures[slot].view,
			                                        vk::ImageLayout::eShaderReadOnlyOptimal};
			m_textures[slot].last_use = m_tick + 1;
		}
		const vk::DescriptorBufferInfo output {m_outputs[slot_index].buffer, 0, VK_WHOLE_SIZE};
		const uint64_t                 bound = m_table_bound.load(std::memory_order_acquire);
		const vk::DescriptorBufferInfo table {m_table, 0, VK_WHOLE_SIZE};
		std::array<vk::WriteDescriptorSet, 3> writes {};
		writes[0].dstSet          = m_sets[slot_index];
		writes[0].dstBinding      = 0;
		writes[0].descriptorCount = Slots;
		writes[0].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
		writes[0].pImageInfo      = images.data();
		writes[1].dstSet          = m_sets[slot_index];
		writes[1].dstBinding      = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[1].pBufferInfo     = &output;
		writes[2]                 = writes[1];
		writes[2].dstBinding      = 2;
		writes[2].pBufferInfo     = &table;
		m_graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, 1, &m_sets[slot_index], 0,
		                           nullptr);
		struct {
			uint32_t iterations;
			uint32_t texture_count;
			uint32_t table_words;
			float    max_lod;
		} params {m_options.work, Slots, static_cast<uint32_t>(bound * m_table_block / 4), 12.0f};
		command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
		command.dispatch(Slots * 16, 1, 1);
		Check(command.end(), "end");
	}

	static void Barriers(vk::CommandBuffer command, const std::vector<vk::ImageMemoryBarrier2>& barriers) {
		if (barriers.empty()) {
			return;
		}
		vk::DependencyInfo dependency {};
		dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
		dependency.pImageMemoryBarriers    = barriers.data();
		command.pipelineBarrier2(&dependency);
	}

	void Submit(uint32_t slot_index) {
		const uint64_t                  value = ++m_tick;
		vk::TimelineSemaphoreSubmitInfo timeline {};
		timeline.signalSemaphoreValueCount = 1;
		timeline.pSignalSemaphoreValues    = &value;
		// CommandScheduler::Submit: every batch waits for the image binds submitted before it.
		const uint64_t               sparse      = m_graphics.SparseBindPendingValue();
		const vk::Semaphore          sparse_sem  = m_graphics.SparseBindSemaphore();
		const vk::PipelineStageFlags sparse_wait = vk::PipelineStageFlagBits::eAllCommands;
		timeline.waitSemaphoreValueCount        = sparse != 0 ? 1u : 0u;
		timeline.pWaitSemaphoreValues           = &sparse;
		vk::SubmitInfo submit {};
		submit.pNext                = &timeline;
		submit.waitSemaphoreCount   = sparse != 0 ? 1u : 0u;
		submit.pWaitSemaphores      = &sparse_sem;
		submit.pWaitDstStageMask    = &sparse_wait;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &m_commands[slot_index];
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &m_timeline;
		{
			Common::LockGuard lock(m_graphics.queue_mutex);
			Check(m_graphics.queue.submit(1, &submit, nullptr), "vkQueueSubmit");
		}
		m_submitted.store(value, std::memory_order_release);
	}

	// BufferCache's side readbacks: copies on the side queue that wait on a main-queue tick already
	// submitted, so the side queue (and the sparse binds behind them) stalls until it completed.
	void SideReadbacks() {
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = m_side_pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		vk::CommandBuffer command = nullptr;
		Check(m_graphics.device.allocateCommandBuffers(&allocate, &command), "side command buffer");
		uint64_t side_value = 0;
		while (!m_stop.load()) {
			const auto producer = m_submitted.load(std::memory_order_acquire);
			if (producer == 0 || producer == m_side_waited) {
				std::this_thread::sleep_for(std::chrono::microseconds(200));
				continue;
			}
			m_side_waited = producer;
			Check(command.reset(), "reset side");
			vk::CommandBufferBeginInfo begin {};
			begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
			Check(command.begin(&begin), "begin side");
			const vk::BufferCopy region {0, 0, SideBytes};
			command.copyBuffer(m_staging.buffer, m_side_target.buffer, 1, &region);
			Check(command.end(), "end side");
			const uint64_t                  signal = ++side_value;
			const vk::PipelineStageFlags    stage  = vk::PipelineStageFlagBits::eTransfer;
			vk::TimelineSemaphoreSubmitInfo timeline {};
			timeline.waitSemaphoreValueCount   = 1;
			timeline.pWaitSemaphoreValues      = &producer;
			timeline.signalSemaphoreValueCount = 1;
			timeline.pSignalSemaphoreValues    = &signal;
			vk::SubmitInfo submit {};
			submit.pNext                = &timeline;
			submit.waitSemaphoreCount   = 1;
			submit.pWaitSemaphores      = &m_timeline;
			submit.pWaitDstStageMask    = &stage;
			submit.commandBufferCount   = 1;
			submit.pCommandBuffers      = &command;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores    = &m_side_timeline;
			{
				Common::LockGuard lock(m_graphics.side_queue_mutex);
				Check(m_graphics.side_queue.submit(1, &submit, nullptr), "side readback submit");
			}
			vk::SemaphoreWaitInfo wait {};
			wait.semaphoreCount = 1;
			wait.pSemaphores    = &m_side_timeline;
			wait.pValues        = &signal;
			Check(m_graphics.device.waitSemaphores(&wait, UINT64_MAX), "side readback wait");
			m_side_copies++;
		}
	}

	// BufferCache's SparsePageTable: zero-filled chunks bound block by block on the side queue,
	// each waited for on the host. Dispatches read only blocks bound before they were recorded.
	void TableGrowth() {
		while (!m_stop.load()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			const auto bound = m_table_bound.load();
			if (bound < TableBlocks) {
				BindTableBlocks(std::min<uint64_t>(TableBlocks - bound, 1 + bound % 4));
			}
		}
	}

	void TableSubmitAndWait(const std::function<vk::Result(vk::Queue)>& submit, const char* what) {
		{
			Common::LockGuard lock(m_graphics.side_queue_mutex);
			Check(submit(m_graphics.side_queue), what);
		}
		Check(m_graphics.device.waitForFences(1, &m_table_fence, VK_TRUE, UINT64_MAX), what);
		Check(m_graphics.device.resetFences(1, &m_table_fence), what);
	}

	void BindTableBlocks(uint64_t count) {
		const uint64_t bytes = count * m_table_block;
		vk::BufferCreateInfo zero_info {};
		zero_info.size  = bytes;
		zero_info.usage = vk::BufferUsageFlagBits::eTransferDst;
		vk::Buffer zero = nullptr;
		Check(m_graphics.device.createBuffer(&zero_info, nullptr, &zero), "table zero buffer");
		vk::MemoryRequirements zero_requirements {};
		m_graphics.device.getBufferMemoryRequirements(zero, &zero_requirements);
		VkMemoryRequirements requirements {};
		requirements.size           = bytes;
		requirements.alignment      = std::max<uint64_t>(m_table_block, zero_requirements.alignment);
		requirements.memoryTypeBits = m_table_type_bits & zero_requirements.memoryTypeBits;
		VmaAllocationCreateInfo create {};
		create.flags         = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		create.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VmaAllocation     allocation = nullptr;
		VmaAllocationInfo info {};
		Check(static_cast<vk::Result>(
		          vmaAllocateMemory(m_graphics.allocator, &requirements, &create, &allocation, &info)),
		      "table memory");
		Check(static_cast<vk::Result>(vmaBindBufferMemory(m_graphics.allocator, allocation, zero)), "bind zero");
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool        = m_table_pool;
		allocate.level              = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = 1;
		vk::CommandBuffer command   = nullptr;
		Check(m_graphics.device.allocateCommandBuffers(&allocate, &command), "table command buffer");
		vk::CommandBufferBeginInfo begin {};
		begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		Check(command.begin(&begin), "begin table");
		command.fillBuffer(zero, 0, VK_WHOLE_SIZE, 0);
		Check(command.end(), "end table");
		vk::SubmitInfo submit {};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &command;
		TableSubmitAndWait([&](vk::Queue queue) { return queue.submit(1, &submit, m_table_fence); }, "zero table");
		m_graphics.device.freeCommandBuffers(m_table_pool, 1, &command);
		m_graphics.device.destroyBuffer(zero, nullptr);
		const auto                        first = m_table_bound.load();
		std::vector<vk::SparseMemoryBind> binds;
		for (uint64_t block = 0; block < count; block++) {
			vk::SparseMemoryBind bind {};
			bind.resourceOffset = (first + block) * m_table_block;
			bind.size           = m_table_block;
			bind.memory         = info.deviceMemory;
			bind.memoryOffset   = info.offset + block * m_table_block;
			binds.push_back(bind);
		}
		vk::SparseBufferMemoryBindInfo buffer_bind {m_table, static_cast<uint32_t>(binds.size()), binds.data()};
		vk::BindSparseInfo             bind_info {};
		bind_info.bufferBindCount = 1;
		bind_info.pBufferBinds    = &buffer_bind;
		TableSubmitAndWait([&](vk::Queue queue) { return queue.bindSparse(1, &bind_info, m_table_fence); },
		                   "bind table");
		m_table_allocations.push_back(allocation);
		m_table_bound.store(first + count, std::memory_order_release);
	}

	void Shutdown() {
		for (auto& texture: m_textures) {
			if (texture.live) {
				Retire(texture);
			}
		}
		CollectRetired(UINT64_MAX);
		auto& device = m_graphics.device;
		device.destroyBuffer(m_table, nullptr);
		for (const auto allocation: m_table_allocations) {
			vmaFreeMemory(m_graphics.allocator, allocation);
		}
		device.destroyFence(m_table_fence, nullptr);
		vmaDestroyBuffer(m_graphics.allocator, m_staging.buffer, m_staging.allocation);
		vmaDestroyBuffer(m_graphics.allocator, m_side_target.buffer, m_side_target.allocation);
		for (auto& output: m_outputs) {
			vmaDestroyBuffer(m_graphics.allocator, output.buffer, output.allocation);
		}
		device.destroyDescriptorPool(m_descriptor_pool, nullptr);
		device.destroyPipeline(m_pipeline, nullptr);
		device.destroyPipelineLayout(m_pipeline_layout, nullptr);
		device.destroyDescriptorSetLayout(m_set_layout, nullptr);
		device.destroySampler(m_sampler, nullptr);
		device.destroyCommandPool(m_pool, nullptr);
		device.destroyCommandPool(m_side_pool, nullptr);
		device.destroyCommandPool(m_table_pool, nullptr);
		device.destroySemaphore(m_timeline, nullptr);
		device.destroySemaphore(m_side_timeline, nullptr);
		m_graphics.DestroyAllocator();
		device.destroy(nullptr);
		if (m_messenger) {
			m_graphics.instance.destroyDebugUtilsMessengerEXT(m_messenger, nullptr);
		}
		m_graphics.instance.destroy(nullptr);
	}

	static constexpr uint64_t StagingBytes = 32ull << 20u;
	static constexpr uint64_t OutputBytes  = uint64_t {Slots} * 16 * 64 * 16;
	static constexpr uint64_t SideBytes    = 1ull << 20u;

	Options                                m_options;
	std::mt19937                           m_random;
	GraphicContext                         m_graphics;
	vk::DebugUtilsMessengerEXT             m_messenger       = nullptr;
	vk::Semaphore                          m_timeline        = nullptr;
	vk::Semaphore                          m_side_timeline   = nullptr;
	vk::CommandPool                        m_pool            = nullptr;
	vk::CommandPool                        m_side_pool       = nullptr;
	vk::CommandPool                        m_table_pool      = nullptr;
	std::array<vk::CommandBuffer, InFlight> m_commands       {};
	vk::Sampler                            m_sampler         = nullptr;
	vk::DescriptorSetLayout                m_set_layout      = nullptr;
	vk::PipelineLayout                     m_pipeline_layout = nullptr;
	vk::Pipeline                           m_pipeline        = nullptr;
	vk::DescriptorPool                     m_descriptor_pool = nullptr;
	std::array<vk::DescriptorSet, InFlight> m_sets           {};
	HostBuffer                             m_staging;
	HostBuffer                             m_side_target;
	std::array<HostBuffer, InFlight>       m_outputs;
	vk::Buffer                             m_table           = nullptr;
	uint64_t                               m_table_block     = 0;
	uint32_t                               m_table_type_bits = 0;
	vk::Fence                              m_table_fence     = nullptr;
	std::vector<VmaAllocation>             m_table_allocations;
	std::atomic<uint64_t>                  m_table_bound {0};
	std::array<Texture, Slots>             m_textures;
	std::vector<Retired>                   m_retired;
	uint64_t                               m_tick = 0;
	std::atomic<uint64_t>                  m_submitted {0};
	uint64_t                               m_side_waited = 0;
	std::atomic<uint64_t>                  m_side_copies {0};
	std::atomic<bool>                      m_stop {false};
	uint64_t                               m_created       = 0;
	uint64_t                               m_growths       = 0;
	uint64_t                               m_retired_count = 0;
};

} // namespace

int main(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; i++) {
		const std::string arg  = argv[i];
		const char*       next = i + 1 < argc ? argv[i + 1] : "";
		if (arg == "--iterations") {
			options.iterations = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
			i++;
		} else if (arg == "--validation") {
			options.validation = next;
			i++;
		} else if (arg == "--views") {
			options.all_levels = std::strcmp(next, "all") == 0;
			i++;
		} else if (arg == "--work") {
			options.work = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
			i++;
		} else if (arg == "--barriers") {
			options.bound_barriers = std::strcmp(next, "bound") == 0;
			i++;
		} else if (arg == "--seed") {
			options.seed = static_cast<uint32_t>(std::strtoul(next, nullptr, 10));
			i++;
		} else {
			std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
			return 2;
		}
	}
	if (options.validation != "off" && options.validation != "core" && options.validation != "sync" &&
	    options.validation != "gpuav") {
		std::fprintf(stderr, "--validation off|core|sync|gpuav\n");
		return 2;
	}
	Stress stress(options);
	return stress.Run();
}