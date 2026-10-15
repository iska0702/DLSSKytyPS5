#include "graphics/presentation/dlss/fsrManager.h"

#include "common/logging/log.h"

#include <chrono>
#include <limits>
#if KYTY_ENABLE_FSR
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3.h>
#endif

namespace Libs::Graphics {
struct FSRManager::Impl {
	std::string status = "FSR: waiting for temporal inputs";
#if KYTY_ENABLE_FSR
	FfxFsr3Context                   context {};
	FfxFsr3UpscalerContext           upscaler {};
	bool                             created = false, history = false, fg = false, failed = false;
	FfxSwapchain                     swapchain = nullptr;
	FfxSwapchainReplacementFunctions functions {};
	vk::Format                       format = vk::Format::eUndefined;
	vk::Extent2D                     input {}, target {};
	VulkanImage                      output;
	std::array<VulkanImage, 3>       sharedImages;
	std::vector<uint8_t>             sharedScratch, upscaleScratch, interpolationScratch;
	uint64_t                         frame           = 0;
	bool                             presentVerified = false;
	uint64_t                         presentSampleStart = 0;
	uint32_t                         presentSamples = 0;
	std::chrono::steady_clock::time_point previous {};
	FfxDevice                             Device(GraphicContext& g) {
		VkDeviceContext d {g.device, g.physical_device,
		                   VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
		return ffxGetDeviceVK(&d);
	}
	bool Check(FfxErrorCode result, const char* operation) {
		if (result == FFX_OK) return true;
		status = fmt::format("FSR: {} failed ({})", operation, static_cast<int>(result));
		Log::WriteToConsoleAndLog(status + "\n");
		return false;
	}
	void Configure(bool enabled) {
		if (!created || !swapchain) return;
		FfxFrameGenerationConfig cfg {};
		cfg.swapChain               = swapchain;
		cfg.frameGenerationEnabled  = enabled;
		cfg.frameGenerationCallback = [](const FfxFrameGenerationDispatchDescription* desc, void*) {
			return ffxFsr3DispatchFrameGeneration(desc);
		};
		cfg.interpolationRect = {0, 0, static_cast<int32_t>(target.width),
		                         static_cast<int32_t>(target.height)};
		cfg.frameID           = frame;
		fg = Check(ffxFsr3ConfigureFrameGeneration(&context, &cfg), "configure frame generation") &&
		     enabled;
	}
#endif
};
FSRManager::FSRManager(): m(std::make_unique<Impl>()) {}
FSRManager::~FSRManager() = default;
FSRManager& FSRManager::Instance() {
	static FSRManager manager;
	return manager;
}
bool FSRManager::OwnsSwapchain() const {
#if KYTY_ENABLE_FSR
	return m->swapchain != nullptr;
#else
	return false;
#endif
}
bool FSRManager::CreateSwapchain(GraphicContext& g, const vk::SwapchainCreateInfoKHR& info,
                                 vk::SwapchainKHR& out) {
#if KYTY_ENABLE_FSR
	if (!Config::FSRFGEnabled()) return false;
	if (g.streamline_queue_count < 2) {
		Log::WriteToConsoleAndLog("[FSR] Two dedicated queues unavailable; FSR upscaling remains "
		                          "available, FG disabled.\n");
		return false;
	}
	const auto device = m->Device(g);
	if (!m->Check(ffxGetSwapchainReplacementFunctionsVK(device, &m->functions),
	              "get swapchain functions"))
		return false;
	vk::Queue presentQueue, acquireQueue;
	g.device.getQueue(g.queue_family, g.streamline_queue_start, &presentQueue);
	g.device.getQueue(g.queue_family, g.streamline_queue_start + 1, &acquireQueue);
	VkBool32 supported = VK_FALSE;
	if (g.physical_device.getSurfaceSupportKHR(g.queue_family, info.surface, &supported) !=
	    vk::Result::eSuccess)
		return false;
	if (!supported) return false;
	VkFrameInterpolationInfoFFX interpolation {};
	interpolation.physicalDevice    = g.physical_device;
	interpolation.device            = g.device;
	interpolation.gameQueue         = {g.queue, g.queue_family, nullptr};
	interpolation.presentQueue      = {presentQueue, g.queue_family, nullptr};
	interpolation.imageAcquireQueue = {acquireQueue, g.queue_family, nullptr};
	interpolation.compositionMode   = VK_COMPOSITION_MODE_PRESENT_QUEUE_FFX;
	FfxSwapchain swapchain          = nullptr;
	if (!m->Check(ffxReplaceSwapchainForFrameinterpolationVK(
	                  ffxGetCommandQueueVK(g.queue), swapchain,
	                  reinterpret_cast<const VkSwapchainCreateInfoKHR*>(&info), &interpolation),
	              "create interpolation swapchain"))
		return false;
	m->swapchain       = swapchain;
	m->presentVerified = false;
	m->presentSampleStart = 0;
	m->presentSamples = 0;
	Log::WriteToConsoleAndLog(
	    "[FSR] Interpolation swapchain ready (dedicated present/acquire queues).\n");
	m->format = info.imageFormat;
	out       = ffxGetVKSwapchain(swapchain);
	return true;
#else
	return false;
#endif
}
vk::Result FSRManager::GetImages(GraphicContext& g, vk::SwapchainKHR sc, uint32_t* count,
                                 vk::Image* images) {
#if KYTY_ENABLE_FSR
	return static_cast<vk::Result>(m->functions.getSwapchainImagesKHR(
	    g.device, sc, count, reinterpret_cast<VkImage*>(images)));
#else
	return vk::Result::eErrorFeatureNotPresent;
#endif
}
vk::Result FSRManager::Acquire(GraphicContext& g, vk::SwapchainKHR sc, vk::Semaphore semaphore,
                               uint32_t* index) {
#if KYTY_ENABLE_FSR
	return static_cast<vk::Result>(m->functions.acquireNextImageKHR(
	    g.device, sc, UINT64_MAX, semaphore, VK_NULL_HANDLE, index));
#else
	return vk::Result::eErrorFeatureNotPresent;
#endif
}
vk::Result FSRManager::Present(GraphicContext& g, const vk::PresentInfoKHR& info) {
#if KYTY_ENABLE_FSR
	const auto result = static_cast<vk::Result>(
	    m->functions.queuePresentKHR(g.queue, reinterpret_cast<const VkPresentInfoKHR*>(&info)));
	if (!m->presentVerified && result == vk::Result::eSuccess) {
		Log::WriteToConsoleAndLog("[FSR] First replacement-swapchain present succeeded.\n");
		m->presentVerified = true;
	}
	if (result == vk::Result::eSuccess && Config::DLSSDiagnosticsEnabled() &&
	    m->functions.getLastPresentCountFFX != nullptr) {
		if (!m->fg || ++m->presentSamples >= 120) {
			const auto count = m->functions.getLastPresentCountFFX(ffxGetVKSwapchain(m->swapchain));
			if (m->fg && count >= m->presentSampleStart) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "[FSR] Presentation sample: {} application presents, {} actual presents\n",
				    m->presentSamples, count - m->presentSampleStart));
			}
			m->presentSampleStart = count;
			m->presentSamples = 0;
		}
	}
	return result;
#else
	return vk::Result::eErrorFeatureNotPresent;
#endif
}
void FSRManager::Suspend() {
#if KYTY_ENABLE_FSR
	m->Configure(false);
	m->history  = false;
	m->previous = {};
#endif
}
void FSRManager::ReleaseResources(GraphicContext& g) {
#if KYTY_ENABLE_FSR
	if (m->swapchain) m->Check(ffxWaitForPresents(m->swapchain), "wait for presents");
	if (m->created || m->output.image)
		RequireVulkanSuccess(g.device.waitIdle(), "wait for FSR resources");
	m->Configure(false);
	if (m->created)
		m->Check(m->swapchain ? ffxFsr3ContextDestroy(&m->context)
		                      : ffxFsr3UpscalerContextDestroy(&m->upscaler),
		         "destroy context");
	m->created = m->history = m->fg = false;
	m->previous                     = {};
	if (m->output.image) g.DeleteImage(m->output);
	for (auto& image: m->sharedImages)
		if (image.image) g.DeleteImage(image);
	m->sharedScratch.clear();
	m->upscaleScratch.clear();
	m->interpolationScratch.clear();
#endif
}
void FSRManager::DestroySwapchain(GraphicContext& g, vk::SwapchainKHR sc) {
#if KYTY_ENABLE_FSR
	ReleaseResources(g);
	m->functions.destroySwapchainKHR(g.device, sc, nullptr);
	m->swapchain = nullptr;
	m->failed    = false;
#endif
}
#if KYTY_ENABLE_FSR
static FfxResource Resource(const VulkanImage& image, FfxResourceStates state,
                            bool writable = false) {
	FfxResourceDescription desc {};
	desc.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
	desc.format   = ffxGetSurfaceFormatVK(static_cast<VkFormat>(image.format));
	desc.width    = image.extent.width;
	desc.height   = image.extent.height;
	desc.depth    = 1;
	desc.mipCount = 1;
	desc.usage    = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
	return ffxGetResourceVK(reinterpret_cast<void*>(static_cast<VkImage>(image.image)), desc,
	                        L"Kyty FSR", state);
}
static void Barrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout,
                    vk::ImageLayout newLayout) {
	vk::ImageMemoryBarrier b {};
	b.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	b.dstAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	b.oldLayout           = oldLayout;
	b.newLayout           = newLayout;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image                                       = image;
	b.subresourceRange                            = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                    vk::PipelineStageFlagBits::eAllCommands, {}, 0, nullptr, 0, nullptr, 1, &b);
}
#endif
bool FSRManager::Evaluate(GraphicContext& g, vk::CommandBuffer cmd, VulkanImage& color,
                          vk::Extent2D input, vk::Extent2D target, const DLSSFrameGuides& guides,
                          VulkanImage*& output) {
	output = nullptr;
#if KYTY_ENABLE_FSR
	if (!Config::FSREnabled() || m->failed) return false;
	if (!guides.valid || !guides.camera.valid || !guides.depth.image || !guides.motion.image ||
	    guides.depth.extent.width != input.width || guides.depth.extent.height != input.height ||
	    guides.motion.extent.width != input.width || guides.motion.extent.height != input.height ||
	    input.width == 0 || input.height == 0 || input.width > target.width ||
	    input.height > target.height) {
		m->Configure(false);
		m->history  = false;
		m->previous = {};
		m->status   = "FSR: spatial fallback (temporal inputs unavailable)";
		return false;
	}
	if (!m->created || m->input != input || m->target != target) {
		ReleaseResources(g);
		m->input  = input;
		m->target = target;
		FfxFsr3ContextDescription desc {};
		desc.flags =
		    FFX_FSR3_ENABLE_AUTO_EXPOSURE | (m->swapchain ? 0 : FFX_FSR3_ENABLE_UPSCALING_ONLY);
		desc.maxRenderSize  = {input.width, input.height};
		desc.maxUpscaleSize = desc.displaySize = {target.width, target.height};
		desc.backBufferFormat                  = ffxGetSurfaceFormatVK(static_cast<VkFormat>(
		    m->format == vk::Format::eUndefined ? vk::Format::eR8G8B8A8Unorm : m->format));
		auto setup = [&](FfxInterface& backend, std::vector<uint8_t>& scratch, size_t count) {
			scratch.resize(ffxGetScratchMemorySizeVK(g.physical_device, count));
			return m->Check(
			    ffxGetInterfaceVK(&backend, m->Device(g), scratch.data(), scratch.size(), count),
			    "initialize Vulkan backend");
		};
		if ((m->swapchain && !setup(desc.backendInterfaceSharedResources, m->sharedScratch, 1)) ||
		    !setup(desc.backendInterfaceUpscaling, m->upscaleScratch,
		           FFX_FSR3UPSCALER_CONTEXT_COUNT) ||
		    (m->swapchain &&
		     !setup(desc.backendInterfaceFrameInterpolation, m->interpolationScratch,
		            FFX_OPTICALFLOW_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT))) {
			m->failed = true;
			return false;
		}
		FfxErrorCode createResult;
		if (m->swapchain) {
			createResult = ffxFsr3ContextCreate(&m->context, &desc);
		} else {
			// SDK 1.1.4's combined wrapper destroys its shared backend twice in
			// UPSCALING_ONLY mode. Use the standalone upscaler for SR-only lifetimes.
			FfxFsr3UpscalerContextDescription upscale {};
			upscale.flags            = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
			upscale.maxRenderSize    = desc.maxRenderSize;
			upscale.maxUpscaleSize   = desc.maxUpscaleSize;
			upscale.backendInterface = desc.backendInterfaceUpscaling;
			createResult             = ffxFsr3UpscalerContextCreate(&m->upscaler, &upscale);
		}
		if (!m->Check(createResult, "create context")) {
			m->failed = true;
			return false;
		}
		m->created = true;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[FSR] Context created: {}x{} -> {}x{}, frame generation {}\n", input.width,
		    input.height, target.width, target.height, m->swapchain ? "available" : "disabled"));
		vk::ImageCreateInfo image {};
		image.imageType = vk::ImageType::e2D;
		image.extent    = {target.width, target.height, 1};
		image.mipLevels = image.arrayLayers = 1;
		image.samples                       = vk::SampleCountFlagBits::e1;
		image.format                        = vk::Format::eR8G8B8A8Unorm;
		image.tiling                        = vk::ImageTiling::eOptimal;
		image.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
		              vk::ImageUsageFlagBits::eTransferSrc;
		if (!g.CreateImage(image, m->output)) {
			m->failed = true;
			return false;
		}
		if (!m->swapchain) {
			image.extent               = {input.width, input.height, 1};
			const vk::Format formats[] = {vk::Format::eR32Sfloat, vk::Format::eR16G16Sfloat,
			                              vk::Format::eR32Uint};
			for (size_t i = 0; i < m->sharedImages.size(); ++i) {
				image.format = formats[i];
				if (!g.CreateImage(image, m->sharedImages[i])) {
					m->failed = true;
					return false;
				}
			}
		}
	}
	++m->frame;
	m->Configure(m->swapchain != nullptr && m->history && !guides.camera.reset);
	const auto now   = std::chrono::steady_clock::now();
	float      delta = m->previous == std::chrono::steady_clock::time_point {}
	                       ? 16.6667f
	                       : std::chrono::duration<float, std::milli>(now - m->previous).count();
	m->previous      = now;
	// The caller tracks color layout; SDK restores registered external resources to their input
	// state.
	const auto oldColor = color.state.layout;
	Barrier(cmd, color.image, oldColor, vk::ImageLayout::eShaderReadOnlyOptimal);
	Barrier(cmd, m->output.image,
	        m->history ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eUndefined,
	        vk::ImageLayout::eGeneral);
	const auto&                       camera = guides.camera;
	FfxFsr3DispatchUpscaleDescription dispatch {};
	dispatch.commandList   = ffxGetCommandListVK(cmd);
	dispatch.color         = Resource(color, FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.depth         = Resource(guides.depth, FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.motionVectors = Resource(guides.motion, FFX_RESOURCE_STATE_COMPUTE_READ);
	dispatch.upscaleOutput = Resource(m->output, FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
	// Captured guides are unjittered normalized current-to-previous UV displacement.
	dispatch.motionVectorScale       = {static_cast<float>(input.width),
	                                    static_cast<float>(input.height)};
	dispatch.jitterOffset            = {camera.jitter_pixels[0], camera.jitter_pixels[1]};
	dispatch.renderSize              = {input.width, input.height};
	dispatch.upscaleSize             = {target.width, target.height};
	dispatch.frameTimeDelta          = std::clamp(delta, 1.0f, 1000.0f);
	dispatch.preExposure             = 1.0f;
	dispatch.reset                   = !m->history || camera.reset;
	dispatch.cameraNear              = camera.near_plane;
	dispatch.cameraFar               = camera.far_plane;
	dispatch.cameraFovAngleVertical  = camera.fov;
	dispatch.viewSpaceToMetersFactor = 1.0f;
	dispatch.enableSharpening        = true;
	dispatch.sharpness               = std::clamp(Config::GetDLSSSharpness(), 0.0f, 1.0f);
	dispatch.frameID                 = m->frame;
	FfxErrorCode dispatchResult;
	if (m->swapchain) {
		dispatchResult = ffxFsr3ContextDispatchUpscale(&m->context, &dispatch);
	} else {
		FfxFsr3UpscalerDispatchDescription upscale {};
		for (auto& image: m->sharedImages)
			Barrier(cmd, image.image,
			        m->history ? vk::ImageLayout::eGeneral : vk::ImageLayout::eUndefined,
			        vk::ImageLayout::eGeneral);
		upscale.dilatedDepth =
		    Resource(m->sharedImages[0], FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
		upscale.dilatedMotionVectors =
		    Resource(m->sharedImages[1], FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
		upscale.reconstructedPrevNearestDepth =
		    Resource(m->sharedImages[2], FFX_RESOURCE_STATE_UNORDERED_ACCESS, true);
		upscale.commandList             = dispatch.commandList;
		upscale.color                   = dispatch.color;
		upscale.depth                   = dispatch.depth;
		upscale.motionVectors           = dispatch.motionVectors;
		upscale.output                  = dispatch.upscaleOutput;
		upscale.jitterOffset            = dispatch.jitterOffset;
		upscale.motionVectorScale       = dispatch.motionVectorScale;
		upscale.renderSize              = dispatch.renderSize;
		upscale.upscaleSize             = dispatch.upscaleSize;
		upscale.enableSharpening        = dispatch.enableSharpening;
		upscale.sharpness               = dispatch.sharpness;
		upscale.frameTimeDelta          = dispatch.frameTimeDelta;
		upscale.preExposure             = dispatch.preExposure;
		upscale.reset                   = dispatch.reset;
		upscale.cameraNear              = dispatch.cameraNear;
		upscale.cameraFar               = dispatch.cameraFar;
		upscale.cameraFovAngleVertical  = dispatch.cameraFovAngleVertical;
		upscale.viewSpaceToMetersFactor = dispatch.viewSpaceToMetersFactor;
		dispatchResult                  = ffxFsr3UpscalerContextDispatch(&m->upscaler, &upscale);
	}
	bool ok = m->Check(dispatchResult, "dispatch upscale");
	if (ok && m->fg) {
		FfxFsr3DispatchFrameGenerationPrepareDescription prepare {};
		prepare.commandList             = dispatch.commandList;
		prepare.depth                   = dispatch.depth;
		prepare.motionVectors           = dispatch.motionVectors;
		prepare.jitterOffset            = dispatch.jitterOffset;
		prepare.motionVectorScale       = dispatch.motionVectorScale;
		prepare.renderSize              = dispatch.renderSize;
		prepare.frameTimeDelta          = dispatch.frameTimeDelta;
		prepare.cameraNear              = dispatch.cameraNear;
		prepare.cameraFar               = dispatch.cameraFar;
		prepare.cameraFovAngleVertical  = dispatch.cameraFovAngleVertical;
		prepare.viewSpaceToMetersFactor = 1.0f;
		prepare.frameID                 = dispatch.frameID;
		if (!m->Check(ffxFsr3ContextDispatchFrameGenerationPrepare(&m->context, &prepare),
		              "prepare frame generation"))
			m->Configure(false);
	}
	Barrier(cmd, color.image, vk::ImageLayout::eShaderReadOnlyOptimal, oldColor);
	Barrier(cmd, m->output.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal);
	m->history = ok;
	if (!ok) {
		m->Configure(false);
		m->failed = true;
		return false;
	}
	const std::string status =
	    m->fg ? "FSR 3.1 + Frame Generation (2x requested)" : "FSR 3.1 Super Resolution";
	if (m->status != status) Log::WriteToConsoleAndLog("[FSR] " + status + "\n");
	m->status = status;
	output    = &m->output;
	return true;
#else
	m->status = "FSR unavailable: build with KYTY_ENABLE_FSR";
	return false;
#endif
}
std::string FSRManager::GetOverlayInfo() const {
	return Config::FSREnabled() ? m->status : "";
}
} // namespace Libs::Graphics
