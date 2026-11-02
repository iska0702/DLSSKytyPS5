#include "graphics/host_gpu/renderer/rrReflectionCapture.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include <array>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace Libs::Graphics {
namespace {
uint64_t Number(const char* name, uint64_t fallback) {
	const auto* value = std::getenv(name);
	if (!value) return fallback;
	const std::string_view text(value);
	uint64_t n = 0;
	const auto parsed = std::from_chars(text.data(), text.data()+text.size(), n);
	return parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() ? n : fallback;
}
}
struct RRReflectionCapture::Payload {
	VmaAllocator allocator = nullptr;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	void* mapped = nullptr;
	uint64_t frame = 0, bytes = 0;
	vk::Extent2D extent {};
	void Report() const {
		if (!extent.width || !extent.height) return;
		vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
		const auto* words = static_cast<const uint32_t*>(mapped);
		uint64_t written = 0, hits = 0, misses = 0, invalid = 0, coverage = 0, repeated = 0, rays = 0;
		float minimum = std::numeric_limits<float>::infinity(), maximum = 0;
		for (uint64_t i=0; i<uint64_t(extent.width)*extent.height; ++i) {
			const auto* pixel = words + 4 + i*4;
			rays += pixel[3];
			if (pixel[3] > 1) ++repeated;
			if (pixel[2] == 2) { ++coverage; continue; }
			if (pixel[2] != 1) continue;
			++coverage;
			++written;
			if (pixel[1] == UINT32_MAX) { ++misses; continue; }
			++hits;
			const auto t = std::bit_cast<float>(pixel[0]);
			if (!std::isfinite(t) || t < 0) { ++invalid; continue; }
			minimum = std::min(minimum,t); maximum = std::max(maximum,t);
		}
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DLSS-RR][RawReflection] frame={} extent={}x{} covered={} written={} hits={} misses={} invalid_t={} min_t={} max_t={} ray_exports={} repeated_pixels={} (not certified)\n",
		    frame,extent.width,extent.height,coverage,written,hits,misses,invalid,
		    std::isfinite(minimum)?minimum:0,maximum,rays,repeated));
		const auto* groups=words+4+uint64_t(extent.width)*extent.height*4;
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DLSS-RR][RawDispatch] frame={} groups={},{},{};{},{},{};{},{},{};{},{},{}\n",
		    frame,groups[0],groups[1],groups[2],groups[3],groups[4],groups[5],
		    groups[6],groups[7],groups[8],groups[9],groups[10],groups[11]));
		Log::WriteToConsoleAndLog(fmt::format(
		    "[DLSS-RR][RawTiles] frame={} first={:08X},{:08X},{:08X},{:08X}\n",
		    frame,groups[12],groups[13],groups[14],groups[15]));
		if (const char* prefix = std::getenv("KYTY_RR_GUIDE_FILE"); prefix && prefix[0]) {
			const auto path = Common::PathFromUtf8(fmt::format("{}.{}.bin",prefix,frame));
			std::ofstream file(path, std::ios::binary);
			file.write(static_cast<const char*>(mapped), static_cast<std::streamsize>(bytes));
			if (!file) Log::WriteToConsoleAndLog("[DLSS-RR] Failed to save raw reflection capture\n");
		}
	}
	~Payload() { if (buffer) vmaDestroyBuffer(allocator,buffer,allocation); }
};

std::shared_ptr<RRReflectionCapture::Payload> RRReflectionCapture::Allocate(vk::Extent2D extent, uint64_t frame) {
	const uint64_t bytes=16+uint64_t(extent.width)*extent.height*16+64;
	if (extent.width>8192 || extent.height>8192 || bytes>128ull*1024*1024) return {};
	auto p = std::make_shared<Payload>();
	p->allocator=m_graphics.allocator; p->frame=frame; p->extent=extent;
	p->bytes=bytes;
	VkBufferCreateInfo info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=p->bytes;
	info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo alloc {}; alloc.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	alloc.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	VmaAllocationInfo mapped {};
	if (vmaCreateBuffer(p->allocator,&info,&alloc,&p->buffer,&p->allocation,&mapped)!=VK_SUCCESS)
		return {};
	p->mapped=mapped.pMappedData;
	if (!p->mapped) return {};
	std::memset(p->mapped,0,static_cast<size_t>(p->bytes));
	const std::array<uint32_t,4> header {extent.width,extent.height,static_cast<uint32_t>(frame),5};
	std::memcpy(p->mapped,header.data(),sizeof(header));
	const std::array<uint32_t,4> unknown_tiles {0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu,0xDEADBEEFu};
	std::memcpy(static_cast<char*>(p->mapped)+p->bytes-16,unknown_tiles.data(),16);
	vmaFlushAllocation(p->allocator,p->allocation,0,VK_WHOLE_SIZE);
	return p;
}

RRReflectionCapture::~RRReflectionCapture() { if (m_current) m_current->Report(); }

void RRReflectionCapture::CaptureDispatch(CommandBuffer& command, vk::Buffer source, uint64_t offset, uint64_t shader,
                                          vk::DescriptorBufferInfo tile_head) {
	if (!m_current) return;
	uint32_t index=0;
	switch(shader) {
		case 0x5DF28D2058CAF728ull: index=0; break;
		case 0x55D28021295DC816ull: index=1; break;
		case 0xC18F4AD08601D141ull: index=2; break;
		case 0xD65C9DF1AD5E3AF5ull: index=3; break;
		default: return;
	}
	vk::MemoryBarrier ready {};
	ready.srcAccessMask=vk::AccessFlagBits::eShaderWrite|vk::AccessFlagBits::eTransferWrite;
	ready.dstAccessMask=vk::AccessFlagBits::eTransferRead;
	command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	    vk::PipelineStageFlagBits::eTransfer,{},1,&ready,0,nullptr,0,nullptr);
	vk::BufferCopy copy {offset,m_current->bytes-64+index*12,12};
	command.Handle().copyBuffer(source,m_current->buffer,1,&copy);
	if (tile_head.buffer && tile_head.range>=4) {
		vk::BufferCopy tile {tile_head.offset,m_current->bytes-16+index*4,4};
		command.Handle().copyBuffer(tile_head.buffer,m_current->buffer,1,&tile);
	}
	vk::BufferMemoryBarrier done {};
	done.srcAccessMask=vk::AccessFlagBits::eTransferWrite; done.dstAccessMask=vk::AccessFlagBits::eHostRead;
	done.srcQueueFamilyIndex=done.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
	done.buffer=m_current->buffer; done.offset=m_current->bytes-64; done.size=64;
	command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	    vk::PipelineStageFlagBits::eHost,{},0,nullptr,1,&done,0,nullptr);
}

vk::DescriptorBufferInfo RRReflectionCapture::Prepare(CommandBuffer& command, vk::Extent2D extent, uint64_t frame) {
	static const auto first=Number("KYTY_RR_TRACE_FRAME",UINT64_MAX);
	static const auto count=std::clamp<uint64_t>(Number("KYTY_RR_TRACE_COUNT",1),1,8);
	if (m_current && (m_current->frame!=frame || m_current->extent!=extent)) {
		// No subsequent dispatch binds this allocation. Hold it until all already
		// recorded GPU writes complete, then invalidate/read/free it on the host.
		command.GetContext().GetCommandScheduler().DeferOperation(
		    [p=std::move(m_current)] { p->Report(); });
	}
	std::shared_ptr<Payload> selected;
	if (!m_dummy) m_dummy=Allocate({},0);
	if (!m_dummy) throw std::runtime_error("RR capture dummy allocation failed");
	selected=m_dummy;
	if (frame>=first && frame-first<count && extent.width && extent.height) {
		if (!m_current && m_failed_frame!=frame) {
			m_current=Allocate(extent,frame);
			if (!m_current) {
				m_failed_frame=frame;
				Log::WriteToConsoleAndLog("[DLSS-RR] Raw capture skipped: allocation failed or capture exceeds 128 MiB\n");
			}
		}
		if (m_current) selected=m_current;
	}
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask=vk::AccessFlagBits::eHostWrite;
	barrier.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
	command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eHost,
	    vk::PipelineStageFlagBits::eComputeShader,{},1,&barrier,0,nullptr,0,nullptr);
	return {selected->buffer,0,selected->bytes};
}
}
