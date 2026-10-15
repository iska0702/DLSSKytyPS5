#pragma once

#include "graphics/presentation/dlss/dlssCamera.h"
#include "graphics/presentation/dlss/dlssConfig.h"
#include <vulkan/vulkan.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace Libs::Graphics {

// Caller-owned snapshots, kept alive until the submitted evaluation completes.
// These must come from the lighting pass, before guest denoising/TAA/tone mapping.
struct RRTexture {
	vk::Image image {};
	vk::ImageView view {};
	vk::DeviceMemory memory {};
	vk::Format format = vk::Format::eUndefined;
	vk::Extent2D extent {};
	vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::ImageUsageFlags usage {};
};

struct RRInputs {
	RRTexture noisy_hdr, diffuse_albedo, specular_albedo, normal_roughness;
	RRTexture depth, motion, specular_hit_distance;
	RRTexture output_hdr; // RGBA16F/32F, GENERAL, storage usage, output resolution.
	DLSSCamera camera;
	DLSSMatrix world_to_view {}, view_to_world {};
	bool certified_pre_denoise = false;
	float pre_exposure = 1.0f;
};

// Offscreen HDR denoising/upscaling. The caller must composite/tone-map the
// returned HDR output in the guest lighting pipeline, never blit it as SDR.
class DLSSRayReconstruction final {
public:
	void Initialize(vk::PhysicalDevice physical_device);
	void Shutdown(); // GPU must be idle before releasing Streamline resources.
	[[nodiscard]] bool IsSupported() const { return m_supported; }
	[[nodiscard]] bool IsActive() const { return m_active; }
	[[nodiscard]] std::string Status() const { std::lock_guard lock(m_status_mutex); return m_status; }
	[[nodiscard]] static std::string Validate(const RRInputs& inputs);
	[[nodiscard]] bool Evaluate(vk::CommandBuffer command, const RRInputs& inputs,
	                            DLSSMode mode, uint32_t frame_index);

private:
	void SetStatus(std::string status) { std::lock_guard lock(m_status_mutex); m_status = std::move(status); }
	std::atomic<bool> m_supported {false}, m_active {false};
	bool m_allocated = false, m_history = false;
	mutable std::mutex m_status_mutex;
	std::string m_status = "RR not requested";
	vk::Extent2D m_previous_input {}, m_previous_output {};
	DLSSMode m_previous_mode = DLSSMode::Off;
	uint32_t m_viewport = 1; // Separate from the final SDR presentation viewport.
};
} // namespace Libs::Graphics
