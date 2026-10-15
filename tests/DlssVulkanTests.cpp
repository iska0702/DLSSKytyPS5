// Optional Windows/GPU integration test for the production DLSSManager.
#define NOMINMAX
#include "graphics/presentation/dlss/dlssManager.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace Libs::Graphics;
int RunDLSSVulkanSmoke(bool requirementsOnly, bool fullHD) {
    const vk::Extent2D targetExtent = fullHD ? vk::Extent2D{1920,1080} : vk::Extent2D{1280,720};
    const vk::Extent2D inputExtent = fullHD ? vk::Extent2D{1280,720} : vk::Extent2D{854,480};
#if !defined(_WIN32)
    std::puts("DLSS Vulkan presentation test requires Windows");
    return 1;
#else
    _putenv("DISABLE_VULKAN_OBS_CAPTURE=1");
    _putenv("VK_LOADER_LAYERS_DISABLE=VK_LAYER_OBS_HOOK,VK_LAYER_RTSS");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Config::ConfigOptions config;
    config.dlss_enabled = config.dlss_fg_enabled = true;
    Config::Load(config);
    Log::Initialize();
    auto graphics = std::make_unique<GraphicContext>();
    auto& g = *graphics;
    auto check = [](vk::Result result, const char* operation) {
        if (result != vk::Result::eSuccess) throw std::runtime_error(std::string(operation) + ": " + vk::to_string(result));
    };
    try {
        vk::detail::DynamicLoader loader;
        VULKAN_HPP_DEFAULT_DISPATCHER.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
        const char* instanceExtensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_get_physical_device_properties2",
            "VK_KHR_external_memory_capabilities", "VK_KHR_external_semaphore_capabilities"};
        vk::ApplicationInfo app {}; app.pApplicationName = "Kyty DLSS Vulkan test"; app.apiVersion = VK_API_VERSION_1_3;
        vk::InstanceCreateInfo instance {}; instance.pApplicationInfo = &app;
        instance.enabledExtensionCount = std::size(instanceExtensions); instance.ppEnabledExtensionNames = instanceExtensions;
        check(vk::createInstance(&instance, nullptr, &g.instance), "create instance");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(g.instance);
        const auto devices = EnumerateVulkan<vk::PhysicalDevice>("enumerate devices", [&](uint32_t* count, vk::PhysicalDevice* values) {
            return g.instance.enumeratePhysicalDevices(count, values);
        });
        for (auto device : devices) if (device.getProperties().vendorID == 0x10de) { g.physical_device = device; break; }
        if (!g.physical_device) throw std::runtime_error("NVIDIA GPU unavailable");
        g.physical_device_properties = g.physical_device.getProperties();
        g.physical_device_memory_properties = g.physical_device.getMemoryProperties();
        uint32_t count = 0; g.physical_device.getQueueFamilyProperties(&count, nullptr);
        std::vector<vk::QueueFamilyProperties> families(count);
        g.physical_device.getQueueFamilyProperties(&count, families.data());
        for (uint32_t i = 0; i < count; ++i) if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            (families[i].queueFlags & vk::QueueFlagBits::eCompute) && families[i].queueCount >= 4) { g.queue_family = i; break; }
        if (g.queue_family == UINT32_MAX) throw std::runtime_error("Insufficient dedicated graphics/compute queues");
        auto supported = EnumerateVulkan<vk::ExtensionProperties>("device extensions", [&](uint32_t* n, vk::ExtensionProperties* values) {
            return g.physical_device.enumerateDeviceExtensionProperties(nullptr, n, values);
        });
        std::vector<const char*> extensions;
        for (const char* name : {"VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_KHR_external_memory", "VK_KHR_external_memory_win32",
            "VK_KHR_external_semaphore", "VK_KHR_external_semaphore_win32", "VK_NVX_binary_import", "VK_NVX_image_view_handle",
            "VK_NV_optical_flow", "VK_KHR_timeline_semaphore", "VK_KHR_maintenance4", "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation"})
            for (const auto& ext : supported) if (std::strcmp(ext.extensionName, name) == 0) { extensions.push_back(name); break; }
        vk::PhysicalDeviceOpticalFlowFeaturesNV optical {};
        vk::PhysicalDeviceVulkan13Features features13 {}; features13.pNext = &optical;
        vk::PhysicalDeviceVulkan12Features features12 {}; features12.pNext = &features13;
        vk::PhysicalDeviceFeatures2 features {}; features.pNext = &features12;
        g.physical_device.getFeatures2(&features);
        std::array<float, 8> priorities {1,1,1,1,1,1,1,1};
        const uint32_t queues = std::min(families[g.queue_family].queueCount, 8u);
        vk::DeviceQueueCreateInfo queueInfo {}; queueInfo.queueFamilyIndex = g.queue_family;
        queueInfo.queueCount = queues; queueInfo.pQueuePriorities = priorities.data();
        vk::DeviceCreateInfo device {}; device.pNext = &features12; device.pEnabledFeatures = &features.features;
        device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queueInfo;
        device.enabledExtensionCount = extensions.size(); device.ppEnabledExtensionNames = extensions.data();
        check(g.physical_device.createDevice(&device, nullptr, &g.device), "create device");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(g.device);
        g.device.getQueue(g.queue_family, 0, &g.queue);
        g.streamline_queue_start = 1; g.streamline_queue_count = queues - 1;
        if (!g.CreateAllocator()) throw std::runtime_error("Create allocator failed");
        auto& dlss = DLSSManager::Instance();
        DLSSSettings settings; settings.enabled = settings.fg_enabled = true;
        settings.mode = DLSSMode::Quality; settings.fg_mode = DLSSGMode::FG_2X;
        settings.reflex_mode = ReflexMode::On; settings.diagnostics = requirementsOnly;
        if (!dlss.Initialize(g, settings) || !dlss.IsFGSupported() || !dlss.IsReflexSupported())
            throw std::runtime_error("DLSS-G and Reflex not available: " + dlss.GetStatusMessage());
        if (requirementsOnly) {
            dlss.Shutdown(g); g.DestroyAllocator(); g.device.destroy(nullptr); g.instance.destroy(nullptr);
            return 0;
        }
        const DWORD testStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT rectangle {0, 0, static_cast<LONG>(targetExtent.width), static_cast<LONG>(targetExtent.height)};
        AdjustWindowRect(&rectangle, testStyle, FALSE);
        HWND window = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"DLSS FG test - KEEP THIS WINDOW ACTIVE", testStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right-rectangle.left, rectangle.bottom-rectangle.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) throw std::runtime_error("Create hidden window failed");
        const HWND previousForeground = GetForegroundWindow();
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        SetForegroundWindow(window);
        MSG startupMessage;
        for (int poll = 0; poll < 4500 && GetForegroundWindow() != window; ++poll) {
            while (PeekMessageW(&startupMessage, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&startupMessage); DispatchMessageW(&startupMessage); }
            Sleep(10);
        }
        if (GetForegroundWindow() != window) throw std::runtime_error("DLSS-G test window requires foreground focus");
        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        check(static_cast<vk::Result>(dlss.CreateWin32SurfaceForHwnd(g.instance, window, &rawSurface)), "create interposer surface");
        vk::SurfaceKHR surface = rawSurface;
        const auto formats = EnumerateVulkan<vk::SurfaceFormatKHR>("surface formats", [&](uint32_t* n, vk::SurfaceFormatKHR* values) {
            return g.physical_device.getSurfaceFormatsKHR(surface, n, values);
        });
        auto format = formats.at(0);
        for (const auto& f : formats) if (f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm) { format = f; break; }
        vk::SurfaceCapabilitiesKHR caps; check(g.physical_device.getSurfaceCapabilitiesKHR(surface, &caps), "surface caps");
        vk::SwapchainCreateInfoKHR create {}; create.surface = surface;
        create.minImageCount = caps.maxImageCount ? std::min(caps.maxImageCount, std::max(6u, caps.minImageCount)) : std::max(6u, caps.minImageCount);
        create.imageFormat = format.format; create.imageColorSpace = format.colorSpace;
        create.imageExtent = targetExtent; create.imageArrayLayers = 1;
        create.imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
        create.preTransform = caps.currentTransform; create.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        create.presentMode = vk::PresentModeKHR::eImmediate; create.clipped = true;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        check(static_cast<vk::Result>(dlss.CreateSwapchain(g.device, reinterpret_cast<const VkSwapchainCreateInfoKHR*>(&create), nullptr, &swapchain)), "create interposer swapchain");
        uint32_t imageCount = 0;
        check(static_cast<vk::Result>(dlss.GetSwapchainImages(g.device, swapchain, &imageCount, nullptr)), "image count");
        std::vector<VkImage> images(imageCount);
        check(static_cast<vk::Result>(dlss.GetSwapchainImages(g.device, swapchain, &imageCount, images.data())), "images");
        dlss.SetSwapchainFormat(format.format); dlss.SetSwapchainImageCount(imageCount);
        auto view = [&](vk::Image image, vk::Format imageFormat) {
            vk::ImageView result; vk::ImageViewCreateInfo info {}; info.image = image; info.viewType = vk::ImageViewType::e2D;
            info.format = imageFormat; info.subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1};
            check(g.device.createImageView(&info, nullptr, &result), "image view"); return result;
        };
        std::vector<vk::ImageView> views; for (auto image : images) views.push_back(view(image, format.format));
        auto allocate = [&](VulkanImage& image, vk::Format imageFormat) {
            vk::ImageCreateInfo info {}; info.imageType = vk::ImageType::e2D; info.format = imageFormat;
            info.extent = {inputExtent.width,inputExtent.height,1}; info.mipLevels = info.arrayLayers = 1; info.samples = vk::SampleCountFlagBits::e1;
            info.tiling = vk::ImageTiling::eOptimal;
            info.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
            if (!g.CreateImage(info, image)) throw std::runtime_error("Image allocation failed");
        };
        VulkanImage color; DLSSFrameGuides guides;
        allocate(color, vk::Format::eR8G8B8A8Unorm); allocate(guides.depth, vk::Format::eR32Sfloat); allocate(guides.motion, vk::Format::eR16G16Sfloat);
        auto colorView = view(color.image, color.format);
        guides.depth_view = view(guides.depth.image, guides.depth.format); guides.motion_view = view(guides.motion.image, guides.motion.format);
        std::array<float,124> camera {};
        camera[0] = 1.20628512f; camera[5] = 2.14450693f; camera[10] = -1.00010002f; camera[11] = -1; camera[14] = -0.100010008f;
        std::copy_n(camera.data(), 16, camera.data()+32);
        camera[16] = camera[21] = camera[26] = camera[31] = camera[48] = camera[53] = camera[58] = camera[63] = 1;
        camera[106] = .1f; camera[107] = 999.934143f; camera[108] = static_cast<float>(inputExtent.width); camera[109] = static_cast<float>(inputExtent.height);
        guides.camera = DecodeAstroDLSSCamera(camera,inputExtent.width,inputExtent.height); guides.valid = true;
        if (!guides.camera.valid) throw std::runtime_error("Synthetic camera validation failed");
        vk::CommandPoolCreateInfo poolInfo {}; poolInfo.queueFamilyIndex = g.queue_family; poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        vk::CommandPool pool; check(g.device.createCommandPool(&poolInfo,nullptr,&pool),"command pool");
        vk::CommandBufferAllocateInfo allocateCommand {}; allocateCommand.commandPool = pool; allocateCommand.commandBufferCount = 1;
        vk::CommandBuffer command; check(g.device.allocateCommandBuffers(&allocateCommand,&command),"command buffer");
        vk::SemaphoreCreateInfo semaphoreInfo {}; vk::Semaphore acquired, ready;
        check(g.device.createSemaphore(&semaphoreInfo,nullptr,&acquired),"acquire semaphore");
        check(g.device.createSemaphore(&semaphoreInfo,nullptr,&ready),"ready semaphore");
        auto barrier = [&](vk::Image image, vk::ImageLayout before, vk::ImageLayout after) {
            vk::ImageMemoryBarrier b {}; b.srcAccessMask = vk::AccessFlagBits::eMemoryWrite|vk::AccessFlagBits::eMemoryRead;
            b.dstAccessMask = b.srcAccessMask; b.oldLayout=before; b.newLayout=after;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=image;
            b.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1};
            command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eAllCommands,{},0,nullptr,0,nullptr,1,&b);
        };
        for (int frame=0; frame<130; ++frame) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (GetForegroundWindow() != window) throw std::runtime_error("DLSS-G test lost foreground focus");
            if (frame % 16 == 0) std::printf("DLSS synthetic frame %d\n", frame);
            uint32_t index=0;
            check(static_cast<vk::Result>(dlss.AcquireNextImage(g.device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&index)),"acquire");
            check(command.reset({}),"reset"); vk::CommandBufferBeginInfo begin {}; check(command.begin(&begin),"begin");
            for (auto* image : {&color,&guides.depth,&guides.motion}) {
                barrier(image->image,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
                vk::ClearColorValue value {}; if (image==&color) { value.float32[0]=.25f; value.float32[1]=.5f; value.float32[2]=.75f; value.float32[3]=1; }
                if (image==&guides.depth) value.float32[0]=.5f;
                vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor,0,1,0,1};
                command.clearColorImage(image->image,vk::ImageLayout::eTransferDstOptimal,&value,1,&range);
                barrier(image->image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eShaderReadOnlyOptimal);
            }
            guides.camera.reset = frame==0;
            VulkanImage* output=nullptr;
            if (!dlss.Evaluate(g,command,color,colorView,inputExtent,targetExtent,output,images[index],views[index],&guides))
                throw std::runtime_error("DLSS evaluation failed: " + dlss.GetStatusMessage());
            barrier(images[index],vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            vk::ImageBlit blit {}; blit.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1}; blit.dstSubresource=blit.srcSubresource;
            blit.srcOffsets[1]={static_cast<int32_t>(targetExtent.width),static_cast<int32_t>(targetExtent.height),1}; blit.dstOffsets[1]=blit.srcOffsets[1];
            command.blitImage(output->image,vk::ImageLayout::eTransferSrcOptimal,images[index],vk::ImageLayout::eTransferDstOptimal,1,&blit,vk::Filter::eNearest);
            if (dlss.IsFGActive()) barrier(output->image,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eGeneral);
            barrier(images[index],vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::ePresentSrcKHR);
            check(command.end(),"end"); vk::PipelineStageFlags wait=vk::PipelineStageFlagBits::eAllCommands;
            vk::SubmitInfo submit {}; submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&acquired; submit.pWaitDstStageMask=&wait;
            submit.commandBufferCount=1; submit.pCommandBuffers=&command; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&ready;
            check(g.queue.submit(1,&submit,nullptr),"submit");
            VkSemaphore signal=ready; VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&index;
            present.waitSemaphoreCount=1; present.pWaitSemaphores=&signal;
            dlss.SetPresentMarker(true); check(static_cast<vk::Result>(dlss.QueuePresent(g.queue,&present)),"present"); dlss.SetPresentMarker(false);
            check(g.device.waitIdle(),"wait idle");
        }
        const auto multiplier=dlss.GetEffectiveMultiplier();
        std::printf("DLSS Vulkan: SR evaluated, observed FG multiplier=%u\n",multiplier);
        check(g.device.waitIdle(),"drain");
        dlss.ReleaseResources(g);
        for (auto imageView:views) g.device.destroyImageView(imageView,nullptr);
        dlss.DestroySwapchain(g.device,swapchain,nullptr);
        dlss.DestroySurface(g.instance,surface);
        dlss.Shutdown(g);
        for (auto imageView : {colorView,guides.depth_view,guides.motion_view}) g.device.destroyImageView(imageView,nullptr);
        for (auto* image : {&color,&guides.depth,&guides.motion}) g.DeleteImage(*image);
        g.device.destroySemaphore(acquired,nullptr); g.device.destroySemaphore(ready,nullptr); g.device.destroyCommandPool(pool,nullptr);
        g.DestroyAllocator(); g.device.destroy(nullptr); g.instance.destroy(nullptr); DestroyWindow(window);
        if (previousForeground) SetForegroundWindow(previousForeground);
        if (multiplier<2) throw std::runtime_error("DLSS-G did not report additional presented frames");
        std::puts("DLSS Vulkan SR + FG replacement presentation: PASS"); return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"DLSS Vulkan integration: FAIL: %s\n",error.what());
        return 1;
    }
#endif
}
