#include "pch.h"

LRESULT WINAPI main_window_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

int main()
{
    vk::ApplicationInfo instance_app_info;
    instance_app_info.pApplicationName = "VulkanSample";
    instance_app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    instance_app_info.pEngineName = "Custom";
    instance_app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    std::array<const char*, 1> instance_layers {
        "VK_LAYER_LUNARG_standard_validation",
    };
    std::array<const char*, 2> instance_extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    vk::InstanceCreateInfo instance_info;
    instance_info.pApplicationInfo = &instance_app_info;
    instance_info.enabledLayerCount = (uint32_t)instance_layers.size();
    instance_info.ppEnabledLayerNames = instance_layers.data();
    instance_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    vk::UniqueInstance instance = vk::createInstanceUnique(instance_info);

    WNDCLASS wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = TEXT("MainWindow");
    RegisterClass(&wc);
    HWND hWnd = CreateWindow(TEXT("MainWindow"), TEXT("VulkanSample"), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, wc.hInstance, NULL);

    vk::Win32SurfaceCreateInfoKHR surface_info;
    surface_info.hinstance = wc.hInstance;
    surface_info.hwnd = hWnd;
    vk::UniqueSurfaceKHR surface = instance->createWin32SurfaceKHRUnique(surface_info);

    vk::UniqueDevice device;
    vk::PhysicalDevice physical_device;
    uint32_t device_family;
    std::vector<vk::PhysicalDevice> physical_devices = instance->enumeratePhysicalDevices();
    for (const auto& pd : physical_devices)
    {
        auto props = pd.getQueueFamilyProperties();
        for (int family_index = 0; family_index < props.size(); family_index++)
        {
            bool support_graphics = (bool)(props[family_index].queueFlags & vk::QueueFlagBits::eGraphics);
            bool support_present = pd.getSurfaceSupportKHR(family_index, *surface);
            if (support_graphics && support_present)
            {
                std::array<const char*, 0> device_layers{
                };
                std::array<const char*, 1> device_extensions{
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                };
                std::array<float, 1> queue_priorities{ 1.f };
                vk::DeviceQueueCreateInfo queue_info;
                queue_info.queueFamilyIndex = family_index;
                queue_info.queueCount = 1;
                queue_info.pQueuePriorities = queue_priorities.data();
                vk::PhysicalDeviceFeatures device_features;
                vk::DeviceCreateInfo device_info;
                device_info.queueCreateInfoCount = 1;
                device_info.pQueueCreateInfos = &queue_info;
                device_info.enabledLayerCount = (uint32_t)device_layers.size();
                device_info.ppEnabledLayerNames = device_layers.data();
                device_info.enabledExtensionCount = (uint32_t)device_extensions.size();
                device_info.ppEnabledExtensionNames = device_extensions.data();
                device_info.pEnabledFeatures = &device_features;
                device = pd.createDeviceUnique(device_info);
                physical_device = pd;
                device_family = family_index;
            }
        }
    }

    auto pd_props = physical_device.getProperties();
    std::cout << "Device: " << pd_props.deviceName << "\n";

    vk::SurfaceCapabilitiesKHR surface_caps = physical_device.getSurfaceCapabilitiesKHR(*surface);
    std::vector<vk::SurfaceFormatKHR> swapchain_formats = physical_device.getSurfaceFormatsKHR(*surface);
    vk::SwapchainCreateInfoKHR swapchain_info;
    swapchain_info.surface = *surface;
    swapchain_info.minImageCount = 2;
    swapchain_info.imageFormat = vk::Format::eB8G8R8A8Unorm;
    swapchain_info.imageColorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
    swapchain_info.imageExtent = surface_caps.currentExtent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
    swapchain_info.presentMode = vk::PresentModeKHR::eFifo;
    swapchain_info.clipped = true;
    vk::UniqueSwapchainKHR swapchain = device->createSwapchainKHRUnique(swapchain_info);
    std::vector<vk::Image> swapchain_images = device->getSwapchainImagesKHR(*swapchain);

    vk::Queue q = device->getQueue(device_family, 0);

    vk::UniqueCommandPool cmdpool = device->createCommandPoolUnique({ {}, device_family });
    vk::CommandBufferAllocateInfo cmd_clear_info;
    cmd_clear_info.commandPool = *cmdpool;
    cmd_clear_info.level = vk::CommandBufferLevel::ePrimary;
    cmd_clear_info.commandBufferCount = (uint32_t)swapchain_images.size();
    std::vector<vk::UniqueCommandBuffer> cmd_clear = device->allocateCommandBuffersUnique(cmd_clear_info);

    std::vector<vk::CommandBuffer> submit_commands(swapchain_images.size());
    for (int i = 0; i < swapchain_images.size(); i++)
    {
        cmd_clear[i]->begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
        vk::ImageMemoryBarrier barrier;
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchain_images[i];
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        cmd_clear[i]->pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
        //cmd_clear[i]->clearColorImage(*swapchain_images[i], )
        cmd_clear[i]->end();
        submit_commands[i] = *cmd_clear[i];
    }

    vk::UniqueFence submit_fence = device->createFenceUnique({});
    vk::SubmitInfo submit_info;
    submit_info.commandBufferCount = 2;
    submit_info.pCommandBuffers = submit_commands.data();
    q.submit(submit_info, *submit_fence);
    q.waitIdle();

    MSG msg;
    while (GetMessage(&msg, hWnd, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        vk::UniqueSemaphore backbuffer_semaphore = device->createSemaphoreUnique({});
        auto backbuffer = device->acquireNextImageKHR(*swapchain, UINT64_MAX, *backbuffer_semaphore, nullptr);
        if (backbuffer.result == vk::Result::eSuccess)
        {
            vk::PresentInfoKHR present_info;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &backbuffer_semaphore.get();
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain.get();
            present_info.pImageIndices = &backbuffer.value;
            q.presentKHR(present_info);
            q.waitIdle();
        }
    }

    device->waitIdle();
}
