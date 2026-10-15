// GPU smoke test for the supplied SDK ABI, shader permutations and
// external-image transitions.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <bit> // SDK ffx_util.h uses std::popcount without including <bit>.

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static void Check(FfxErrorCode code, const char *operation) {
  if (code != FFX_OK)
    throw std::runtime_error(std::string(operation) + ": " +
                             std::to_string(code));
}
struct Image {
  vk::Image image;
  vk::DeviceMemory memory;
  FfxResource resource;
};
int main(int argc, char** argv) {
  const bool presentMode = argc > 1 && std::strcmp(argv[1], "--present") == 0;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  try {
    vk::detail::DynamicLoader loader;
    auto getInstanceProcAddr = loader.getProcAddress<PFN_vkGetInstanceProcAddr>(
        "vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
    vk::ApplicationInfo app("Kyty FSR GPU smoke", 1, "Kyty", 1,
                            VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceInfo({}, &app);
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (presentMode) { instanceInfo.enabledExtensionCount = 2; instanceInfo.ppEnabledExtensionNames = instanceExtensions; }
    auto instance = vk::createInstance(instanceInfo);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);
    HWND hwnd = nullptr;
    vk::SurfaceKHR surface;
    if (presentMode) {
        hwnd = CreateWindowExW(0, L"STATIC", L"Kyty FSR swapchain audit", WS_POPUP,
            0, 0, 256, 256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) throw std::runtime_error("Hidden test window creation failed");
        surface = instance.createWin32SurfaceKHR({{}, GetModuleHandleW(nullptr), hwnd});
    }
    auto devices = instance.enumeratePhysicalDevices();
    auto physical = devices.at(0);
    for (auto candidate : devices)
      if (candidate.getProperties().deviceType ==
          vk::PhysicalDeviceType::eDiscreteGpu) {
        physical = candidate;
        break;
      }
    auto families = physical.getQueueFamilyProperties();
    uint32_t family = 0;
    while (family < families.size() &&
           !(families[family].queueFlags & vk::QueueFlagBits::eCompute))
      ++family;
    if (family == families.size())
      throw std::runtime_error("No compute queue");
    auto supported =
        physical.getFeatures2<vk::PhysicalDeviceFeatures2,
                              vk::PhysicalDeviceVulkan12Features,
                              vk::PhysicalDeviceVulkan13Features>();
    auto features12 = supported.get<vk::PhysicalDeviceVulkan12Features>();
    auto features13 = supported.get<vk::PhysicalDeviceVulkan13Features>();
    features12.pNext = &features13;
    features13.pNext = nullptr;
    const float priorities[] = {1, 1, 1};
    if (presentMode && (families[family].queueCount < 3 || !physical.getSurfaceSupportKHR(family, surface)))
        throw std::runtime_error("Swapchain test requires three graphics/present queues");
    vk::DeviceQueueCreateInfo queueInfo({}, family, presentMode ? 3 : 1, priorities);
    auto basic = supported.get<vk::PhysicalDeviceFeatures2>().features;
    const char *extensions[] = {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                                VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk::DeviceCreateInfo deviceInfo({}, 1, &queueInfo, 0, nullptr, 2,
                                    extensions, &basic);
    deviceInfo.enabledExtensionCount = presentMode ? 3 : 2;
    deviceInfo.pNext = &features12;
    auto device = physical.createDevice(deviceInfo);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    auto queue = device.getQueue(family, 0);
    std::printf("GPU: %s\n", physical.getProperties().deviceName.data());
    auto memoryProperties = physical.getMemoryProperties();
    auto allocate = [&](vk::MemoryRequirements requirements,
                        vk::MemoryPropertyFlags flags) {
      for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
          return device.allocateMemory({requirements.size, i});
      throw std::runtime_error("Memory type unavailable");
    };
    auto image = [&](vk::Format format, uint32_t width, uint32_t height,
                     bool output) {
      vk::ImageCreateInfo info(
          {}, vk::ImageType::e2D, format, {width, height, 1}, 1, 1,
          vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
          vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
              vk::ImageUsageFlagBits::eTransferSrc |
              vk::ImageUsageFlagBits::eTransferDst);
      Image result{};
      result.image = device.createImage(info);
      result.memory = allocate(device.getImageMemoryRequirements(result.image),
                               vk::MemoryPropertyFlagBits::eDeviceLocal);
      device.bindImageMemory(result.image, result.memory, 0);
      FfxResourceDescription desc{};
      desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
      desc.format = ffxGetSurfaceFormatVK(static_cast<VkFormat>(format));
      desc.width = width;
      desc.height = height;
      desc.depth = 1;
      desc.mipCount = 1;
      desc.usage =
          output ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
      result.resource = ffxGetResourceVK(
          reinterpret_cast<void *>(static_cast<VkImage>(result.image)), desc,
          L"FSR smoke",
          output ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
                 : FFX_RESOURCE_STATE_COMPUTE_READ);
      return result;
    };
    auto color = image(vk::Format::eR8G8B8A8Unorm, 128, 128, false);
    auto depth = image(vk::Format::eR32Sfloat, 128, 128, false);
    auto motion = image(vk::Format::eR16G16Sfloat, 128, 128, false);
    auto output = image(vk::Format::eR8G8B8A8Unorm, 256, 256, true);
    auto interpolated = image(vk::Format::eR8G8B8A8Unorm, 256, 256, true);
    auto dilatedDepth = image(vk::Format::eR32Sfloat, 128, 128, true);
    auto dilatedMotion = image(vk::Format::eR16G16Sfloat, 128, 128, true);
    auto reconstructedDepth = image(vk::Format::eR32Uint, 128, 128, true);
    auto readback = device.createBuffer(
        {{}, 256 * 256 * 4, vk::BufferUsageFlagBits::eTransferDst});
    auto readbackMemory =
        allocate(device.getBufferMemoryRequirements(readback),
                 vk::MemoryPropertyFlagBits::eHostVisible |
                     vk::MemoryPropertyFlagBits::eHostCoherent);
    device.bindBufferMemory(readback, readbackMemory, 0);
    auto pool = device.createCommandPool(
        {vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family});
    auto command =
        device
            .allocateCommandBuffers({pool, vk::CommandBufferLevel::ePrimary, 1})
            .at(0);
    auto barrier = [&](vk::Image im, vk::ImageLayout oldLayout,
                       vk::ImageLayout newLayout) {
      vk::ImageMemoryBarrier b(
          vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
          vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
          oldLayout, newLayout, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, im,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
      command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                              vk::PipelineStageFlagBits::eAllCommands, {}, {},
                              {}, b);
    };
    VkDeviceContext vkDevice{device, physical,
                             VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
    auto ffxDevice = ffxGetDeviceVK(&vkDevice);
    FfxSwapchain swapchain = nullptr;
    FfxSwapchainReplacementFunctions replacement {};
    vk::Semaphore acquired, ready;
    std::vector<VkImage> proxyImages;
    vk::Format swapchainFormat = vk::Format::eR8G8B8A8Unorm;
    if (presentMode) {
        const auto caps = physical.getSurfaceCapabilitiesKHR(surface);
        const auto formats = physical.getSurfaceFormatsKHR(surface);
        auto chosen = formats.at(0);
        for (const auto& format : formats) if (format.format == vk::Format::eR8G8B8A8Unorm ||
            format.format == vk::Format::eB8G8R8A8Unorm) { chosen = format; break; }
        swapchainFormat = chosen.format;
        if (caps.currentExtent.width != 256 || caps.currentExtent.height != 256)
            throw std::runtime_error("Hidden window drawable must be 256x256");
        VkSwapchainCreateInfoKHR create {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface; create.minImageCount = caps.minImageCount;
        create.imageFormat = static_cast<VkFormat>(chosen.format); create.imageColorSpace = static_cast<VkColorSpaceKHR>(chosen.colorSpace);
        create.imageExtent = {256, 256}; create.imageArrayLayers = 1;
        create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        create.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(caps.currentTransform);
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
        VkFrameInterpolationInfoFFX info {};
        info.device = device; info.physicalDevice = physical;
        info.gameQueue = {queue, family, nullptr};
        info.presentQueue = {device.getQueue(family, 1), family, nullptr};
        info.imageAcquireQueue = {device.getQueue(family, 2), family, nullptr};
        info.compositionMode = VK_COMPOSITION_MODE_PRESENT_QUEUE_FFX;
        Check(ffxGetSwapchainReplacementFunctionsVK(ffxDevice, &replacement), "replacement functions");
        Check(ffxReplaceSwapchainForFrameinterpolationVK(ffxGetCommandQueueVK(queue), swapchain, &create, &info), "replacement swapchain create");
        uint32_t count = 0;
        if (replacement.getSwapchainImagesKHR(device, ffxGetVKSwapchain(swapchain), &count, nullptr) != VK_SUCCESS)
            throw std::runtime_error("Get swapchain image count failed");
        proxyImages.resize(count);
        if (replacement.getSwapchainImagesKHR(device, ffxGetVKSwapchain(swapchain), &count, proxyImages.data()) != VK_SUCCESS)
            throw std::runtime_error("Get swapchain images failed");
        acquired = device.createSemaphore({}); ready = device.createSemaphore({});
    }
    // Exercise both the SR-only and the combined SR/optical-flow/interpolation
    // context.
    for (bool withInterpolation : {false, true}) {
      auto context = std::make_unique<FfxFsr3Context>();
      auto upscaler = std::make_unique<FfxFsr3UpscalerContext>();
      FfxFsr3ContextDescription desc{};
      desc.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE |
                   (withInterpolation ? 0 : FFX_FSR3_ENABLE_UPSCALING_ONLY);
      desc.maxRenderSize = {128, 128};
      desc.maxUpscaleSize = desc.displaySize = {256, 256};
      desc.backBufferFormat = ffxGetSurfaceFormatVK(static_cast<VkFormat>(swapchainFormat));
      std::array<std::vector<uint8_t>, 3> scratch;
      FfxInterface *interfaces[] = {&desc.backendInterfaceUpscaling,
                                    &desc.backendInterfaceSharedResources,
                                    &desc.backendInterfaceFrameInterpolation};
      const size_t counts[] = {FFX_FSR3UPSCALER_CONTEXT_COUNT, 1,
                               FFX_OPTICALFLOW_CONTEXT_COUNT +
                                   FFX_FRAMEINTERPOLATION_CONTEXT_COUNT};
      for (int i = 0; i < (withInterpolation ? 3 : 1); ++i) {
        std::printf("Initialize backend %d\n", i);
        scratch[i].resize(ffxGetScratchMemorySizeVK(physical, counts[i]));
        Check(ffxGetInterfaceVK(interfaces[i], ffxDevice, scratch[i].data(),
                                scratch[i].size(), counts[i]),
              "backend");
      }
      // Exercise effect dispatch directly; this test has no native window or
      // swapchain.
      if (withInterpolation && !presentMode)
        desc.backendInterfaceFrameInterpolation
            .fpSwapChainConfigureFrameGeneration =
            [](const FfxFrameGenerationConfig *) -> FfxErrorCode {
          return FFX_OK;
        };
      std::puts("Create FSR context");
      if (withInterpolation)
        Check(ffxFsr3ContextCreate(context.get(), &desc), "context create");
      else {
        FfxFsr3UpscalerContextDescription upscale{};
        upscale.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        upscale.maxRenderSize = desc.maxRenderSize;
        upscale.maxUpscaleSize = desc.maxUpscaleSize;
        upscale.backendInterface = desc.backendInterfaceUpscaling;
        Check(ffxFsr3UpscalerContextCreate(upscaler.get(), &upscale),
              "upscaler create");
      }
      std::puts("FSR context ready");
      if (withInterpolation) {
        FfxFrameGenerationConfig config{};
        config.frameGenerationEnabled = true;
        if (presentMode) {
            config.swapChain = swapchain;
            config.interpolationRect = {0, 0, 256, 256};
            config.frameGenerationCallback = [](const FfxFrameGenerationDispatchDescription* desc, void*) -> FfxErrorCode {
                return ffxFsr3DispatchFrameGeneration(desc);
            };
        }
        Check(ffxFsr3ConfigureFrameGeneration(context.get(), &config),
              "configure effects");
      }
      for (int frame = 0; frame < 3; ++frame) {
        uint32_t proxyIndex = 0;
        if (presentMode && withInterpolation) {
            FfxFrameGenerationConfig config {};
            config.swapChain = swapchain; config.frameGenerationEnabled = true;
            config.interpolationRect = {0, 0, 256, 256}; config.frameID = frame;
            config.frameGenerationCallback = [](const FfxFrameGenerationDispatchDescription* desc, void*) -> FfxErrorCode { return ffxFsr3DispatchFrameGeneration(desc); };
            Check(ffxFsr3ConfigureFrameGeneration(context.get(), &config), "configure frame ID");
            if (replacement.acquireNextImageKHR(device, ffxGetVKSwapchain(swapchain), UINT64_MAX, acquired, VK_NULL_HANDLE, &proxyIndex) != VK_SUCCESS)
                throw std::runtime_error("Replacement acquire failed");
        }
        command.reset();
        command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        std::array<Image *, 3> inputs{&color, &depth, &motion};
        const std::array<std::array<float, 4>, 3> clears{
            {{0.25f, 0.5f, 0.75f, 1}, {0.5f, 0, 0, 0}, {0, 0, 0, 0}}};
        for (size_t i = 0; i < inputs.size(); ++i) {
          barrier(inputs[i]->image, vk::ImageLayout::eUndefined,
                  vk::ImageLayout::eTransferDstOptimal);
          command.clearColorImage(
              inputs[i]->image, vk::ImageLayout::eTransferDstOptimal,
              vk::ClearColorValue(clears[i]),
              vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1,
                                        0, 1));
          barrier(inputs[i]->image, vk::ImageLayout::eTransferDstOptimal,
                  vk::ImageLayout::eShaderReadOnlyOptimal);
        }
        barrier(output.image, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral);
        FfxFsr3DispatchUpscaleDescription dispatch{};
        dispatch.commandList = ffxGetCommandListVK(command);
        dispatch.color = color.resource;
        dispatch.depth = depth.resource;
        dispatch.motionVectors = motion.resource;
        dispatch.upscaleOutput = output.resource;
        dispatch.motionVectorScale = {128, 128};
        dispatch.renderSize = {128, 128};
        dispatch.upscaleSize = {256, 256};
        dispatch.enableSharpening = true;
        dispatch.sharpness = 0.35f;
        dispatch.preExposure = 1;
        dispatch.frameTimeDelta = 16.6667f;
        dispatch.reset = frame == 0;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000;
        dispatch.cameraFovAngleVertical = 1;
        dispatch.viewSpaceToMetersFactor = 1;
        dispatch.frameID = frame;
        std::puts("Dispatch FSR");
        if (withInterpolation)
          Check(ffxFsr3ContextDispatchUpscale(context.get(), &dispatch),
                "upscale dispatch");
        else {
          FfxFsr3UpscalerDispatchDescription upscale{};
          for (auto *im : {&dilatedDepth, &dilatedMotion, &reconstructedDepth})
            barrier(im->image, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eGeneral);
          upscale.dilatedDepth = dilatedDepth.resource;
          upscale.dilatedMotionVectors = dilatedMotion.resource;
          upscale.reconstructedPrevNearestDepth = reconstructedDepth.resource;
          upscale.commandList = dispatch.commandList;
          upscale.color = dispatch.color;
          upscale.depth = dispatch.depth;
          upscale.motionVectors = dispatch.motionVectors;
          upscale.output = dispatch.upscaleOutput;
          upscale.motionVectorScale = dispatch.motionVectorScale;
          upscale.renderSize = dispatch.renderSize;
          upscale.upscaleSize = dispatch.upscaleSize;
          upscale.frameTimeDelta = dispatch.frameTimeDelta;
          upscale.preExposure = dispatch.preExposure;
          upscale.enableSharpening = dispatch.enableSharpening;
          upscale.sharpness = dispatch.sharpness;
          upscale.reset = dispatch.reset;
          upscale.cameraNear = dispatch.cameraNear;
          upscale.cameraFar = dispatch.cameraFar;
          upscale.cameraFovAngleVertical = dispatch.cameraFovAngleVertical;
          upscale.viewSpaceToMetersFactor = 1;
          Check(ffxFsr3UpscalerContextDispatch(upscaler.get(), &upscale),
                "upscale dispatch");
        }
        if (withInterpolation) {
          FfxFsr3DispatchFrameGenerationPrepareDescription prepare{};
          prepare.commandList = dispatch.commandList;
          prepare.depth = dispatch.depth;
          prepare.motionVectors = dispatch.motionVectors;
          prepare.renderSize = dispatch.renderSize;
          prepare.motionVectorScale = dispatch.motionVectorScale;
          prepare.frameTimeDelta = dispatch.frameTimeDelta;
          prepare.cameraNear = 0.1f;
          prepare.cameraFar = 1000;
          prepare.cameraFovAngleVertical = 1;
          prepare.viewSpaceToMetersFactor = 1;
          prepare.frameID = frame;
          Check(ffxFsr3ContextDispatchFrameGenerationPrepare(context.get(),
                                                             &prepare),
                "FG prepare");
          if (!presentMode) {
          barrier(interpolated.image, vk::ImageLayout::eUndefined,
                  vk::ImageLayout::eGeneral);
          FfxFrameGenerationDispatchDescription generate{};
          generate.commandList = dispatch.commandList;
          generate.presentColor = output.resource;
          generate.outputs[0] = interpolated.resource;
          generate.numInterpolatedFrames = 1;
          generate.reset = frame == 0;
          generate.frameID = frame;
          generate.interpolationRect = {0, 0, 256, 256};
          generate.minMaxLuminance[1] = 1;
          generate.backBufferTransferFunction =
              FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
          Check(ffxFsr3DispatchFrameGeneration(&generate),
                "optical flow and interpolation dispatch");
          }
        }
        auto resultImage =
            withInterpolation && !presentMode ? interpolated.image : output.image;
        barrier(resultImage, vk::ImageLayout::eGeneral,
                vk::ImageLayout::eTransferSrcOptimal);
        vk::BufferImageCopy copy(0, 0, 0,
                                 {vk::ImageAspectFlagBits::eColor, 0, 0, 1}, {},
                                 {256, 256, 1});
        command.copyImageToBuffer(
            resultImage, vk::ImageLayout::eTransferSrcOptimal, readback, copy);
        vk::MemoryBarrier hostRead(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                              {}, hostRead, {}, {});
      
        if (presentMode && withInterpolation) {
            barrier(proxyImages[proxyIndex], vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);
            vk::ImageBlit blit;
            blit.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1}; blit.dstSubresource = blit.srcSubresource;
            blit.srcOffsets[1] = vk::Offset3D(256, 256, 1); blit.dstOffsets[1] = vk::Offset3D(256, 256, 1);
            command.blitImage(output.image, vk::ImageLayout::eTransferSrcOptimal, proxyImages[proxyIndex],
                vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
            barrier(proxyImages[proxyIndex], vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
        }
        command.end();
        vk::SubmitInfo submit({}, {}, command);
        vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
        if (presentMode && withInterpolation) {
            submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
            submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready;
        }
        queue.submit(submit);
        if (presentMode && withInterpolation) {
            const auto sc = ffxGetVKSwapchain(swapchain);
            const VkSemaphore signal = ready;
            VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.waitSemaphoreCount = 1; present.pWaitSemaphores = &signal;
            present.swapchainCount = 1; present.pSwapchains = &sc; present.pImageIndices = &proxyIndex;
            if (replacement.queuePresentKHR(queue, &present) != VK_SUCCESS) throw std::runtime_error("Replacement present failed");
            Check(ffxWaitForPresents(swapchain), "wait for replacement presents");
        }
        queue.waitIdle();
        auto *bytes = static_cast<uint8_t *>(
            device.mapMemory(readbackMemory, 0, VK_WHOLE_SIZE));
        const auto *pixel = bytes + (128 * 256 + 128) * 4;
        std::printf("%s frame %d: center RGB=%u,%u,%u\n",
                    withInterpolation ? "SR+FG context" : "SR", frame, pixel[0],
                    pixel[1], pixel[2]);
        const bool valid = pixel[0] > 40 && pixel[0] < 90 && pixel[1] > 100 &&
                           pixel[1] < 155 && pixel[2] > 165 && pixel[2] < 215;
        device.unmapMemory(readbackMemory);
        if (!valid)
          throw std::runtime_error(
              "FSR output readback did not preserve constant-color input");
      }
      if (presentMode && withInterpolation) {
          const auto count = replacement.getLastPresentCountFFX(ffxGetVKSwapchain(swapchain));
          std::printf("Replacement swapchain: 3 application frames, %llu actual presents\n", static_cast<unsigned long long>(count));
          if (count <= 3) throw std::runtime_error("No additional generated frames were presented");
          FfxFrameGenerationConfig off {}; off.swapChain = swapchain;
          Check(ffxFsr3ConfigureFrameGeneration(context.get(), &off), "disable generation");
          Check(ffxWaitForPresents(swapchain), "drain swapchain");
      }
      Check(withInterpolation ? ffxFsr3ContextDestroy(context.get())
                              : ffxFsr3UpscalerContextDestroy(upscaler.get()),
            "context destroy");
    }
    device.waitIdle();
    device.destroyCommandPool(pool);
    for (auto *im : {&color, &depth, &motion, &output, &interpolated,
                     &dilatedDepth, &dilatedMotion, &reconstructedDepth}) {
      device.destroyImage(im->image);
      device.freeMemory(im->memory);
    }
    device.destroyBuffer(readback);
    device.freeMemory(readbackMemory);
    if (presentMode) {
        replacement.destroySwapchainKHR(device, ffxGetVKSwapchain(swapchain), nullptr);
        device.destroySemaphore(acquired); device.destroySemaphore(ready);
        instance.destroySurfaceKHR(surface); DestroyWindow(hwnd);
    }
    device.destroy();
    instance.destroy();
    std::puts("FSR Vulkan smoke: PASS (upscale + optical flow/interpolation + "
              "readback)");
    if (presentMode) std::puts("FSR replacement swapchain and generated-frame presents: PASS");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FSR Vulkan smoke: FAIL: %s\n", error.what());
    return 1;
  }
}
