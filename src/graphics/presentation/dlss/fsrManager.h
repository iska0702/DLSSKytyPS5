#pragma once
#include "graphics/presentation/dlss/dlssManager.h"

namespace Libs::Graphics {
// FSR shares the validated guest-camera and guide capture with DLSS.
class FSRManager final {
public:
	static FSRManager& Instance();
	FSRManager();
	~FSRManager();
	bool CreateSwapchain(GraphicContext&, const vk::SwapchainCreateInfoKHR&, vk::SwapchainKHR&);
	bool OwnsSwapchain() const;
	vk::Result GetImages(GraphicContext&, vk::SwapchainKHR, uint32_t*, vk::Image*);
	vk::Result Acquire(GraphicContext&, vk::SwapchainKHR, vk::Semaphore, uint32_t*);
	vk::Result Present(GraphicContext&, const vk::PresentInfoKHR&);
	void       DestroySwapchain(GraphicContext&, vk::SwapchainKHR);
	void       ReleaseResources(GraphicContext&);
	bool Evaluate(GraphicContext&, vk::CommandBuffer, VulkanImage&, vk::Extent2D, vk::Extent2D,
	              const DLSSFrameGuides&, VulkanImage*&);
	void Suspend();
	std::string GetOverlayInfo() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};
} // namespace Libs::Graphics
