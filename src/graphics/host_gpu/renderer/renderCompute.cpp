#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/graphicContext.h"
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
#include "kernel/memory.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "graphics/host_gpu/renderer/gpuOpProfiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
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

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
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
		// A metadata resource that is also read is not proven to be a full overwrite. Execute it
		// conservatively instead of replacing the dispatch with a coarse full-surface clear.
		if ((!resource.written || resource.read) && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48())) {
					return true;
				}
			}
		}
	}
	return false;
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

void RenderExecutor::DispatchDirect(uint64_t submit_id, CommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	KYTY_GPU_OP_SITE("dispatch");
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

	Common::LockGuard lock(m_context.GetMutex());
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
	input_info.dispatch_thread_dimensions = use_thread_dimensions;
	const auto compute_program =
	    m_context.GetPipelineCache().GetComputeProgram(cs_regs, sh_regs, input_info,
	                                                   m_compute_prep);
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
	if (TryConsumeComputeMetaClear(input_info, buffer)) {
		ResetBindings();
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		ResetBindings();
		return;
	}
	const bool large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const bool                   has_sampler = !program.info.samplers.empty();
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
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		const auto sampled_images = std::count_if(
		    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
			    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
		    });
		const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
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

	// The removed host fence also ordered read-only dispatches before later writers.
	ShaderAccessBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
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
                                      uint64_t args_addr, uint32_t mode) {
	KYTY_GPU_OP_SITE("dispatch.indirect");
	EXIT_IF(buffer.IsInvalid() || args_addr == 0 || (args_addr & 3u) != 0 ||
	        (mode & Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0);
	m_context.GetCommandScheduler().PopReadyOperations();
	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchIndirect), submit_id,
	                    static_cast<uint32_t>(args_addr), static_cast<uint32_t>(args_addr >> 32u),
	                    0, mode, buffer.GetShaders().GetCs().cs_regs.data_addr);
	Common::LockGuard lock(m_context.GetMutex());
	// KYTY_DRAW_RUN: a dispatch ends the run of the draws before it (drawPrep/drawRun.h).
	BeginDrawRun();
	const auto& cs_regs = buffer.GetShaders().GetCs();
	if (cs_regs.cs_regs.data_addr == 0) {
		return;
	}
	ShaderComputeInputInfo input_info {};
	const auto compute_program = m_context.GetPipelineCache().GetComputeProgram(
	    cs_regs, buffer.GetRegisters().GetShaderRegisters(), input_info, m_compute_prep);
	if (!compute_program) {
		// Temporary until RT is implemented.
		return;
	}
	buffer.EndRendering();
	auto& pipeline = m_context.GetPipelineCache().GetComputePipeline(input_info, compute_program);
	auto& bindings = m_compute_bindings;
	PrepareBindings(input_info.stage, bindings);
	// GPU-produced dispatch arguments: workgroup ids stay unbounded for write-range proofs.
	bindings.has_dispatch_groups = false;
	FindBuffers(bindings);
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
	RebindImages(bindings);
	// Acquiring arguments can merge cache buffers; finalize shader bindings afterward.
	const auto [args_buffer, args_offset] = m_context.GetBufferCache().ObtainBuffer(
	    args_addr, sizeof(vk::DispatchIndirectCommand), false);
	EXIT_IF(args_buffer == nullptr || (args_offset & 3u) != 0);
	RebindBuffers(bindings);
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
	const bool has_storage_writes = HasShaderBufferWrites(input_info.stage) ||
	    program.info.bda_writes ||
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
	ShaderAccessBarrier(buffer, vk::PipelineStageFlagBits::eComputeShader);
	CaptureRRMaterials(m_context,buffer,bindings,rr_material_camera);
	if (program.info.bda_writes) {
		m_context.GetBufferCache().SettleBdaWrites(program.shader_hash);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
