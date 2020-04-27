#pragma once
#include <vulkan/vulkan_core.h>

vk::UniqueDebugUtilsMessengerEXT init_debug_message(const vk::UniqueInstance& inst);

constexpr vk::DebugReportObjectTypeEXT type2report(vk::ObjectType type)
{
    switch (type)
    {
    case vk::ObjectType::eUnknown:
        return vk::DebugReportObjectTypeEXT::eUnknown;
    case vk::ObjectType::eInstance:
        return vk::DebugReportObjectTypeEXT::eInstance;
    case vk::ObjectType::ePhysicalDevice:
        return vk::DebugReportObjectTypeEXT::ePhysicalDevice;
    case vk::ObjectType::eDevice:
        return vk::DebugReportObjectTypeEXT::eDevice;
    case vk::ObjectType::eQueue:
        return vk::DebugReportObjectTypeEXT::eQueue;
    case vk::ObjectType::eSemaphore:
        return vk::DebugReportObjectTypeEXT::eSemaphore;
    case vk::ObjectType::eCommandBuffer:
        return vk::DebugReportObjectTypeEXT::eCommandBuffer;
    case vk::ObjectType::eFence:
        return vk::DebugReportObjectTypeEXT::eFence;
    case vk::ObjectType::eDeviceMemory:
        return vk::DebugReportObjectTypeEXT::eDeviceMemory;
    case vk::ObjectType::eBuffer:
        return vk::DebugReportObjectTypeEXT::eBuffer;
    case vk::ObjectType::eImage:
        return vk::DebugReportObjectTypeEXT::eImage;
    case vk::ObjectType::eEvent:
        return vk::DebugReportObjectTypeEXT::eEvent;
    case vk::ObjectType::eQueryPool:
        return vk::DebugReportObjectTypeEXT::eQueryPool;
    case vk::ObjectType::eBufferView:
        return vk::DebugReportObjectTypeEXT::eBufferView;
    case vk::ObjectType::eImageView:
        return vk::DebugReportObjectTypeEXT::eImageView;
    case vk::ObjectType::eShaderModule:
        return vk::DebugReportObjectTypeEXT::eShaderModule;
    case vk::ObjectType::ePipelineCache:
        return vk::DebugReportObjectTypeEXT::ePipelineCache;
    case vk::ObjectType::ePipelineLayout:
        return vk::DebugReportObjectTypeEXT::ePipelineLayout;
    case vk::ObjectType::eRenderPass:
        return vk::DebugReportObjectTypeEXT::eRenderPass;
    case vk::ObjectType::ePipeline:
        return vk::DebugReportObjectTypeEXT::ePipeline;
    case vk::ObjectType::eDescriptorSetLayout:
        return vk::DebugReportObjectTypeEXT::eDescriptorSetLayout;
    case vk::ObjectType::eSampler:
        return vk::DebugReportObjectTypeEXT::eSampler;
    case vk::ObjectType::eDescriptorPool:
        return vk::DebugReportObjectTypeEXT::eDescriptorPool;
    case vk::ObjectType::eDescriptorSet:
        return vk::DebugReportObjectTypeEXT::eDescriptorSet;
    case vk::ObjectType::eFramebuffer:
        return vk::DebugReportObjectTypeEXT::eFramebuffer;
    case vk::ObjectType::eCommandPool:
        return vk::DebugReportObjectTypeEXT::eCommandPool;
    //case vk::ObjectType::eSamplerYcbcrConversionKHR:
    case vk::ObjectType::eSamplerYcbcrConversion:
        return vk::DebugReportObjectTypeEXT::eSamplerYcbcrConversion;
    //case vk::ObjectType::eDescriptorUpdateTemplateKHR:
    case vk::ObjectType::eDescriptorUpdateTemplate:
        return vk::DebugReportObjectTypeEXT::eDescriptorUpdateTemplate;
    case vk::ObjectType::eSurfaceKHR:
        return vk::DebugReportObjectTypeEXT::eSurfaceKHR;
    case vk::ObjectType::eSwapchainKHR:
        return vk::DebugReportObjectTypeEXT::eSwapchainKHR;
    case vk::ObjectType::eDisplayKHR:
        return vk::DebugReportObjectTypeEXT::eDisplayKHR;
    case vk::ObjectType::eDisplayModeKHR:
        return vk::DebugReportObjectTypeEXT::eDisplayModeKHR;
    case vk::ObjectType::eDebugReportCallbackEXT:
        return vk::DebugReportObjectTypeEXT::eDebugReportCallbackEXT;
    //case vk::ObjectType::eObjectTableNVX:
    //    return vk::DebugReportObjectTypeEXT::eObjectTableNVX;
    //case vk::ObjectType::eIndirectCommandsLayoutNVX:
    //    return vk::DebugReportObjectTypeEXT::eIndirectCommandsLayoutNVX;
    case vk::ObjectType::eDebugUtilsMessengerEXT:
        break;
    case vk::ObjectType::eValidationCacheEXT:
        return vk::DebugReportObjectTypeEXT::eValidationCacheEXT;
    case vk::ObjectType::eAccelerationStructureNV:
        return vk::DebugReportObjectTypeEXT::eAccelerationStructureNV;
    case vk::ObjectType::ePerformanceConfigurationINTEL:
        break;
    default:
        break;
    }
    throw std::runtime_error("type2report failed conversion");
}

template<typename T>
void debug_name(const vk::UniqueDevice& dev, T obj, const std::string& name)
{
    using _VkType = typename T::CType;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkDebugMarkerSetObjectNameEXT)
    {
        vk::DebugMarkerObjectNameInfoEXT dbg_info;
        dbg_info.object = (uint64_t)((_VkType)obj);
        dbg_info.objectType = type2report(obj.objectType);
        dbg_info.pObjectName = name.c_str();
        dev->debugMarkerSetObjectNameEXT(dbg_info);
    }
    else if (VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT)
    {
        vk::DebugUtilsObjectNameInfoEXT dbg_info;
        dbg_info.objectHandle = (uint64_t)((_VkType)obj);
        dbg_info.objectType   = obj.objectType;
        dbg_info.pObjectName  = name.c_str();
        dev->setDebugUtilsObjectNameEXT(dbg_info);
    }
}

template<class T>
void debug_name(const vk::UniqueHandle<T, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>& obj, const std::string& name)
{
    using _VkType = typename T::CType;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkDebugMarkerSetObjectNameEXT)
    {
        VkDebugMarkerObjectNameInfoEXT dbg_info{ VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT };
        dbg_info.object = (uint64_t)((_VkType)*obj);
        dbg_info.objectType = (VkDebugReportObjectTypeEXT)type2report(obj->objectType);
        dbg_info.pObjectName = name.c_str();
        VULKAN_HPP_DEFAULT_DISPATCHER.vkDebugMarkerSetObjectNameEXT(obj.getOwner(), &dbg_info);
    }
    else if (VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT)
    {
        VkDebugUtilsObjectNameInfoEXT dbg_info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        dbg_info.objectHandle = (uint64_t)((_VkType)*obj);
        dbg_info.objectType = (VkObjectType)obj->objectType;
        dbg_info.pObjectName = name.c_str();
        VULKAN_HPP_DEFAULT_DISPATCHER.vkSetDebugUtilsObjectNameEXT(obj.getOwner(), &dbg_info);
    }
}

void debug_mark_begin(const vk::UniqueCommandBuffer& cmd, const std::string& name)
{
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDebugMarkerBeginEXT)
        cmd->debugMarkerBeginEXT({ name.c_str() });
    else if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginDebugUtilsLabelEXT)
        cmd->beginDebugUtilsLabelEXT({ name.c_str() });
}

void debug_mark_end(const vk::UniqueCommandBuffer& cmd)
{
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDebugMarkerEndEXT)
        cmd->debugMarkerEndEXT();
    else if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdEndDebugUtilsLabelEXT)
        cmd->endDebugUtilsLabelEXT();
}

void debug_mark_insert(const vk::UniqueCommandBuffer& cmd, const std::string& name)
{
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDebugMarkerInsertEXT)
        cmd->debugMarkerInsertEXT({ name.c_str() });
    else if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdInsertDebugUtilsLabelEXT)
        cmd->insertDebugUtilsLabelEXT({ name.c_str() });
}
