#include "graphics/presentation/dlss/dlssRayReconstruction.h"
#include <cstdio>

using namespace Libs::Graphics;
int main() {
	int failures = 0;
	auto check = [&](bool value, const char* name) {
		std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
		failures += !value;
	};
	RRInputs in;
	check(!DLSSRayReconstruction::Validate(in).empty(), "ordinary presentation inputs do not certify RR");
	in.certified_pre_denoise = true;
	in.camera.valid = true;
	in.camera.near_plane = .1f; in.camera.far_plane = 1000;
	in.camera.fov = 1; in.camera.aspect = 16.0f/9.0f;
	in.world_to_view[0] = in.world_to_view[5] = in.world_to_view[10] = in.world_to_view[15] = 1;
	in.view_to_world = in.world_to_view;
	uintptr_t handle = 1;
	auto texture = [&](RRTexture& t, vk::Format format, bool output = false) {
		t.image = (VkImage)(handle++);
		t.view = (VkImageView)(handle++);
		t.memory = (VkDeviceMemory)(handle++);
		t.extent = output ? vk::Extent2D{1280, 720} : vk::Extent2D{640, 360};
		t.format = format;
		t.usage = output ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
		t.layout = output ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal;
	};
	for (auto* t : {&in.noisy_hdr, &in.diffuse_albedo, &in.specular_albedo, &in.normal_roughness})
		texture(*t, vk::Format::eR16G16B16A16Sfloat);
	texture(in.depth, vk::Format::eR32Sfloat);
	texture(in.motion, vk::Format::eR16G16Sfloat);
	texture(in.specular_hit_distance, vk::Format::eR16Sfloat);
	texture(in.output_hdr, vk::Format::eR16G16B16A16Sfloat, true);
	check(DLSSRayReconstruction::Validate(in).empty(), "complete HDR lighting contract accepted");
	auto bad = in; bad.noisy_hdr.format = vk::Format::eR8G8B8A8Unorm;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "SDR final frame rejected");
	bad = in; bad.diffuse_albedo.format = vk::Format::eR8G8B8A8Srgb;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "sRGB material guide rejected");
	bad = in; bad.specular_hit_distance.image = nullptr;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "missing reflection distance rejected");
	bad = in; bad.motion.extent.width /= 2;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "inconsistent render grids rejected");
	bad = in; bad.output_hdr.image = bad.noisy_hdr.image;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "input/output alias rejected");
	bad = in; bad.output_hdr.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "read-only output rejected");
	bad = in; bad.view_to_world[0] = 2;
	check(!DLSSRayReconstruction::Validate(bad).empty(), "incorrect reflection view transform rejected");
	return failures != 0;
}
