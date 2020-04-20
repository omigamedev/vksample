#include "pch.h"

static bool running = true;

static vk::UniqueInstance instance;
static vk::UniqueSurfaceKHR surface;
static vk::UniqueDevice device;
static vk::UniqueCommandPool cmdpool;
static vk::UniqueDescriptorPool descrpool;

static uint32_t device_family = 0;
static vk::PhysicalDevice physical_device;
static vk::Queue q;

LRESULT WINAPI main_window_proc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        return 0;
    case WM_DESTROY:
        device->waitIdle();
        running = false;
    case WM_CLOSE:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

void find_device()
{
    uint32_t device_family = 0;
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
                return;
            }
        }
    }
}

uint32_t find_memory(const vk::MemoryRequirements& req, vk::MemoryPropertyFlags flags)
{
    vk::PhysicalDeviceMemoryProperties mp = physical_device.getMemoryProperties();
    for (uint32_t mem_i = 0; mem_i < mp.memoryTypeCount; mem_i++)
        if ((1 << mem_i) & req.memoryTypeBits && (mp.memoryTypes[mem_i].propertyFlags & flags) == flags)
            return mem_i;
    throw std::runtime_error("find_memory failed");
}

void change_layout(const vk::UniqueImage& image, vk::ImageLayout src, vk::ImageLayout dst)
{
    auto match = [](vk::ImageLayout layout) -> std::tuple<vk::AccessFlags, vk::PipelineStageFlags>
    {
        switch (layout)
        {
        case vk::ImageLayout::eGeneral:
        case vk::ImageLayout::eUndefined:
        case vk::ImageLayout::ePreinitialized:
            return { {}, vk::PipelineStageFlagBits::eAllGraphics };
        case vk::ImageLayout::eColorAttachmentOptimal:
            return { vk::AccessFlagBits::eColorAttachmentWrite, vk::PipelineStageFlagBits::eColorAttachmentOutput };
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return { vk::AccessFlagBits::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits::eEarlyFragmentTests };
        //case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
        //    return {vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return { vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eFragmentShader };
        case vk::ImageLayout::eTransferSrcOptimal:
            return { vk::AccessFlagBits::eTransferRead, vk::PipelineStageFlagBits::eTransfer };
        case vk::ImageLayout::eTransferDstOptimal:
            return { vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eTransfer };
        //case vk::ImageLayout::eDepthAttachmentOptimal:
        //    break;
        //case vk::ImageLayout::eDepthReadOnlyOptimal:
        //    break;
        case vk::ImageLayout::ePresentSrcKHR:
            return { vk::AccessFlagBits::eTransferWrite, vk::PipelineStageFlagBits::eTransfer };
        default:
            throw std::runtime_error("unsupported layout" + vk::to_string(layout));
        }
    };
    vk::CommandBufferAllocateInfo cmd_info;
    cmd_info.commandPool = *cmdpool;
    cmd_info.level = vk::CommandBufferLevel::ePrimary;
    cmd_info.commandBufferCount = 1;
    std::vector<vk::UniqueCommandBuffer> cmd = device->allocateCommandBuffersUnique(cmd_info);
    cmd[0]->begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    {
        vk::ImageMemoryBarrier barrier;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        barrier.image = *image;

        auto [srcAccessMask, srcStageMask] = match(src);
        auto [dstAccessMask, dstStageMask] = match(src);
        barrier.srcAccessMask = srcAccessMask;
        barrier.dstAccessMask = dstAccessMask;
        barrier.oldLayout = src;
        barrier.newLayout = dst;
        cmd[0]->pipelineBarrier(srcStageMask, dstStageMask,
            vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
    }
    cmd[0]->end();

    vk::UniqueFence submit_fence = device->createFenceUnique({});
    vk::SubmitInfo submit_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd[0].get();
    q.submit(submit_info, *submit_fence);
    q.waitIdle();
}

vk::UniqueShaderModule load_shader_module(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw std::runtime_error("cannot open the file " + path);
    size_t size = file.tellg();
    file.seekg(std::ios::beg);
    std::unique_ptr<char[]> buffer = std::make_unique<char[]>(size);
    file.read(buffer.get(), size);

    vk::ShaderModuleCreateInfo module_info;
    module_info.codeSize = size;
    module_info.pCode = reinterpret_cast<uint32_t*>(buffer.get());
    return device->createShaderModuleUnique(module_info);
}

int main()
{
    // Instance creation

    vk::ApplicationInfo instance_app_info;
    instance_app_info.pApplicationName = "VulkanSample";
    instance_app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    instance_app_info.pEngineName = "Custom";
    instance_app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    instance_app_info.apiVersion = VK_VERSION_1_2;
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
    instance = vk::createInstanceUnique(instance_info);

    // Window/Surface creation

    WNDCLASS wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_window_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = TEXT("MainWindow");
    RegisterClass(&wc);
    RECT window_rect = { 0, 0, 800, 600 };
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, false);
    HWND hWnd = CreateWindow(TEXT("MainWindow"), TEXT("VulkanSample"), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, window_rect.right - window_rect.left, 
        window_rect.bottom - window_rect.top, NULL, NULL, wc.hInstance, NULL);

    vk::Win32SurfaceCreateInfoKHR surface_info;
    surface_info.hinstance = wc.hInstance;
    surface_info.hwnd = hWnd;
    surface = instance->createWin32SurfaceKHRUnique(surface_info);

    // Create device

    find_device();
    auto pd_props = physical_device.getProperties();

    // Create Swapchain

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

    // Create useful global objects
    
    q = device->getQueue(device_family, 0);
    cmdpool = device->createCommandPoolUnique({ {}, device_family });
    std::array<vk::DescriptorPoolSize, 2> descrpool_sizes {
        vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, 2 },
        vk::DescriptorPoolSize{ vk::DescriptorType::eInputAttachment, 1 },
    };
    descrpool = device->createDescriptorPoolUnique({ {}, 2, (uint32_t)descrpool_sizes.size(), descrpool_sizes.data() });

    // Load the image into a staging vulkan image

    int image_width, image_height, image_comp;
    auto image_rgba = std::unique_ptr<uint8_t[]>(stbi_load("data/image.png", &image_width, &image_height, &image_comp, 4));

    vk::ImageCreateInfo image_info;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eR8G8B8A8Unorm;
    image_info.extent = vk::Extent3D(image_width, image_height, 1);
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eLinear;
    image_info.usage = vk::ImageUsageFlagBits::eTransferSrc;
    image_info.initialLayout = vk::ImageLayout::ePreinitialized;
    vk::UniqueImage image = device->createImageUnique(image_info);
    
    vk::SubresourceLayout image_layout = device->getImageSubresourceLayout(*image, 
        vk::ImageSubresource(vk::ImageAspectFlagBits::eColor, 0, 0));
    
    vk::MemoryRequirements image_mem_req = device->getImageMemoryRequirements(*image);
    vk::MemoryAllocateInfo image_mem_info;
    image_mem_info.allocationSize = image_mem_req.size;
    image_mem_info.memoryTypeIndex = find_memory(image_mem_req, 
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory image_mem = device->allocateMemoryUnique(image_mem_info);

    device->bindImageMemory(*image, *image_mem, 0);

    change_layout(image, vk::ImageLayout::ePreinitialized, vk::ImageLayout::eTransferSrcOptimal);

    uint8_t* image_mem_ptr = static_cast<uint8_t*>(device->mapMemory(*image_mem, 0, VK_WHOLE_SIZE));
    for (int row = 0; row < image_height; row++)
        std::copy_n(image_rgba.get() + row * image_width * 4,   // source
            (uint64_t)image_width * 4,                          // size
            image_mem_ptr + row * image_layout.rowPitch);       // destination
    device->unmapMemory(*image_mem);

    // Create PipelineLayout

    std::array<vk::DescriptorSetLayoutBinding, 2> descrset_layout_bindings {
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment),
    };
    vk::DescriptorSetLayoutCreateInfo descrset_layout_info;
    descrset_layout_info.bindingCount = (uint32_t)descrset_layout_bindings.size();
    descrset_layout_info.pBindings = descrset_layout_bindings.data();
    vk::UniqueDescriptorSetLayout descrset_layout = device->createDescriptorSetLayoutUnique(descrset_layout_info);
    vk::PipelineLayoutCreateInfo pipeline_layout_info;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descrset_layout.get();
    pipeline_layout_info.pushConstantRangeCount = 0;
    vk::UniquePipelineLayout pipeline_layout = device->createPipelineLayoutUnique(pipeline_layout_info);

    // Create PipelineLayout Composite

    std::array<vk::DescriptorSetLayoutBinding, 1> descrset_comp_layout_bindings {
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment),
    };
    vk::DescriptorSetLayoutCreateInfo descrset_comp_layout_info;
    descrset_comp_layout_info.bindingCount = (uint32_t)descrset_comp_layout_bindings.size();
    descrset_comp_layout_info.pBindings = descrset_comp_layout_bindings.data();
    vk::UniqueDescriptorSetLayout descrset_comp_layout = device->createDescriptorSetLayoutUnique(descrset_comp_layout_info);
    vk::PipelineLayoutCreateInfo pipeline_comp_layout_info;
    pipeline_comp_layout_info.setLayoutCount = 1;
    pipeline_comp_layout_info.pSetLayouts = &descrset_comp_layout.get();
    pipeline_comp_layout_info.pushConstantRangeCount = 0;
    vk::UniquePipelineLayout pipeline_comp_layout = device->createPipelineLayoutUnique(pipeline_comp_layout_info);

    // Create gbuffer

    vk::ImageCreateInfo gbuffer_pos_info;
    gbuffer_pos_info.imageType = vk::ImageType::e2D;
    gbuffer_pos_info.format = vk::Format::eR32G32B32A32Sfloat;
    gbuffer_pos_info.extent = vk::Extent3D(surface_caps.currentExtent, 1);
    gbuffer_pos_info.mipLevels = 1;
    gbuffer_pos_info.arrayLayers = 1;
    gbuffer_pos_info.samples = vk::SampleCountFlagBits::e1;
    gbuffer_pos_info.tiling = vk::ImageTiling::eOptimal;
    gbuffer_pos_info.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment;
    gbuffer_pos_info.initialLayout = vk::ImageLayout::eUndefined;
    vk::UniqueImage gbuffer_pos = device->createImageUnique(gbuffer_pos_info);
    vk::MemoryRequirements gbuffer_pos_mem_req = device->getImageMemoryRequirements(*gbuffer_pos);
    uint32_t gbuffer_pos_mem_idx = find_memory(gbuffer_pos_mem_req, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::UniqueDeviceMemory gbuffer_pos_mem = device->allocateMemoryUnique({ gbuffer_pos_mem_req.size, gbuffer_pos_mem_idx });
    device->bindImageMemory(*gbuffer_pos, *gbuffer_pos_mem, 0);
    vk::ImageViewCreateInfo gbuffer_pos_view_info;
    gbuffer_pos_view_info.image = *gbuffer_pos;
    gbuffer_pos_view_info.viewType = vk::ImageViewType::e2D;
    gbuffer_pos_view_info.format = gbuffer_pos_info.format;
    gbuffer_pos_view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    gbuffer_pos_view_info.subresourceRange.baseArrayLayer = 0;
    gbuffer_pos_view_info.subresourceRange.baseMipLevel = 0;
    gbuffer_pos_view_info.subresourceRange.layerCount = 1;
    gbuffer_pos_view_info.subresourceRange.levelCount = 1;
    vk::UniqueImageView gbuffer_pos_view = device->createImageViewUnique(gbuffer_pos_view_info);

    // Create DescriptorSets

    struct uniform_buffers_t
    {
        glm::mat4 mvp;
        uint8_t pad[0x100 - sizeof(mvp)]; // alignment
        glm::vec4 col;
    };

    uniform_buffers_t uniforms;
    uniforms.col = glm::vec4(0, 1, 0, 1);
    uniforms.mvp = glm::eulerAngleZ(glm::radians(45.f));

    vk::BufferCreateInfo uniform_buffer_info;
    uniform_buffer_info.size = sizeof(uniform_buffers_t);
    uniform_buffer_info.usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::UniqueBuffer uniform_buffer = device->createBufferUnique(uniform_buffer_info);
    vk::MemoryRequirements uniform_mem_req = device->getBufferMemoryRequirements(*uniform_buffer);
    uint32_t uniform_mem_idx = find_memory(uniform_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory uniform_mem = device->allocateMemoryUnique({ uniform_mem_req.size, uniform_mem_idx });
    device->bindBufferMemory(*uniform_buffer, *uniform_mem, 0);
    if (uniform_buffers_t* uniforms_ptr = reinterpret_cast<uniform_buffers_t*>(device->mapMemory(*uniform_mem, 0, VK_WHOLE_SIZE)))
    {
        *uniforms_ptr = uniforms;
        device->unmapMemory(*uniform_mem);
    }

    std::vector<vk::UniqueDescriptorSet> descr_sets = device->allocateDescriptorSetsUnique({ *descrpool, 1, &descrset_layout.get() });
    
    std::array<vk::DescriptorBufferInfo, 2> descr_sets_buffer{
        vk::DescriptorBufferInfo(*uniform_buffer, offsetof(uniform_buffers_t, mvp), sizeof(uniform_buffers_t::mvp)),
        vk::DescriptorBufferInfo(*uniform_buffer, offsetof(uniform_buffers_t, col), sizeof(uniform_buffers_t::col)),
    };
    std::array<vk::WriteDescriptorSet, 2> descr_sets_write{
        vk::WriteDescriptorSet(*descr_sets[0], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &descr_sets_buffer[0], nullptr),
        vk::WriteDescriptorSet(*descr_sets[0], 1, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &descr_sets_buffer[1], nullptr),
    };
    device->updateDescriptorSets(descr_sets_write, nullptr);

    // Create DescriptorSets Composite

    std::vector<vk::UniqueDescriptorSet> descr_sets_comp = device->allocateDescriptorSetsUnique({ *descrpool, 1, &descrset_comp_layout.get() });

    std::array<vk::DescriptorImageInfo, 1> descr_sets_comp_buffer {
        vk::DescriptorImageInfo(nullptr, *gbuffer_pos_view, vk::ImageLayout::eShaderReadOnlyOptimal),
    };
    std::array<vk::WriteDescriptorSet, 1> descr_sets_comp_write{
        vk::WriteDescriptorSet(*descr_sets_comp[0], 0, 0, 1, vk::DescriptorType::eInputAttachment, &descr_sets_comp_buffer[0], nullptr, nullptr),
    };
    device->updateDescriptorSets(descr_sets_comp_write, nullptr);

    // Renderpass

    std::array<vk::AttachmentDescription, 2> renderpass_attachments;
    // color buffer
    renderpass_attachments[0].format = swapchain_info.imageFormat;
    renderpass_attachments[0].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[0].loadOp = vk::AttachmentLoadOp::eLoad;
    renderpass_attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
    renderpass_attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[0].initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
    renderpass_attachments[0].finalLayout = vk::ImageLayout::ePresentSrcKHR;
    // gbuffer pos
    renderpass_attachments[1].format = gbuffer_pos_info.format;
    renderpass_attachments[1].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[1].loadOp = vk::AttachmentLoadOp::eClear;
    renderpass_attachments[1].storeOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[1].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[1].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[1].initialLayout = vk::ImageLayout::eUndefined;
    renderpass_attachments[1].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    
    std::array<vk::SubpassDescription, 2> renderpass_subpasses;
    
    std::array<vk::AttachmentReference, 1> renderpass_references_first;
    renderpass_references_first[0].attachment = 1;
    renderpass_references_first[0].layout = vk::ImageLayout::eColorAttachmentOptimal;
    renderpass_subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    renderpass_subpasses[0].colorAttachmentCount = (uint32_t)renderpass_references_first.size();
    renderpass_subpasses[0].pColorAttachments = renderpass_references_first.data();

    std::array<vk::AttachmentReference, 1> renderpass_references_second;
    renderpass_references_second[0].attachment = 0;
    renderpass_references_second[0].layout = vk::ImageLayout::eColorAttachmentOptimal;
    std::array<vk::AttachmentReference, 1> renderpass_references_second_input;
    renderpass_references_second_input[0].attachment = 1;
    renderpass_references_second_input[0].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    renderpass_subpasses[1].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    renderpass_subpasses[1].colorAttachmentCount = (uint32_t)renderpass_references_second.size();
    renderpass_subpasses[1].pColorAttachments = renderpass_references_second.data();
    renderpass_subpasses[1].inputAttachmentCount = (uint32_t)renderpass_references_second_input.size();
    renderpass_subpasses[1].pInputAttachments = renderpass_references_second_input.data();

    std::array<vk::SubpassDependency, 1> renderpass_deps;
    renderpass_deps[0].srcSubpass = 0;
    renderpass_deps[0].dstSubpass = 1;
    renderpass_deps[0].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    renderpass_deps[0].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    renderpass_deps[0].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    renderpass_deps[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    renderpass_deps[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    vk::RenderPassCreateInfo renderpass_info;
    renderpass_info.attachmentCount = (uint32_t)renderpass_attachments.size();
    renderpass_info.pAttachments = renderpass_attachments.data();
    renderpass_info.subpassCount = (uint32_t)renderpass_subpasses.size();
    renderpass_info.pSubpasses = renderpass_subpasses.data();
    renderpass_info.dependencyCount = (uint32_t)renderpass_deps.size();
    renderpass_info.pDependencies = renderpass_deps.data();
    vk::UniqueRenderPass renderpass = device->createRenderPassUnique(renderpass_info);

    // Load 3D model

    struct vertex_t
    {
        glm::vec3 pos;
        vertex_t() = default;
        vertex_t(glm::vec2 pos) : pos(glm::vec3(pos, 0)) {}
    };

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile("D:\\3D\\buildings.fbx", aiProcessPreset_TargetRealtime_Fast);

    std::vector<vertex_t> mesh_vert;
    std::vector<uint32_t> mesh_idx;
    auto m = scene->mMeshes[1];
    for (uint32_t i = 0; i < m->mNumVertices; i++)
    {
        vertex_t v;
        v.pos.x = m->mVertices[i].x;
        v.pos.y = m->mVertices[i].y;
        v.pos.z = m->mVertices[i].z;
        mesh_vert.push_back(v);
    }

    for (uint32_t i = 0; i < scene->mMeshes[0]->mNumFaces; i++)
    {
        for (int v = 0; v < 3; v++)
        {
            mesh_idx.push_back(m->mFaces[i].mIndices[v]);
        }
    }

    // vertex buffer
    vk::BufferCreateInfo triangle_buffer_info;
    triangle_buffer_info.size = mesh_vert.size() * sizeof(vertex_t);
    triangle_buffer_info.usage = vk::BufferUsageFlagBits::eVertexBuffer;
    vk::UniqueBuffer triangle_buffer = device->createBufferUnique(triangle_buffer_info);
    vk::MemoryRequirements triangle_buffer_mem_req = device->getBufferMemoryRequirements(*triangle_buffer);
    uint32_t triangle_buffer_mem_idx = find_memory(triangle_buffer_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory triangle_buffer_mem = device->allocateMemoryUnique({ triangle_buffer_mem_req.size, triangle_buffer_mem_idx });
    device->bindBufferMemory(*triangle_buffer, *triangle_buffer_mem, 0);
    if (auto* ptr = reinterpret_cast<vertex_t*>(device->mapMemory(*triangle_buffer_mem, 0, VK_WHOLE_SIZE)))
    {
        std::copy(mesh_vert.begin(), mesh_vert.end(), ptr);
        device->unmapMemory(*triangle_buffer_mem);
    }

    // index buffer
    vk::BufferCreateInfo triangle_buffer_idx_info;
    triangle_buffer_idx_info.size = mesh_idx.size() * sizeof(uint32_t);
    triangle_buffer_idx_info.usage = vk::BufferUsageFlagBits::eIndexBuffer;
    vk::UniqueBuffer triangle_buffer_idx = device->createBufferUnique(triangle_buffer_idx_info);
    vk::MemoryRequirements triangle_buffer_idx_mem_req = device->getBufferMemoryRequirements(*triangle_buffer_idx);
    uint32_t triangle_buffer_idx_mem_idx = find_memory(triangle_buffer_idx_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory triangle_buffer_idx_mem = device->allocateMemoryUnique({ triangle_buffer_idx_mem_req.size, triangle_buffer_idx_mem_idx });
    device->bindBufferMemory(*triangle_buffer_idx, *triangle_buffer_idx_mem, 0);
    if (auto* ptr = reinterpret_cast<uint32_t*>(device->mapMemory(*triangle_buffer_idx_mem, 0, VK_WHOLE_SIZE)))
    {
        std::copy(mesh_idx.begin(), mesh_idx.end(), ptr);
        device->unmapMemory(*triangle_buffer_idx_mem);
    }


    // Create Pipeline

    vk::UniqueShaderModule module_triangle_vert = load_shader_module("shaders/triangle.vert.spv");
    vk::UniqueShaderModule module_triangle_frag = load_shader_module("shaders/triangle.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> pipeline_stages {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *module_triangle_vert, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *module_triangle_frag, "main"),
    };

    std::array<vk::VertexInputBindingDescription, 1> pipeline_input_bindings{
        vk::VertexInputBindingDescription(0, sizeof(vertex_t), vk::VertexInputRate::eVertex),
    };
    std::array<vk::VertexInputAttributeDescription, 1> pipeline_input_attributes{
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(vertex_t, pos)),
    };
    vk::PipelineVertexInputStateCreateInfo pipeline_input;
    pipeline_input.vertexBindingDescriptionCount = (uint32_t)pipeline_input_bindings.size();
    pipeline_input.pVertexBindingDescriptions = pipeline_input_bindings.data();
    pipeline_input.vertexAttributeDescriptionCount = (uint32_t)pipeline_input_attributes.size();
    pipeline_input.pVertexAttributeDescriptions = pipeline_input_attributes.data();

    vk::PipelineInputAssemblyStateCreateInfo pipeline_assembly;
    pipeline_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    pipeline_assembly.primitiveRestartEnable = false;

    vk::Viewport pipeline_viewport_vp{ 0, 0, (float)surface_caps.currentExtent.width, (float)surface_caps.currentExtent.height, 0.f, 1.f };
    vk::Rect2D pipeline_viewport_scissor{ {}, surface_caps.currentExtent };
    vk::PipelineViewportStateCreateInfo pipeline_viewport;
    pipeline_viewport.viewportCount = 1;
    pipeline_viewport.pViewports = &pipeline_viewport_vp;
    pipeline_viewport.scissorCount = 1;
    pipeline_viewport.pScissors = &pipeline_viewport_scissor;

    vk::PipelineRasterizationStateCreateInfo pipeline_raster;
    pipeline_raster.depthClampEnable = false;
    pipeline_raster.rasterizerDiscardEnable = false;
    pipeline_raster.polygonMode = vk::PolygonMode::eFill;
    pipeline_raster.cullMode = vk::CullModeFlagBits::eNone;
    pipeline_raster.frontFace = vk::FrontFace::eClockwise;
    pipeline_raster.depthBiasEnable = false;
    pipeline_raster.lineWidth = 1.f;

    vk::PipelineMultisampleStateCreateInfo pipeline_multisample;
    pipeline_multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    pipeline_multisample.sampleShadingEnable = false;

    vk::PipelineColorBlendAttachmentState pipeline_blend_color;
    pipeline_blend_color.blendEnable = false;
    pipeline_blend_color.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo pipeline_blend;
    pipeline_blend.logicOpEnable = false;
    pipeline_blend.attachmentCount = 1;
    pipeline_blend.pAttachments = &pipeline_blend_color;

    vk::PipelineDynamicStateCreateInfo pipeline_dynamic;
    pipeline_dynamic.dynamicStateCount = 0;

    vk::GraphicsPipelineCreateInfo pipeline_info;
    pipeline_info.stageCount = (uint32_t)pipeline_stages.size();
    pipeline_info.pStages = pipeline_stages.data();
    pipeline_info.pVertexInputState = &pipeline_input;
    pipeline_info.pInputAssemblyState = &pipeline_assembly;
    pipeline_info.pTessellationState = nullptr;
    pipeline_info.pViewportState = &pipeline_viewport;
    pipeline_info.pRasterizationState = &pipeline_raster;
    pipeline_info.pMultisampleState = &pipeline_multisample;
    pipeline_info.pDepthStencilState = nullptr;
    pipeline_info.pColorBlendState = &pipeline_blend;
    pipeline_info.pDynamicState = &pipeline_dynamic;
    pipeline_info.layout = *pipeline_layout;
    pipeline_info.renderPass = *renderpass;
    pipeline_info.subpass = 0;

    vk::UniquePipeline pipeline = device->createGraphicsPipelineUnique(nullptr, pipeline_info);

    // Create Composite Pipeline

    vk::UniqueShaderModule module_composite_vert = load_shader_module("shaders/composite.vert.spv");
    vk::UniqueShaderModule module_composite_frag = load_shader_module("shaders/composite.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> pipeline_stages_composite {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, * module_composite_vert, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, * module_composite_frag, "main"),
    };

    vk::PipelineVertexInputStateCreateInfo pipeline_input_comp;

    pipeline_info.stageCount = (uint32_t)pipeline_stages_composite.size();
    pipeline_info.pStages = pipeline_stages_composite.data();
    pipeline_info.pVertexInputState = &pipeline_input_comp;
    pipeline_info.layout = *pipeline_comp_layout;
    pipeline_info.subpass = 1;

    vk::UniquePipeline pipeline_comp = device->createGraphicsPipelineUnique(nullptr, pipeline_info);


    // Create clear screen command

    vk::CommandBufferAllocateInfo cmd_clear_info;
    cmd_clear_info.commandPool = *cmdpool;
    cmd_clear_info.level = vk::CommandBufferLevel::ePrimary;
    cmd_clear_info.commandBufferCount = (uint32_t)swapchain_images.size();
    std::vector<vk::UniqueCommandBuffer> cmd_clear = device->allocateCommandBuffersUnique(cmd_clear_info);
    std::vector<vk::UniqueFramebuffer> framebuffers(swapchain_images.size());
    std::vector<vk::UniqueImageView> swapchain_views(swapchain_images.size());
    std::vector<vk::CommandBuffer> submit_commands(swapchain_images.size());
    for (int i = 0; i < swapchain_images.size(); i++)
    {
        cmd_clear[i]->begin({ vk::CommandBufferUsageFlags() });
        {
            vk::ImageMemoryBarrier barrier;
            barrier.image = swapchain_images[i];
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            barrier.srcAccessMask = {};
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            cmd_clear[i]->pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);

            vk::ImageBlit blit_region;
            blit_region.srcSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            blit_region.srcOffsets[0] = vk::Offset3D(0, 0, 0);
            blit_region.srcOffsets[1] = vk::Offset3D(image_width, image_height, 1);
            blit_region.dstSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            blit_region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            blit_region.dstOffsets[1] = vk::Offset3D(surface_caps.currentExtent.width, surface_caps.currentExtent.height, 1);
            cmd_clear[i]->blitImage(*image, vk::ImageLayout::eTransferSrcOptimal,
                swapchain_images[i], vk::ImageLayout::eTransferDstOptimal, blit_region, vk::Filter::eLinear);

            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
            cmd_clear[i]->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);

            vk::ImageViewCreateInfo view_info;
            view_info.image = swapchain_images[i];
            view_info.viewType = vk::ImageViewType::e2D;
            view_info.format = swapchain_info.imageFormat;
            view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.layerCount = 1;
            view_info.subresourceRange.levelCount = 1;
            swapchain_views[i] = device->createImageViewUnique(view_info);
            std::array<vk::ImageView, 2> framebuffer_attachments{ *swapchain_views[i], *gbuffer_pos_view };
            vk::FramebufferCreateInfo framebuffer_info;
            framebuffer_info.renderPass = *renderpass;
            framebuffer_info.attachmentCount = (uint32_t)framebuffer_attachments.size();
            framebuffer_info.pAttachments = framebuffer_attachments.data();
            framebuffer_info.width = surface_caps.currentExtent.width;
            framebuffer_info.height = surface_caps.currentExtent.height;
            framebuffer_info.layers = 1;
            framebuffers[i] = device->createFramebufferUnique(framebuffer_info);

            std::array<vk::ClearValue, 2> clear_values {
                vk::ClearValue(),
                vk::ClearColorValue({ std::array<float,4>{1, 0, 1, 1} }),
            };
            vk::RenderPassBeginInfo renderpass_begin_info;
            renderpass_begin_info.renderPass = *renderpass;
            renderpass_begin_info.framebuffer = *framebuffers[i];
            renderpass_begin_info.renderArea.extent = surface_caps.currentExtent;
            renderpass_begin_info.renderArea.offset = {};
            renderpass_begin_info.clearValueCount = (uint32_t)clear_values.size();
            renderpass_begin_info.pClearValues = clear_values.data();
            cmd_clear[i]->beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
            {
                cmd_clear[i]->bindVertexBuffers(0, *triangle_buffer, { 0 });
                cmd_clear[i]->bindIndexBuffer(*triangle_buffer_idx, 0, vk::IndexType::eUint32);
                cmd_clear[i]->bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, descr_sets[0].get(), nullptr);
                cmd_clear[i]->bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
                cmd_clear[i]->drawIndexed((uint32_t)mesh_idx.size(), 1, 0, 0, 0);
            }
            cmd_clear[i]->nextSubpass(vk::SubpassContents::eInline);
            {
                cmd_clear[i]->bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_comp_layout, 0, descr_sets_comp[0].get(), nullptr);
                cmd_clear[i]->bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_comp);
                cmd_clear[i]->draw(6, 1, 0, 0);
            }
            cmd_clear[i]->endRenderPass();

            //vk::ClearColorValue clear_color = std::array<float, 4>({ 1, 0, 1, 1 });
            //cmd_clear[i]->clearColorImage(swapchain_images[i], vk::ImageLayout::eTransferDstOptimal, 
            //    clear_color, barrier.subresourceRange);
        }
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

        if (!running)
            break;

        vk::UniqueSemaphore backbuffer_semaphore = device->createSemaphoreUnique({});
        auto backbuffer = device->acquireNextImageKHR(*swapchain, UINT64_MAX, *backbuffer_semaphore, nullptr);
        if (backbuffer.result == vk::Result::eSuccess)
        {
            static float angle = 0.f;
            angle += glm::radians(5.f);
            if (uniform_buffers_t* uniforms_ptr = reinterpret_cast<uniform_buffers_t*>(device->mapMemory(*uniform_mem, 0, VK_WHOLE_SIZE)))
            {
                float aspect = (float)surface_caps.currentExtent.width / (float)surface_caps.currentExtent.height;
                uniforms_ptr->mvp = glm::perspective(glm::radians(85.f), aspect, .1f, 100.f) *
                    glm::lookAt(glm::vec3(0, -2, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0)) * 
                    glm::scale(glm::vec3(.05f)) *
                    glm::eulerAngleY(angle);
                uniforms_ptr->col.r = glm::sin(angle);
                device->unmapMemory(*uniform_mem);
            }

            vk::UniqueSemaphore render_sem = device->createSemaphoreUnique({});
            std::array<vk::PipelineStageFlags, 1> wait_stages{ vk::PipelineStageFlagBits::eColorAttachmentOutput };
            vk::SubmitInfo submit_info;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &render_sem.get();
            submit_info.waitSemaphoreCount = 1;
            submit_info.pWaitSemaphores = &backbuffer_semaphore.get();
            submit_info.pWaitDstStageMask = wait_stages.data();
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &submit_commands[backbuffer.value];
            q.submit(submit_info, nullptr);

            vk::PresentInfoKHR present_info;
            present_info.waitSemaphoreCount = 1;
            present_info.pWaitSemaphores = &render_sem.get();
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain.get();
            present_info.pImageIndices = &backbuffer.value;
            q.presentKHR(present_info);
            q.waitIdle();
        }
    }

    exit(0);
}
