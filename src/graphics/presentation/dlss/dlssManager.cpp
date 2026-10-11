#include "graphics/presentation/dlss/dlssManager.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_helpers_vk.h>

#include <algorithm>
#include <filesystem>
#include <windows.h>

namespace Libs::Graphics {

static void StreamlineLogCallback(sl::LogType type, const char* msg) {
	if (msg == nullptr) {
		return;
	}
	switch (type) {
		case sl::LogType::eError:
			std::fprintf(stderr, "[Streamline/DLSS][Error] %s\n", msg);
			break;
		case sl::LogType::eWarn:
			std::printf("[Streamline/DLSS][Warn] %s\n", msg);
			break;
		case sl::LogType::eInfo:
		default:
			std::printf("[Streamline/DLSS][Info] %s\n", msg);
			break;
	}
	std::fflush(stdout);
}

static sl::DLSSMode ConvertDLSSMode(DLSSMode mode) noexcept {
	switch (mode) {
		case DLSSMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
		case DLSSMode::Performance: return sl::DLSSMode::eMaxPerformance;
		case DLSSMode::Balanced: return sl::DLSSMode::eBalanced;
		case DLSSMode::Quality: return sl::DLSSMode::eMaxQuality;
		case DLSSMode::DLAA: return sl::DLSSMode::eDLAA;
		case DLSSMode::Off:
		default: return sl::DLSSMode::eOff;
	}
}

static sl::DLSSPreset ConvertDLSSPreset(DLSSPreset preset) noexcept {
	switch (preset) {
		case DLSSPreset::PresetJ: return sl::DLSSPreset::ePresetJ;
		case DLSSPreset::PresetK: return sl::DLSSPreset::ePresetK;
		case DLSSPreset::PresetL: return sl::DLSSPreset::ePresetL;
		case DLSSPreset::PresetM: return sl::DLSSPreset::ePresetM;
		case DLSSPreset::Default:
		default: return sl::DLSSPreset::eDefault;
	}
}

static sl::ReflexMode ConvertReflexMode(ReflexMode mode) noexcept {
	switch (mode) {
		case ReflexMode::Boost: return sl::ReflexMode::eLowLatencyWithBoost;
		case ReflexMode::On: return sl::ReflexMode::eLowLatency;
		case ReflexMode::Off:
		default: return sl::ReflexMode::eOff;
	}
}

static VkDeviceMemory GetImageMemory(GraphicContext& graphics, VmaAllocation allocation) {
	if (graphics.allocator == nullptr || allocation == nullptr) {
		return VK_NULL_HANDLE;
	}
	VmaAllocationInfo alloc_info {};
	vmaGetAllocationInfo(graphics.allocator, allocation, &alloc_info);
	return alloc_info.deviceMemory;
}

DLSSManager& DLSSManager::Instance() {
	static DLSSManager s_instance;
	return s_instance;
}

DLSSManager::DLSSManager() = default;

DLSSManager::~DLSSManager() = default;

void DLSSManager::UpdateSettings(const DLSSSettings& settings) {
	m_settings = settings;
	if (!m_settings.enabled) {
		m_status = DLSSStatus::Disabled;
		m_status_message = "DLSS Disabled in configuration";
	} else if (m_available) {
		m_status = DLSSStatus::Available;
		m_status_message = "DLSS Available and ready";
	}
}

bool DLSSManager::SetupStreamline(GraphicContext& graphics) {
	if (m_streamline_loaded) {
		return true;
	}

	m_status = DLSSStatus::Initializing;
	m_status_message = "Initializing NVIDIA Streamline...";

	sl::Preferences pref {};
	pref.showConsole = false;
	pref.logLevel = m_settings.diagnostics ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	pref.logMessageCallback = StreamlineLogCallback;

	wchar_t module_path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, module_path, MAX_PATH);
	std::filesystem::path bin_dir = std::filesystem::path(module_path).parent_path();
	std::wstring bin_dir_w = bin_dir.wstring();
	const wchar_t* plugin_paths[] = { bin_dir_w.c_str() };
	pref.pathsToPlugins = plugin_paths;
	pref.numPathsToPlugins = 1;

	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
	             sl::PreferenceFlags::eUseFrameBasedResourceTagging |
	             sl::PreferenceFlags::eUseManualHooking;
	sl::Feature features[] = {
		sl::kFeatureDLSS,
		sl::kFeatureDLSS_G,
		sl::kFeatureReflex,
		sl::kFeaturePCL,
	};
	pref.featuresToLoad = features;
	pref.numFeaturesToLoad = 4;
	pref.renderAPI = sl::RenderAPI::eVulkan;
	pref.applicationId = 0x505335; // 'PS5' custom application ID
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";

	sl::Result res = slInit(pref, sl::kSDKVersion);
	if (res != sl::Result::eOk) {
		m_status = DLSSStatus::Error;
		m_status_message = fmt::format("Streamline slInit failed: code {}", static_cast<int>(res));
		LOGF("[DLSS] %s\n", m_status_message.c_str());
		return false;
	}

	HMODULE hInterposer = GetModuleHandleA("sl.interposer.dll");
	if (!hInterposer) {
		hInterposer = LoadLibraryA("sl.interposer.dll");
	}
	if (hInterposer) {
		m_pfn_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
			GetProcAddress(hInterposer, "vkQueuePresentKHR"));
		m_pfn_vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
			GetProcAddress(hInterposer, "vkAcquireNextImageKHR"));
		m_pfn_vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
			GetProcAddress(hInterposer, "vkCreateSwapchainKHR"));
		m_pfn_vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
			GetProcAddress(hInterposer, "vkDestroySwapchainKHR"));
#if defined(_WIN32)
		m_pfn_vkCreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
			GetProcAddress(hInterposer, "vkCreateWin32SurfaceKHR"));
		m_pfn_vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
			GetProcAddress(hInterposer, "vkDestroySurfaceKHR"));
#endif
		std::printf("[DLSS] Interposer Vulkan hooks resolved: QueuePresent=%p, AcquireNextImage=%p, CreateSwapchain=%p, DestroySwapchain=%p, CreateWin32Surface=%p, DestroySurface=%p\n",
		            (void*)m_pfn_vkQueuePresentKHR, (void*)m_pfn_vkAcquireNextImageKHR,
		            (void*)m_pfn_vkCreateSwapchainKHR, (void*)m_pfn_vkDestroySwapchainKHR,
#if defined(_WIN32)
		            (void*)m_pfn_vkCreateWin32SurfaceKHR, (void*)m_pfn_vkDestroySurfaceKHR);
#else
		            nullptr, nullptr);
#endif
		std::fflush(stdout);
	}

	sl::FeatureRequirements reqs_dlss {};
	sl::FeatureRequirements reqs_dlss_g {};
	uint32_t extra_compute_queues = 0;
	uint32_t extra_graphics_queues = 0;
	if (slGetFeatureRequirements(sl::kFeatureDLSS, reqs_dlss) == sl::Result::eOk) {
		extra_compute_queues += reqs_dlss.vkNumComputeQueuesRequired;
		extra_graphics_queues += reqs_dlss.vkNumGraphicsQueuesRequired;
		std::printf("[DLSS] DLSS feature requirements: compute queues=%u, graphics queues=%u\n",
		            reqs_dlss.vkNumComputeQueuesRequired, reqs_dlss.vkNumGraphicsQueuesRequired);
	}
	if (slGetFeatureRequirements(sl::kFeatureDLSS_G, reqs_dlss_g) == sl::Result::eOk) {
		extra_compute_queues += reqs_dlss_g.vkNumComputeQueuesRequired;
		extra_graphics_queues += reqs_dlss_g.vkNumGraphicsQueuesRequired;
		std::printf("[DLSS] DLSS-G feature requirements: compute queues=%u, graphics queues=%u, opticalFlow queues=%u\n",
		            reqs_dlss_g.vkNumComputeQueuesRequired, reqs_dlss_g.vkNumGraphicsQueuesRequired,
		            reqs_dlss_g.vkNumOpticalFlowQueuesRequired);
	}
	std::printf("[DLSS] Streamline requested extra queues: compute=%u, graphics=%u\n",
	            extra_compute_queues, extra_graphics_queues);

	const uint32_t host_queues = graphics.streamline_queue_start;
	const uint32_t compute_queues_reserved = (std::max)(extra_compute_queues, 1u);

	sl::VulkanInfo vk_info {};
	vk_info.device = static_cast<VkDevice>(graphics.device);
	vk_info.instance = static_cast<VkInstance>(graphics.instance);
	vk_info.physicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	vk_info.computeQueueFamily = graphics.queue_family;
	vk_info.computeQueueIndex = host_queues;
	vk_info.graphicsQueueFamily = graphics.queue_family;
	vk_info.graphicsQueueIndex = host_queues + compute_queues_reserved;

	std::printf("[DLSS] Configuring Streamline Vulkan queues: host queues=%u, computeQueue=(family %u, index %u), graphicsQueue=(family %u, index %u)\n",
	            host_queues, vk_info.computeQueueFamily, vk_info.computeQueueIndex,
	            vk_info.graphicsQueueFamily, vk_info.graphicsQueueIndex);
	std::fflush(stdout);

	res = slSetVulkanInfo(vk_info);
	if (res != sl::Result::eOk) {
		m_status = DLSSStatus::Error;
		m_status_message = fmt::format("Streamline slSetVulkanInfo failed: code {}", static_cast<int>(res));
		LOGF("[DLSS] %s\n", m_status_message.c_str());
		slShutdown();
		return false;
	}

	vk::PhysicalDeviceIDProperties id_props {};
	vk::PhysicalDeviceProperties2 props2 {};
	props2.pNext = &id_props;
	graphics.physical_device.getProperties2(&props2);

	std::printf("[DLSS] Initializing on GPU: %s (Vendor: 0x%04X, LUID valid: %d)\n",
	            graphics.GetPhysicalDeviceProperties().deviceName.data(),
	            graphics.GetPhysicalDeviceProperties().vendorID,
	            id_props.deviceLUIDValid ? 1 : 0);

	sl::AdapterInfo adapter_info {};
	if (id_props.deviceLUIDValid) {
		adapter_info.deviceLUID = id_props.deviceLUID.data();
		adapter_info.deviceLUIDSizeInBytes = static_cast<uint32_t>(id_props.deviceLUID.size());
	}
	res = slIsFeatureSupported(sl::kFeatureDLSS, adapter_info);
	std::printf("[DLSS] slIsFeatureSupported DLSS (LUID) result: %d\n", static_cast<int>(res));

	if (res != sl::Result::eOk) {
		adapter_info.deviceLUID = nullptr;
		adapter_info.deviceLUIDSizeInBytes = 0;
		adapter_info.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
		res = slIsFeatureSupported(sl::kFeatureDLSS, adapter_info);
		std::printf("[DLSS] slIsFeatureSupported DLSS (VkPhysicalDevice) result: %d\n", static_cast<int>(res));
	}

	// Explicitly ensure features are loaded for this device
	slSetFeatureLoaded(sl::kFeatureDLSS, true);
	bool is_loaded = false;
	const sl::Result load_res = slIsFeatureLoaded(sl::kFeatureDLSS, is_loaded);
	std::printf("[DLSS] slIsFeatureLoaded DLSS result: %d, loaded: %d\n", static_cast<int>(load_res), is_loaded ? 1 : 0);

	// Check Frame Generation (DLSS-G) support
	slSetFeatureLoaded(sl::kFeatureDLSS_G, true);
	bool fg_loaded = false;
	slIsFeatureLoaded(sl::kFeatureDLSS_G, fg_loaded);
	sl::Result fg_res = slIsFeatureSupported(sl::kFeatureDLSS_G, adapter_info);
	m_fg_supported = (fg_res == sl::Result::eOk || fg_loaded);
	std::printf("[DLSS] Frame Generation (DLSS-G) support: %d, loaded: %d\n",
	            static_cast<int>(fg_res), fg_loaded ? 1 : 0);

	// Check NVIDIA Reflex support
	slSetFeatureLoaded(sl::kFeatureReflex, true);
	bool reflex_loaded = false;
	slIsFeatureLoaded(sl::kFeatureReflex, reflex_loaded);
	sl::Result reflex_res = slIsFeatureSupported(sl::kFeatureReflex, adapter_info);
	m_reflex_supported = (reflex_res == sl::Result::eOk || reflex_loaded);
	std::printf("[DLSS] NVIDIA Reflex support: %d, loaded: %d\n",
	            static_cast<int>(reflex_res), reflex_loaded ? 1 : 0);

	slSetFeatureLoaded(sl::kFeaturePCL, true);

	// Query Frame Generation / Multi-Frame Generation capabilities
	sl::ViewportHandle viewport(m_viewport_id);
	sl::DLSSGState fg_state {};
	if (slDLSSGGetState(viewport, fg_state, nullptr) == sl::Result::eOk) {
		m_max_fg_frames = fg_state.numFramesToGenerateMax;
		m_dynamic_mfg_supported = (fg_state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
		std::printf("[DLSS] DLSS-G Capabilities: max generated frames = %u (MFG %s), dynamic MFG = %d\n",
		            m_max_fg_frames, m_max_fg_frames > 1 ? "Supported" : "Single FG only",
		            m_dynamic_mfg_supported ? 1 : 0);
	}

	if (res != sl::Result::eOk && !is_loaded) {
		// As a final check, test if DLSS optimal settings can be queried on the active device
		sl::DLSSOptions test_opt {};
		test_opt.mode = sl::DLSSMode::eMaxQuality;
		test_opt.outputWidth = 1920;
		test_opt.outputHeight = 1080;
		sl::DLSSOptimalSettings opt_settings {};
		if (slDLSSGetOptimalSettings(test_opt, opt_settings) == sl::Result::eOk) {
			std::printf("[DLSS] slDLSSGetOptimalSettings succeeded -> DLSS is supported and active!\n");
			res = sl::Result::eOk;
		} else {
			m_status = DLSSStatus::Unsupported;
			m_status_message = fmt::format("DLSS unsupported on GPU (res {}, load {})", static_cast<int>(res), static_cast<int>(load_res));
			std::printf("[DLSS] %s\n", m_status_message.c_str());
			std::fflush(stdout);
			m_available = false;
			return false;
		}
	}

	m_streamline_loaded = true;
	m_available = true;
	m_status = DLSSStatus::Available;
	m_status_message = "NVIDIA DLSS Initialized and Available";
	std::printf("[DLSS] Streamline and DLSS initialized successfully on GPU: %s\n",
	            graphics.GetPhysicalDeviceProperties().deviceName.data());
	std::fflush(stdout);
	return true;
}

bool DLSSManager::Initialize(GraphicContext& graphics, const DLSSSettings& settings) {
	m_settings = settings;

	if (!SetupStreamline(graphics)) {
		return false;
	}

	m_initialized = true;
	if (m_settings.enabled && m_settings.mode != DLSSMode::Off) {
		m_status = DLSSStatus::Available;
		m_status_message = "DLSS Enabled & Available";
	} else {
		m_status = DLSSStatus::Disabled;
		m_status_message = "DLSS Disabled";
	}

	return true;
}

void DLSSManager::CreateFallbackBuffers(GraphicContext& graphics, vk::Extent2D extent) {
	DestroyFallbackBuffers(graphics);

	// Fallback Depth: D32_SFLOAT, cleared to neutral 1.0f
	vk::ImageCreateInfo depth_info {};
	depth_info.sType         = vk::StructureType::eImageCreateInfo;
	depth_info.imageType     = vk::ImageType::e2D;
	depth_info.extent        = { extent.width, extent.height, 1 };
	depth_info.mipLevels     = 1;
	depth_info.arrayLayers   = 1;
	depth_info.format        = vk::Format::eD32Sfloat;
	depth_info.tiling        = vk::ImageTiling::eOptimal;
	depth_info.initialLayout = vk::ImageLayout::eUndefined;
	depth_info.usage         = vk::ImageUsageFlagBits::eDepthStencilAttachment |
	                           vk::ImageUsageFlagBits::eSampled |
	                           vk::ImageUsageFlagBits::eTransferDst;
	depth_info.sharingMode   = vk::SharingMode::eExclusive;
	depth_info.samples       = vk::SampleCountFlagBits::e1;

	if (graphics.CreateImage(depth_info, m_fallback_depth_image)) {
		vk::ImageViewCreateInfo view_info {};
		view_info.sType            = vk::StructureType::eImageViewCreateInfo;
		view_info.image            = m_fallback_depth_image.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = m_fallback_depth_image.format;
		view_info.subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 };
		if (graphics.device.createImageView(&view_info, nullptr, &m_fallback_depth_view) == vk::Result::eSuccess) {
			m_depth_extent = extent;
			m_depth_initialized = false;
		}
	}

	// Fallback Motion Vectors: R16G16_SFLOAT, cleared to neutral {0.0f, 0.0f}
	vk::ImageCreateInfo mvec_info {};
	mvec_info.sType         = vk::StructureType::eImageCreateInfo;
	mvec_info.imageType     = vk::ImageType::e2D;
	mvec_info.extent        = { extent.width, extent.height, 1 };
	mvec_info.mipLevels     = 1;
	mvec_info.arrayLayers   = 1;
	mvec_info.format        = vk::Format::eR16G16Sfloat;
	mvec_info.tiling        = vk::ImageTiling::eOptimal;
	mvec_info.initialLayout = vk::ImageLayout::eUndefined;
	mvec_info.usage         = vk::ImageUsageFlagBits::eStorage |
	                          vk::ImageUsageFlagBits::eColorAttachment |
	                          vk::ImageUsageFlagBits::eSampled |
	                          vk::ImageUsageFlagBits::eTransferDst;
	mvec_info.sharingMode   = vk::SharingMode::eExclusive;
	mvec_info.samples       = vk::SampleCountFlagBits::e1;

	if (graphics.CreateImage(mvec_info, m_fallback_mvec_image)) {
		vk::ImageViewCreateInfo view_info {};
		view_info.sType            = vk::StructureType::eImageViewCreateInfo;
		view_info.image            = m_fallback_mvec_image.image;
		view_info.viewType         = vk::ImageViewType::e2D;
		view_info.format           = m_fallback_mvec_image.format;
		view_info.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
		if (graphics.device.createImageView(&view_info, nullptr, &m_fallback_mvec_view) == vk::Result::eSuccess) {
			m_mvec_extent = extent;
			m_mvec_initialized = false;
		}
	}
}

void DLSSManager::DestroyFallbackBuffers(GraphicContext& graphics) {
	if (m_fallback_depth_view != nullptr) {
		graphics.device.destroyImageView(m_fallback_depth_view, nullptr);
		m_fallback_depth_view = nullptr;
	}
	if (m_fallback_depth_image.image != nullptr) {
		graphics.DeleteImage(m_fallback_depth_image);
	}
	m_depth_extent = {};
	m_depth_initialized = false;

	if (m_fallback_mvec_view != nullptr) {
		graphics.device.destroyImageView(m_fallback_mvec_view, nullptr);
		m_fallback_mvec_view = nullptr;
	}
	if (m_fallback_mvec_image.image != nullptr) {
		graphics.DeleteImage(m_fallback_mvec_image);
	}
	m_mvec_extent = {};
	m_mvec_initialized = false;
}

bool DLSSManager::PrepareResources(GraphicContext& graphics, vk::Extent2D input_extent,
                                   vk::Extent2D target_extent) {
	if (m_output_image.image != nullptr && m_output_extent.width == target_extent.width &&
	    m_output_extent.height == target_extent.height &&
	    m_depth_extent.width == input_extent.width && m_depth_extent.height == input_extent.height &&
	    m_applied_mode == m_settings.mode && m_applied_sharpness == m_settings.sharpness &&
	    m_applied_preset == m_settings.preset && m_applied_fg_mode == m_settings.fg_mode) {
		return true;
	}

	ReleaseResources(graphics);

	// Output buffer: RGBA8_UNORM at presentation target resolution
	vk::ImageCreateInfo out_info {};
	out_info.sType         = vk::StructureType::eImageCreateInfo;
	out_info.imageType     = vk::ImageType::e2D;
	out_info.extent        = { target_extent.width, target_extent.height, 1 };
	out_info.mipLevels     = 1;
	out_info.arrayLayers   = 1;
	out_info.format        = vk::Format::eR8G8B8A8Unorm;
	out_info.tiling        = vk::ImageTiling::eOptimal;
	out_info.initialLayout = vk::ImageLayout::eUndefined;
	out_info.usage         = vk::ImageUsageFlagBits::eStorage |
	                         vk::ImageUsageFlagBits::eColorAttachment |
	                         vk::ImageUsageFlagBits::eTransferSrc |
	                         vk::ImageUsageFlagBits::eSampled;
	out_info.sharingMode   = vk::SharingMode::eExclusive;
	out_info.samples       = vk::SampleCountFlagBits::e1;

	if (!graphics.CreateImage(out_info, m_output_image)) {
		m_status = DLSSStatus::Error;
		m_status_message = "Failed to create DLSS output image";
		LOGF("[DLSS] %s\n", m_status_message.c_str());
		return false;
	}

	vk::ImageViewCreateInfo view_info {};
	view_info.sType            = vk::StructureType::eImageViewCreateInfo;
	view_info.image            = m_output_image.image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = m_output_image.format;
	view_info.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
	if (graphics.device.createImageView(&view_info, nullptr, &m_output_view) != vk::Result::eSuccess) {
		m_status = DLSSStatus::Error;
		m_status_message = "Failed to create DLSS output image view";
		LOGF("[DLSS] %s\n", m_status_message.c_str());
		graphics.DeleteImage(m_output_image);
		return false;
	}
	m_output_extent = target_extent;

	CreateFallbackBuffers(graphics, input_extent);

	// Set DLSS Options in Streamline
	sl::ViewportHandle viewport(m_viewport_id);
	sl::DLSSOptions dlss_options {};
	auto effective_mode = m_settings.mode;
	if (input_extent.width >= target_extent.width && input_extent.height >= target_extent.height &&
	    effective_mode != DLSSMode::Off && effective_mode != DLSSMode::DLAA) {
		effective_mode = DLSSMode::DLAA;
	}
	dlss_options.mode = ConvertDLSSMode(effective_mode);
	dlss_options.outputWidth = target_extent.width;
	dlss_options.outputHeight = target_extent.height;
	dlss_options.sharpness = m_settings.sharpness;
	dlss_options.colorBuffersHDR = sl::Boolean::eFalse;
	dlss_options.qualityPreset = ConvertDLSSPreset(m_settings.preset);
	dlss_options.performancePreset = ConvertDLSSPreset(m_settings.preset);
	dlss_options.balancedPreset = ConvertDLSSPreset(m_settings.preset);
	dlss_options.ultraPerformancePreset = ConvertDLSSPreset(m_settings.preset);
	dlss_options.dlaaPreset = ConvertDLSSPreset(m_settings.preset);

	sl::Result res = slDLSSSetOptions(viewport, dlss_options);
	if (res != sl::Result::eOk) {
		LOGF("[DLSS] slDLSSSetOptions returned %d\n", static_cast<int>(res));
	}

	// Configure Frame Generation & Multi-Frame Generation (DLSS-G)
	if (m_fg_supported && m_settings.fg_enabled && m_settings.fg_mode != DLSSGMode::Off) {
		sl::DLSSGOptions g_options {};
		if (m_settings.fg_mode == DLSSGMode::MFG_Dynamic) {
			if (m_dynamic_mfg_supported) {
				g_options.mode = sl::DLSSGMode::eDynamic;
				g_options.dynamicTargetFrameRate = 0.0f; // Auto-align with display refresh rate
			} else {
				// Dynamic MFG is D3D12-only in current Streamline; in Vulkan backend, use maximum supported multiplier
				g_options.mode = sl::DLSSGMode::eOn;
				g_options.numFramesToGenerate = (m_max_fg_frames > 1) ? (std::min)(m_max_fg_frames - 1, 3u) : 1u;
				std::printf("[DLSS] Dynamic MFG is D3D12-only; falling back to fixed %ux multiplier for Vulkan\n",
				            g_options.numFramesToGenerate + 1);
			}
		} else {
			g_options.mode = sl::DLSSGMode::eOn;
			if (m_settings.fg_mode == DLSSGMode::MFG_4X) {
				g_options.numFramesToGenerate = 3; // 4x multiplier
			} else if (m_settings.fg_mode == DLSSGMode::MFG_3X) {
				g_options.numFramesToGenerate = 2; // 3x multiplier
			} else {
				g_options.numFramesToGenerate = 1; // 2x multiplier
			}
		}
		g_options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		g_options.numBackBuffers = 6;
		g_options.colorWidth = target_extent.width;
		g_options.colorHeight = target_extent.height;
		const auto format_to_use = (m_swapchain_format != vk::Format::eUndefined)
		                               ? static_cast<uint32_t>(m_swapchain_format)
		                               : static_cast<uint32_t>(m_output_image.format);
		g_options.colorBufferFormat = format_to_use;
		g_options.mvecDepthWidth = input_extent.width;
		g_options.mvecDepthHeight = input_extent.height;
		g_options.mvecBufferFormat = static_cast<uint32_t>(m_fallback_mvec_image.format);
		g_options.depthBufferFormat = static_cast<uint32_t>(m_fallback_depth_image.format);

		sl::Result g_res = slDLSSGSetOptions(viewport, g_options);
		m_fg_active = (g_res == sl::Result::eOk);
		if (g_res != sl::Result::eOk) {
			LOGF("[DLSS] slDLSSGSetOptions returned %d\n", static_cast<int>(g_res));
		} else {
			std::printf("[DLSS] Frame Generation active: mode %s (%u generated frame(s) per real frame)\n",
			            DLSSGModeToString(m_settings.fg_mode), g_options.numFramesToGenerate);
			std::fflush(stdout);
		}
	} else if (m_fg_supported) {
		sl::DLSSGOptions g_options {};
		g_options.mode = sl::DLSSGMode::eOff;
		slDLSSGSetOptions(viewport, g_options);
		m_fg_active = false;
	}

	// Configure NVIDIA Reflex Low Latency
	if (m_reflex_supported) {
		sl::ReflexOptions reflex_options {};
		if (m_settings.fg_enabled) {
			reflex_options.mode = (m_settings.reflex_mode == ReflexMode::Boost)
			                          ? sl::ReflexMode::eLowLatencyWithBoost
			                          : sl::ReflexMode::eLowLatency;
		} else {
			reflex_options.mode = ConvertReflexMode(m_settings.reflex_mode);
		}
		slReflexSetOptions(reflex_options);
	}

	if (m_settings.diagnostics) {
		sl::DLSSOptimalSettings opt {};
		if (slDLSSGetOptimalSettings(dlss_options, opt) == sl::Result::eOk) {
			LOGF("[DLSS][Diag] Mode: %s (Effective: %s), Target: %ux%u, Optimal Render: %ux%u (Min: %ux%u, Max: %ux%u)\n",
			     DLSSModeToString(m_settings.mode), DLSSModeToString(effective_mode),
			     target_extent.width, target_extent.height,
			     opt.optimalRenderWidth, opt.optimalRenderHeight, opt.renderWidthMin, opt.renderHeightMin,
			     opt.renderWidthMax, opt.renderHeightMax);
		}
	}

	m_applied_mode      = m_settings.mode;
	m_applied_sharpness = m_settings.sharpness;
	m_applied_preset    = m_settings.preset;
	m_applied_fg_mode   = m_settings.fg_mode;

	return true;
}

bool DLSSManager::Evaluate(GraphicContext& graphics, vk::CommandBuffer cmd_buffer,
                           VulkanImage& source_image, vk::ImageView source_view,
                           vk::Extent2D source_extent, vk::Extent2D target_extent,
                           VulkanImage*& out_upscaled_image) {
	out_upscaled_image = nullptr;

	if (!m_settings.enabled || m_settings.mode == DLSSMode::Off || !m_available || !m_streamline_loaded) {
		return false;
	}

	if (source_extent.width > target_extent.width || source_extent.height > target_extent.height) {
		// DLSS Super Resolution is an upscaler; it cannot downscale.
		// When the game renders at higher resolution than the output window (e.g. 4K frame to 720p window),
		// fall back smoothly to the standard presentation blit/downscale.
		m_status = DLSSStatus::Available;
		m_status_message = fmt::format("Input ({}x{}) > Target ({}x{})",
		                               source_extent.width, source_extent.height,
		                               target_extent.width, target_extent.height);
		return false;
	}

	if (!PrepareResources(graphics, source_extent, target_extent)) {
		return false;
	}

	const bool extent_changed = (source_extent.width != m_prev_input_extent.width ||
	                             source_extent.height != m_prev_input_extent.height ||
	                             target_extent.width != m_prev_target_extent.width ||
	                             target_extent.height != m_prev_target_extent.height);
	m_prev_input_extent = source_extent;
	m_prev_target_extent = target_extent;

	// Clear fallback depth buffer on initial run or resolution change
	if (!m_depth_initialized || extent_changed) {
		vk::ImageSubresourceRange depth_range { vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 };
		vk::ImageMemoryBarrier to_clear {};
		to_clear.sType               = vk::StructureType::eImageMemoryBarrier;
		to_clear.oldLayout           = vk::ImageLayout::eUndefined;
		to_clear.newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_clear.srcAccessMask       = vk::AccessFlagBits {};
		to_clear.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_clear.image               = m_fallback_depth_image.image;
		to_clear.subresourceRange    = depth_range;
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
		                           vk::PipelineStageFlagBits::eTransfer,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_clear);

		const vk::ClearDepthStencilValue clear_depth { 1.0f, 0 };
		cmd_buffer.clearDepthStencilImage(m_fallback_depth_image.image,
		                                  vk::ImageLayout::eTransferDstOptimal,
		                                  &clear_depth, 1, &depth_range);

		vk::ImageMemoryBarrier to_shader {};
		to_shader.sType               = vk::StructureType::eImageMemoryBarrier;
		to_shader.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_shader.newLayout           = vk::ImageLayout::eShaderReadOnlyOptimal;
		to_shader.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_shader.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
		to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.image               = m_fallback_depth_image.image;
		to_shader.subresourceRange    = depth_range;
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_shader);
		m_depth_initialized = true;
	}

	// Clear fallback motion vectors buffer on initial run or resolution change
	if (!m_mvec_initialized || extent_changed) {
		vk::ImageSubresourceRange mvec_range { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
		vk::ImageMemoryBarrier to_clear {};
		to_clear.sType               = vk::StructureType::eImageMemoryBarrier;
		to_clear.oldLayout           = vk::ImageLayout::eUndefined;
		to_clear.newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_clear.srcAccessMask       = vk::AccessFlagBits {};
		to_clear.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_clear.image               = m_fallback_mvec_image.image;
		to_clear.subresourceRange    = mvec_range;
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
		                           vk::PipelineStageFlagBits::eTransfer,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_clear);

		const vk::ClearColorValue clear_zero {};
		cmd_buffer.clearColorImage(m_fallback_mvec_image.image,
		                           vk::ImageLayout::eTransferDstOptimal,
		                           &clear_zero, 1, &mvec_range);

		vk::ImageMemoryBarrier to_shader {};
		to_shader.sType               = vk::StructureType::eImageMemoryBarrier;
		to_shader.oldLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_shader.newLayout           = vk::ImageLayout::eShaderReadOnlyOptimal;
		to_shader.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_shader.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
		to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_shader.image               = m_fallback_mvec_image.image;
		to_shader.subresourceRange    = mvec_range;
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_shader);
		m_mvec_initialized = true;
	}

	// Transition output image to eGeneral for DLSS evaluation
	{
		vk::ImageMemoryBarrier to_general {};
		to_general.sType               = vk::StructureType::eImageMemoryBarrier;
		to_general.oldLayout           = vk::ImageLayout::eUndefined;
		to_general.newLayout           = vk::ImageLayout::eGeneral;
		to_general.srcAccessMask       = vk::AccessFlagBits {};
		to_general.dstAccessMask       = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eShaderRead;
		to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.image               = m_output_image.image;
		to_general.subresourceRange    = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
		                           vk::PipelineStageFlagBits::eAllCommands,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_general);
	}

	// Build Streamline Native Resources
	VkDeviceMemory src_mem = GetImageMemory(graphics, source_image.allocation);
	VkDeviceMemory dst_mem = GetImageMemory(graphics, m_output_image.allocation);
	VkDeviceMemory depth_mem = GetImageMemory(graphics, m_fallback_depth_image.allocation);
	VkDeviceMemory mvec_mem = GetImageMemory(graphics, m_fallback_mvec_image.allocation);

	sl::Resource in_color_res(sl::ResourceType::eTex2d,
	                          static_cast<void*>(static_cast<VkImage>(source_image.image)),
	                          src_mem,
	                          static_cast<void*>(static_cast<VkImageView>(source_view)),
	                          static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	in_color_res.width = source_extent.width;
	in_color_res.height = source_extent.height;
	in_color_res.nativeFormat = static_cast<uint32_t>(source_image.format);
	in_color_res.mipLevels = 1;
	in_color_res.arrayLayers = 1;
	in_color_res.usage = static_cast<uint32_t>(source_image.usage);

	sl::Resource out_color_res(sl::ResourceType::eTex2d,
	                           static_cast<void*>(static_cast<VkImage>(m_output_image.image)),
	                           dst_mem,
	                           static_cast<void*>(static_cast<VkImageView>(m_output_view)),
	                           static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
	out_color_res.width = target_extent.width;
	out_color_res.height = target_extent.height;
	out_color_res.nativeFormat = static_cast<uint32_t>(m_output_image.format);
	out_color_res.mipLevels = 1;
	out_color_res.arrayLayers = 1;
	out_color_res.usage = static_cast<uint32_t>(m_output_image.usage);

	sl::Resource depth_res(sl::ResourceType::eTex2d,
	                       static_cast<void*>(static_cast<VkImage>(m_fallback_depth_image.image)),
	                       depth_mem,
	                       static_cast<void*>(static_cast<VkImageView>(m_fallback_depth_view)),
	                       static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	depth_res.width = source_extent.width;
	depth_res.height = source_extent.height;
	depth_res.nativeFormat = static_cast<uint32_t>(m_fallback_depth_image.format);
	depth_res.mipLevels = 1;
	depth_res.arrayLayers = 1;
	depth_res.usage = static_cast<uint32_t>(m_fallback_depth_image.usage);

	sl::Resource mvec_res(sl::ResourceType::eTex2d,
	                      static_cast<void*>(static_cast<VkImage>(m_fallback_mvec_image.image)),
	                      mvec_mem,
	                      static_cast<void*>(static_cast<VkImageView>(m_fallback_mvec_view)),
	                      static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	mvec_res.width = source_extent.width;
	mvec_res.height = source_extent.height;
	mvec_res.nativeFormat = static_cast<uint32_t>(m_fallback_mvec_image.format);
	mvec_res.mipLevels = 1;
	mvec_res.arrayLayers = 1;
	mvec_res.usage = static_cast<uint32_t>(m_fallback_mvec_image.usage);

	std::vector<sl::ResourceTag> tags;
	tags.emplace_back(&in_color_res, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate);
	tags.emplace_back(&out_color_res, sl::kBufferTypeScalingOutputColor,
	                  m_fg_active ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eValidUntilEvaluate);
	tags.emplace_back(&depth_res, sl::kBufferTypeDepth,
	                  m_fg_active ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eValidUntilEvaluate);
	tags.emplace_back(&mvec_res, sl::kBufferTypeMotionVectors,
	                  m_fg_active ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eValidUntilEvaluate);
	if (m_fg_active) {
		tags.emplace_back(&out_color_res, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent);
	}

	sl::ViewportHandle viewport(m_viewport_id);
	sl::FrameToken* frame_token = nullptr;
	uint32_t frame_index = m_frame_counter++;
	sl::Result res = slGetNewFrameToken(frame_token, &frame_index);
	if (res != sl::Result::eOk || frame_token == nullptr) {
		LOGF("[DLSS] slGetNewFrameToken failed: %d\n", static_cast<int>(res));
		return false;
	}

	// Reflex sleep and simulation/submit markers (required by Reflex & DLSS-G runtime)
	if (m_reflex_supported) {
		slReflexSleep(*frame_token);
		slPCLSetMarker(sl::PCLMarker::eSimulationStart, *frame_token);
		slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *frame_token);
		slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *frame_token);
	}

	res = slSetTagForFrame(*frame_token, viewport, tags.data(), static_cast<uint32_t>(tags.size()), static_cast<sl::CommandBuffer*>(cmd_buffer));
	if (res != sl::Result::eOk) {
		LOGF("[DLSS] slSetTagForFrame failed: %d\n", static_cast<int>(res));
		return false;
	}

	sl::Constants constants {};
	constants.jitterOffset        = { 0.0f, 0.0f };
	constants.mvecScale           = { 1.0f, 1.0f };
	constants.cameraPinholeOffset = { 0.0f, 0.0f };
	constants.cameraPos           = { 0.0f, 0.0f, 0.0f };
	constants.cameraUp            = { 0.0f, 1.0f, 0.0f };
	constants.cameraRight         = { 1.0f, 0.0f, 0.0f };
	constants.cameraFwd           = { 0.0f, 0.0f, 1.0f };
	constants.reset               = (m_frame_counter <= 2 || extent_changed) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D      = sl::Boolean::eFalse;
	constants.depthInverted        = sl::Boolean::eFalse;
	constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated   = sl::Boolean::eFalse;
	constants.motionVectorsJittered  = sl::Boolean::eFalse;

	sl::float4x4 identity {};
	identity[0] = sl::float4 { 1.0f, 0.0f, 0.0f, 0.0f };
	identity[1] = sl::float4 { 0.0f, 1.0f, 0.0f, 0.0f };
	identity[2] = sl::float4 { 0.0f, 0.0f, 1.0f, 0.0f };
	identity[3] = sl::float4 { 0.0f, 0.0f, 0.0f, 1.0f };

	constants.cameraViewToClip = identity;
	constants.clipToCameraView = identity;
	constants.clipToPrevClip   = identity;
	constants.prevClipToClip   = identity;
	constants.cameraNear = 0.1f;
	constants.cameraFar  = 1000.0f;
	constants.cameraFOV  = 1.0f;
	constants.cameraAspectRatio = static_cast<float>(target_extent.width) / static_cast<float>(target_extent.height);

	res = slSetConstants(constants, *frame_token, viewport);
	if (res != sl::Result::eOk) {
		LOGF("[DLSS] slSetConstants failed: %d\n", static_cast<int>(res));
		return false;
	}

	// Evaluate Feature
	const sl::BaseStructure* inputs[] = { &viewport };
	res = slEvaluateFeature(sl::kFeatureDLSS, *frame_token, inputs, 1, static_cast<sl::CommandBuffer*>(cmd_buffer));
	if (res != sl::Result::eOk) {
		m_status = DLSSStatus::Error;
		m_status_message = fmt::format("slEvaluateFeature failed: {}", static_cast<int>(res));
		LOGF("[DLSS] %s\n", m_status_message.c_str());
		return false;
	}

	if (m_reflex_supported) {
		slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *frame_token);
	}

	m_last_frame_token = frame_token;

	// Transition output image to eTransferSrcOptimal so swapchain blit/copy can read it
	{
		vk::ImageMemoryBarrier to_src {};
		to_src.sType               = vk::StructureType::eImageMemoryBarrier;
		to_src.oldLayout           = vk::ImageLayout::eGeneral;
		to_src.newLayout           = vk::ImageLayout::eTransferSrcOptimal;
		to_src.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
		to_src.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
		to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_src.image               = m_output_image.image;
		to_src.subresourceRange    = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
		cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                           vk::PipelineStageFlagBits::eTransfer,
		                           vk::DependencyFlags {}, 0, nullptr, 0, nullptr, 1, &to_src);
	}

	m_status = DLSSStatus::Active;
	m_status_message = fmt::format("DLSS Active: {} -> {}",
	                               DLSSModeToString(m_settings.mode),
	                               fmt::format("{}x{}", target_extent.width, target_extent.height));
	out_upscaled_image = &m_output_image;
	return true;
}

void DLSSManager::SetPresentMarker(bool start) {
	if (m_last_frame_token != nullptr && m_reflex_supported) {
		auto* token = static_cast<sl::FrameToken*>(m_last_frame_token);
		slPCLSetMarker(start ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd, *token);
	}
}

void DLSSManager::ReleaseResources(GraphicContext& graphics) {
	if (m_output_view != nullptr) {
		graphics.device.destroyImageView(m_output_view, nullptr);
		m_output_view = nullptr;
	}
	if (m_output_image.image != nullptr) {
		graphics.DeleteImage(m_output_image);
	}
	m_output_extent = {};

	DestroyFallbackBuffers(graphics);

	m_applied_mode      = DLSSMode::Off;
	m_applied_sharpness = -1.0f;
	m_applied_preset    = DLSSPreset::Default;
	m_applied_fg_mode   = DLSSGMode::Off;
}

void DLSSManager::Shutdown(GraphicContext& graphics) {
	ReleaseResources(graphics);

	if (m_streamline_loaded) {
		sl::ViewportHandle viewport(m_viewport_id);
		if (m_fg_supported) {
			sl::DLSSGOptions g_options {};
			g_options.mode = sl::DLSSGMode::eOff;
			slDLSSGSetOptions(viewport, g_options);
			slFreeResources(sl::kFeatureDLSS_G, viewport);
		}
		slFreeResources(sl::kFeatureDLSS, viewport);
		slShutdown();
		m_streamline_loaded = false;
	}

	m_initialized = false;
	m_available = false;
	m_fg_active = false;
	m_status = DLSSStatus::Disabled;
	m_status_message = "DLSS Shutdown complete";
	LOGF("[DLSS] DLSSManager shutdown complete\n");
}

uint32_t DLSSManager::GetRequestedMultiplier() const noexcept {
	if (!m_settings.fg_enabled || !m_fg_active) {
		return 1;
	}
	switch (m_settings.fg_mode) {
		case DLSSGMode::MFG_4X: return 4;
		case DLSSGMode::MFG_3X: return 3;
		case DLSSGMode::FG_2X: return 2;
		case DLSSGMode::MFG_Dynamic: return (m_max_fg_frames > 1) ? (std::min)(m_max_fg_frames, 4u) : 2u;
		default: return 1;
	}
}

uint32_t DLSSManager::GetEffectiveMultiplier() const noexcept {
	if (!m_settings.fg_enabled || !m_fg_active) {
		return 1;
	}
	return (m_presented_multiplier > 1) ? m_presented_multiplier : GetRequestedMultiplier();
}

std::string DLSSManager::GetOverlayInfo() const {
	if (!m_settings.enabled || m_settings.mode == DLSSMode::Off) {
		if (m_settings.fg_enabled && m_fg_active) {
			const uint32_t mult = GetEffectiveMultiplier();
			return fmt::format("DLSS: Off (FG {}x)", mult);
		}
		return "DLSS: Off";
	}
	const auto st = m_status.load();
	if (st == DLSSStatus::Active) {
		std::string fg_str;
		if (m_settings.fg_enabled && m_fg_active) {
			const uint32_t mult = GetEffectiveMultiplier();
			fg_str = fmt::format(" + {} ({}x)", DLSSGModeToString(m_settings.fg_mode), mult);
		}
		std::string reflex_str;
		if (m_settings.fg_enabled || m_settings.reflex_mode != ReflexMode::Off) {
			reflex_str = fmt::format(" | {}", ReflexModeToString(m_settings.reflex_mode));
		}
		return fmt::format("DLSS: Active ({} {}x{}->{}x{}){}{}",
		                   DLSSModeToString(m_settings.mode),
		                   m_prev_input_extent.width, m_prev_input_extent.height,
		                   m_prev_target_extent.width, m_prev_target_extent.height,
		                   fg_str, reflex_str);
	}
	if (st == DLSSStatus::Available) {
		return fmt::format("DLSS: Ready ({})", DLSSModeToString(m_settings.mode));
	}
	return fmt::format("DLSS: {}", DLSSStatusToString(st));
}

VkResult DLSSManager::QueuePresent(VkQueue queue, const VkPresentInfoKHR* presentInfo) {
	if (m_pfn_vkQueuePresentKHR != nullptr) {
		const auto res = m_pfn_vkQueuePresentKHR(queue, presentInfo);
		if (m_fg_active) {
			sl::ViewportHandle viewport(m_viewport_id);
			sl::DLSSGState fg_state {};
			if (slDLSSGGetState(viewport, fg_state, nullptr) == sl::Result::eOk) {
				if (fg_state.numFramesActuallyPresented > 0) {
					m_presented_multiplier = fg_state.numFramesActuallyPresented;
				}
				if (fg_state.status != sl::DLSSGStatus::eOk) {
					static uint32_t s_last_status = 0;
					const uint32_t cur_status = static_cast<uint32_t>(fg_state.status);
					if (cur_status != s_last_status) {
						s_last_status = cur_status;
						std::printf("[DLSS][Warn] DLSS-G Runtime Status: 0x%X (", cur_status);
						if (cur_status & static_cast<uint32_t>(sl::DLSSGStatus::eFailResolutionTooLow)) std::printf("ResolutionTooLow ");
						if (cur_status & static_cast<uint32_t>(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) std::printf("ReflexNotDetected ");
						if (cur_status & static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported)) std::printf("HDRNotSupported ");
						if (cur_status & static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid)) std::printf("CommonConstantsInvalid ");
						if (cur_status & static_cast<uint32_t>(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)) std::printf("GetBackBufferNotCalled ");
						std::printf(")\n");
						std::fflush(stdout);
					}
				}
			}
		}
		return res;
	}
	return VK_ERROR_INITIALIZATION_FAILED;
}

bool DLSSManager::HasQueuePresentProxy() const noexcept {
	return m_available && (m_pfn_vkQueuePresentKHR != nullptr);
}

VkResult DLSSManager::AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                       VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex) {
	if (m_pfn_vkAcquireNextImageKHR != nullptr) {
		return m_pfn_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, imageIndex);
	}
	return VK_ERROR_INITIALIZATION_FAILED;
}

bool DLSSManager::HasAcquireNextImageProxy() const noexcept {
	return m_available && (m_pfn_vkAcquireNextImageKHR != nullptr);
}

VkResult DLSSManager::CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
                                      const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
	if (m_pfn_vkCreateSwapchainKHR != nullptr) {
		return m_pfn_vkCreateSwapchainKHR(device, createInfo, allocator, swapchain);
	}
	return VK_ERROR_INITIALIZATION_FAILED;
}

bool DLSSManager::HasCreateSwapchainProxy() const noexcept {
	return m_available && (m_pfn_vkCreateSwapchainKHR != nullptr);
}

void DLSSManager::DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
                                   const VkAllocationCallbacks* allocator) {
	if (m_pfn_vkDestroySwapchainKHR != nullptr) {
		m_pfn_vkDestroySwapchainKHR(device, swapchain, allocator);
	}
}

bool DLSSManager::HasDestroySwapchainProxy() const noexcept {
	return m_available && (m_pfn_vkDestroySwapchainKHR != nullptr);
}

#if defined(_WIN32)
VkResult DLSSManager::CreateWin32Surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* createInfo,
                                        const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
	if (m_pfn_vkCreateWin32SurfaceKHR != nullptr) {
		return m_pfn_vkCreateWin32SurfaceKHR(instance, createInfo, allocator, surface);
	}
	return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult DLSSManager::CreateWin32SurfaceForHwnd(VkInstance instance, HWND hwnd, VkSurfaceKHR* out_surface) {
	if (!HasCreateWin32SurfaceProxy() || hwnd == nullptr || out_surface == nullptr) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkWin32SurfaceCreateInfoKHR create_info {};
	create_info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	create_info.pNext     = nullptr;
	create_info.flags     = 0;
	create_info.hinstance = GetModuleHandle(nullptr);
	create_info.hwnd      = hwnd;
	return m_pfn_vkCreateWin32SurfaceKHR(instance, &create_info, nullptr, out_surface);
}

bool DLSSManager::HasCreateWin32SurfaceProxy() const noexcept {
	return m_available && (m_pfn_vkCreateWin32SurfaceKHR != nullptr);
}

void DLSSManager::DestroySurface(VkInstance instance, VkSurfaceKHR surface,
                                 const VkAllocationCallbacks* allocator) {
	if (m_pfn_vkDestroySurfaceKHR != nullptr && surface != VK_NULL_HANDLE) {
		m_pfn_vkDestroySurfaceKHR(instance, surface, allocator);
	}
}

bool DLSSManager::HasDestroySurfaceProxy() const noexcept {
	return m_available && (m_pfn_vkDestroySurfaceKHR != nullptr);
}
#endif

} // namespace Libs::Graphics
