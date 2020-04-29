#include "pch.h"
#include "debug_message.h"

static bool running = true;

static vk::UniqueInstance instance;
static vk::UniqueSurfaceKHR surface;
static vk::UniqueDevice device;
static vk::UniqueCommandPool cmdpool;
static vk::UniqueDescriptorPool descrpool;

static uint32_t device_family = 0;
static vk::PhysicalDevice physical_device;
static vk::Queue q;

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE;

struct uniform_buffers_t
{
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    static constexpr uint32_t mvp_size = sizeof(model) + sizeof(view) + sizeof(proj);
    uint8_t pad2[0x100 - mvp_size & ~0x100]; // alignment
    
    glm::vec4 col;
    uint8_t pad3[0x100 - sizeof(col) & ~0x100]; // alignment
};

struct uniform_buffers_comp_t
{
    glm::vec4 camera;
    glm::vec4 light_pos;
    static constexpr uint32_t size = sizeof(camera) + sizeof(light_pos);
    uint8_t pad3[0x100 - size & ~0x100]; // alignment
};

struct uniform_rt_buffers_t
{
    glm::mat4 view_inverse;
    glm::mat4 proj_inverse;
    glm::vec4 color;
    static constexpr uint32_t rgen_size = sizeof(view_inverse) + sizeof(proj_inverse);
    uint8_t pad1[0x100 - rgen_size & ~0x100]; // alignment

    glm::vec4 light_pos;
    static constexpr uint32_t rhit_size = sizeof(light_pos);
    static constexpr uint32_t rhit_offset = rgen_size + sizeof(pad1);
    uint8_t pad2[0x100 - rgen_size & ~0x100]; // alignment
};

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
        auto pd_props = pd.getProperties();
        auto props = pd.getQueueFamilyProperties();
        for (int family_index = 0; family_index < props.size(); family_index++)
        {
            bool support_graphics = (bool)(props[family_index].queueFlags & vk::QueueFlagBits::eGraphics);
            bool support_present = pd.getSurfaceSupportKHR(family_index, *surface);
            if (support_graphics && support_present && pd_props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
            {
                std::array<const char*, 0> device_layers{
                };
                std::vector<const char*> device_extensions{
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                    VK_KHR_RAY_TRACING_EXTENSION_NAME,
                    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                };
                // Add debug names extension which is available only when it's profiled
                for (auto ext : pd.enumerateDeviceExtensionProperties())
                    if (strcmp(ext.extensionName, VK_EXT_DEBUG_MARKER_EXTENSION_NAME) == 0)
                        device_extensions.push_back(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);

                std::array<float, 1> queue_priorities{ 1.f };
                vk::DeviceQueueCreateInfo queue_info;
                queue_info.queueFamilyIndex = family_index;
                queue_info.queueCount = 1;
                queue_info.pQueuePriorities = queue_priorities.data();
                vk::PhysicalDeviceFeatures device_features;
                vk::PhysicalDeviceVulkan12Features  device_features12;
                device_features12.bufferDeviceAddress = true;
                vk::DeviceCreateInfo device_info;
                device_info.queueCreateInfoCount = 1;
                device_info.pQueueCreateInfos = &queue_info;
                device_info.enabledLayerCount = (uint32_t)device_layers.size();
                device_info.ppEnabledLayerNames = device_layers.data();
                device_info.enabledExtensionCount = (uint32_t)device_extensions.size();
                device_info.ppEnabledExtensionNames = device_extensions.data();
                device_info.pEnabledFeatures = &device_features;
                device = pd.createDeviceUnique(vk::StructureChain(device_info, device_features12)
                    .get<vk::DeviceCreateInfo>());
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

auto create_gbuffer(const std::string& name, const vk::Extent2D& extent, vk::Format format, vk::ImageUsageFlags usage)
{
    vk::ImageCreateInfo info;
    info.imageType = vk::ImageType::e2D;
    info.format = format;
    info.extent = vk::Extent3D(extent, 1);
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = usage;
    info.initialLayout = vk::ImageLayout::eUndefined;
    vk::UniqueImage image = device->createImageUnique(info);
    debug_name(image, name + " Image");
    vk::MemoryRequirements mem_req = device->getImageMemoryRequirements(*image);
    uint32_t mem_idx = find_memory(mem_req, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::UniqueDeviceMemory mem = device->allocateMemoryUnique({ mem_req.size, mem_idx });
    debug_name(mem, name + " Memory");
    device->bindImageMemory(*image, *mem, 0);
    vk::ImageViewCreateInfo view_info;
    view_info.image = *image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = info.format;
    view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.layerCount = 1;
    view_info.subresourceRange.levelCount = 1;
    vk::UniqueImageView view = device->createImageViewUnique(view_info);
    debug_name(view, name + " View");
    return std::tuple(std::move(image), std::move(mem), std::move(view));
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
    vk::UniqueCommandBuffer cmd = std::move(
        device->allocateCommandBuffersUnique(cmd_info)[0]);
    debug_name(cmd, "Change Layout OneTime Command");
    cmd->begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    {
        debug_mark_insert(cmd, "Change Image Layout");

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
        cmd->pipelineBarrier(srcStageMask, dstStageMask,
            vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
    }
    cmd->end();

    vk::SubmitInfo submit_info;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd.get();
    q.submit(submit_info, nullptr);
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
    vk::UniqueShaderModule m = device->createShaderModuleUnique(module_info);
    debug_name(m, "ShaderModule " + path);
    return m;
}

int main_run()
{
    // Instance creation

    vk::DynamicLoader dl;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    vk::ApplicationInfo instance_app_info;
    instance_app_info.pApplicationName = "VulkanSample";
    instance_app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    instance_app_info.pEngineName = "Custom";
    instance_app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    instance_app_info.apiVersion = VK_VERSION_1_2;
    std::array<const char*, 1> instance_layers {
        "VK_LAYER_KHRONOS_validation",
        //"VK_LAYER_RENDERDOC_Capture",
    };
    std::array<const char*, 4> instance_extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
    };
    vk::InstanceCreateInfo instance_info;
    instance_info.pApplicationInfo = &instance_app_info;
    instance_info.enabledLayerCount = (uint32_t)instance_layers.size();
    instance_info.ppEnabledLayerNames = instance_layers.data();
    instance_info.enabledExtensionCount = (uint32_t)instance_extensions.size();
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    instance = vk::createInstanceUnique(instance_info);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance);

    // Debugging
    
    auto debug_messenger = init_debug_message(instance);

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
    HWND hWnd = CreateWindow(TEXT("MainWindow"), TEXT("VulkanSample - RayTraced"), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, window_rect.right - window_rect.left, 
        window_rect.bottom - window_rect.top, NULL, NULL, wc.hInstance, NULL);

    vk::Win32SurfaceCreateInfoKHR surface_info;
    surface_info.hinstance = wc.hInstance;
    surface_info.hwnd = hWnd;
    surface = instance->createWin32SurfaceKHRUnique(surface_info);

    // Create device

    find_device();
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);

    auto pd_props = physical_device.getProperties();
    std::string title = fmt::format("VulkanSample - RayTraced - {}", pd_props.deviceName);
    SetWindowTextA(hWnd, title.c_str());

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

    // Create gbuffers

    auto [gbuffer_pos, gbuffer_pos_mem, gbuffer_pos_view] =
        create_gbuffer("GBufferPOS", surface_caps.currentExtent, vk::Format::eR32G32B32A32Sfloat, 
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);
    auto [gbuffer_nor, gbuffer_nor_mem, gbuffer_nor_view] =
        create_gbuffer("GBufferNOR", surface_caps.currentExtent, vk::Format::eR32G32B32A32Sfloat,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);
    auto [gbuffer_alb, gbuffer_alb_mem, gbuffer_alb_view] =
        create_gbuffer("GBufferALB", surface_caps.currentExtent, vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment);

    // Load 3D model

    struct vertex_t
    {
        glm::vec3 pos;
        glm::vec3 nor;
        vertex_t() = default;
        vertex_t(glm::vec3 pos) : pos(pos), nor(0) {}
        vertex_t(glm::vec2 pos) : pos(glm::vec3(pos, 0)), nor(0) {}
        vertex_t(glm::vec3 pos, glm::vec3 nor) : pos(pos), nor(nor) {}
    };

    struct mesh_t
    {
        uint32_t id;
        uint32_t vtx_offset;
        uint32_t vtx_count;
        uint32_t idx_offset;
        uint32_t idx_count;
        vk::DeviceAddress blas_addr;
        vk::DeviceSize blas_offset;
        vk::DeviceSize blas_size;
        vk::UniqueAccelerationStructureKHR blas;
        vk::AccelerationStructureBuildGeometryInfoKHR build_geo;
        vk::AccelerationStructureBuildOffsetInfoKHR build_offset;
    };

    struct node_t
    {
        std::vector<uint32_t> mesh_indices;
        glm::mat4 mat;
        glm::vec3 col;
    };

    std::vector<vertex_t> mesh_data_vert;
    std::vector<uint32_t> mesh_data_idx;

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile("D:\\3D\\cars.fbx", aiProcessPreset_TargetRealtime_Fast);
    std::vector<mesh_t> meshes;
    for (uint32_t mesh_index = 0; mesh_index < scene->mNumMeshes; mesh_index++)
    {
        aiMesh* scene_mesh = scene->mMeshes[mesh_index];
        mesh_t& mesh = meshes.emplace_back();
        mesh.id = mesh_index;
        mesh.idx_offset = (uint32_t)mesh_data_idx.size();
        mesh.idx_count = scene_mesh->mNumFaces * 3;
        mesh.vtx_offset = (uint32_t)mesh_data_vert.size();
        mesh.vtx_count = (uint32_t)scene_mesh->mNumVertices;
        for (uint32_t vertex_index = 0; vertex_index < scene_mesh->mNumVertices; vertex_index++)
        {
            glm::vec3 pos = glm::make_vec3(&scene_mesh->mVertices[vertex_index].x);
            glm::vec3 nor = glm::make_vec3(&scene_mesh->mNormals[vertex_index].x);
            mesh_data_vert.emplace_back(pos, nor);
        }
        for (uint32_t face_index = 0; face_index < scene_mesh->mNumFaces; face_index++)
        {
            mesh_data_idx.insert(mesh_data_idx.end(), 
                scene_mesh->mFaces[face_index].mIndices, 
                scene_mesh->mFaces[face_index].mIndices + 3);
        }
    }
    std::vector<node_t> nodes;
    for (uint32_t node_index = 0; node_index < scene->mRootNode->mNumChildren; node_index++)
    {
        aiNode* scene_node = scene->mRootNode->mChildren[node_index];
        node_t& node = nodes.emplace_back();
        node.col = glm::linearRand(glm::vec3(0), glm::vec3(1));
        node.mat = glm::identity<glm::mat4>();
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                node.mat[i][j] = scene_node->mTransformation[j][i];
        node.mesh_indices.insert(node.mesh_indices.end(), 
            scene_node->mMeshes, 
            scene_node->mMeshes + scene_node->mNumMeshes);
    }
    importer.FreeScene();

    // vertex buffer
    vk::BufferCreateInfo triangle_buffer_info;
    triangle_buffer_info.size = mesh_data_vert.size() * sizeof(vertex_t);
    triangle_buffer_info.usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    vk::UniqueBuffer triangle_buffer = device->createBufferUnique(triangle_buffer_info);
    vk::MemoryRequirements triangle_buffer_mem_req = device->getBufferMemoryRequirements(*triangle_buffer);
    uint32_t triangle_buffer_mem_idx = find_memory(triangle_buffer_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::StructureChain triangle_buffer_mem_info{
        vk::MemoryAllocateInfo(triangle_buffer_mem_req.size, triangle_buffer_mem_idx),
        vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress) };
    vk::UniqueDeviceMemory triangle_buffer_mem = device->allocateMemoryUnique(
        triangle_buffer_mem_info.get<vk::MemoryAllocateInfo>());
    device->bindBufferMemory(*triangle_buffer, *triangle_buffer_mem, 0);
    if (auto* ptr = reinterpret_cast<vertex_t*>(device->mapMemory(*triangle_buffer_mem, 0, VK_WHOLE_SIZE)))
    {
        std::copy(mesh_data_vert.begin(), mesh_data_vert.end(), ptr);
        device->unmapMemory(*triangle_buffer_mem);
    }

    // index buffer
    vk::BufferCreateInfo triangle_buffer_idx_info;
    triangle_buffer_idx_info.size = mesh_data_idx.size() * sizeof(uint32_t);
    triangle_buffer_idx_info.usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    vk::UniqueBuffer triangle_buffer_idx = device->createBufferUnique(triangle_buffer_idx_info);
    vk::MemoryRequirements triangle_buffer_idx_mem_req = device->getBufferMemoryRequirements(*triangle_buffer_idx);
    uint32_t triangle_buffer_idx_mem_idx = find_memory(triangle_buffer_idx_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::StructureChain triangle_buffer_idx_mem_info{
        vk::MemoryAllocateInfo(triangle_buffer_idx_mem_req.size, triangle_buffer_idx_mem_idx),
        vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress) };
    vk::UniqueDeviceMemory triangle_buffer_idx_mem = device->allocateMemoryUnique(
        triangle_buffer_mem_info.get<vk::MemoryAllocateInfo>());
    device->bindBufferMemory(*triangle_buffer_idx, *triangle_buffer_idx_mem, 0);
    if (auto* ptr = reinterpret_cast<uint32_t*>(device->mapMemory(*triangle_buffer_idx_mem, 0, VK_WHOLE_SIZE)))
    {
        std::copy(mesh_data_idx.begin(), mesh_data_idx.end(), ptr);
        device->unmapMemory(*triangle_buffer_idx_mem);
    }

    // Create Queue and Pools

    q = device->getQueue(device_family, 0);
    cmdpool = device->createCommandPoolUnique({ {}, device_family });
    std::array<vk::DescriptorPoolSize, 4> descrpool_sizes{
        vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, (uint32_t)nodes.size() * 3 },
        vk::DescriptorPoolSize{ vk::DescriptorType::eInputAttachment, 4 },
        vk::DescriptorPoolSize{ vk::DescriptorType::eAccelerationStructureKHR, 1 },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 1 },
    };
    uint32_t pool_size =
        (uint32_t)nodes.size()  // geometry pass
        + 1                     // composition
        + 1                     // raytracing
    ;
    uint32_t pool_size_comp = 1;
    uint32_t pool_size_rt = 1;
    descrpool = device->createDescriptorPoolUnique({ {}, pool_size,
        (uint32_t)descrpool_sizes.size(), descrpool_sizes.data() });

    // Create RT objects

    auto rt_props = physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPropertiesKHR>()
        .get<vk::PhysicalDeviceRayTracingPropertiesKHR>();
    
    // BLAS
    vk::DeviceSize blas_mem_size = 0;
    vk::DeviceSize scratch_size = 0;
    vk::MemoryRequirements2 blas_mem_req;
    for (auto& m : meshes)
    {
        vk::AccelerationStructureCreateGeometryTypeInfoKHR geo_info;
        geo_info.geometryType = vk::GeometryTypeKHR::eTriangles;
        geo_info.maxPrimitiveCount = m.idx_count / 3;
        geo_info.indexType = vk::IndexType::eUint32;
        geo_info.maxVertexCount = m.vtx_count;
        geo_info.vertexFormat = vk::Format::eR32G32B32Sfloat;
        geo_info.allowsTransforms = false;

        vk::AccelerationStructureGeometryKHR geo;
        geo.flags = vk::GeometryFlagBitsKHR::eOpaque;
        geo.geometryType = vk::GeometryTypeKHR::eTriangles;
        geo.geometry.triangles.vertexFormat = geo_info.vertexFormat;
        geo.geometry.triangles.vertexStride = sizeof(vertex_t);
        geo.geometry.triangles.vertexData = device->getBufferAddressKHR({ *triangle_buffer });
        geo.geometry.triangles.indexData = device->getBufferAddressKHR({ *triangle_buffer_idx });
        geo.geometry.triangles.indexType = geo_info.indexType;

        vk::AccelerationStructureCreateInfoKHR blas_info;
        blas_info.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        blas_info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        blas_info.maxGeometryCount = 1;
        blas_info.pGeometryInfos = &geo_info;
        m.blas = device->createAccelerationStructureKHRUnique(blas_info);
        debug_name(m.blas, fmt::format("BLAS mesh#{}", m.id));

        blas_mem_req = device->getAccelerationStructureMemoryRequirementsKHR({
            vk::AccelerationStructureMemoryRequirementsTypeKHR::eObject, 
            vk::AccelerationStructureBuildTypeKHR::eDevice, *m.blas });
        m.blas_offset = blas_mem_size;
        m.blas_size = blas_mem_req.memoryRequirements.size;
        blas_mem_size += m.blas_size;

        vk::MemoryRequirements2 scratch_req = device->getAccelerationStructureMemoryRequirementsKHR({
            vk::AccelerationStructureMemoryRequirementsTypeKHR::eBuildScratch, 
            vk::AccelerationStructureBuildTypeKHR::eDevice, *m.blas });
        scratch_size = std::max(scratch_size, scratch_req.memoryRequirements.size);

        const vk::AccelerationStructureGeometryKHR* pGeometry = &geo;
        m.build_geo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
        m.build_geo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        m.build_geo.update = false;
        m.build_geo.dstAccelerationStructure = *m.blas;
        m.build_geo.geometryArrayOfPointers = false;
        m.build_geo.geometryCount = 1;
        m.build_geo.ppGeometries = &pGeometry;

        m.build_offset.primitiveCount = geo_info.maxPrimitiveCount;
        m.build_offset.primitiveOffset = m.idx_offset * sizeof(uint32_t);
        m.build_offset.firstVertex = m.vtx_offset;
    }
    
    uint32_t blas_mem_idx = find_memory(blas_mem_req.memoryRequirements, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::UniqueDeviceMemory blas_mem = device->allocateMemoryUnique({ blas_mem_size, blas_mem_idx });
    debug_name(blas_mem, "BLAS Memory");

    for (auto& m : meshes)
    {
        device->bindAccelerationStructureMemoryKHR({ { *m.blas, *blas_mem, m.blas_offset } });
        m.blas_addr = device->getAccelerationStructureAddressKHR({ *m.blas });
    }


    // TLAS
    std::vector<vk::AccelerationStructureInstanceKHR> rt_instances;
    for (const auto& n : nodes)
    {
        for (const auto& mesh_index : n.mesh_indices)
        {
            auto& inst = rt_instances.emplace_back();
            // glm:column-major to NV:row-major
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 4; j++)
                    inst.transform.matrix[i][j] = n.mat[j][i];
            inst.instanceCustomIndex = rt_instances.size() - 1;
            inst.mask = 0xFF;
            inst.instanceShaderBindingTableRecordOffset = 0;
            inst.flags = (uint8_t)vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable;
            inst.accelerationStructureReference = meshes[mesh_index].blas_addr;
        }
    }

    vk::AccelerationStructureCreateGeometryTypeInfoKHR tlas_geo_info;
    tlas_geo_info.geometryType = vk::GeometryTypeKHR::eInstances;
    tlas_geo_info.maxPrimitiveCount = (uint32_t)rt_instances.size();
    tlas_geo_info.allowsTransforms = true;

    vk::AccelerationStructureCreateInfoKHR tlas_info;
    tlas_info.type = vk::AccelerationStructureTypeKHR::eTopLevel;
    tlas_info.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    tlas_info.maxGeometryCount = 1;
    tlas_info.pGeometryInfos = &tlas_geo_info;
    vk::UniqueAccelerationStructureKHR tlas = device->createAccelerationStructureKHRUnique(tlas_info);
    debug_name(tlas, "TLAS");

    vk::MemoryRequirements2 tlas_mem_req = device->getAccelerationStructureMemoryRequirementsKHR({
        vk::AccelerationStructureMemoryRequirementsTypeKHR::eObject,
        vk::AccelerationStructureBuildTypeKHR::eDevice, *tlas });
    uint32_t tlas_mem_idx = find_memory(tlas_mem_req.memoryRequirements, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::UniqueDeviceMemory tlas_mem = device->allocateMemoryUnique({ tlas_mem_req.memoryRequirements.size, tlas_mem_idx });
    debug_name(tlas_mem, "TLAS Memory");
    device->bindAccelerationStructureMemoryKHR({ {*tlas, *tlas_mem, 0} });

    vk::MemoryRequirements2 tlas_scratch_req = device->getAccelerationStructureMemoryRequirementsKHR({
        vk::AccelerationStructureMemoryRequirementsTypeKHR::eBuildScratch,
        vk::AccelerationStructureBuildTypeKHR::eDevice, *tlas });
    scratch_size = std::max(scratch_size, tlas_scratch_req.memoryRequirements.size);

    // Scratch buffer
    vk::BufferCreateInfo scratch_buffer_info;
    scratch_buffer_info.size = scratch_size;
    scratch_buffer_info.usage = vk::BufferUsageFlagBits::eRayTracingKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    vk::UniqueBuffer scratch_buffer = device->createBufferUnique(scratch_buffer_info);
    debug_name(scratch_buffer, "Scratch Buffer");
    vk::MemoryRequirements scratch_mem_req = device->getBufferMemoryRequirements(*scratch_buffer);
    uint32_t scratch_mem_idx = find_memory(scratch_mem_req, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::StructureChain scratch_mem_info{
        vk::MemoryAllocateInfo(scratch_mem_req.size, scratch_mem_idx),
        vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress) };
    vk::UniqueDeviceMemory scratch_mem = device->allocateMemoryUnique(
        scratch_mem_info.get<vk::MemoryAllocateInfo>());
    debug_name(scratch_mem, "Scratch Buffer Memory");

    device->bindBufferMemory(*scratch_buffer, *scratch_mem, 0);
    vk::DeviceAddress scratch_addr = device->getBufferAddressKHR(*scratch_buffer);

    // Instance buffer
    vk::BufferCreateInfo instance_buffer_info;
    instance_buffer_info.size = rt_instances.size() * sizeof(vk::AccelerationStructureInstanceKHR);
    instance_buffer_info.usage = vk::BufferUsageFlagBits::eRayTracingKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    vk::UniqueBuffer instance_buffer = device->createBufferUnique(instance_buffer_info);
    debug_name(instance_buffer, "Instance Buffer");
    vk::MemoryRequirements instance_buffer_mem_req = device->getBufferMemoryRequirements(*instance_buffer);
    uint32_t instance_buffer_mem_idx = find_memory(instance_buffer_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::StructureChain instance_buffer_mem_info{
        vk::MemoryAllocateInfo(instance_buffer_mem_req.size, instance_buffer_mem_idx),
        vk::MemoryAllocateFlagsInfo(vk::MemoryAllocateFlagBits::eDeviceAddress) };
    vk::UniqueDeviceMemory instance_buffer_mem = device->allocateMemoryUnique(
        instance_buffer_mem_info.get<vk::MemoryAllocateInfo>());
    debug_name(instance_buffer_mem, "Instance Buffer Memory");
    device->bindBufferMemory(*instance_buffer, *instance_buffer_mem, 0);
    if (auto* ptr = reinterpret_cast<vk::AccelerationStructureInstanceKHR*>(device->mapMemory(*instance_buffer_mem, 0, VK_WHOLE_SIZE)))
    {
        std::copy(rt_instances.begin(), rt_instances.end(), ptr);
        device->unmapMemory(*instance_buffer_mem);
    }

    vk::AccelerationStructureGeometryKHR tlas_geo;
    tlas_geo.geometryType = vk::GeometryTypeKHR::eInstances;
    tlas_geo.geometry.instances.arrayOfPointers = false;
    tlas_geo.geometry.instances.data = device->getBufferAddressKHR(*instance_buffer);

    const vk::AccelerationStructureGeometryKHR* tlas_build_geo_pGeometry = &tlas_geo;
    vk::AccelerationStructureBuildGeometryInfoKHR tlas_build_geo;
    tlas_build_geo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
    tlas_build_geo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    tlas_build_geo.update = false;
    tlas_build_geo.dstAccelerationStructure = *tlas;
    tlas_build_geo.geometryArrayOfPointers = false;
    tlas_build_geo.geometryCount = 1;
    tlas_build_geo.ppGeometries = &tlas_build_geo_pGeometry;
    tlas_build_geo.scratchData = scratch_addr;

    vk::AccelerationStructureBuildOffsetInfoKHR tlas_build_offset;
    tlas_build_offset.primitiveCount = (uint32_t)rt_instances.size();
    
    // Build BLAS
    for (auto& m : meshes)
    {
        vk::UniqueCommandBuffer cmd_builder = std::move(
            device->allocateCommandBuffersUnique({ *cmdpool, vk::CommandBufferLevel::ePrimary, 1 })[0]);
        debug_name(cmd_builder, "AS Build Command");
        cmd_builder->begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
        debug_mark_insert(cmd_builder, "Build BLAS");
        debug_mark_begin(cmd_builder, fmt::format("Build Mesh#{}", m.id));
        
        m.build_geo.scratchData = scratch_addr;
        const vk::AccelerationStructureBuildOffsetInfoKHR* pBuildOffsetInfo = &m.build_offset;
        cmd_builder->buildAccelerationStructureKHR(m.build_geo, pBuildOffsetInfo);

        vk::MemoryBarrier barrier(vk::AccessFlagBits::eAccelerationStructureWriteKHR,
            vk::AccessFlagBits::eAccelerationStructureReadKHR);
        cmd_builder->pipelineBarrier(vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
            vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR,
            vk::DependencyFlags(), { barrier }, {}, {});
        
        debug_mark_end(cmd_builder);
        cmd_builder->end();

        vk::SubmitInfo cmd_build_submit;
        cmd_build_submit.commandBufferCount = 1;
        cmd_build_submit.pCommandBuffers = &cmd_builder.get();
        q.submit(cmd_build_submit, nullptr);
        q.waitIdle();
    }

    // Build TLAS
    {
        vk::UniqueCommandBuffer cmd_builder = std::move(
            device->allocateCommandBuffersUnique({ *cmdpool, vk::CommandBufferLevel::ePrimary, 1 })[0]);
        debug_name(cmd_builder, "AS Build Command");
        cmd_builder->begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

        debug_mark_insert(cmd_builder, "Build TLAS");
        const vk::AccelerationStructureBuildOffsetInfoKHR* pBuildOffsetInfo = &tlas_build_offset;
        cmd_builder->buildAccelerationStructureKHR(tlas_build_geo, pBuildOffsetInfo);
        
        debug_mark_end(cmd_builder);
        cmd_builder->end();

        vk::SubmitInfo cmd_build_submit;
        cmd_build_submit.commandBufferCount = 1;
        cmd_build_submit.pCommandBuffers = &cmd_builder.get();
        q.submit(cmd_build_submit, nullptr);
        q.waitIdle();
    }

    // RT Pipeline

    // DescriptorSet Layout
    std::array<vk::DescriptorSetLayoutBinding, 4> rt_descrset_layout_bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eAccelerationStructureKHR, 1, vk::ShaderStageFlagBits::eRaygenKHR),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eRaygenKHR),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eRaygenKHR),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eRaygenKHR),
    };
    vk::DescriptorSetLayoutCreateInfo rt_descrset_layout_info;
    rt_descrset_layout_info.bindingCount = (uint32_t)rt_descrset_layout_bindings.size();
    rt_descrset_layout_info.pBindings = rt_descrset_layout_bindings.data();
    vk::UniqueDescriptorSetLayout rt_descrset_layout = device->createDescriptorSetLayoutUnique(rt_descrset_layout_info);    
    debug_name(rt_descrset_layout, "RT Descriptor Set Layout");

    // Allocate DescriptorSets
    vk::UniqueDescriptorSet rt_descr_sets = std::move(
        device->allocateDescriptorSetsUnique({ *descrpool, 1, &rt_descrset_layout.get() })[0]);
    debug_name(rt_descr_sets, "RT Descriptor Set");

    // Create Uniform Buffer
    vk::BufferCreateInfo uniform_rt_buffer_info;
    uniform_rt_buffer_info.size = sizeof(uniform_rt_buffers_t);
    uniform_rt_buffer_info.usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::UniqueBuffer uniform_rt_buffer = device->createBufferUnique(uniform_rt_buffer_info);
    debug_name(uniform_rt_buffer, "RT Uniform Buffer");
    vk::MemoryRequirements uniform_rt_mem_req = device->getBufferMemoryRequirements(*uniform_rt_buffer);
    uint32_t uniform_rt_mem_idx = find_memory(uniform_rt_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory uniform_rt_mem = device->allocateMemoryUnique({ uniform_rt_mem_req.size, uniform_rt_mem_idx });
    debug_name(uniform_rt_mem, "RT Uniform Buffer Memory");
    device->bindBufferMemory(*uniform_rt_buffer, *uniform_rt_mem, 0);

    // Create Output Image
    auto [rt_output, rt_output_mem, rt_output_view] =
        create_gbuffer("GBufferPOS", surface_caps.currentExtent, vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage);

    // Update DescriptorSets
    vk::DescriptorImageInfo rt_descr_set_image(nullptr, *rt_output_view, vk::ImageLayout::eGeneral);
    vk::DescriptorBufferInfo rt_descr_set_ubo_rgen(*uniform_rt_buffer, 0, uniform_rt_buffers_t::rgen_size);
    vk::DescriptorBufferInfo rt_descr_set_ubo_rhit(*uniform_rt_buffer, uniform_rt_buffers_t::rhit_offset, uniform_rt_buffers_t::rhit_size);
    vk::StructureChain rt_descr_set_tlas_chain(
        vk::WriteDescriptorSet(*rt_descr_sets, 0, 0, 1, vk::DescriptorType::eAccelerationStructureKHR),
        vk::WriteDescriptorSetAccelerationStructureKHR(1, &tlas.get())
    );
    std::array<vk::WriteDescriptorSet, 4> rt_descr_set_write{
        rt_descr_set_tlas_chain.get<vk::WriteDescriptorSet>(),
        vk::WriteDescriptorSet(*rt_descr_sets, 1, 0, 1, vk::DescriptorType::eStorageImage, &rt_descr_set_image),
        vk::WriteDescriptorSet(*rt_descr_sets, 2, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &rt_descr_set_ubo_rgen),
        vk::WriteDescriptorSet(*rt_descr_sets, 3, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &rt_descr_set_ubo_rhit),
    };
    device->updateDescriptorSets(rt_descr_set_write, nullptr);

    // Pipeline Layout
    vk::PipelineLayoutCreateInfo rt_pipeline_layout_info;
    rt_pipeline_layout_info.setLayoutCount = 1;
    rt_pipeline_layout_info.pSetLayouts = &rt_descrset_layout.get();
    rt_pipeline_layout_info.pushConstantRangeCount = 0;
    vk::UniquePipelineLayout rt_pipeline_layout = device->createPipelineLayoutUnique(rt_pipeline_layout_info);
    debug_name(rt_pipeline_layout, "RT Pipeline Layout");

    // Ray-tracing Pipeline
    
    // Load shaders
    vk::UniqueShaderModule module_trace_rgen = load_shader_module("shaders/trace.rgen.spv");
    vk::UniqueShaderModule module_trace_rmiss = load_shader_module("shaders/trace.rmiss.spv");
    vk::UniqueShaderModule module_trace_rchit = load_shader_module("shaders/trace.rchit.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 3> rt_pipeline_stages{
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eRaygenKHR, *module_trace_rgen, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eMissKHR, *module_trace_rmiss, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eClosestHitKHR, *module_trace_rchit, "main"),
    };

    // Shader groups
    std::array<vk::RayTracingShaderGroupCreateInfoKHR, 3> rt_groups{
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral,
            0, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR),
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eGeneral,
            1, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR),
        vk::RayTracingShaderGroupCreateInfoKHR(vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
            VK_SHADER_UNUSED_KHR, 2, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR),
    };

    // Ray-tracing Pipeline
    vk::RayTracingPipelineCreateInfoKHR rt_pipeline_info;
    rt_pipeline_info.stageCount = (uint32_t)rt_pipeline_stages.size();
    rt_pipeline_info.pStages = rt_pipeline_stages.data();
    rt_pipeline_info.groupCount = (uint32_t)rt_groups.size();
    rt_pipeline_info.pGroups = rt_groups.data();
    rt_pipeline_info.maxRecursionDepth = 1;
    rt_pipeline_info.layout = *rt_pipeline_layout;
    vk::UniquePipeline rt_pipeline = device->createRayTracingPipelineKHRUnique(nullptr, rt_pipeline_info).value;
    debug_name(rt_pipeline, "RT Pipeline");

    // Shaders Binding Table

    // Create Buffer
    vk::BufferCreateInfo sbt_buffer_info;
    sbt_buffer_info.size = rt_props.shaderGroupHandleSize * rt_pipeline_stages.size();
    sbt_buffer_info.usage = vk::BufferUsageFlagBits::eRayTracingKHR;
    vk::UniqueBuffer sbt_buffer = device->createBufferUnique(sbt_buffer_info);
    debug_name(sbt_buffer, "SBT Buffer");
    vk::MemoryRequirements sbt_buffer_mem_req = device->getBufferMemoryRequirements(*sbt_buffer);
    uint32_t sbt_buffer_mem_idx = find_memory(sbt_buffer_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory sbt_buffer_mem = device->allocateMemoryUnique({ sbt_buffer_mem_req.size, sbt_buffer_mem_idx });
    debug_name(sbt_buffer_mem, "SBT Buffer Memory");
    device->bindBufferMemory(*sbt_buffer, *sbt_buffer_mem, 0);
    if (auto ptr = reinterpret_cast<uint8_t*>(device->mapMemory(*sbt_buffer_mem, 0, VK_WHOLE_SIZE)))
    {
        device->getRayTracingShaderGroupHandlesKHR<uint8_t>(*rt_pipeline, 0, (uint32_t)rt_pipeline_stages.size(), { (uint32_t)sbt_buffer_info.size, ptr });
        device->unmapMemory(*sbt_buffer_mem);
    }

    vk::UniqueCommandBuffer cmd_trace = std::move(
        device->allocateCommandBuffersUnique({ *cmdpool, vk::CommandBufferLevel::ePrimary, 1 })[0]);
    debug_name(cmd_trace, "cmd_trace");
    cmd_trace->begin({ { vk::CommandBufferUsageFlags() } });
    cmd_trace->bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, *rt_pipeline);
    cmd_trace->bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, 
        *rt_pipeline_layout, 0, *rt_descr_sets, nullptr);
    
    vk::ImageMemoryBarrier barrier;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    barrier.image = *rt_output;
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    cmd_trace->pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eRayTracingShaderKHR,
        vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);
    
    debug_mark_insert(cmd_trace, "Trace Rays");
    vk::DeviceSize sbt_size = rt_props.shaderGroupHandleSize * 3;
    cmd_trace->traceRaysKHR(
        { *sbt_buffer, rt_props.shaderGroupHandleSize * 0, rt_props.shaderGroupHandleSize, sbt_size },
        { *sbt_buffer, rt_props.shaderGroupHandleSize * 1, rt_props.shaderGroupHandleSize, sbt_size },
        { *sbt_buffer, rt_props.shaderGroupHandleSize * 2, rt_props.shaderGroupHandleSize, sbt_size },
        { },
        surface_caps.currentExtent.width, surface_caps.currentExtent.height, 1);
    cmd_trace->end();

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
    debug_name(image, "image.png Image");

    vk::SubresourceLayout image_layout = device->getImageSubresourceLayout(*image, 
        vk::ImageSubresource(vk::ImageAspectFlagBits::eColor, 0, 0));
    
    vk::MemoryRequirements image_mem_req = device->getImageMemoryRequirements(*image);
    vk::MemoryAllocateInfo image_mem_info;
    image_mem_info.allocationSize = image_mem_req.size;
    image_mem_info.memoryTypeIndex = find_memory(image_mem_req, 
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory image_mem = device->allocateMemoryUnique(image_mem_info);
    debug_name(image_mem, "image.png Memory");

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
    debug_name(descrset_layout, "GEO DescriptorSet Layout");

    vk::PipelineLayoutCreateInfo pipeline_layout_info;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descrset_layout.get();
    pipeline_layout_info.pushConstantRangeCount = 0;
    vk::UniquePipelineLayout pipeline_layout = device->createPipelineLayoutUnique(pipeline_layout_info);
    debug_name(pipeline_layout, "GEO Pipeline Layout");

    // Create PipelineLayout Composite

    std::array<vk::DescriptorSetLayoutBinding, 5> descrset_comp_layout_bindings {
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eInputAttachment, 1, vk::ShaderStageFlagBits::eFragment),
    };
    vk::DescriptorSetLayoutCreateInfo descrset_comp_layout_info;
    descrset_comp_layout_info.bindingCount = (uint32_t)descrset_comp_layout_bindings.size();
    descrset_comp_layout_info.pBindings = descrset_comp_layout_bindings.data();
    vk::UniqueDescriptorSetLayout descrset_comp_layout = device->createDescriptorSetLayoutUnique(descrset_comp_layout_info);
    debug_name(descrset_comp_layout, "COMP DescriptorSet Layout");
    vk::PipelineLayoutCreateInfo pipeline_comp_layout_info;
    pipeline_comp_layout_info.setLayoutCount = 1;
    pipeline_comp_layout_info.pSetLayouts = &descrset_comp_layout.get();
    pipeline_comp_layout_info.pushConstantRangeCount = 0;
    vk::UniquePipelineLayout pipeline_comp_layout = device->createPipelineLayoutUnique(pipeline_comp_layout_info);
    debug_name(pipeline_comp_layout, "COMP Pipeline Layout");

    // Create depth image

    vk::ImageCreateInfo depth_info;
    depth_info.imageType = vk::ImageType::e2D;
    depth_info.format = vk::Format::eD16Unorm;
    depth_info.extent = vk::Extent3D(surface_caps.currentExtent, 1);
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = vk::SampleCountFlagBits::e1;
    depth_info.tiling = vk::ImageTiling::eOptimal; // check support
    depth_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eInputAttachment;
    depth_info.initialLayout = vk::ImageLayout::eUndefined;
    vk::UniqueImage depth = device->createImageUnique(depth_info);
    debug_name(depth, "Depth Image");
    vk::MemoryRequirements depth_mem_req = device->getImageMemoryRequirements(*depth);
    uint32_t depth_mem_idx = find_memory(depth_mem_req, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::UniqueDeviceMemory depth_mem = device->allocateMemoryUnique({ depth_mem_req.size, depth_mem_idx });
    debug_name(depth_mem, "Depth Image Memory");
    device->bindImageMemory(*depth, *depth_mem, 0);
        vk::ImageViewCreateInfo depth_view_info;
    depth_view_info.image = *depth;
    depth_view_info.viewType = vk::ImageViewType::e2D;
    depth_view_info.format = depth_info.format;
    depth_view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    depth_view_info.subresourceRange.baseArrayLayer = 0;
    depth_view_info.subresourceRange.baseMipLevel = 0;
    depth_view_info.subresourceRange.layerCount = 1;
    depth_view_info.subresourceRange.levelCount = 1;
    vk::UniqueImageView depth_view = device->createImageViewUnique(depth_view_info);
    debug_name(depth_view, "Depth View");

    // Create DescriptorSets

    vk::BufferCreateInfo uniform_buffer_info;
    uniform_buffer_info.size = sizeof(uniform_buffers_t) * nodes.size();
    uniform_buffer_info.usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::UniqueBuffer uniform_buffer = device->createBufferUnique(uniform_buffer_info);
    debug_name(uniform_buffer, "GEO Uniform Buffer");
    vk::MemoryRequirements uniform_mem_req = device->getBufferMemoryRequirements(*uniform_buffer);
    uint32_t uniform_mem_idx = find_memory(uniform_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory uniform_mem = device->allocateMemoryUnique({ uniform_mem_req.size, uniform_mem_idx });
    debug_name(uniform_mem, "GEO Uniform Buffer Memory");
    device->bindBufferMemory(*uniform_buffer, *uniform_mem, 0);

    std::vector<vk::DescriptorSetLayout> descr_sets_layouts(nodes.size(), *descrset_layout);
    std::vector<vk::UniqueDescriptorSet> descr_sets = device->allocateDescriptorSetsUnique({ *descrpool, 
        (uint32_t)descr_sets_layouts.size(), descr_sets_layouts.data() });
    
    std::vector<vk::DescriptorBufferInfo> descr_sets_buffer;
    std::vector<vk::WriteDescriptorSet> descr_sets_write;
    descr_sets_buffer.reserve(nodes.size() * 4);
    descr_sets_write.reserve(nodes.size() * 2);
    for (uint32_t node_index = 0; node_index < nodes.size(); node_index++)
    {
        debug_name(descr_sets[node_index], fmt::format("GEO DescriptorSet node#{}", node_index));

        // model, view, proj
        vk::DeviceSize buffer_offset = node_index * sizeof(uniform_buffers_t);
        descr_sets_buffer.emplace_back(*uniform_buffer, 
            buffer_offset + offsetof(uniform_buffers_t, model), uniform_buffers_t::mvp_size);
        descr_sets_write.emplace_back(*descr_sets[node_index], 0, 0, 1,
            vk::DescriptorType::eUniformBuffer, nullptr, &descr_sets_buffer.back(), nullptr);
        // col
        descr_sets_buffer.emplace_back(*uniform_buffer, 
            buffer_offset + offsetof(uniform_buffers_t, col), sizeof(uniform_buffers_t::col));
        descr_sets_write.emplace_back(*descr_sets[node_index], 1, 0, 1, 
            vk::DescriptorType::eUniformBuffer, nullptr, &descr_sets_buffer.back(), nullptr);
    }
    device->updateDescriptorSets(descr_sets_write, nullptr);

    // Create DescriptorSets Composite

    vk::BufferCreateInfo uniform_comp_buffer_info;
    uniform_comp_buffer_info.size = sizeof(uniform_buffers_comp_t);
    uniform_comp_buffer_info.usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::UniqueBuffer uniform_buffer_comp = device->createBufferUnique(uniform_comp_buffer_info);
    debug_name(uniform_buffer_comp, "COMP Uniform Buffer");
    vk::MemoryRequirements uniform_comp_mem_req = device->getBufferMemoryRequirements(*uniform_buffer_comp);
    uint32_t uniform_comp_mem_idx = find_memory(uniform_comp_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::UniqueDeviceMemory uniform_comp_mem = device->allocateMemoryUnique({ uniform_comp_mem_req.size, uniform_comp_mem_idx });
    debug_name(uniform_comp_mem, "COMP Uniform Buffer Memory");
    device->bindBufferMemory(*uniform_buffer_comp, *uniform_comp_mem, 0);

    vk::UniqueDescriptorSet descr_sets_comp = std::move(
        device->allocateDescriptorSetsUnique({ *descrpool, 1, &descrset_comp_layout.get() })[0]);
    debug_name(descr_sets_comp, "COMP DescriptorSet");

    vk::DescriptorBufferInfo descr_sets_comp_buffer(*uniform_buffer_comp, 0, sizeof(uniform_buffers_comp_t));
    vk::DescriptorImageInfo descr_sets_comp_depth(nullptr, *depth_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::DescriptorImageInfo descr_sets_comp_pos(nullptr, *gbuffer_pos_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::DescriptorImageInfo descr_sets_comp_nor(nullptr, *gbuffer_nor_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::DescriptorImageInfo descr_sets_comp_alb(nullptr, *gbuffer_alb_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    std::array<vk::WriteDescriptorSet, 5> descr_sets_comp_write{
        vk::WriteDescriptorSet(*descr_sets_comp, 0, 0, 1,
            vk::DescriptorType::eUniformBuffer, nullptr, &descr_sets_comp_buffer, nullptr),
        vk::WriteDescriptorSet(*descr_sets_comp, 1, 0, 1,
            vk::DescriptorType::eInputAttachment, &descr_sets_comp_depth, nullptr, nullptr),
        vk::WriteDescriptorSet(*descr_sets_comp, 2, 0, 1,
            vk::DescriptorType::eInputAttachment, &descr_sets_comp_pos, nullptr, nullptr),
        vk::WriteDescriptorSet(*descr_sets_comp, 3, 0, 1,
            vk::DescriptorType::eInputAttachment, &descr_sets_comp_nor, nullptr, nullptr),
        vk::WriteDescriptorSet(*descr_sets_comp, 4, 0, 1,
            vk::DescriptorType::eInputAttachment, &descr_sets_comp_alb, nullptr, nullptr),
    };
    device->updateDescriptorSets(descr_sets_comp_write, nullptr);

    // Renderpass

    std::array<vk::AttachmentDescription, 5> renderpass_attachments;
    // color buffer
    renderpass_attachments[0].format = swapchain_info.imageFormat;
    renderpass_attachments[0].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[0].loadOp = vk::AttachmentLoadOp::eLoad;
    renderpass_attachments[0].storeOp = vk::AttachmentStoreOp::eStore;
    renderpass_attachments[0].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[0].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[0].initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
    renderpass_attachments[0].finalLayout = vk::ImageLayout::ePresentSrcKHR;
    // depth
    renderpass_attachments[1].format = depth_info.format;
    renderpass_attachments[1].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[1].loadOp = vk::AttachmentLoadOp::eClear;
    renderpass_attachments[1].storeOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[1].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[1].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[1].initialLayout = vk::ImageLayout::eUndefined;
    renderpass_attachments[1].finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    // gbuffer pos
    renderpass_attachments[2].format = vk::Format::eR32G32B32A32Sfloat;
    renderpass_attachments[2].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[2].loadOp = vk::AttachmentLoadOp::eClear;
    renderpass_attachments[2].storeOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[2].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[2].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[2].initialLayout = vk::ImageLayout::eUndefined;
    renderpass_attachments[2].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    // gbuffer nor
    renderpass_attachments[3].format = vk::Format::eR32G32B32A32Sfloat;
    renderpass_attachments[3].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[3].loadOp = vk::AttachmentLoadOp::eClear;
    renderpass_attachments[3].storeOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[3].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[3].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[3].initialLayout = vk::ImageLayout::eUndefined;
    renderpass_attachments[3].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;
    // gbuffer alb
    renderpass_attachments[4].format = vk::Format::eR8G8B8A8Unorm;
    renderpass_attachments[4].samples = vk::SampleCountFlagBits::e1;
    renderpass_attachments[4].loadOp = vk::AttachmentLoadOp::eClear;
    renderpass_attachments[4].storeOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[4].stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    renderpass_attachments[4].stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    renderpass_attachments[4].initialLayout = vk::ImageLayout::eUndefined;
    renderpass_attachments[4].finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

    std::array<vk::SubpassDescription, 2> renderpass_subpasses;
    
    std::array<vk::AttachmentReference, 3> renderpass_references_first;
    renderpass_references_first[0].attachment = 2; // pos
    renderpass_references_first[0].layout = vk::ImageLayout::eColorAttachmentOptimal;
    renderpass_references_first[1].attachment = 3; // nor
    renderpass_references_first[1].layout = vk::ImageLayout::eColorAttachmentOptimal;
    renderpass_references_first[2].attachment = 4; // alb
    renderpass_references_first[2].layout = vk::ImageLayout::eColorAttachmentOptimal;
    vk::AttachmentReference renderpass_references_first_depth;
    renderpass_references_first_depth.attachment = 1; // depth
    renderpass_references_first_depth.layout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    renderpass_subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    renderpass_subpasses[0].colorAttachmentCount = (uint32_t)renderpass_references_first.size();
    renderpass_subpasses[0].pColorAttachments = renderpass_references_first.data();
    renderpass_subpasses[0].pDepthStencilAttachment = &renderpass_references_first_depth;

    std::array<vk::AttachmentReference, 1> renderpass_references_second;
    renderpass_references_second[0].attachment = 0;
    renderpass_references_second[0].layout = vk::ImageLayout::eColorAttachmentOptimal;
    std::array<vk::AttachmentReference, 4> renderpass_references_second_input;
    renderpass_references_second_input[0].attachment = 1; // depth
    renderpass_references_second_input[0].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    renderpass_references_second_input[1].attachment = 2; // pos
    renderpass_references_second_input[1].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    renderpass_references_second_input[2].attachment = 3; // nor
    renderpass_references_second_input[2].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    renderpass_references_second_input[3].attachment = 4; // alb
    renderpass_references_second_input[3].layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    renderpass_subpasses[1].pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    renderpass_subpasses[1].colorAttachmentCount = (uint32_t)renderpass_references_second.size();
    renderpass_subpasses[1].pColorAttachments = renderpass_references_second.data();
    renderpass_subpasses[1].inputAttachmentCount = (uint32_t)renderpass_references_second_input.size();
    renderpass_subpasses[1].pInputAttachments = renderpass_references_second_input.data();

    std::array<vk::SubpassDependency, 2> renderpass_deps;
    // gbuffer
    renderpass_deps[0].srcSubpass = 0;
    renderpass_deps[0].dstSubpass = 1;
    renderpass_deps[0].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    renderpass_deps[0].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    renderpass_deps[0].srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    renderpass_deps[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    renderpass_deps[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;
    // depth
    renderpass_deps[1].srcSubpass = 0;
    renderpass_deps[1].dstSubpass = 1;
    renderpass_deps[1].srcStageMask = vk::PipelineStageFlagBits::eLateFragmentTests;
    renderpass_deps[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
    renderpass_deps[1].srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    renderpass_deps[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    renderpass_deps[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    vk::RenderPassCreateInfo renderpass_info;
    renderpass_info.attachmentCount = (uint32_t)renderpass_attachments.size();
    renderpass_info.pAttachments = renderpass_attachments.data();
    renderpass_info.subpassCount = (uint32_t)renderpass_subpasses.size();
    renderpass_info.pSubpasses = renderpass_subpasses.data();
    renderpass_info.dependencyCount = (uint32_t)renderpass_deps.size();
    renderpass_info.pDependencies = renderpass_deps.data();
    vk::UniqueRenderPass renderpass = device->createRenderPassUnique(renderpass_info);
    debug_name(renderpass, "GEO RenderPass");

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
    std::array<vk::VertexInputAttributeDescription, 2> pipeline_input_attributes{
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32Sfloat, offsetof(vertex_t, pos)),
        vk::VertexInputAttributeDescription(1, 0, vk::Format::eR32G32B32Sfloat, offsetof(vertex_t, nor)),
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
    pipeline_raster.cullMode = vk::CullModeFlagBits::eBack;
    pipeline_raster.frontFace = vk::FrontFace::eClockwise;
    pipeline_raster.depthBiasEnable = false;
    pipeline_raster.lineWidth = 1.f;

    vk::PipelineMultisampleStateCreateInfo pipeline_multisample;
    pipeline_multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    pipeline_multisample.sampleShadingEnable = false;

    vk::PipelineDepthStencilStateCreateInfo pipeline_depth;
    pipeline_depth.depthTestEnable = true;
    pipeline_depth.depthWriteEnable = true;
    pipeline_depth.depthCompareOp = vk::CompareOp::eLess;
    pipeline_depth.depthBoundsTestEnable = false;
    pipeline_depth.stencilTestEnable = false;

    vk::ColorComponentFlags gbuffer_mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    std::array<vk::PipelineColorBlendAttachmentState, 3> pipeline_blend_attachments;
    pipeline_blend_attachments[0].blendEnable = false;
    pipeline_blend_attachments[0].colorWriteMask = gbuffer_mask;
    pipeline_blend_attachments[1].blendEnable = false;
    pipeline_blend_attachments[1].colorWriteMask = gbuffer_mask;
    pipeline_blend_attachments[2].blendEnable = false;
    pipeline_blend_attachments[2].colorWriteMask = gbuffer_mask;
    vk::PipelineColorBlendStateCreateInfo pipeline_blend;
    pipeline_blend.logicOpEnable = false;
    pipeline_blend.attachmentCount = (uint32_t)pipeline_blend_attachments.size();
    pipeline_blend.pAttachments = pipeline_blend_attachments.data();

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
    pipeline_info.pDepthStencilState = &pipeline_depth;
    pipeline_info.pColorBlendState = &pipeline_blend;
    pipeline_info.pDynamicState = &pipeline_dynamic;
    pipeline_info.layout = *pipeline_layout;
    pipeline_info.renderPass = *renderpass;
    pipeline_info.subpass = 0;

    vk::UniquePipeline pipeline = device->createGraphicsPipelineUnique(nullptr, pipeline_info);
    debug_name(pipeline, "GEO Pipeline");

    // Create Composite Pipeline

    vk::UniqueShaderModule module_composite_vert = load_shader_module("shaders/composite.vert.spv");
    vk::UniqueShaderModule module_composite_frag = load_shader_module("shaders/composite.frag.spv");
    std::array<vk::PipelineShaderStageCreateInfo, 2> pipeline_stages_composite {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, * module_composite_vert, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, * module_composite_frag, "main"),
    };

    vk::PipelineVertexInputStateCreateInfo pipeline_input_comp;

    vk::PipelineColorBlendAttachmentState pipeline_blend_color;
    pipeline_blend_color.blendEnable = false;
    pipeline_blend_color.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo pipeline_blend_comp;
    pipeline_blend_comp.logicOpEnable = false;
    pipeline_blend_comp.attachmentCount = 1;
    pipeline_blend_comp.pAttachments = &pipeline_blend_color;

    pipeline_info.stageCount = (uint32_t)pipeline_stages_composite.size();
    pipeline_info.pStages = pipeline_stages_composite.data();
    pipeline_info.pVertexInputState = &pipeline_input_comp;
    pipeline_info.pDepthStencilState = nullptr;
    pipeline_info.pColorBlendState = &pipeline_blend_comp;
    pipeline_info.layout = *pipeline_comp_layout;
    pipeline_info.subpass = 1;

    vk::UniquePipeline pipeline_comp = device->createGraphicsPipelineUnique(nullptr, pipeline_info);
    debug_name(pipeline_comp, "COMP Pipeline");

    // Create clear screen command

    vk::CommandBufferAllocateInfo cmd_clear_info;
    cmd_clear_info.commandPool = *cmdpool;
    cmd_clear_info.level = vk::CommandBufferLevel::ePrimary;
    cmd_clear_info.commandBufferCount = (uint32_t)swapchain_images.size();
    std::vector<vk::UniqueCommandBuffer> cmd_draw = device->allocateCommandBuffersUnique(cmd_clear_info);
    std::vector<vk::UniqueFramebuffer> framebuffers(swapchain_images.size());
    std::vector<vk::UniqueImageView> swapchain_views(swapchain_images.size());
    std::vector<vk::CommandBuffer> submit_commands(swapchain_images.size());
    for (int i = 0; i < swapchain_images.size(); i++)
    {
        debug_name(cmd_draw[i], fmt::format("Draw Command#{}", i));
        cmd_draw[i]->begin({ vk::CommandBufferUsageFlags() });
        {
            vk::ImageMemoryBarrier barrier;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            barrier.image = swapchain_images[i];
            barrier.srcAccessMask = {};
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            cmd_draw[i]->pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);

            barrier.image = *rt_output;
            barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            barrier.oldLayout = vk::ImageLayout::eGeneral;
            barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            cmd_draw[i]->pipelineBarrier(vk::PipelineStageFlagBits::eRayTracingShaderKHR, vk::PipelineStageFlagBits::eTransfer,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);

            vk::ImageBlit blit_region;
            blit_region.srcSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            blit_region.srcOffsets[0] = vk::Offset3D(0, 0, 0);
            blit_region.srcOffsets[1] = vk::Offset3D(image_width, image_height, 1);
            blit_region.dstSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            blit_region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            blit_region.dstOffsets[1] = vk::Offset3D(surface_caps.currentExtent.width, surface_caps.currentExtent.height, 1);
            cmd_draw[i]->blitImage(*rt_output, vk::ImageLayout::eTransferSrcOptimal,
                swapchain_images[i], vk::ImageLayout::eTransferDstOptimal, blit_region, vk::Filter::eLinear);

            barrier.image = *rt_output;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eGeneral;
            cmd_draw[i]->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eRayTracingShaderKHR,
                vk::DependencyFlagBits::eByRegion, nullptr, nullptr, barrier);

            barrier.image = swapchain_images[i];
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
            cmd_draw[i]->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
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
            std::array<vk::ImageView, 5> framebuffer_attachments{ 
                *swapchain_views[i], 
                *depth_view, 
                *gbuffer_pos_view,
                *gbuffer_nor_view,
                *gbuffer_alb_view,
            };
            vk::FramebufferCreateInfo framebuffer_info;
            framebuffer_info.renderPass = *renderpass;
            framebuffer_info.attachmentCount = (uint32_t)framebuffer_attachments.size();
            framebuffer_info.pAttachments = framebuffer_attachments.data();
            framebuffer_info.width = surface_caps.currentExtent.width;
            framebuffer_info.height = surface_caps.currentExtent.height;
            framebuffer_info.layers = 1;
            framebuffers[i] = device->createFramebufferUnique(framebuffer_info);
            debug_name(framebuffers[i], fmt::format("Framebuffer#{}", i));

            std::array<vk::ClearValue, 5> clear_values{
                vk::ClearValue(),                                         // color (not cleared)
                vk::ClearDepthStencilValue(1.f, 0),                       // depth
                vk::ClearColorValue({ std::array<float,4>{0, 0, 0, 1} }), // pos
                vk::ClearColorValue({ std::array<float,4>{0, 0, 0, 1} }), // nor
                vk::ClearColorValue({ std::array<float,4>{0, 0, 0, 1} }), // alb
            };
            vk::RenderPassBeginInfo renderpass_begin_info;
            renderpass_begin_info.renderPass = *renderpass;
            renderpass_begin_info.framebuffer = *framebuffers[i];
            renderpass_begin_info.renderArea.extent = surface_caps.currentExtent;
            renderpass_begin_info.renderArea.offset = vk::Offset2D{};
            renderpass_begin_info.clearValueCount = (uint32_t)clear_values.size();
            renderpass_begin_info.pClearValues = clear_values.data();
            /*
            cmd_draw[i]->beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
            {
                cmd_draw[i]->bindVertexBuffers(0, *triangle_buffer, { 0 });
                cmd_draw[i]->bindIndexBuffer(*triangle_buffer_idx, 0, vk::IndexType::eUint32);
                cmd_draw[i]->bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
                for (uint32_t node_index = 0; node_index < nodes.size(); node_index++)
                {
                    cmd_draw[i]->bindDescriptorSets(vk::PipelineBindPoint::eGraphics, 
                        *pipeline_layout, 0, descr_sets[node_index].get(), nullptr);
                    for (uint32_t mesh_index : nodes[node_index].mesh_indices)
                    {
                        cmd_draw[i]->drawIndexed(meshes[mesh_index].idx_count, 1, 
                            meshes[mesh_index].idx_offset, meshes[mesh_index].vtx_offset, 0);
                    }
                }
            }
            cmd_draw[i]->nextSubpass(vk::SubpassContents::eInline);
            {
                cmd_draw[i]->bindDescriptorSets(vk::PipelineBindPoint::eGraphics, 
                    *pipeline_comp_layout, 0, descr_sets_comp.get(), nullptr);
                cmd_draw[i]->bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_comp);
                cmd_draw[i]->draw(6, 1, 0, 0);
            }
            cmd_draw[i]->endRenderPass();
            */
            //vk::ClearColorValue clear_color = std::array<float, 4>({ 1, 0, 1, 1 });
            //cmd_clear[i]->clearColorImage(swapchain_images[i], vk::ImageLayout::eTransferDstOptimal, 
            //    clear_color, barrier.subresourceRange);
        }
        cmd_draw[i]->end();
        submit_commands[i] = *cmd_draw[i];
    }

    MSG msg;
    while (running)
    {
        if (PeekMessage(&msg, hWnd, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!running)
            break;

        vk::UniqueSemaphore backbuffer_semaphore = device->createSemaphoreUnique({});
        debug_name(backbuffer_semaphore, "backbuffer_semaphore");
        auto backbuffer = device->acquireNextImageKHR(*swapchain, UINT64_MAX, *backbuffer_semaphore, nullptr);
        if (backbuffer.result == vk::Result::eSuccess)
        {
            static float angle = 0.f;
            angle += glm::radians(1.f);
            glm::vec3 cam_pos = glm::vec3(glm::cos(angle * 0.1f), 0.5f, glm::sin(angle * 0.1f)) * 3.f;
            glm::vec3 light_pos = glm::vec3(glm::cos(angle), 0.3f, glm::sin(angle)) * 5.f;
            float aspect = (float)surface_caps.currentExtent.width / (float)surface_caps.currentExtent.height;
            if (auto ptr = reinterpret_cast<uniform_buffers_t*>(device->mapMemory(*uniform_mem, 0, VK_WHOLE_SIZE)))
            {
                for (const auto& n : nodes)
                {
                    ptr->proj = glm::perspective(glm::radians(85.f), aspect, .1f, 100.f);
                    ptr->view = glm::lookAt(cam_pos, glm::vec3(0, 0, 0), glm::vec3(0, -1, 0));
                    ptr->model = n.mat;
                    ptr->col = glm::vec4(n.col, 1.f);
                    ptr++;
                }
                device->unmapMemory(*uniform_mem);
            }
            if (auto ptr = reinterpret_cast<uniform_buffers_comp_t*>(device->mapMemory(*uniform_comp_mem, 0, VK_WHOLE_SIZE)))
            {
                ptr->camera = { cam_pos, 1.f };
                ptr->light_pos = { light_pos, 1.f };
                device->unmapMemory(*uniform_comp_mem);
            }
            if (auto ptr = reinterpret_cast<uniform_rt_buffers_t*>(device->mapMemory(*uniform_rt_mem, 0, VK_WHOLE_SIZE)))
            {
                ptr->proj_inverse = glm::inverse(glm::perspective(glm::radians(85.f), aspect, .1f, 100.f));
                ptr->view_inverse = glm::inverse(glm::lookAt(cam_pos, glm::vec3(0, 0, 0), glm::vec3(0, -1, 0)));
                ptr->color = glm::vec4(glm::sin(angle * 5.f), 0, 0, 1);
                ptr->light_pos = glm::vec4(light_pos, 1.f);
                device->unmapMemory(*uniform_rt_mem);
            }

            vk::SubmitInfo cmd_trace_submit;
            cmd_trace_submit.commandBufferCount = 1;
            cmd_trace_submit.pCommandBuffers = &cmd_trace.get();
            q.submit(cmd_trace_submit, nullptr);
            
            vk::UniqueSemaphore render_sem = device->createSemaphoreUnique({});
            debug_name(render_sem, "render_sem");
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

    debug_messenger.reset();
    exit(EXIT_SUCCESS);
}

int main()
{
    try
    {
        main_run();
    }
    catch (vk::DeviceLostError* e)
    {
//         device.reset();
//         instance.reset();
        std::cout << "DEVICE LOST: " << e->what() << std::endl;
        abort();
    }
}
