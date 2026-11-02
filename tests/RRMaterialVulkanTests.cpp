#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/dlss/rrPlayroomMaterials.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace Libs::Graphics;
namespace {
double Half(uint16_t h) {
    const unsigned e=(h>>10)&31,m=h&1023;
    return (h&0x8000?-1.0:1.0)*(e==0?std::ldexp(double(m),-24):e==31?
        std::numeric_limits<double>::infinity():std::ldexp(1.0+double(m)/1024,int(e)-15));
}
template<size_t R,size_t C> double Bilinear(const std::array<double,R*C>& matrix,
        const std::array<double,R>& y,const std::array<double,C>& x) {
    double sum=0;
    for(size_t r=0;r<R;++r)for(size_t c=0;c<C;++c)sum+=y[r]*matrix[r*C+c]*x[c];
    return sum;
}
std::array<double,3> Specular(const std::array<double,3>& f0,double rough,double nv) {
    const double n=std::abs(nv),a=rough*rough;
    const std::array<double,2> x2 {1,n},y2 {1,a};
    const std::array<double,3> x3 {1,n,n*n*n},xz3 {1,n*n,n*n*n},y3 {1,a,a*a*a};
    double bias=Bilinear<2,2>({.99044,-1.28514,1.29678,-.755907},y2,x2)/
        Bilinear<3,3>({1,2.92338,59.4188,20.3225,-27.0302,222.592,121.563,626.13,316.627},y3,x3);
    const double scale=Bilinear<2,2>({.0365463,3.32707,9.0632,-9.04756},y2,x2)/
        Bilinear<3,3>({1,3.59685,-1.36772,9.04401,-16.3174,9.22949,5.56589,19.7886,-20.2123},y3,xz3);
    bias*=std::clamp(f0[1]*50.0,0.0,1.0);
    std::array<double,3> result {};
    for(int c=0;c<3;++c)result[c]=f0[c]*std::max(0.0,scale)+std::max(0.0,bias);
    return result;
}
}
void RunRRMaterialSmoke(GraphicContext& g) {
    auto check=[](vk::Result r,const char* label) {
        if(r!=vk::Result::eSuccess)throw std::runtime_error(std::string(label)+": "+vk::to_string(r));
    };
    constexpr unsigned width=9,height=3,pixels=width*height;
    struct Cleanup {
        GraphicContext& g;
        std::array<VulkanImage,4> images;
        std::array<vk::ImageView,4> views {};
        vk::CommandPool pool {};
        VkBuffer buffer=VK_NULL_HANDLE;VmaAllocation allocation=nullptr;
        ~Cleanup() {
            (void)g.device.waitIdle();
            if(pool)g.device.destroyCommandPool(pool,nullptr);
            for(size_t i=0;i<4;++i) {
                if(views[i])g.device.destroyImageView(views[i],nullptr);
                if(images[i].image)g.DeleteImage(images[i]);
            }
            if(buffer)vmaDestroyBuffer(g.allocator,buffer,allocation);
        }
    } owned {g};
    RRPlayroomMaterialInputs in;
    std::array<RRTexture*,4> textures {&in.base_color,&in.metal_roughness,&in.view_normal,&in.depth};
    for(unsigned i=0;i<4;++i) {
        auto& t=*textures[i];t.extent={width,height};
        t.format=i==3?vk::Format::eR32Sfloat:vk::Format::eR32G32B32A32Sfloat;
        t.usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst;
        t.layout=vk::ImageLayout::eShaderReadOnlyOptimal;
        vk::ImageCreateInfo ci {};ci.imageType=vk::ImageType::e2D;ci.format=t.format;
        ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=vk::SampleCountFlagBits::e1;
        ci.tiling=vk::ImageTiling::eOptimal;ci.usage=t.usage;
        if(!g.CreateImage(ci,owned.images[i]))throw std::runtime_error("RR material test image allocation");
        t.image=owned.images[i].image;
        vk::ImageViewCreateInfo view {};view.image=t.image;view.format=t.format;view.viewType=vk::ImageViewType::e2D;
        view.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
        check(g.device.createImageView(&view,nullptr,&owned.views[i]),"RR material test view");t.view=owned.views[i];
    }
    constexpr size_t input_bytes=pixels*13*sizeof(float),output_bytes=pixels*3*8;
    // Include output canaries beyond the last copy, initialized before dispatch.
    constexpr size_t output_offset=(input_bytes+255)&~size_t(255),total_bytes=output_offset+output_bytes+64;
    VkBufferCreateInfo bi {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=total_bytes;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo host {};host.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    host.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    VmaAllocationInfo mapping {};
    if(vmaCreateBuffer(g.allocator,&bi,&host,&owned.buffer,&owned.allocation,&mapping)!=VK_SUCCESS)
        throw std::runtime_error("RR material test staging");
    vk::CommandPoolCreateInfo pc {};pc.queueFamilyIndex=g.queue_family;pc.flags=vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    check(g.device.createCommandPool(&pc,nullptr,&owned.pool),"RR material test pool");
    vk::CommandBufferAllocateInfo ca {};ca.commandPool=owned.pool;ca.commandBufferCount=1;
    vk::CommandBuffer command;check(g.device.allocateCommandBuffers(&ca,&command),"RR material test command");
    std::array<float,pixels*13> upload {};
    for(unsigned i=0;i<pixels;++i) {
        auto* base=upload.data()+i*4;auto* mat=upload.data()+pixels*4+i*4;
        auto* normal=upload.data()+pixels*8+i*4;
        base[0]=.2f+float(i%5)*.1f;base[1]=.15f+float(i%3)*.2f;base[2]=.7f;base[3]=1;
        mat[0]=float(i%3)*.5f;mat[1]=float(i%5)*.25f;mat[2]=mat[3]=1;
        normal[0]=float(i%3)*.2f;normal[1]=.3f;normal[2]=1;
        upload[pixels*12+i]=.8f;
    }
    // Malformed material/geometry must produce a zero validity mask, never NaNs.
    upload[pixels*8+3*4]=upload[pixels*8+3*4+1]=upload[pixels*8+3*4+2]=0;
    upload[pixels*4+7*4+1]=1.5f;
    upload[11*4]=std::numeric_limits<float>::quiet_NaN();
    upload[pixels*12+17]=1.1f;
    upload[20*4+3]=std::numeric_limits<float>::quiet_NaN();
    upload[pixels*4+20*4+2]=std::numeric_limits<float>::infinity();
    upload[pixels*8+20*4+3]=std::numeric_limits<float>::quiet_NaN();
    RRPlayroomMaterials materials(g);
    for(unsigned scenario=0;scenario<3;++scenario) {
        DLSSMatrix projection {},view {},inverse_view {},inverse_projection {};
        projection[0]=1.2f;projection[5]=2.1f;projection[10]=-1.0001f;projection[11]=-1;projection[14]=-.10001f;
        inverse_view[0]=inverse_view[5]=inverse_view[10]=inverse_view[15]=1;
        if(scenario==1) {
            inverse_view[0]=inverse_view[10]=.8f;inverse_view[2]=.6f;inverse_view[8]=-.6f;
            inverse_view[12]=5;inverse_view[13]=-3;inverse_view[14]=2;
            // Nonzero raster jitter tests use of the guest inverse projection.
            projection[8]=.02f/width;projection[9]=-.02f/height;
        }
        if(scenario==2) {
            projection.fill(0);projection[0]=.25f;projection[5]=.4444444f;
            projection[10]=-.0010001f;projection[14]=-.0001000166f;projection[15]=1;
            projection[8]=.000219555f;projection[9]=-.0000385802f;
            inverse_view[14]=1;
        }
        if(!DLSSInvert(projection,inverse_projection)||!DLSSInvert(inverse_view,view))throw std::runtime_error("test matrix inversion");
        const auto vp=DLSSMultiply(view,projection);
        in.camera.fill(0);std::copy(vp.begin(),vp.end(),in.camera.begin());
        std::copy(view.begin(),view.end(),in.camera.begin()+16);std::copy(projection.begin(),projection.end(),in.camera.begin()+32);
        std::copy(inverse_view.begin(),inverse_view.end(),in.camera.begin()+48);std::copy(inverse_projection.begin(),inverse_projection.end(),in.camera.begin()+64);
        in.camera[96]=scenario==1?.01f/width:0;in.camera[97]=scenario==1?.01f/height:0;
        std::copy_n(inverse_view.data()+12,3,in.camera.data()+100);
        in.camera[106]=.1f;in.camera[107]=1000;in.camera[108]=width;in.camera[109]=height;
        if(!RRPlayroomMaterials::Validate(in).empty())throw std::runtime_error(RRPlayroomMaterials::Validate(in));
        auto bad=in;bad.camera[64]=0;
        if(RRPlayroomMaterials::Validate(bad).empty())throw std::runtime_error("material inverse projection accepted after corruption");
        bad=in;bad.metal_roughness.format=vk::Format::eR8G8B8A8Srgb;
        if(RRPlayroomMaterials::Validate(bad).empty())throw std::runtime_error("sRGB roughness accepted");
        bad=in;bad.depth.extent.width++;
        if(RRPlayroomMaterials::Validate(bad).empty())throw std::runtime_error("mismatched material grid accepted");
        bad=in;bad.camera[48]=2;
        if(RRPlayroomMaterials::Validate(bad).empty())throw std::runtime_error("non-rigid material camera accepted");
        bad=in;bad.camera[100]+=10;
        if(RRPlayroomMaterials::Validate(bad).empty())throw std::runtime_error("inconsistent material camera position accepted");
        std::memset(mapping.pMappedData,0xA5,total_bytes);std::memcpy(mapping.pMappedData,upload.data(),input_bytes);
        vmaFlushAllocation(g.allocator,owned.allocation,0,VK_WHOLE_SIZE);
        check(command.reset({}),"RR material reset");vk::CommandBufferBeginInfo begin {};check(command.begin(&begin),"RR material begin");
        vk::MemoryBarrier host_ready {};host_ready.srcAccessMask=vk::AccessFlagBits::eHostWrite;host_ready.dstAccessMask=vk::AccessFlagBits::eTransferRead;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eHost,vk::PipelineStageFlagBits::eTransfer,{},1,&host_ready,0,nullptr,0,nullptr);
        for(unsigned i=0;i<4;++i) {
            vk::ImageMemoryBarrier b {};b.oldLayout=scenario?vk::ImageLayout::eShaderReadOnlyOptimal:vk::ImageLayout::eUndefined;
            b.newLayout=vk::ImageLayout::eTransferDstOptimal;b.srcAccessMask=scenario?vk::AccessFlagBits::eShaderRead:vk::AccessFlags{};
            b.dstAccessMask=vk::AccessFlagBits::eTransferWrite;b.image=textures[i]->image;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},0,nullptr,0,nullptr,1,&b);
            vk::BufferImageCopy copy {};copy.bufferOffset=i*pixels*4*sizeof(float);
            copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};copy.imageExtent={width,height,1};
            command.copyBufferToImage(owned.buffer,b.image,vk::ImageLayout::eTransferDstOptimal,1,&copy);
            b.oldLayout=b.newLayout;b.newLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
            b.srcAccessMask=vk::AccessFlagBits::eTransferWrite;b.dstAccessMask=vk::AccessFlagBits::eShaderRead;
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader,{},0,nullptr,0,nullptr,1,&b);
        }
        auto frame=materials.Record(command,in);
        frame->RecordReadback(command);
        const std::array<const RRTexture*,3> outputs {&frame->Diffuse(),&frame->Specular(),&frame->NormalRoughness()};
        for(unsigned i=0;i<3;++i) {
            vk::ImageMemoryBarrier b {};b.oldLayout=vk::ImageLayout::eShaderReadOnlyOptimal;b.newLayout=vk::ImageLayout::eTransferSrcOptimal;
            b.srcAccessMask=vk::AccessFlagBits::eShaderWrite;b.dstAccessMask=vk::AccessFlagBits::eTransferRead;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=outputs[i]->image;b.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eTransfer,{},0,nullptr,0,nullptr,1,&b);
            vk::BufferImageCopy copy {};copy.bufferOffset=output_offset+i*pixels*8;
            copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};copy.imageExtent={width,height,1};
            command.copyImageToBuffer(b.image,b.newLayout,owned.buffer,1,&copy);
        }
        host_ready.srcAccessMask=vk::AccessFlagBits::eTransferWrite;host_ready.dstAccessMask=vk::AccessFlagBits::eHostRead;
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},1,&host_ready,0,nullptr,0,nullptr);
        check(command.end(),"RR material end");vk::SubmitInfo submit {};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        check(g.queue.submit(1,&submit,nullptr),"RR material submit");check(g.device.waitIdle(),"RR material wait");
        vmaInvalidateAllocation(g.allocator,owned.allocation,0,VK_WHOLE_SIZE);
        frame->Report(scenario);
        const auto* result=reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapping.pMappedData)+output_offset);
        for(unsigned i=0;i<pixels;++i) {
            const bool valid=i!=3&&i!=7&&i!=11&&i!=17;
            std::array<double,12> expected {};
            if(valid) {
                std::array<double,4> clip {2.0*(i%width+.5)/width-1,1-2.0*(i/width+.5)/height,upload[pixels*12+i],1},v {},w {};
                for(int c=0;c<4;++c)for(int r=0;r<4;++r)v[c]+=clip[r]*inverse_projection[r*4+c];
                for(int c=0;c<3;++c)v[c]/=v[3];v[3]=1;
                for(int c=0;c<4;++c)for(int r=0;r<4;++r)w[c]+=v[r]*inverse_view[r*4+c];
                std::array<double,3> n {},eye {},f0 {};
                double nn=0,vv=0;
                for(int c=0;c<3;++c) {
                    for(int r=0;r<3;++r)n[c]+=upload[pixels*8+i*4+r]*inverse_view[r*4+c];
                    if(scenario==2)for(int r=0;r<3;++r)eye[c]-=inverse_projection[8+r]*inverse_view[r*4+c];
                    else eye[c]=inverse_view[12+c]-w[c];
                    nn+=n[c]*n[c];vv+=eye[c]*eye[c];
                }
                double nv=0;
                for(int c=0;c<3;++c){n[c]/=std::sqrt(nn);eye[c]/=std::sqrt(vv);nv+=n[c]*eye[c];}
                const double metal=upload[pixels*4+i*4],rough=upload[pixels*4+i*4+1];
                for(int c=0;c<3;++c){expected[c]=upload[i*4+c]*(1-metal);f0[c]=.05*(1-metal)+upload[i*4+c]*metal;expected[8+c]=n[c];}
                const auto spec=Specular(f0,rough,nv);std::copy(spec.begin(),spec.end(),expected.begin()+4);
                expected[3]=expected[7]=1;expected[11]=rough;
            }
            for(unsigned channel=0;channel<12;++channel) {
                const double actual=Half(result[(channel/4)*pixels*4+i*4+channel%4]);
                if(!std::isfinite(actual)||std::abs(actual-expected[channel])>.003*(1+std::abs(expected[channel])))
                    throw std::runtime_error("RR materials GPU/reference mismatch scenario="+std::to_string(scenario)+" pixel="+std::to_string(i)+" channel="+std::to_string(channel));
            }
        }
        const auto* guard=static_cast<const unsigned char*>(mapping.pMappedData)+output_offset+output_bytes;
        for(unsigned i=0;i<64;++i)if(guard[i]!=0xA5)throw std::runtime_error("RR materials output canary corrupted");
    }
    std::puts("RR PLAYROOM material GPU conversion: 81 pixels, metals/roughness, perspective/orthographic/rotated camera/jitter, invalid masks, reference and canaries: PASS");
}
