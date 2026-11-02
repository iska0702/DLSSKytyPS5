#pragma once
#include "graphics/host_gpu/vulkanCommon.h"
#include <cstdint>
#include <memory>

namespace Libs::Graphics {
class CommandBuffer;
struct GraphicContext;
class RRReflectionCapture {
public:
	explicit RRReflectionCapture(GraphicContext& graphics) : m_graphics(graphics) {}
	~RRReflectionCapture(); // caller has drained guest GPU work
	vk::DescriptorBufferInfo Prepare(CommandBuffer& command, vk::Extent2D extent, uint64_t frame);
	void CaptureDispatch(CommandBuffer& command, vk::Buffer source, uint64_t offset, uint64_t shader,
	                     vk::DescriptorBufferInfo tile_head);
private:
	struct Payload;
	std::shared_ptr<Payload> Allocate(vk::Extent2D extent, uint64_t frame);
	GraphicContext& m_graphics;
	std::shared_ptr<Payload> m_current, m_dummy;
	uint64_t m_failed_frame = UINT64_MAX;
};
}
