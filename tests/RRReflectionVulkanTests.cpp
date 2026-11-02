#include "graphics/host_gpu/graphicContext.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace Libs::Graphics;

void RunRRReflectionExportSmoke(GraphicContext& g) {
	namespace IR=ShaderRecompiler::IR;
	auto check=[](vk::Result r,const char* label) {
		if(r!=vk::Result::eSuccess) throw std::runtime_error(std::string(label)+": "+vk::to_string(r));
	};
	struct Cleanup {
		GraphicContext& g;
		VkBuffer buffer=VK_NULL_HANDLE; VmaAllocation allocation=nullptr;
		vk::ShaderModule shader {}; vk::Pipeline pipeline {}; vk::PipelineLayout layout {};
		vk::DescriptorSetLayout set_layout {}; vk::DescriptorPool descriptors {}; vk::CommandPool commands {};
		~Cleanup() {
			(void)g.device.waitIdle();
			if(pipeline)g.device.destroyPipeline(pipeline,nullptr);
			if(layout)g.device.destroyPipelineLayout(layout,nullptr);
			if(shader)g.device.destroyShaderModule(shader,nullptr);
			if(descriptors)g.device.destroyDescriptorPool(descriptors,nullptr);
			if(set_layout)g.device.destroyDescriptorSetLayout(set_layout,nullptr);
			if(commands)g.device.destroyCommandPool(commands,nullptr);
			if(buffer)vmaDestroyBuffer(g.allocator,buffer,allocation);
		}
	} owned {g};
	IR::Program program;
	program.stage=ShaderType::Compute; program.wave_size=32;
	program.shader_info_complete=true; program.resource_tracking_complete=true;
	program.srt_plan_complete=true; // no guest descriptors/SRT reads in this focused program
	program.block_storage.push_back(std::make_unique<IR::Block>());
	program.blocks.push_back(program.block_storage.back().get());
	program.block_info.push_back({.id=0});
	program.block_info.back().terminator.kind=ShaderRecompiler::CFG::TerminatorKind::Return;
	auto store=[&](uint32_t x,uint32_t y,uint32_t mode,bool active,uint32_t t=0x40200000u) {
		program.blocks[0]->AppendNewInst(IR::ValueOpcode::StoreRayGuide,
		    {IR::Value(t),IR::Value(17u),IR::Value(x),IR::Value(y),IR::Value(mode),IR::Value(active)});
	};
	store(0,0,1,true); store(0,0,0,true); store(0,0,1,true); // coverage must preserve the hit
	store(0,0,0,true); // multiple probe-loop visits must not look like a unique ray
	store(1,0,1,true); // coverage without a ray is not a miss
	store(0,0,0,false,0x40800000u); // inactive lane must preserve its previous value
	store(UINT32_MAX,0,0,true); store(0,UINT32_MAX,0,true); store(2,0,0,true);
	IR::AllocateBindings(program);
	ShaderComputeInputInfo input {}; input.threads_num[0]=input.threads_num[1]=input.threads_num[2]=1;
	input.wave_size=32; input.host_subgroup_size=32;
	ShaderStageInputInfo stage {}; stage.compute=&input;
	const auto spirv=ShaderRecompiler::Spirv::EmitProgram(program,stage);
	vk::ShaderModuleCreateInfo module {}; module.codeSize=spirv.size()*4; module.pCode=spirv.data();
	check(g.device.createShaderModule(&module,nullptr,&owned.shader),"ray export shader");
	vk::DescriptorSetLayoutBinding binding {IR::NativeBinding(ShaderType::Compute,IR::DescriptorBindingKind::RayGuide),
	    vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute,nullptr};
	vk::DescriptorSetLayoutCreateInfo set {}; set.bindingCount=1; set.pBindings=&binding;
	check(g.device.createDescriptorSetLayout(&set,nullptr,&owned.set_layout),"ray export descriptor layout");
	vk::PipelineLayoutCreateInfo layout {}; layout.setLayoutCount=1; layout.pSetLayouts=&owned.set_layout;
	check(g.device.createPipelineLayout(&layout,nullptr,&owned.layout),"ray export pipeline layout");
	vk::ComputePipelineCreateInfo pipeline {}; pipeline.layout=owned.layout;
	pipeline.stage.stage=vk::ShaderStageFlagBits::eCompute; pipeline.stage.module=owned.shader; pipeline.stage.pName="main";
	check(g.device.createComputePipelines(nullptr,1,&pipeline,nullptr,&owned.pipeline),"ray export pipeline");
	vk::DescriptorPoolSize size {vk::DescriptorType::eStorageBuffer,1};
	vk::DescriptorPoolCreateInfo pool {}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
	check(g.device.createDescriptorPool(&pool,nullptr,&owned.descriptors),"ray export descriptor pool");
	vk::DescriptorSetAllocateInfo alloc {}; alloc.descriptorPool=owned.descriptors;
	alloc.descriptorSetCount=1; alloc.pSetLayouts=&owned.set_layout;
	vk::DescriptorSet descriptor; check(g.device.allocateDescriptorSets(&alloc,&descriptor),"ray export descriptors");
	std::array<uint32_t,20> initial {};
	VkBufferCreateInfo buffer {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; buffer.size=sizeof(initial); buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VmaAllocationCreateInfo host {}; host.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	host.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	VmaAllocationInfo mapping {};
	if(vmaCreateBuffer(g.allocator,&buffer,&host,&owned.buffer,&owned.allocation,&mapping)!=VK_SUCCESS)
		throw std::runtime_error("ray export test buffer allocation failed");
	vk::CommandPoolCreateInfo commands {}; commands.queueFamilyIndex=g.queue_family; commands.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	check(g.device.createCommandPool(&commands,nullptr,&owned.commands),"ray export command pool");
	vk::CommandBufferAllocateInfo ca {}; ca.commandPool=owned.commands; ca.commandBufferCount=1;
	vk::CommandBuffer command; check(g.device.allocateCommandBuffers(&ca,&command),"ray export command");
	for(unsigned test=0; test<3; ++test) {
		initial.fill(0xA5DEAD5Au); initial[0]=test==1?16385u:2u; initial[1]=1; initial[2]=0; initial[3]=5;
		std::fill(initial.begin()+4,initial.begin()+12,0u);
		auto expected=initial;
		if(test==0) {expected[4]=0x40200000u; expected[5]=17; expected[6]=1; expected[7]=2; expected[10]=2;}
		std::memcpy(mapping.pMappedData,initial.data(),sizeof(initial));
		vmaFlushAllocation(g.allocator,owned.allocation,0,VK_WHOLE_SIZE);
		vk::DescriptorBufferInfo view {owned.buffer,0,test==2?16u:sizeof(initial)};
		vk::WriteDescriptorSet write {}; write.dstSet=descriptor; write.dstBinding=binding.binding;
		write.descriptorCount=1; write.descriptorType=vk::DescriptorType::eStorageBuffer; write.pBufferInfo=&view;
		g.device.updateDescriptorSets(1,&write,0,nullptr);
		check(command.reset({}),"ray export reset"); vk::CommandBufferBeginInfo begin {};
		check(command.begin(&begin),"ray export begin");
		vk::MemoryBarrier visible {}; visible.srcAccessMask=vk::AccessFlagBits::eHostWrite;
		visible.dstAccessMask=vk::AccessFlagBits::eShaderRead|vk::AccessFlagBits::eShaderWrite;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eComputeShader,{},1,&visible,0,nullptr,0,nullptr);
		command.bindPipeline(vk::PipelineBindPoint::eCompute,owned.pipeline);
		command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,owned.layout,0,1,&descriptor,0,nullptr);
		command.dispatch(1,1,1);
		visible.srcAccessMask=vk::AccessFlagBits::eShaderWrite; visible.dstAccessMask=vk::AccessFlagBits::eHostRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eHost,{},1,&visible,0,nullptr,0,nullptr);
		check(command.end(),"ray export end"); vk::SubmitInfo submit {}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
		check(g.queue.submit(1,&submit,nullptr),"ray export submit"); check(g.device.waitIdle(),"ray export wait");
		vmaInvalidateAllocation(g.allocator,owned.allocation,0,VK_WHOLE_SIZE);
		if(std::memcmp(mapping.pMappedData,expected.data(),sizeof(expected)))
			throw std::runtime_error("ray export corrupted inactive/out-of-bounds/guard data, case "+std::to_string(test));
	}
	std::puts("RR raw GPU export: hit/coverage, repeated rays, inactive lanes, coordinate overflow, extent/descriptor bounds and canaries: PASS");
}
