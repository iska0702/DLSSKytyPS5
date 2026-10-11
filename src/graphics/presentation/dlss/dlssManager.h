#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss/dlssConfig.h"

#include <atomic>
#include <memory>
#include <string>

namespace Libs::Graphics {

class DLSSManager final {
public:
	static DLSSManager& Instance();

	DLSSManager();
	~DLSSManager();
	KYTY_CLASS_NO_COPY(DLSSManager);

	/// Initializes Streamline and checks DLSS feature support for the active Vulkan device.
	[[nodiscard]] bool Initialize(GraphicContext& graphics, const DLSSSettings& settings);

	/// Shuts down DLSS and Streamline.
	void Shutdown(GraphicContext& graphics);

	[[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }
	[[nodiscard]] bool IsAvailable() const noexcept { return m_available; }
	[[nodiscard]] bool IsActive() const noexcept { return m_status == DLSSStatus::Active; }
	[[nodiscard]] DLSSStatus GetStatus() const noexcept { return m_status; }
	[[nodiscard]] const std::string& GetStatusMessage() const noexcept { return m_status_message; }
	[[nodiscard]] const DLSSSettings& GetSettings() const noexcept { return m_settings; }

	void UpdateSettings(const DLSSSettings& settings);

	/// Allocates or resizes intermediate target and fallback buffers (depth, motion vectors).
	[[nodiscard]] bool PrepareResources(GraphicContext& graphics, vk::Extent2D input_extent,
	                                    vk::Extent2D target_extent);

	/// Evaluates DLSS upscaling from source_image into the upscaled target buffer.
	/// Returns true on success and points out_upscaled_image to the result.
	/// If false is returned, the caller should fall back to the native rendering path.
	[[nodiscard]] bool Evaluate(GraphicContext& graphics, vk::CommandBuffer cmd_buffer,
	                            VulkanImage& source_image, vk::ImageView source_view,
	                            vk::Extent2D source_extent, vk::Extent2D target_extent,
	                            VulkanImage*& out_upscaled_image);

	/// Frees GPU resources allocated by DLSS.
	void ReleaseResources(GraphicContext& graphics);

	/// Returns real-time status string for window title bar or OSD overlay.
	[[nodiscard]] std::string GetOverlayInfo() const;

	/// FG / MFG (DLSS-G) status queries
	[[nodiscard]] bool IsFGSupported() const noexcept { return m_fg_supported; }
	[[nodiscard]] bool IsFGActive() const noexcept { return m_fg_active; }
	[[nodiscard]] uint32_t GetMaxFGFrames() const noexcept { return m_max_fg_frames; }
	[[nodiscard]] bool IsDynamicMFGSupported() const noexcept { return m_dynamic_mfg_supported; }
	[[nodiscard]] bool IsReflexSupported() const noexcept { return m_reflex_supported; }

	/// Notifies Reflex / PCL markers around presentation
	void SetPresentMarker(bool start);

	/// Streamline interposer Vulkan hook proxies
	VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* presentInfo);
	[[nodiscard]] bool HasQueuePresentProxy() const noexcept;

	VkResult AcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	                          VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex);
	[[nodiscard]] bool HasAcquireNextImageProxy() const noexcept;

	VkResult CreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
	                         const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);
	[[nodiscard]] bool HasCreateSwapchainProxy() const noexcept;

	void DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain,
	                      const VkAllocationCallbacks* allocator);
	[[nodiscard]] bool HasDestroySwapchainProxy() const noexcept;

private:
	bool SetupStreamline(GraphicContext& graphics);
	void CreateFallbackBuffers(GraphicContext& graphics, vk::Extent2D extent);
	void DestroyFallbackBuffers(GraphicContext& graphics);

	DLSSSettings            m_settings;
	std::atomic<DLSSStatus> m_status { DLSSStatus::Disabled };
	std::string             m_status_message;
	bool                    m_initialized       = false;
	bool                    m_available         = false;
	bool                    m_streamline_loaded = false;

	uint32_t                m_frame_counter = 0;
	uint32_t                m_viewport_id   = 0;

	// Interposer Vulkan function pointers
	PFN_vkQueuePresentKHR     m_pfn_vkQueuePresentKHR     = nullptr;
	PFN_vkAcquireNextImageKHR m_pfn_vkAcquireNextImageKHR = nullptr;
	PFN_vkCreateSwapchainKHR  m_pfn_vkCreateSwapchainKHR  = nullptr;
	PFN_vkDestroySwapchainKHR m_pfn_vkDestroySwapchainKHR = nullptr;

	// DLSS Intermediate Output Image (RGBA8 UNORM or format matching swapchain)
	VulkanImage             m_output_image;
	vk::ImageView           m_output_view   = nullptr;
	vk::Extent2D            m_output_extent {};

	// Fallback Depth buffer (D32_SFLOAT) for console emulation compatibility
	VulkanImage             m_fallback_depth_image;
	vk::ImageView           m_fallback_depth_view = nullptr;
	vk::Extent2D            m_depth_extent {};
	bool                    m_depth_initialized = false;

	// Fallback Motion Vectors buffer (R16G16_SFLOAT) for console emulation compatibility
	VulkanImage             m_fallback_mvec_image;
	vk::ImageView           m_fallback_mvec_view = nullptr;
	vk::Extent2D            m_mvec_extent {};
	bool                    m_mvec_initialized = false;

	vk::Extent2D            m_prev_input_extent {};
	vk::Extent2D            m_prev_target_extent {};

	// Frame Generation & Reflex state
	bool                    m_fg_supported          = false;
	bool                    m_fg_active             = false;
	uint32_t                m_max_fg_frames         = 0;
	bool                    m_dynamic_mfg_supported = false;
	bool                    m_reflex_supported      = false;
	uint32_t                m_presented_multiplier  = 1;
	void*                   m_last_frame_token      = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_DLSS_DLSSMANAGER_H_
