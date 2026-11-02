#include "graphics/presentation/dlss/rrPlayroomMaterials.h"
#include "graphics/host_gpu/graphicContext.h"
#include "gpu_blit_shaders/gpu_rr_playroom_materials_spv.h"
#include "common/logging/log.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Libs::Graphics {
namespace {
void Check(vk::Result result, const char* operation) {
    if(result!=vk::Result::eSuccess)throw std::runtime_error(std::string(operation)+": "+vk::to_string(result));
}
struct alignas(16) Constants {
    DLSSMatrix inverse_projection, view_to_world;
    std::array<float,4> position;
    std::array<uint32_t,4> extent;
};
static_assert(sizeof(Constants)==160);
bool Color(vk::Format f) {
    return f==vk::Format::eR16G16B16A16Sfloat||f==vk::Format::eR32G32B32A32Sfloat||
        f==vk::Format::eR8G8B8A8Unorm||f==vk::Format::eR8G8B8A8Srgb||
        f==vk::Format::eB8G8R8A8Unorm||f==vk::Format::eB8G8R8A8Srgb||
        f==vk::Format::eR16G16B16A16Snorm;
}
}
struct RRPlayroomMaterials::Pipeline {
    GraphicContext& g;
    vk::DescriptorSetLayout set_layout {};
    vk::PipelineLayout layout {};
    vk::ShaderModule shader {};
    vk::Pipeline pipeline {};
    vk::Sampler sampler {};
    explicit Pipeline(GraphicContext& graphics):g(graphics) {}
    ~Pipeline() {
        if(pipeline)g.device.destroyPipeline(pipeline,nullptr);
        if(layout)g.device.destroyPipelineLayout(layout,nullptr);
        if(shader)g.device.destroyShaderModule(shader,nullptr);
        if(set_layout)g.device.destroyDescriptorSetLayout(set_layout,nullptr);
        if(sampler)g.device.destroySampler(sampler,nullptr);
    }
    void Initialize() {
        std::array<vk::DescriptorSetLayoutBinding,8> bindings {};
        for(uint32_t i=0;i<8;++i)bindings[i]={i,i<4?vk::DescriptorType::eCombinedImageSampler:
            i<7?vk::DescriptorType::eStorageImage:vk::DescriptorType::eUniformBuffer,1,vk::ShaderStageFlagBits::eCompute,nullptr};
        vk::DescriptorSetLayoutCreateInfo set {};set.bindingCount=8;set.pBindings=bindings.data();
        Check(g.device.createDescriptorSetLayout(&set,nullptr,&set_layout),"RR materials descriptor layout");
        vk::PipelineLayoutCreateInfo l {};l.setLayoutCount=1;l.pSetLayouts=&set_layout;
        Check(g.device.createPipelineLayout(&l,nullptr,&layout),"RR materials pipeline layout");
        vk::ShaderModuleCreateInfo s {};s.codeSize=sizeof(GPU_RR_PLAYROOM_MATERIALS_SPV);s.pCode=GPU_RR_PLAYROOM_MATERIALS_SPV;
        Check(g.device.createShaderModule(&s,nullptr,&shader),"RR materials shader");
        vk::ComputePipelineCreateInfo p {};p.layout=layout;
        p.stage.stage=vk::ShaderStageFlagBits::eCompute;p.stage.module=shader;p.stage.pName="main";
        Check(g.device.createComputePipelines(nullptr,1,&p,nullptr,&pipeline),"RR materials pipeline");
        vk::SamplerCreateInfo sam {};sam.magFilter=sam.minFilter=vk::Filter::eNearest;
        sam.mipmapMode=vk::SamplerMipmapMode::eNearest;
        sam.addressModeU=sam.addressModeV=sam.addressModeW=vk::SamplerAddressMode::eClampToEdge;
        Check(g.device.createSampler(&sam,nullptr,&sampler),"RR materials sampler");
    }
};
struct RRPlayroomMaterials::Frame::Storage {
    std::shared_ptr<Pipeline> pipeline;
    std::array<VulkanImage,3> images;
    std::array<RRTexture,3> textures;
    vk::DescriptorPool descriptors {};
    VkBuffer constants=VK_NULL_HANDLE;
    VmaAllocation allocation=nullptr;
    VkBuffer readback=VK_NULL_HANDLE;
    VmaAllocation readback_allocation=nullptr;
    void* readback_mapping=nullptr;
    explicit Storage(std::shared_ptr<Pipeline> p):pipeline(std::move(p)) {}
    ~Storage() {
        auto& g=pipeline->g;
        if(descriptors)g.device.destroyDescriptorPool(descriptors,nullptr);
        for(size_t i=0;i<images.size();++i) {
            if(textures[i].view)g.device.destroyImageView(textures[i].view,nullptr);
            if(images[i].image)g.DeleteImage(images[i]);
        }
        if(constants)vmaDestroyBuffer(g.allocator,constants,allocation);
        if(readback)vmaDestroyBuffer(g.allocator,readback,readback_allocation);
    }
};
RRPlayroomMaterials::Frame::~Frame()=default;
const RRTexture& RRPlayroomMaterials::Frame::Diffuse()const{return storage->textures[0];}
const RRTexture& RRPlayroomMaterials::Frame::Specular()const{return storage->textures[1];}
const RRTexture& RRPlayroomMaterials::Frame::NormalRoughness()const{return storage->textures[2];}
void RRPlayroomMaterials::Frame::RecordReadback(vk::CommandBuffer command) {
    auto& s=*storage;auto& g=s.pipeline->g;
    if(s.readback)throw std::logic_error("material readback already recorded");
    const auto extent=s.textures[0].extent;
    const uint64_t image_bytes=uint64_t(extent.width)*extent.height*8;
    VkBufferCreateInfo bi {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=image_bytes*3;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo host {};host.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    host.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    VmaAllocationInfo ai {};
    if(vmaCreateBuffer(g.allocator,&bi,&host,&s.readback,&s.readback_allocation,&ai)!=VK_SUCCESS)
        throw std::runtime_error("material readback allocation failed");
    s.readback_mapping=ai.pMappedData;
    for(unsigned i=0;i<3;++i) {
        vk::ImageMemoryBarrier b {};b.oldLayout=vk::ImageLayout::eShaderReadOnlyOptimal;b.newLayout=vk::ImageLayout::eTransferSrcOptimal;
        b.srcAccessMask=vk::AccessFlagBits::eShaderWrite|vk::AccessFlagBits::eShaderRead;b.dstAccessMask=vk::AccessFlagBits::eTransferRead;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=s.textures[i].image;
        b.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},0,nullptr,0,nullptr,1,&b);
        vk::BufferImageCopy copy {};copy.bufferOffset=i*image_bytes;
        copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};copy.imageExtent={extent.width,extent.height,1};
        command.copyImageToBuffer(b.image,b.newLayout,s.readback,1,&copy);
        b.oldLayout=b.newLayout;b.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
        b.srcAccessMask=vk::AccessFlagBits::eTransferRead;b.dstAccessMask=vk::AccessFlagBits::eShaderRead;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eAllCommands,{},0,nullptr,0,nullptr,1,&b);
    }
    vk::MemoryBarrier done {};done.srcAccessMask=vk::AccessFlagBits::eTransferWrite;done.dstAccessMask=vk::AccessFlagBits::eHostRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},1,&done,0,nullptr,0,nullptr);
}
void RRPlayroomMaterials::Frame::Report(uint64_t guest_frame) const {
    const auto& s=*storage;if(!s.readback_mapping)return;
    auto& g=s.pipeline->g;vmaInvalidateAllocation(g.allocator,s.readback_allocation,0,VK_WHOLE_SIZE);
    const auto extent=s.textures[0].extent;
    const uint64_t pixels=uint64_t(extent.width)*extent.height;
    const auto* values=static_cast<const uint16_t*>(s.readback_mapping);
    const auto half=[](uint16_t h) {
        const unsigned e=(h>>10)&31,m=h&1023;
        return (h&0x8000?-1.0:1.0)*(e==0?std::ldexp(double(m),-24):
            e==31?std::numeric_limits<double>::infinity():std::ldexp(1.0+double(m)/1024,int(e)-15));
    };
    uint64_t valid=0,invalid=0;double min_rough=1,max_rough=0;
    for(uint64_t i=0;i<pixels;++i) {
        if(values[i*4+3]!=0x3C00||values[pixels*4+i*4+3]!=0x3C00)continue;
        bool finite=true;
        for(unsigned c=0;c<3;++c)for(unsigned j=0;j<4;++j)
            finite=finite&&std::isfinite(half(values[c*pixels*4+i*4+j]));
        const auto* normal=values+pixels*8+i*4;
        const double x=half(normal[0]),y=half(normal[1]),z=half(normal[2]),r=half(normal[3]);
        if(!finite||std::abs(x*x+y*y+z*z-1)>.005||r<0||r>1){++invalid;continue;}
        ++valid;min_rough=std::min(min_rough,r);max_rough=std::max(max_rough,r);
    }
    Log::WriteToConsoleAndLog(fmt::format("[DLSS-RR][Materials] frame={} extent={}x{} valid={} masked={} invalid={} roughness={}..{} (lighting/reflection certification pending)\n",
        guest_frame,extent.width,extent.height,valid,pixels-valid-invalid,invalid,valid?min_rough:0,max_rough));
}
RRPlayroomMaterials::RRPlayroomMaterials(GraphicContext& graphics):m_graphics(graphics) {}
RRPlayroomMaterials::~RRPlayroomMaterials()=default;

std::string RRPlayroomMaterials::Validate(const RRPlayroomMaterialInputs& in) {
    const auto extent=in.base_color.extent;
    if(!extent.width||!extent.height||extent.width>8192||extent.height>8192||
        uint64_t(extent.width)*extent.height*24>256ull*1024*1024)return "invalid or excessive material extent";
    for(const auto* t:{&in.base_color,&in.metal_roughness,&in.view_normal,&in.depth}) {
        if(!t->image||!t->view)return "missing material image/view";
        if(t->extent!=extent)return "material inputs use different render grids";
        if((t->layout!=vk::ImageLayout::eShaderReadOnlyOptimal&&t->layout!=vk::ImageLayout::eGeneral&&
            !(t==&in.depth&&t->layout==vk::ImageLayout::eDepthStencilReadOnlyOptimal))||!(t->usage&vk::ImageUsageFlagBits::eSampled))
            return "material input must be shader-readable";
    }
    if(!Color(in.base_color.format)||!Color(in.metal_roughness.format)||!Color(in.view_normal.format))
        return "unsupported material color format";
    if(in.metal_roughness.format==vk::Format::eR8G8B8A8Srgb||in.metal_roughness.format==vk::Format::eB8G8R8A8Srgb||
        in.view_normal.format==vk::Format::eR8G8B8A8Srgb||in.view_normal.format==vk::Format::eB8G8R8A8Srgb)
        return "normal and metal/roughness inputs must be linear";
    if(in.depth.format!=vk::Format::eR32Sfloat&&in.depth.format!=vk::Format::eD32Sfloat&&
        in.depth.format!=vk::Format::eD32SfloatS8Uint)return "unsupported material depth format";
    for(float value:in.camera)if(!std::isfinite(value))return "non-finite material camera block";
    if(in.camera[108]!=float(extent.width)||in.camera[109]!=float(extent.height))return "material camera viewport mismatch";
    DLSSMatrix inverse {},projection {},identity {},view {},view_inverse {};identity[0]=identity[5]=identity[10]=identity[15]=1;
    std::copy_n(in.camera.data()+64,16,inverse.data());std::copy_n(in.camera.data()+32,16,projection.data());
    std::copy_n(in.camera.data()+16,16,view.data());std::copy_n(in.camera.data()+48,16,view_inverse.data());
    const auto view_product=DLSSMultiply(view,view_inverse);
    for(float value:view_product)if(!std::isfinite(value))return "overflow in material view transform pair";
    if(!DLSSMatricesMatch(view_product,identity))return "invalid material view transform pair";
    const bool perspective=std::abs(projection[11]+1)<1e-5f&&std::abs(projection[15])<1e-5f;
    const bool orthographic=std::abs(projection[11])<1e-5f&&std::abs(projection[15]-1)<1e-5f;
    if((!perspective&&!orthographic)||projection[0]<=0||projection[5]<=0)return "unsupported material projection";
    for(int r=0;r<3;++r) {
        if(std::abs(view_inverse[r*4+3])>1e-5f ||
           std::abs(in.camera[100+r]-view_inverse[12+r])>1e-4f*(1+std::abs(in.camera[100+r])))
            return "material camera position/affine transform mismatch";
        for(int c=0;c<3;++c) {
            double dot=0;for(int k=0;k<3;++k)dot+=double(view_inverse[r*4+k])*view_inverse[c*4+k];
            if(std::abs(dot-(r==c?1.0:0.0))>1e-4)return "material camera basis is not orthonormal";
        }
    }
    if(std::abs(view_inverse[15]-1)>1e-5f)return "invalid material affine transform";
    const auto product=DLSSMultiply(projection,inverse);
    for(float v:product)if(!std::isfinite(v))return "invalid material inverse projection";
    if(!DLSSMatricesMatch(product,identity))return "material inverse projection does not match guest projection";
    return {};
}
std::shared_ptr<RRPlayroomMaterials::Frame> RRPlayroomMaterials::Record(vk::CommandBuffer command,const RRPlayroomMaterialInputs& in) {
    const auto error=Validate(in);
    if(!error.empty()||!command)throw std::invalid_argument(error.empty()?"invalid material command buffer":error);
    if(!m_pipeline) {
        auto p=std::make_shared<Pipeline>(m_graphics);p->Initialize();m_pipeline=std::move(p);
    }
    auto frame=std::make_shared<Frame>();frame->storage=std::make_unique<Frame::Storage>(m_pipeline);
    auto& owned=*frame->storage;auto& g=m_graphics;
    const auto extent=in.base_color.extent;
    for(size_t i=0;i<3;++i) {
        auto& t=owned.textures[i];t.extent=extent;t.format=vk::Format::eR16G16B16A16Sfloat;
        t.usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eTransferSrc;
        t.layout=vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::ImageCreateInfo ci {};ci.imageType=vk::ImageType::e2D;ci.format=t.format;
        ci.extent={extent.width,extent.height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=vk::SampleCountFlagBits::e1;
        ci.tiling=vk::ImageTiling::eOptimal;ci.usage=t.usage;
        if(!g.CreateImage(ci,owned.images[i]))throw std::runtime_error("RR material image allocation failed");
        t.image=owned.images[i].image;
        vk::ImageViewCreateInfo view {};view.image=t.image;view.format=t.format;view.viewType=vk::ImageViewType::e2D;
        view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
        Check(g.device.createImageView(&view,nullptr,&t.view),"RR materials image view");
        VmaAllocationInfo ai {};vmaGetAllocationInfo(g.allocator,owned.images[i].allocation,&ai);t.memory=ai.deviceMemory;
    }
    Constants values {};std::copy_n(in.camera.data()+64,16,values.inverse_projection.data());
    std::copy_n(in.camera.data()+48,16,values.view_to_world.data());
    std::copy_n(in.camera.data()+100,3,values.position.data());
    values.extent={extent.width,extent.height,std::abs(in.camera[43])<1e-5f?1u:0u,0};
    VkBufferCreateInfo bi {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=sizeof(values);bi.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo host {};host.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    host.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VmaAllocationInfo mapped {};
    if(vmaCreateBuffer(g.allocator,&bi,&host,&owned.constants,&owned.allocation,&mapped)!=VK_SUCCESS)
        throw std::runtime_error("RR material constants allocation failed");
    std::memcpy(mapped.pMappedData,&values,sizeof(values));vmaFlushAllocation(g.allocator,owned.allocation,0,VK_WHOLE_SIZE);
    const std::array<vk::DescriptorPoolSize,3> sizes {{{vk::DescriptorType::eCombinedImageSampler,4},
        {vk::DescriptorType::eStorageImage,3},{vk::DescriptorType::eUniformBuffer,1}}};
    vk::DescriptorPoolCreateInfo pool {};pool.maxSets=1;pool.poolSizeCount=3;pool.pPoolSizes=sizes.data();
    Check(g.device.createDescriptorPool(&pool,nullptr,&owned.descriptors),"RR materials descriptor pool");
    vk::DescriptorSetAllocateInfo alloc {};alloc.descriptorPool=owned.descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&m_pipeline->set_layout;
    vk::DescriptorSet descriptor;Check(g.device.allocateDescriptorSets(&alloc,&descriptor),"RR materials descriptor set");
    std::array<vk::DescriptorImageInfo,7> image_info {};
    const std::array<const RRTexture*,4> inputs {&in.base_color,&in.metal_roughness,&in.view_normal,&in.depth};
    for(size_t i=0;i<7;++i)image_info[i]={i<4?m_pipeline->sampler:vk::Sampler{},
        i<4?inputs[i]->view:owned.textures[i-4].view,i<4?inputs[i]->layout:vk::ImageLayout::eGeneral};
    vk::DescriptorBufferInfo uniform {owned.constants,0,sizeof(values)};
    std::array<vk::WriteDescriptorSet,8> writes {};
    for(uint32_t i=0;i<8;++i) {
        auto& w=writes[i];w.dstSet=descriptor;w.dstBinding=i;w.descriptorCount=1;
        w.descriptorType=i<4?vk::DescriptorType::eCombinedImageSampler:i<7?vk::DescriptorType::eStorageImage:vk::DescriptorType::eUniformBuffer;
        if(i<7)w.pImageInfo=&image_info[i];else w.pBufferInfo=&uniform;
    }
    g.device.updateDescriptorSets(8,writes.data(),0,nullptr);
    // No commands referencing owned resources are recorded until allocation succeeds.
    std::array<vk::ImageMemoryBarrier,3> barriers {};
    for(size_t i=0;i<3;++i) {
        auto& b=barriers[i];b.dstAccessMask=vk::AccessFlagBits::eShaderWrite;
        b.oldLayout=vk::ImageLayout::eUndefined;b.newLayout=vk::ImageLayout::eGeneral;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=owned.textures[i].image;b.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
    }
    vk::MemoryBarrier host_visible {};host_visible.srcAccessMask=vk::AccessFlagBits::eHostWrite;host_visible.dstAccessMask=vk::AccessFlagBits::eUniformRead;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe|vk::PipelineStageFlagBits::eHost,
        vk::PipelineStageFlagBits::eComputeShader,{},1,&host_visible,0,nullptr,3,barriers.data());
    command.bindPipeline(vk::PipelineBindPoint::eCompute,m_pipeline->pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,m_pipeline->layout,0,1,&descriptor,0,nullptr);
    command.dispatch((extent.width+7)/8,(extent.height+7)/8,1);
    for(auto& b:barriers) {
        b.srcAccessMask=vk::AccessFlagBits::eShaderWrite;b.dstAccessMask=vk::AccessFlagBits::eShaderRead;
        b.oldLayout=vk::ImageLayout::eGeneral;b.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eAllCommands,
        {},0,nullptr,0,nullptr,3,barriers.data());
    return frame;
}
}
