#include "graphics/presentation/dlss/dlssRayReconstruction.h"
#include "common/logging/log.h"
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_d.h>
#include <algorithm>
#include <cmath>

namespace Libs::Graphics {
namespace {
bool FloatColor(vk::Format f) {
	return f == vk::Format::eR16G16B16A16Sfloat || f == vk::Format::eR32G32B32A32Sfloat;
}
sl::float4x4 Matrix(const DLSSMatrix& m) {
	sl::float4x4 result {};
	for (int r = 0; r < 4; ++r) result[r] = {m[r*4], m[r*4+1], m[r*4+2], m[r*4+3]};
	return result;
}
sl::DLSSMode Mode(DLSSMode mode) {
	switch (mode) {
		case DLSSMode::DLAA: return sl::DLSSMode::eDLAA;
		case DLSSMode::Quality: return sl::DLSSMode::eMaxQuality;
		case DLSSMode::Balanced: return sl::DLSSMode::eBalanced;
		case DLSSMode::Performance: return sl::DLSSMode::eMaxPerformance;
		case DLSSMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
		default: return sl::DLSSMode::eOff;
	}
}
sl::Resource Resource(const RRTexture& t) {
	sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(t.image),
	               static_cast<VkDeviceMemory>(t.memory), static_cast<VkImageView>(t.view),
	               static_cast<uint32_t>(t.layout));
	r.width = t.extent.width; r.height = t.extent.height;
	r.nativeFormat = static_cast<uint32_t>(t.format);
	r.mipLevels = r.arrayLayers = 1;
	r.flags = 0; r.usage = static_cast<uint32_t>(t.usage);
	return r;
}
}

std::string DLSSRayReconstruction::Validate(const RRInputs& in) {
	if (!in.certified_pre_denoise) return "waiting for certified pre-denoise HDR/material inputs";
	if (!in.camera.valid) return "invalid lighting camera";
	if (!std::isfinite(in.camera.near_plane) || !std::isfinite(in.camera.far_plane) ||
	    !std::isfinite(in.camera.fov) || !std::isfinite(in.camera.aspect) ||
	    in.camera.near_plane <= 0 || in.camera.far_plane <= in.camera.near_plane ||
	    in.camera.fov <= 0 || in.camera.aspect <= 0) return "invalid lighting camera range";
	if (!std::isfinite(in.pre_exposure) || in.pre_exposure <= 0) return "invalid pre-exposure";
	if (!FloatColor(in.noisy_hdr.format) || !FloatColor(in.output_hdr.format))
		return "HDR input/output must be RGBA16F or RGBA32F";
	if (!FloatColor(in.normal_roughness.format)) return "normal/roughness must be packed RGBA float";
	if (in.motion.format != vk::Format::eR16G16Sfloat && in.motion.format != vk::Format::eR32G32Sfloat)
		return "motion must be RG16F or RG32F";
	if (in.depth.format != vk::Format::eR32Sfloat && in.depth.format != vk::Format::eD32Sfloat)
		return "depth must be R32F or D32F";
	if (in.specular_hit_distance.format != vk::Format::eR16Sfloat &&
	    in.specular_hit_distance.format != vk::Format::eR32Sfloat) return "invalid specular hit distance format";
	for (const auto* albedo : {&in.diffuse_albedo, &in.specular_albedo}) {
		if (!FloatColor(albedo->format) && albedo->format != vk::Format::eR8G8B8A8Unorm)
			return "albedo must be linear RGB (sRGB tags are not accepted)";
	}
	DLSSMatrix identity {}; identity[0] = identity[5] = identity[10] = identity[15] = 1;
	for (const auto* m : {&in.world_to_view, &in.view_to_world})
		for (float v : *m) if (!std::isfinite(v)) return "non-finite view transform";
	const auto product = DLSSMultiply(in.world_to_view, in.view_to_world);
	for (float v : product) if (!std::isfinite(v)) return "overflow in view transform product";
	if (!DLSSMatricesMatch(product, identity))
		return "view transforms are not inverses";
	const auto extent = in.noisy_hdr.extent;
	if (extent.width == 0 || extent.height == 0 || in.output_hdr.extent.width < extent.width ||
	    in.output_hdr.extent.height < extent.height) return "invalid RR render/output extents";
	for (const auto* t : {&in.noisy_hdr, &in.diffuse_albedo, &in.specular_albedo,
	                     &in.normal_roughness, &in.depth, &in.motion, &in.specular_hit_distance}) {
		if (!t->image || !t->view || !t->memory) return "missing lighting resource or image view";
		if (t->extent != extent) return "lighting inputs use different render grids";
		if (t->layout != vk::ImageLayout::eShaderReadOnlyOptimal ||
		    !(t->usage & vk::ImageUsageFlagBits::eSampled)) return "lighting input must be shader-readable";
		if (t->image == in.output_hdr.image) return "RR output aliases an input";
	}
	if (!in.output_hdr.image || !in.output_hdr.view || !in.output_hdr.memory ||
	    in.output_hdr.layout != vk::ImageLayout::eGeneral ||
	    !(in.output_hdr.usage & vk::ImageUsageFlagBits::eStorage)) return "invalid HDR output storage target";
	return {};
}

void DLSSRayReconstruction::Initialize(vk::PhysicalDevice physical) {
	sl::AdapterInfo adapter {}; adapter.vkPhysicalDevice = physical;
	const auto support = slIsFeatureSupported(sl::kFeatureDLSS_RR, adapter);
	bool loaded = false;
	const auto query = slIsFeatureLoaded(sl::kFeatureDLSS_RR, loaded);
	m_supported = support == sl::Result::eOk && query == sl::Result::eOk && loaded;
	SetStatus(m_supported ? "RR available; waiting for certified pre-denoise HDR/material inputs"
	                       : fmt::format("RR unavailable (support={}, load={}, loaded={})",
	                                     static_cast<int>(support), static_cast<int>(query), loaded));
	Log::WriteToConsoleAndLog("[DLSS-RR] " + Status() + "\n");
}

void DLSSRayReconstruction::Shutdown() {
	if (m_allocated) slFreeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(m_viewport));
	m_allocated = m_history = false; m_active = false; m_supported = false;
	m_previous_input = m_previous_output = vk::Extent2D {};
	m_previous_mode = DLSSMode::Off;
	SetStatus("RR not requested");
}

bool DLSSRayReconstruction::Evaluate(vk::CommandBuffer command, const RRInputs& in,
                                    DLSSMode mode, uint32_t frame_index) {
	m_active = false;
	const bool had_history = m_history;
	m_history = false;
	if (!m_supported) return false;
	const auto invalid = Validate(in);
	if (!invalid.empty()) { SetStatus("RR: " + invalid); return false; }
	if (!command || mode == DLSSMode::Off) { SetStatus("RR: disabled or invalid command buffer"); return false; }
	auto check = [&](sl::Result result, const char* operation) {
		if (result == sl::Result::eOk) return true;
		SetStatus(fmt::format("RR {} failed ({})", operation, static_cast<int>(result)));
		Log::WriteToConsoleAndLog("[DLSS-RR] " + Status() + "\n"); return false;
	};
	sl::ViewportHandle viewport(m_viewport);
	sl::DLSSDOptions options {};
	options.mode = Mode(mode);
	options.outputWidth = in.output_hdr.extent.width; options.outputHeight = in.output_hdr.extent.height;
	options.colorBuffersHDR = sl::Boolean::eTrue;
	options.preExposure = in.pre_exposure;
	options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
	options.worldToCameraView = Matrix(in.world_to_view); options.cameraViewToWorld = Matrix(in.view_to_world);
	if (!check(slDLSSDSetOptions(viewport, options), "options")) return false;
	sl::FrameToken* token = nullptr;
	if (!check(slGetNewFrameToken(token, &frame_index), "frame token") || token == nullptr) return false;
	sl::Constants constants {};
	const auto& camera = in.camera;
	constants.cameraViewToClip = Matrix(camera.projection); constants.clipToCameraView = Matrix(camera.inverse_projection);
	constants.clipToPrevClip = Matrix(camera.clip_to_previous); constants.prevClipToClip = Matrix(camera.previous_to_clip);
	constants.jitterOffset = {camera.jitter_pixels[0], camera.jitter_pixels[1]};
	constants.mvecScale = {1, 1};
	constants.cameraPos = {camera.position[0], camera.position[1], camera.position[2]};
	constants.cameraRight = {camera.right[0], camera.right[1], camera.right[2]};
	constants.cameraUp = {camera.up[0], camera.up[1], camera.up[2]};
	constants.cameraFwd = {camera.forward[0], camera.forward[1], camera.forward[2]};
	constants.cameraNear = camera.near_plane; constants.cameraFar = camera.far_plane;
	constants.cameraFOV = camera.fov; constants.cameraAspectRatio = camera.aspect;
	constants.cameraMotionIncluded = sl::Boolean::eTrue;
	constants.motionVectors3D = constants.depthInverted = constants.orthographicProjection = sl::Boolean::eFalse;
	constants.motionVectorsDilated = constants.motionVectorsJittered = sl::Boolean::eFalse;
	constants.reset = camera.reset || !had_history || m_previous_input != in.noisy_hdr.extent ||
	    m_previous_output != in.output_hdr.extent || m_previous_mode != mode
	    ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	if (!check(slSetConstants(constants, *token, viewport), "constants")) return false;
	std::array resources {Resource(in.noisy_hdr), Resource(in.diffuse_albedo), Resource(in.specular_albedo),
	                      Resource(in.normal_roughness), Resource(in.depth), Resource(in.motion),
	                      Resource(in.specular_hit_distance), Resource(in.output_hdr)};
	const std::array<sl::BufferType, 8> types {sl::kBufferTypeScalingInputColor, sl::kBufferTypeAlbedo,
	    sl::kBufferTypeSpecularAlbedo, sl::kBufferTypeNormalRoughness, sl::kBufferTypeDepth,
	    sl::kBufferTypeMotionVectors, sl::kBufferTypeSpecularHitDistance, sl::kBufferTypeScalingOutputColor};
	const sl::Extent render {0, 0, in.noisy_hdr.extent.width, in.noisy_hdr.extent.height};
	const sl::Extent output {0, 0, options.outputWidth, options.outputHeight};
	std::array<sl::ResourceTag, 8> tags;
	for (size_t i = 0; i < tags.size(); ++i)
		tags[i] = sl::ResourceTag(&resources[i], types[i], sl::ResourceLifecycle::eValidUntilEvaluate,
		                          i == tags.size()-1 ? &output : &render);
	if (!check(slSetTagForFrame(*token, viewport, tags.data(), static_cast<uint32_t>(tags.size()),
	                           static_cast<sl::CommandBuffer*>(command)), "resource tags")) return false;
	const sl::BaseStructure* inputs[] = {&viewport};
	m_allocated = true; // An unsuccessful evaluation can still allocate SDK resources.
	if (!check(slEvaluateFeature(sl::kFeatureDLSS_RR, *token, inputs, 1,
	                            static_cast<sl::CommandBuffer*>(command)), "evaluation")) return false;
	m_previous_input = in.noisy_hdr.extent; m_previous_output = in.output_hdr.extent; m_previous_mode = mode;
	m_active = true; m_history = true; SetStatus("RR HDR lighting evaluated");
	return true;
}
} // namespace Libs::Graphics
