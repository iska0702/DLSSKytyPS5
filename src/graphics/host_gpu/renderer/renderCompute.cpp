#include "common/debugCounters.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/liveSwitch.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/drawPrep/cpGaps.h"
#include "graphics/host_gpu/renderer/drawPrep/drawPrep.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/rrReflectionCapture.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss/rrGuestTrace.h"
#include "graphics/presentation/dlss/rrPlayroomMaterials.h"
#include "graphics/shader/recompiler/CodegenOptions.h"
#include "kernel/memory.h" 
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "graphics/host_gpu/renderer/gpuOpProfiler.h"

#include <algorithm>
#include <tuple>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
<<<<<<< HEAD
static std::optional<std::array<float,124>> ReadRRMaterialCamera(RenderContext& context,
        const ShaderStageRuntime& stage) {
	const auto* enabled=std::getenv("KYTY_RR_MATERIAL_CAPTURE");
	if (!enabled || std::string_view(enabled)!="1" || !context.HasGpu() ||
	    !stage.program || !stage.resources || stage.program->shader_hash!=0x5DF28D2058CAF728ull)
		return {};
	const auto frame=context.GetGpu().GetFrameNum();
	if (!RRGuestTrace::SelectedFrame(frame) || context.rr_material_frame==frame) return {};
	context.rr_material_frame=frame;
	const auto& info=stage.program->info;
	if(info.images.size()<4 || info.images[0].source!=1 || info.images[1].source!=2 ||
	   info.images[2].source!=3 || info.images[3].source!=4) return {};
	// Scalar S_BUFFER_LOAD camera reads are flattened SRT values and do not
	// necessarily appear in info.buffers (which describes native buffer bindings).
	// Exact shader: s14:s15 is the untouched initial table pointer; PC 0x100
	// loads the camera V# from table+336, then PCs 0x110/0x124 read its matrices.
	const auto read=[&](uint64_t address,void* dst,uint64_t bytes) {
		return context.IsMapped(address,bytes) && context.SynchronizeGpuBackingForRead(address,bytes) &&
		    Libs::LibKernel::Memory::TryReadBacking(address,dst,bytes);
	};
	const auto& user=stage.resources->user_data;
	if(user.size()>=16) {
		const uint64_t table=uint64_t(user[14])|(uint64_t(user[15])<<32);
		ShaderBufferResource descriptor {};
		std::array<float,124> values {};
		if(table<=UINT64_MAX-368 && read(table+336,descriptor.fields,sizeof(descriptor.fields)) &&
		   descriptor.GetSize()>=sizeof(values) && read(descriptor.Base48(),values.data(),sizeof(values))) {
			ShaderBufferResource lighting {};
			std::array<uint32_t,8> header {};
			if(read(table+352,lighting.fields,sizeof(lighting.fields)) && lighting.GetSize()>=sizeof(header) &&
			   read(lighting.Base48(),header.data(),sizeof(header))) {
				const uint64_t bvh_reference=uint64_t(user[0])|(uint64_t(user[1])<<32);
				Log::WriteToConsoleAndLog(fmt::format(
				    "[DLSS-RR][LightingGate] frame={} probe_count={} ray_gate={} bvh_reference_mapped={}\n",
				    frame,header[3],header[7],context.IsMapped(bvh_reference,8)));
			}
			if(const char* prefix=std::getenv("KYTY_RR_GUIDE_FILE");prefix && prefix[0]) {
				std::ofstream raw(Common::PathFromUtf8(fmt::format("{}.{}.camera.bin",prefix,frame)),std::ios::binary);
				raw.write(reinterpret_cast<const char*>(values.data()),sizeof(values));
			}
			return values;
		}
	}
	Log::WriteToConsoleAndLog("[DLSS-RR][Materials] Matching camera block unavailable; capture skipped\n");
	return {};
}

static void CaptureRRMaterials(RenderContext& context, CommandBuffer& command,
        const PreparedBindings& bindings, const std::optional<std::array<float,124>>& camera) {
	if(!camera || bindings.images.size()<4)return;
	RRPlayroomMaterialInputs in;in.camera=*camera;
	const std::array<RRTexture*,4> inputs {&in.view_normal,&in.depth,&in.base_color,&in.metal_roughness};
	for(unsigned i=0;i<4;++i) {
		const auto& binding=bindings.images[i];const auto& view=binding.desc.view_info;
		// This decoder addresses single, unmapped 2D views. Do not reinterpret arrays,
		// mip offsets or component swizzles without a matching game profile.
		const auto identity=[](vk::ComponentSwizzle s,vk::ComponentSwizzle channel) {
			return s==vk::ComponentSwizzle::eIdentity || s==channel;
		};
		if(view.type!=vk::ImageViewType::e2D || view.base_level || view.base_layer ||
		   view.level_count!=1 || view.layer_count!=1 || !binding.image_id ||
		   !identity(view.mapping.r,vk::ComponentSwizzle::eR) ||
		   (i!=1 && (!identity(view.mapping.g,vk::ComponentSwizzle::eG) ||
		             !identity(view.mapping.b,vk::ComponentSwizzle::eB)))) {
			Log::WriteToConsoleAndLog("[DLSS-RR][Materials] Capture skipped: unsupported native view\n");return;
		}
		const auto& image=context.GetTextureCache().GetImage(binding.image_id);
		auto& t=*inputs[i];t.image=image.backing.image;t.view=binding.image_view;
		t.format=view.format;t.extent={binding.desc.info.extent.width,binding.desc.info.extent.height};
		t.usage=view.usage;t.layout=binding.layout;
	}
	const auto error=RRPlayroomMaterials::Validate(in);
	if(!error.empty()) {
		Log::WriteToConsoleAndLog("[DLSS-RR][Materials] Capture skipped: "+error+"\n");return;
	}
	std::shared_ptr<RRPlayroomMaterials::Frame> captured;
	const auto previous_pipeline=command.BoundPipeline(vk::PipelineBindPoint::eCompute);
	try {
		if(!context.rr_playroom_materials)
			context.rr_playroom_materials=std::make_unique<RRPlayroomMaterials>(context.GetGraphics());
		// Handle() flushes pending barriers before this helper binds its own state.
		captured=context.rr_playroom_materials->Record(command.Handle(),in);
		captured->RecordReadback(command.Handle());
	} catch(const std::exception& e) {
		Log::WriteToConsoleAndLog(std::string("[DLSS-RR][Materials] Capture failed: ")+e.what()+"\n");
	}
	// Raw helper commands bypass the command buffer's pipeline/descriptor cache.
	// Restore its cached pipeline and force the next guest descriptor commit.
	if(captured) {
		if(previous_pipeline)command.Handle().bindPipeline(vk::PipelineBindPoint::eCompute,previous_pipeline);
		command.InvalidateDescriptors(vk::PipelineBindPoint::eCompute);
		command.InvalidatePushConstants();
	}
	// Even a failed optional readback must retain the already-recorded conversion.
	if(captured)context.GetCommandScheduler().DeferOperation(
	    [captured=std::move(captured),frame=context.GetGpu().GetFrameNum()] {captured->Report(frame);});
}

static void PrepareRayGuideBinding(RenderContext& context, CommandBuffer& buffer,
                                    PreparedBindings& bindings,
                                    const ShaderRecompiler::IR::CompiledShaderInfo& program) {
	if (!ShaderRecompiler::IR::FindBinding(program.bindings,
	        ShaderRecompiler::IR::DescriptorBindingKind::RayGuide)) return;
	if (!context.rr_reflection_capture)
		context.rr_reflection_capture=std::make_unique<RRReflectionCapture>(context.GetGraphics());
	vk::Extent2D extent {};
	for (size_t i=0; i<program.info.images.size() && i<bindings.images.size(); ++i)
		if (program.info.images[i].written) {
			extent={bindings.images[i].desc.info.extent.width,bindings.images[i].desc.info.extent.height};
			break;
		}
	bindings.ray_guide=context.rr_reflection_capture->Prepare(buffer,
	    context.HasGpu()?extent:vk::Extent2D{},context.HasGpu()?context.GetGpu().GetFrameNum():0);
}

static void FinishRayGuideWrite(CommandBuffer& buffer, const PreparedBindings& bindings) {
	if (!bindings.ray_guide.buffer) return;
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask=vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask=vk::AccessFlagBits::eHostRead|vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
	barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer=bindings.ray_guide.buffer; barrier.offset=0; barrier.size=bindings.ray_guide.range;
	buffer.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	    vk::PipelineStageFlagBits::eHost|vk::PipelineStageFlagBits::eComputeShader,
	    {},0,nullptr,1,&barrier,0,nullptr);
=======
// KYTY_DISPATCH_OVERLAP accounting, printed every 10 s once a direct dispatch ran:
//   DispatchOverlap 10s (on|off): <direct> direct dispatches, <pairs> unordered pairs (<pct>%),
//   <overlapped> overlapped, <indirect> indirect (<empty> empty skipped, KYTY_SKIP_EMPTY_INDIRECT)
// An unordered pair is a direct dispatch that followed another with nothing in between (it would
// overlap with the switch on); counted with the switch off too, for sizing.
static std::atomic<uint64_t> g_overlap_direct {0}, g_overlap_pairs {0}, g_overlap_taken {0},
    g_overlap_indirect {0}, g_overlap_empty_indirect {0};
static Live::Switch g_skip_empty_indirect("KYTY_SKIP_EMPTY_INDIRECT", Live::ParseDefaultOn);
static std::atomic<int64_t> g_overlap_last_print_ns {0};

static void PrintDispatchOverlap(bool enabled) {
	const auto now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                          std::chrono::steady_clock::now().time_since_epoch())
	                                          .count());
	auto last = g_overlap_last_print_ns.load(std::memory_order_relaxed);
	if (last == 0) {
		g_overlap_last_print_ns.store(now, std::memory_order_relaxed);
		return;
	}
	if (now - last < 10'000'000'000ll ||
	    !g_overlap_last_print_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
		return;
	}
	const auto direct   = g_overlap_direct.exchange(0, std::memory_order_relaxed);
	const auto pairs    = g_overlap_pairs.exchange(0, std::memory_order_relaxed);
	const auto taken    = g_overlap_taken.exchange(0, std::memory_order_relaxed);
	const auto indirect = g_overlap_indirect.exchange(0, std::memory_order_relaxed);
	const auto empty    = g_overlap_empty_indirect.exchange(0, std::memory_order_relaxed);
	std::printf("DispatchOverlap 10s (%s): %llu direct dispatches, %llu unordered pairs (%.1f%%), "
	            "%llu overlapped, %llu indirect (%llu empty skipped)\n",
	            enabled ? "on" : "off", static_cast<unsigned long long>(direct),
	            static_cast<unsigned long long>(pairs),
	            direct != 0 ? 100.0 * static_cast<double>(pairs) / static_cast<double>(direct) : 0.0,
	            static_cast<unsigned long long>(taken), static_cast<unsigned long long>(indirect),
	            static_cast<unsigned long long>(empty));
}

static void CountDispatchOverlap(bool unordered, bool enabled) {
	g_overlap_direct.fetch_add(1, std::memory_order_relaxed);
	if (unordered) {
		g_overlap_pairs.fetch_add(1, std::memory_order_relaxed);
		if (enabled) {
			g_overlap_taken.fetch_add(1, std::memory_order_relaxed);
		}
	}
	PrintDispatchOverlap(enabled);
>>>>>>> upstream/main
}

static bool FillSourcesDisjoint(std::span<const ShaderRecompiler::IR::DescriptorValue> sources,
                                 GuestRange destination, uint32_t output_buffer = UINT32_MAX) {
	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (i == output_buffer) continue;
		const auto source = DecodeNativeDescriptor<ShaderBufferResource>(sources[i]);
		const auto bytes  = source.GetSize();
		if (source.Base48() < destination.End() && destination.address < source.Base48() + bytes)
			return false;
	}
	return true;
}

// KYTY_COMPUTE_META_CLEAR_DISPATCH=1 (default off; upstream 01ce97388's behaviour): a dispatch
// whose buffer writes recorded a metadata clear (UpdateComputeMetaClears) still runs, so its guest
// writes to the metadata memory are kept for readers that reuse it as an ordinary buffer. Off, the
// recorded clear consumes the dispatch, as before.
static bool ComputeMetaClearDispatchEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_COMPUTE_META_CLEAR_DISPATCH");
		return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

// Whether a metadata clear was recorded.
bool RenderExecutor::UpdateComputeMetaClears(const ShaderComputeInputInfo& input,
                                            const CommandBuffer&          buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		// Reads and transforms of metadata do not establish a clear state.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	bool cleared = false;
	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				cleared |= cache.ClearMeta(descriptor.Base48());
			}
		}
	}
	return cleared;
}

bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	if (fill.kind != ShaderRecompiler::IR::UniformFillKind::Buffer) {
		return false;
	}
	const auto element_size = fill.words * sizeof(uint32_t);
	const auto descriptor =
	    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[fill.resource]);
	constexpr std::array formats {
	    Prospero::BufferFormat::k32UInt, Prospero::BufferFormat::k32_32UInt,
	    Prospero::BufferFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt};
	if (descriptor.Stride() != element_size || descriptor.Format() != formats[fill.words - 1] ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    descriptor.Base48() == 0) {
		return false;
	}
	if (input.threads_num[0] == 0 || input.threads_num[0] != fill.group_stride[0] ||
	    input.threads_num[1] != 1 || input.threads_num[2] != 1 || group_x == 0 || group_y != 1 ||
	    group_z != 1 || mode != (input.dispatch_thread_dimensions ? 0x61u : 0x41u)) {
		return false;
	}
	const uint64_t invocations = input.dispatch_thread_dimensions
	                                 ? group_x
	                                 : static_cast<uint64_t>(group_x) * input.threads_num[0];
	const auto     size        = descriptor.GetSize();
	if (invocations != descriptor.NumRecords() || size == 0 || size > UINT32_MAX ||
	    (input.dispatch_thread_dimensions &&
	     (group_x % input.threads_num[0] != 0 || input.dispatch_threads_num[0] != group_x ||
	      input.dispatch_threads_num[1] != 1 || input.dispatch_threads_num[2] != 1))) {
		return false;
	}
	if (!FillSourcesDisjoint(resources.buffers, {descriptor.Base48(), size}, fill.resource))
		return false;
	resolved_descriptor = descriptor;
	resolved_clear      = fill.value;
	resolved_size       = size;
	return true;
}

bool RenderExecutor::TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
                                                CommandBuffer& command, uint32_t group_x,
                                                uint32_t group_y, uint32_t group_z, uint32_t mode) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	const auto& fill      = resources.uniform_fill;
	auto&       cache     = command.GetContext().GetTextureCache();
	if (fill.kind == ShaderRecompiler::IR::UniformFillKind::Image) {
		if (mode != 0x41u || input.dispatch_thread_dimensions || fill.value > 255 ||
		    input.threads_num[2] != 1)
			return false;
		const auto  descriptor = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[0]);
		const auto& resource   = program.info.images[0];
		if (descriptor.IsNull() || descriptor.Format() != Prospero::BufferFormat::k8UInt ||
		    descriptor.Type() != Prospero::ImageType::kColor2DArray || descriptor.MetaCompress() ||
		    descriptor.WriteCompress() || descriptor.BaseLevel() > descriptor.LastLevel() ||
		    descriptor.BaseLevel() > descriptor.MaxMip() ||
		    descriptor.BaseArray5() > descriptor.Depth() || descriptor.DstSelX() != 4)
			return false;
		const std::array extents {
		    std::max(1u, (descriptor.Width5() + 1u) >> descriptor.BaseLevel()),
		    std::max(1u, (descriptor.Height5() + 1u) >> descriptor.BaseLevel()),
		    descriptor.Depth() - descriptor.BaseArray5() + 1u};
		const std::array groups {group_x, group_y, group_z};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const uint64_t threads = input.threads_num[axis];
			// Guest image writes outside the descriptor dimensions are discarded. Only the
			// final workgroup may extend beyond the selected image view.
			if (threads == 0 || threads != fill.group_stride[axis] ||
			    groups[axis] != (extents[axis] + threads - 1) / threads ||
			    groups[axis] * threads > UINT32_MAX) return false;
		}
		const auto  binding     = ResolveTexture(resource, resources.images[0]);
		const auto& destination = binding.desc.info.data;
		if (!FillSourcesDisjoint(resources.buffers, destination)) return false;
		std::scoped_lock lock {cache.m_lock};
		const auto&      image = cache.GetImage(binding.image_id);
		const auto&      view  = binding.desc.view_info;
		if (image.backing.format != vk::Format::eD32SfloatS8Uint || image.info.samples != 1 ||
		    image.info.stencil != destination || view.base_level >= image.backing.mip_levels ||
		    view.base_layer >= image.backing.layers || view.layer_count != extents[2] ||
		    view.layer_count > image.backing.layers - view.base_layer ||
		    std::max(1u, image.info.extent.width >> view.base_level) != extents[0] ||
		    std::max(1u, image.info.extent.height >> view.base_level) != extents[1]) return false;
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eStencil, view.base_level,
		                                       1, view.base_layer, view.layer_count};
		vk::ClearValue clear {};
		clear.depthStencil = vk::ClearDepthStencilValue {0.0f, fill.value};
		// A rejected clear (reported) leaves the guest shader to run.
		return cache.ClearImage(command, binding.image_id, image.backing.format, range, clear,
		                        "compute-stencil-fill");
	}
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeBufferFill(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

// KYTY_LDS_DEVICE_BUFFER_MB (default 1024): the most memory the compute LDS device buffer may
// take; a direct dispatch with more workgroups than it holds runs its excess workgroups without
// LDS (their accesses read zero and drop writes), and says so once per shader.
static uint64_t ComputeLdsMaxBytes(const GraphicContext& graphics) {
	static const uint64_t configured = [] {
		const auto* value = std::getenv("KYTY_LDS_DEVICE_BUFFER_MB");
		const auto  mib   = value != nullptr && value[0] != '\0' ? std::strtoull(value, nullptr, 0) : 1024u;
		return std::max<uint64_t>(mib, 1u) << 20u;
	}();
	return std::min<uint64_t>(configured,
	                          graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange);
}

// An indirect dispatch's workgroup count is GPU data: it gets at least this much (1,365 regions
// of 48 KiB), or the buffer as a direct dispatch grew it.
static constexpr uint64_t LdsIndirectBytes = 64ull << 20u;

bool RenderExecutor::BindComputeLds(const ShaderComputeInputInfo& input, PreparedBindings& bindings,
                                    const uint32_t* groups) {
	const auto& program = *input.stage.program;
	if (ShaderRecompiler::IR::FindBinding(
	        program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::SharedMemory) == nullptr) {
		return false;
	}
	auto&          graphics = m_context.GetGraphics();
	const uint64_t region =
	    uint64_t {ShaderRecompiler::LdsStorageRegionDwords(input.lds_size_dwords)} * sizeof(uint32_t);
	EXIT_IF(region == 0);
	const uint64_t max_regions = std::max<uint64_t>(ComputeLdsMaxBytes(graphics) / region, 1u);
	uint64_t       regions     = 0;
	if (groups != nullptr) {
		const uint64_t count = uint64_t {groups[0]} * groups[1] * groups[2];
		regions              = std::min(count, max_regions);
		if (count > max_regions) {
			static std::atomic<uint32_t> logged {0};
			if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
				char text[256];
				std::snprintf(text, sizeof(text),
				              "Kyty LDS device buffer: compute shader 0x%016" PRIx64
				              " dispatches %" PRIu64 " workgroups of %u bytes of LDS; the buffer holds "
				              "%" PRIu64 " (KYTY_LDS_DEVICE_BUFFER_MB), the others run without LDS\n",
				              program.shader_hash, count, input.lds_size_dwords * 4u, max_regions);
				Log::WriteToConsoleAndLog(text);
			}
		}
	} else {
		const auto current = m_compute_lds != nullptr ? m_compute_lds->Size() / region : 0u;
		regions = std::min(std::max<uint64_t>(current, LdsIndirectBytes / region), max_regions);
	}
	regions            = std::max<uint64_t>(regions, 1u);
	const auto needed = regions * region;
	if (m_compute_lds == nullptr || m_compute_lds->Size() < needed) {
		auto& scheduler = m_context.GetCommandScheduler();
		const auto old_size = m_compute_lds != nullptr ? m_compute_lds->Size() : 0u;
		// Grows by at least half, so a growing series of dispatches reallocates a few times only.
		const auto size =
		    std::min(std::max(needed, old_size + old_size / 2u), max_regions * region);
		if (m_compute_lds != nullptr) {
			// Recorded commands of the current tick may still use the old buffer.
			scheduler.DeferOperation([old = std::move(m_compute_lds)]() mutable { old.reset(); });
		}
		m_compute_lds = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
		                                         vk::BufferUsageFlagBits::eStorageBuffer, size);
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
			char text[256];
			std::snprintf(text, sizeof(text),
			              "Kyty LDS device buffer (KYTY_LDS_DEVICE_BUFFER): %.1f MiB for compute shader "
			              "0x%016" PRIx64 " (%u bytes of LDS per workgroup, %" PRIu64 " workgroups%s)\n",
			              static_cast<double>(size) / (1024.0 * 1024.0), program.shader_hash,
			              input.lds_size_dwords * 4u, regions, groups == nullptr ? ", indirect" : "");
			Log::WriteToConsoleAndLog(text);
		}
	}
	// A direct dispatch binds its own regions; an indirect one every region the buffer holds.
	const auto range =
	    groups != nullptr ? needed : m_compute_lds->Size() / region * region;
	bindings.shared_memory = {m_compute_lds->Handle(), 0, range};
	return true;
}

// KYTY_ORDERED_APPEND=1 (default off; upstream d4465faa7): a dispatch with
// DISPATCH_INITIATOR.ORDERED_APPEND_ENABLE whose program uses GDS and whose workgroups are one
// guest wave without emulated LDS runs one workgroup per vkCmdDispatchBase, in launch order with a
// barrier between them, so ordered-append counters see the workgroups in order. Logged once.
bool OrderedAppendSplitEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_ORDERED_APPEND");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode, bool async_compute) {
	KYTY_GPU_OP_SITE("dispatch");
	// KYTY_CP_GAP_STATS: this scope's phases (drawPrep/cpGaps.h).
	const CpGaps::Scope gap(CpGaps::Cat::DispatchOp);
	EXIT_IF(buffer.IsInvalid());
	m_context.GetCommandScheduler().PopReadyOperations();
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	RenderLockGuard lock(m_context.GetMutex());
	// KYTY_DRAW_RUN: a dispatch ends the run of the draws before it (drawPrep/drawRun.h).
	BeginDrawRun();
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();

	ShaderComputeInputInfo input_info {};
	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	DrawPrep::MakeComputeInputInfo(cs_regs, {thread_group_x, thread_group_y, thread_group_z, mode},
	                               input_info);
	// After MakeComputeInputInfo (it resets the info). Part of the program key (GETREG HW_ID2).
	input_info.async_compute = async_compute;
	CpGaps::Phase(CpGaps::Cat::DispatchProgram);
	ShaderProgram compute_program;
	// KYTY_DRAW_PREP_DISPATCH (drawPrep.h): the program a worker prepared from this dispatch's
	// register snapshot, when its certificate holds; otherwise the serial lookup. Workers prepare
	// graphics-queue dispatches only (MakeComputeInputInfo leaves async_compute false).
	auto* const prepared = std::exchange(m_prepared_dispatch, nullptr);
	if (prepared != nullptr && !async_compute && DrawPrep::ValidateDispatch(*prepared, input_info)) {
		if (DrawPrep::VerifyMode() != 0) {
			// The serial lookup on copies; it only reads memory the certificate proved clean.
			auto serial_prep = std::make_unique<PipelineCache::StagePrep>();
			auto serial_info = input_info;
			const auto serial_program = m_context.GetPipelineCache().GetComputeProgram(
			    cs_regs, sh_regs, serial_info, *serial_prep);
			(void)DrawPrep::VerifyDispatch(*prepared, serial_program, serial_info, *serial_prep);
		}
		std::swap(m_compute_prep, prepared->compute_prep);
		input_info                 = prepared->compute_info;
		input_info.stage.resources = &m_compute_prep.resources;
		compute_program            = prepared->compute_program;
	} else {
		compute_program = m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info,
		                                                                 m_compute_prep);
	}
	CpGaps::Phase(CpGaps::Cat::DispatchOp);
	if (!compute_program) {
		// Temporary until RT is implemented.
		return;
	}
	if (use_thread_dimensions) {
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	// A recorded metadata clear consumes the dispatch unless KYTY_COMPUTE_META_CLEAR_DISPATCH keeps
	// its guest writes (upstream 01ce97388).
	if (UpdateComputeMetaClears(input_info, buffer) && !ComputeMetaClearDispatchEnabled()) {
		ResetBindings();
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		ResetBindings();
		return;
	}
	if (Config::DLSSDiagnosticsEnabled()) {
		static std::unordered_set<uint64_t> seen;
		if (seen.size() < 256 && seen.insert(program.shader_hash).second) {
			Log::WriteToConsoleAndLog(fmt::format("[DLSS][GuestCompute] frame={} shader={:016X} images={}\n",
			    m_context.GetGpu().GetFrameNum(), program.shader_hash, resources.images.size()));
			for (size_t i = 0; i < resources.images.size(); ++i) {
				const auto r = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
				Log::WriteToConsoleAndLog(fmt::format("[DLSS][ComputeImage] index={} address=0x{:X} extent={}x{} format={} written={}\n",
				    i, r.Base40(), r.Width5() + 1, r.Height5() + 1, static_cast<uint32_t>(r.Format()),
				    program.info.images[i].written));
			}
		}
	}

	if (use_thread_dimensions) {
		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = input_info.workgroup_counts[0];
		thread_group_y       = input_info.workgroup_counts[1];
		thread_group_z       = input_info.workgroup_counts[2];

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	CpGaps::Phase(CpGaps::Cat::DispatchPipeline);
	// A software ray-tracing kernel whose pipeline is still being built, or any RT kernel after one
	// failed to build (rtSession.h), is skipped: the lighting it feeds lags or stays as it was.
	auto* pipeline_or_null =
	    m_context.GetPipelineCache().TryGetComputePipeline(input_info, compute_program);
	if (pipeline_or_null == nullptr) {
		ResetBindings();
		return;
	}
	auto& pipeline = *pipeline_or_null;
	auto& bindings = m_compute_bindings;
	CpGaps::Phase(CpGaps::Cat::DispatchBind);
	PrepareBindings(input_info.stage, bindings);
	if (program.bindings.dispatch_thread_dword != ShaderRecompiler::IR::PushData::NoStart) {
		// The requested thread extents initialize EXEC (upstream e4aca7a60).
		std::copy(std::begin(input_info.dispatch_threads_num),
		          std::end(input_info.dispatch_threads_num),
		          bindings.shader_data.begin() + program.bindings.dispatch_thread_dword);
	}
	// The native dispatch size bounds workgroup ids for write-range proofs.
	bindings.dispatch_groups     = {thread_group_x, thread_group_y, thread_group_z};
	bindings.has_dispatch_groups = true;
	FindBuffers(bindings);
	CpGaps::Phase(CpGaps::Cat::DispatchBda);
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	if (program.info.uses_bvh) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::RtStubDispatches);
	}
	if (program.info.bda_writes) {
		// KYTY_BDA_WRITES: settled synchronously right after the dispatch below. The settle relies
		// on PrepareBda's pass above having uploaded every CPU-dirty page first.
		EXIT_IF(!program.info.uses_dma);
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	CpGaps::Phase(CpGaps::Cat::DispatchRebind);
	RebindImages(bindings);
	BindComputeLds(input_info, bindings, input_info.workgroup_counts);
	m_rebind_epoch = m_context.GetBufferCache().UnregisterEpoch();
	RebindBuffers(bindings);
	// KYTY_REBIND_AFTER_JOIN: a later binding's lookup joined a buffer an earlier one resolved.
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; pass++) {
			NoteRebindAfterJoin("dispatch", nullptr);
			m_rebind_epoch = cache.UnregisterEpoch();
			RebindBuffers(bindings);
		}
	}
	CpGaps::Phase(CpGaps::Cat::DispatchEmit);

	PreparedBindings* descriptor_stage = &bindings;
	// Emission safe point (KYTY_CP_RECORDER, render.h): no native handle from the preparation above
	// is alive. Binding commits and the barrier requests below record state commands only (image
	// transitions and dependencies are batched); the dispatch goes through Sink(), which records
	// them first as one barrier, exactly as Handle() did.
	buffer.BeginEmission();
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	                          HasShaderBufferWrites(input_info.stage) || program.info.bda_writes;
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	// KYTY_DISPATCH_OVERLAP (render.h): nothing happened since the previous dispatch, so the guest
	// did not order the two; its pending ShaderAccess request is dropped and this dispatch skips
	// its own hazard request. Emulated LDS (one device buffer) and BDA-write settles never overlap.
	const bool no_overlap      = bindings.shared_memory.buffer != nullptr || program.info.bda_writes;
	const bool overlap_enabled = DispatchOverlapEnabled();
	const bool overlap         = buffer.TakeDispatchOverlap(no_overlap, overlap_enabled);
	CountDispatchOverlap(overlap, overlap_enabled);
	if (has_storage_writes && !(overlap && overlap_enabled)) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (Config::GraphicsDebugDumpEnabled()) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " hash=0x%016" PRIx64 " tick=%" PRIu64 " completed_tick=%" PRIu64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, program.shader_hash,
		     m_context.GetCommandScheduler().CurrentTick(),
		     m_context.GetCommandScheduler().GetMasterSemaphore().KnownGpuTick(),
		     thread_group_x, thread_group_y, thread_group_z, mode,
		     input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(),
		     program.bindings.UsesPushData()
		         ? static_cast<uint32_t>(sizeof(ShaderRecompiler::IR::PushData))
		         : 0u);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.RawFormat());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), static_cast<uint32_t>(r.Format()),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     r.Type() == Prospero::ImageType::kColor2DMsaa ||
			             r.Type() == Prospero::ImageType::kColor2DMsaaArray
			         ? 1u
			         : static_cast<uint32_t>(image.r128 ? r.LastLevel() : r.MaxMip()) + 1u,
			     static_cast<uint32_t>(r.TileMode()));
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(
			    resources.samplers[program.info.samplers[i].snapshot_index]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}
<<<<<<< HEAD

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	// The native dispatch size bounds workgroup ids for write-range proofs.
	bindings.dispatch_groups     = {thread_group_x, thread_group_y, thread_group_z};
	bindings.has_dispatch_groups = true;
	FindBuffers(bindings);
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	if (program.info.uses_bvh) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::RtStubDispatches);
	}
	if (program.info.bda_writes) {
		// KYTY_BDA_WRITES: settled synchronously right after the dispatch below. The settle relies
		// on PrepareBda's pass above having uploaded every CPU-dirty page first.
		EXIT_IF(!program.info.uses_dma);
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	RebindImages(bindings);
	RebindBuffers(bindings);

	PreparedBindings* descriptor_stage = &bindings;
	// Emission safe point (KYTY_CP_RECORDER, render.h): no native handle from the preparation above
	// is alive. Binding commits and the barrier requests below record state commands only (image
	// transitions and dependencies are batched); the dispatch goes through Sink(), which records
	// them first as one barrier, exactly as Handle() did.
	buffer.BeginEmission();
	PrepareRayGuideBinding(m_context,buffer,bindings,program);
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	bool has_storage_writes = HasShaderBufferWrites(input_info.stage) || program.info.bda_writes;
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       image.resource_class ==
		                           ShaderRecompiler::IR::ImageResourceClass::Storage;
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		// A host fence used to serialize every dispatch. Preserve its read-before-write ordering
		// while allowing the queue to execute asynchronously.
		ShaderWriteHazardBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	buffer.BindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	const auto rr_trace = Config::DLSSDiagnosticsEnabled() && m_context.HasGpu()
	    ? RRGuestTrace::Begin(m_context.GetGpu().GetFrameNum(), "compute", program.shader_hash) : 0;
	RRGuestTrace::Stage(rr_trace, "CS", program, resources, &bindings);
	RRGuestTrace::End(rr_trace);
	buffer.Sink().dispatch(thread_group_x, thread_group_y, thread_group_z);
	FinishRayGuideWrite(buffer,bindings);
=======
	buffer.BindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	constexpr uint32_t DispatchInitiatorOrderedAppendEnable = 1u << 3u;
	const uint64_t     group_threads = uint64_t {std::max(input_info.threads_num[0], 1u)} *
	                               std::max(input_info.threads_num[1], 1u) *
	                               std::max(input_info.threads_num[2], 1u);
	if (OrderedAppendSplitEnabled() && (mode & DispatchInitiatorOrderedAppendEnable) != 0 &&
	    (ShaderRecompiler::IR::FindBinding(program.bindings, ShaderRecompiler::IR::DescriptorBindingKind::Gds) != nullptr) && group_threads <= input_info.wave_size && !input_info.lds_storage) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			std::printf("KYTY_ORDERED_APPEND: shader 0x%016" PRIx64
			            " runs %ux%ux%u ordered-append workgroups one by one\n",
			            program.shader_hash, thread_group_x, thread_group_y, thread_group_z);
			std::fflush(stdout);
		}
		for (uint32_t z = 0; z < thread_group_z; ++z) {
			for (uint32_t y = 0; y < thread_group_y; ++y) {
				for (uint32_t x = 0; x < thread_group_x; ++x) {
					if ((x | y | z) != 0u) {
						ShaderAccessBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
					}
					buffer.Sink().dispatchBase(x, y, z, 1, 1, 1);
				}
			}
		}
	} else {
		buffer.Sink().dispatch(thread_group_x, thread_group_y, thread_group_z);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
>>>>>>> upstream/main

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	buffer.NoteGuestDispatch(no_overlap);
	if (program.info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(program.shader_hash);
	}
	// A proven uniform buffer fill leaves a known value (e.g. DCC fast-clear codes). Recording
	// it lets consumers skip reading the range back while nothing else writes it.
	{
		ShaderBufferResource known_descriptor;
		uint32_t             known_value = 0;
		uint64_t             known_size  = 0;
		const auto groups_x = use_thread_dimensions ? input_info.dispatch_threads_num[0]
		                                           : thread_group_x;
		if (ResolveComputeBufferFill(input_info, groups_x,
		                             use_thread_dimensions ? input_info.dispatch_threads_num[1]
		                                                   : thread_group_y,
		                             use_thread_dimensions ? input_info.dispatch_threads_num[2]
		                                                   : thread_group_z,
		                             mode, known_descriptor, known_value, known_size)) {
			m_context.GetBufferCache().RecordKnownFill(known_descriptor.Base48(), known_size,
			                                           known_value);
		}
	}
	// Observe only dispatches that actually reached the native path. This reuses the
	// already materialized descriptor/value proof and performs no guest-memory reads.
	if (Profiler::DetailedEnabled() && tracy::ProfilerAvailable() && TracyIsConnected) {
		ShaderBufferResource fill_descriptor;
		uint32_t fill_value = 0;
		uint64_t fill_size = 0;
		const auto groups_x = use_thread_dimensions ? input_info.dispatch_threads_num[0]
		                                           : thread_group_x;
		const auto groups_y = use_thread_dimensions ? input_info.dispatch_threads_num[1]
		                                           : thread_group_y;
		const auto groups_z = use_thread_dimensions ? input_info.dispatch_threads_num[2]
		                                           : thread_group_z;
		if (ResolveComputeBufferFill(input_info, groups_x, groups_y, groups_z, mode,
		                             fill_descriptor, fill_value, fill_size)) {
			KYTY_PROFILER_DETAIL_BLOCK("Compute::UniformFillEligible");
			static std::atomic<uint32_t> fill_count {0};
			constexpr uint32_t MaxFillMessages = 4096;
			const auto ordinal = fill_count.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < MaxFillMessages) {
				char text[512];
				const uint32_t byte = fill_value & 0xffu;
				const auto written = std::snprintf(
				    text, sizeof(text),
				    "COMPUTE_UNIFORM_FILL id=%u shader=0x%" PRIx64 " address=0x%" PRIx64
				    " bytes=%" PRIu64 " value=0x%08x byte_uniform=%u tick=%" PRIu64,
				    ordinal + 1u, program.shader_hash, fill_descriptor.Base48(), fill_size,
				    fill_value, fill_value == byte * 0x01010101u ? 1u : 0u,
				    m_context.GetCommandScheduler().CurrentTick());
				if (written > 0 && static_cast<size_t>(written) < sizeof(text)) {
					TracyMessageS(text, static_cast<size_t>(written), 0);
				}
			} else if (ordinal == MaxFillMessages) {
				TracyMessageLS("COMPUTE_UNIFORM_FILL_LIMIT messages=4096", 0);
			}
		}
	}
	ResetBindings();
}

void RenderExecutor::DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer,
                                      uint64_t args_addr, uint32_t mode, bool async_compute) {
	KYTY_GPU_OP_SITE("dispatch.indirect");
	g_overlap_indirect.fetch_add(1, std::memory_order_relaxed);
	const CpGaps::Scope gap(CpGaps::Cat::DispatchOp);
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopReadyOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	RenderLockGuard lock(m_context.GetMutex());
	// KYTY_DRAW_RUN: a dispatch ends the run of the draws before it (drawPrep/drawRun.h).
	BeginDrawRun();
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	input_info.async_compute = async_compute;
	CpGaps::Phase(CpGaps::Cat::DispatchProgram);
	ShaderProgram compute_program;
	// KYTY_DRAW_PREP_DISPATCH (drawPrep.h): as in DispatchDirect, from no workgroup counts.
	auto* const prepared = std::exchange(m_prepared_dispatch, nullptr);
	// KYTY_SKIP_EMPTY_INDIRECT (live, default on; upstream be5915c24): arguments no GPU write is
	// pending for, read from guest memory, with a zero group count: the dispatch does nothing.
	if (g_skip_empty_indirect.Get() != 0) {
		vk::DispatchIndirectCommand args {};
		if (!m_context.GetBufferCache().HasGpuDirtyBytes(args_addr, sizeof(args)) &&
		    LibKernel::Memory::TryReadBacking(args_addr, &args, sizeof(args)) &&
		    (args.x == 0 || args.y == 0 || args.z == 0)) {
			g_overlap_empty_indirect.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
	if (prepared != nullptr && !async_compute && DrawPrep::ValidateDispatch(*prepared, input_info)) {
		if (DrawPrep::VerifyMode() != 0) {
			auto serial_prep = std::make_unique<PipelineCache::StagePrep>();
			auto serial_info = input_info;
			const auto serial_program = m_context.GetPipelineCache().GetComputeProgram(
			    cs_regs, buffer.GetRegisters().GetShaderRegisters(), serial_info, *serial_prep);
			(void)DrawPrep::VerifyDispatch(*prepared, serial_program, serial_info, *serial_prep);
		}
		std::swap(m_compute_prep, prepared->compute_prep);
		input_info                 = prepared->compute_info;
		input_info.stage.resources = &m_compute_prep.resources;
		compute_program            = prepared->compute_program;
	} else {
		compute_program = m_context.GetPipelineCache().GetComputeProgram(
		    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info, m_compute_prep);
	}
	CpGaps::Phase(CpGaps::Cat::DispatchPipeline);
	if (!compute_program) {
		// Temporary until RT is implemented.
		return;
	}
	buffer.EndRendering();
	auto* pipeline_or_null =
	    m_context.GetPipelineCache().TryGetComputePipeline(input_info, compute_program);
	if (pipeline_or_null == nullptr) {
		ResetBindings();
		return;
	}
	auto& pipeline = *pipeline_or_null;
	auto& bindings = m_compute_bindings;
	CpGaps::Phase(CpGaps::Cat::DispatchBind);
	PrepareBindings(input_info.stage, bindings);
	// GPU-produced dispatch arguments: workgroup ids stay unbounded for write-range proofs.
	bindings.has_dispatch_groups = false;
	FindBuffers(bindings);
	CpGaps::Phase(CpGaps::Cat::DispatchBda);
	const auto& program = *input_info.stage.program;
	if (program.info.uses_dma) {
		m_context.PrepareBda();
	}
	if (program.info.uses_bvh) {
		Profiler::CountFrameEvent(Profiler::FrameEvent::RtStubDispatches);
	}
	if (program.info.bda_writes) {
		// KYTY_BDA_WRITES: settled synchronously right after the dispatch below. The settle relies
		// on PrepareBda's pass above having uploaded every CPU-dirty page first.
		EXIT_IF(!program.info.uses_dma);
		m_context.GetBufferCache().PrepareBdaWrites();
	}
	// Read the matched CPU view block before acquiring immutable native bindings;
	// synchronizing a GPU-dirty camera here may flush/merge cache allocations.
	const auto rr_material_camera=ReadRRMaterialCamera(m_context,input_info.stage);
	CpGaps::Phase(CpGaps::Cat::DispatchRebind);
	BindComputeLds(input_info, bindings, nullptr);
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	m_rebind_epoch = m_context.GetBufferCache().UnregisterEpoch();
	auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
	// KYTY_REBIND_AFTER_JOIN: the bindings' lookups joined the argument buffer (or one another's).
	if (RebindAfterJoinEnabled()) {
		auto& cache = m_context.GetBufferCache();
		for (uint32_t pass = 0; pass < 4 && cache.UnregisterEpoch() != m_rebind_epoch; pass++) {
			NoteRebindAfterJoin("indirect dispatch", nullptr);
			m_rebind_epoch               = cache.UnregisterEpoch();
			std::tie(args_buffer, args_offset) =
			    cache.ObtainBuffer(args_addr, sizeof(vk::DispatchIndirectCommand), false);
			EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
			RebindBuffers(bindings);
		}
	}
	CpGaps::Phase(CpGaps::Cat::DispatchEmit);
	PreparedBindings* descriptor_stage = &bindings;
	// Emission safe point (see DispatchDirect).
	buffer.BeginEmission();
	PrepareRayGuideBinding(m_context,buffer,bindings,program);
	if (bindings.ray_guide.buffer && m_context.rr_reflection_capture) {
		vk::DescriptorBufferInfo tile_head {};
		if (!bindings.buffers.empty() && !bindings.buffer_sources.empty() && bindings.buffer_sources[0].size>=4) {
			tile_head=bindings.buffers[0];
			const auto offset=bindings.shader_data.at(program.bindings.memory_offset_dword)&0xffu;
			if (tile_head.range>=offset+4) { tile_head.offset+=offset; tile_head.range=4; }
			else tile_head={};
		}
		m_context.rr_reflection_capture->CaptureDispatch(buffer,args_buffer->Handle(),args_offset,program.shader_hash,tile_head);
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline,
	               std::span {&descriptor_stage, 1u});
	const bool has_storage_writes = bindings.shared_memory.buffer != nullptr ||
	    HasShaderBufferWrites(input_info.stage) || program.info.bda_writes ||
	    std::any_of(program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.written && image.resource_class ==
		                                ShaderRecompiler::IR::ImageResourceClass::Storage;
	    });
	if (has_storage_writes) {
		ShaderWriteHazardBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	if (BarrierBatchEnabled()) {
		buffer.RequestMemoryBarrier(vk::PipelineStageFlagBits2::eAllGraphics |
		                                vk::PipelineStageFlagBits2::eComputeShader |
		                                vk::PipelineStageFlagBits2::eTransfer,
		                            vk::AccessFlagBits2::eShaderWrite |
		                                vk::AccessFlagBits2::eTransferWrite,
		                            vk::PipelineStageFlagBits2::eDrawIndirect,
		                            vk::AccessFlagBits2::eIndirectCommandRead,
		                            BarrierOrigin::IndirectArgs);
	} else {
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask =
		    vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
		buffer.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics |
		                                    vk::PipelineStageFlagBits::eComputeShader |
		                                    vk::PipelineStageFlagBits::eTransfer,
		                                vk::PipelineStageFlagBits::eDrawIndirect, {}, 1, &barrier,
		                                0, nullptr, 0, nullptr);
	}
	buffer.BindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	const auto rr_trace = Config::DLSSDiagnosticsEnabled() && m_context.HasGpu()
	    ? RRGuestTrace::Begin(m_context.GetGpu().GetFrameNum(), "compute_indirect", program.shader_hash) : 0;
	RRGuestTrace::Stage(rr_trace, "CS", program, *input_info.stage.resources, &bindings);
	RRGuestTrace::End(rr_trace);
	buffer.Sink().dispatchIndirect(args_buffer->Handle(), args_offset);
	FinishRayGuideWrite(buffer,bindings);
	Common::DebugCounters::Add(Common::DebugCounters::Counter::Dispatches);
	ShaderAccessBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	CaptureRRMaterials(m_context,buffer,bindings,rr_material_camera);
	if (program.info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(program.shader_hash);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
