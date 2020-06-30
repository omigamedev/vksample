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
                vk::StructureChain device_info {
                    vk::DeviceCreateInfo()
                        .setQueueCreateInfoCount(1)
                        .setPQueueCreateInfos(&queue_info)
                        .setEnabledLayerCount((uint32_t)device_layers.size())
                        .setPpEnabledLayerNames(device_layers.data())
                        .setEnabledExtensionCount((uint32_t)device_extensions.size())
                        .setPpEnabledExtensionNames(device_extensions.data()),
                    vk::StructureChain{
                        vk::PhysicalDeviceFeatures2(),
                        vk::PhysicalDeviceVulkan12Features()
                            .setBufferDeviceAddress(true),
                        vk::PhysicalDeviceRayTracingFeaturesKHR()
                            .setRayTracing(true),
                    }.get<vk::PhysicalDeviceFeatures2>(),
                };
                device = pd.createDeviceUnique(device_info.get<vk::DeviceCreateInfo>());
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
    std::array<const char*, 5> instance_extensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
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
    vk::SurfaceKHR surface1 = instance->createWin32SurfaceKHR(surface_info);

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

    // Create merged vertex buffers for all scene
    vk::BufferCreateInfo triangle_buffer_info;
    triangle_buffer_info.size = mesh_data_vert.size() * sizeof(vertex_t);
    triangle_buffer_info.usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    vk::UniqueBuffer triangle_buffer = device->createBufferUnique(triangle_buffer_info);
    vk::MemoryRequirements triangle_buffer_mem_req = device->getBufferMemoryRequirements(*triangle_buffer);
    uint32_t triangle_buffer_mem_idx = find_memory(triangle_buffer_mem_req,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    // Use chained properties to request eDeviceAddress flags
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

    // Raytracing properties from getProperties2 extension
    auto rt_props = physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPropertiesKHR>()
        .get<vk::PhysicalDeviceRayTracingPropertiesKHR>();
    
    // BLAS
    // Build bottom level acceleration structure
    // see: https://developer.nvidia.com/blog/vulkan-raytracing/
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

        vk::MemoryBarrier barrier(vk::AccessFlagBits::eAccelerationStructureWriteKHR | vk::AccessFlagBits::eAccelerationStructureReadKHR,
            vk::AccessFlagBits::eAccelerationStructureReadKHR | vk::AccessFlagBits::eAccelerationStructureReadKHR);
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
    const super_sample = 1;
    glm::ivec2 output_size = glm::ivec2(surface_caps.currentExtent.width, surface_caps.currentExtent.height) * super_sample;
    auto [rt_output, rt_output_mem, rt_output_view] =
        create_gbuffer("GBufferPOS", vk::Extent2D(output_size.x, output_size.y), vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eStorage);

    // Update DescriptorSets
    vk::DescriptorImageInfo rt_descr_set_image(nullptr, *rt_output_view, vk::ImageLayout::eGeneral);
    vk::DescriptorBufferInfo rt_descr_set_ubo_rgen(*uniform_rt_buffer, 0, uniform_rt_buffers_t::rgen_size);
    vk::DescriptorBufferInfo rt_descr_set_ubo_rhit(*uniform_rt_buffer, uniform_rt_buffers_t::rhit_offset, uniform_rt_buffers_t::rhit_size);
    vk::DescriptorBufferInfo rt_descr_set_idx(*triangle_buffer_idx, 0, VK_WHOLE_SIZE);
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
        output_size.x, output_size.y, 1);
    cmd_trace->end();

    // Create clear screen command

    vk::CommandBufferAllocateInfo cmd_clear_info;
    cmd_clear_info.commandPool = *cmdpool;
    cmd_clear_info.level = vk::CommandBufferLevel::ePrimary;
    cmd_clear_info.commandBufferCount = (uint32_t)swapchain_images.size();
    std::vector<vk::UniqueCommandBuffer> cmd_draw = device->allocateCommandBuffersUnique(cmd_clear_info);
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
            blit_region.srcOffsets[1] = vk::Offset3D(output_size.x, output_size.y, 1);
            blit_region.dstSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            blit_region.dstOffsets[0] = vk::Offset3D(0, 0, 0);
            blit_region.dstOffsets[1] = vk::Offset3D(surface_caps.currentExtent.width, surface_caps.currentExtent.height, 1);
            cmd_draw[i]->blitImage(*rt_output, vk::ImageLayout::eTransferSrcOptimal,
                swapchain_images[i], vk::ImageLayout::eTransferDstOptimal, blit_region, vk::Filter::eLinear);

            barrier.image = *rt_output;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
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
            float aspect = (float)output_size.x / (float)output_size.y;
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
