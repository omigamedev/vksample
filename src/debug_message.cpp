#include "pch.h"
#include <sstream>

PFN_vkDebugMarkerSetObjectNameEXT pfnvkDebugMarkerSetObjectNameEXT = nullptr;
PFN_vkCmdDebugMarkerBeginEXT pfnvkCmdDebugMarkerBeginEXT = nullptr;
PFN_vkCmdDebugMarkerInsertEXT pfnvkCmdDebugMarkerInsertEXT = nullptr;
PFN_vkCmdDebugMarkerEndEXT pfnvkCmdDebugMarkerEndEXT = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL vkDebugMarkerSetObjectNameEXT(VkDevice device, const VkDebugMarkerObjectNameInfoEXT* pNameInfo)
{
    return pfnvkDebugMarkerSetObjectNameEXT ? pfnvkDebugMarkerSetObjectNameEXT(device, pNameInfo) : VkResult::VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerBeginEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo)
{
    if (pfnvkCmdDebugMarkerBeginEXT)
        pfnvkCmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerInsertEXT(VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo)
{
    if (pfnvkCmdDebugMarkerInsertEXT)
        pfnvkCmdDebugMarkerInsertEXT(commandBuffer, pMarkerInfo);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDebugMarkerEndEXT(VkCommandBuffer commandBuffer)
{
    if (pfnvkCmdDebugMarkerEndEXT)
        pfnvkCmdDebugMarkerEndEXT(commandBuffer);
}

PFN_vkCreateDebugUtilsMessengerEXT pfnVkCreateDebugUtilsMessengerEXT;
PFN_vkDestroyDebugUtilsMessengerEXT pfnVkDestroyDebugUtilsMessengerEXT;

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pMessenger)
{
    return pfnVkCreateDebugUtilsMessengerEXT(instance, pCreateInfo, pAllocator, pMessenger);
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger, VkAllocationCallbacks const* pAllocator)
{
    return pfnVkDestroyDebugUtilsMessengerEXT(instance, messenger, pAllocator);
}


VkBool32 debugMessageFunc(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData, void* /*pUserData*/)
{
    std::ostringstream message;

    message << vk::to_string(static_cast<vk::DebugUtilsMessageSeverityFlagBitsEXT>(messageSeverity)) << 
        ": " << vk::to_string(static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(messageTypes)) << ":\n";
    message << "\t" << "messageIDName   = <" << pCallbackData->pMessageIdName << ">\n";
    message << "\t" << "messageIdNumber = " << pCallbackData->messageIdNumber << "\n";
    message << "\t" << "message         = <" << pCallbackData->pMessage << ">\n";
    if (0 < pCallbackData->queueLabelCount)
    {
        message << "\t" << "Queue Labels:\n";
        for (uint8_t i = 0; i < pCallbackData->queueLabelCount; i++)
        {
            message << "\t\t" << "labelName = <" << pCallbackData->pQueueLabels[i].pLabelName << ">\n";
        }
    }
    if (0 < pCallbackData->cmdBufLabelCount)
    {
        message << "\t" << "CommandBuffer Labels:\n";
        for (uint8_t i = 0; i < pCallbackData->cmdBufLabelCount; i++)
        {
            message << "\t\t" << "labelName = <" << pCallbackData->pCmdBufLabels[i].pLabelName << ">\n";
        }
    }
    if (0 < pCallbackData->objectCount)
    {
        message << "\t" << "Objects:\n";
        for (uint8_t i = 0; i < pCallbackData->objectCount; i++)
        {
            message << "\t\t" << "Object " << i << "\n";
            message << "\t\t\t" << "objectType   = " << 
                vk::to_string(static_cast<vk::ObjectType>(pCallbackData->pObjects[i].objectType)) << "\n";
            message << "\t\t\t" << "objectHandle = " << pCallbackData->pObjects[i].objectHandle << "\n";
            if (pCallbackData->pObjects[i].pObjectName)
            {
                message << "\t\t\t" << "objectName   = <" << pCallbackData->pObjects[i].pObjectName << ">\n";
            }
        }
    }

    // #ifdef _WIN32
    //     MessageBoxA(NULL, message.str().c_str(), "Alert", MB_OK);
    // #else
    std::cout << message.str() << std::endl;
    // #endif

    return false;
}

void init_debug_message(const vk::UniqueInstance& inst)
{
    pfnvkDebugMarkerSetObjectNameEXT = reinterpret_cast<PFN_vkDebugMarkerSetObjectNameEXT>(inst->getProcAddr("vkDebugMarkerSetObjectNameEXT"));
    pfnvkCmdDebugMarkerBeginEXT = reinterpret_cast<PFN_vkCmdDebugMarkerBeginEXT>(inst->getProcAddr("vkCmdDebugMarkerBeginEXT"));
    pfnvkCmdDebugMarkerInsertEXT = reinterpret_cast<PFN_vkCmdDebugMarkerInsertEXT>(inst->getProcAddr("vkCmdDebugMarkerInsertEXT"));
    pfnvkCmdDebugMarkerEndEXT = reinterpret_cast<PFN_vkCmdDebugMarkerEndEXT>(inst->getProcAddr("vkCmdDebugMarkerEndEXT"));

    pfnVkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(inst->getProcAddr("vkCreateDebugUtilsMessengerEXT"));
    if (!pfnVkCreateDebugUtilsMessengerEXT)
    {
        std::cout << "GetInstanceProcAddr: Unable to find pfnVkCreateDebugUtilsMessengerEXT function." << std::endl;
        exit(1);
    }

    pfnVkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(inst->getProcAddr("vkDestroyDebugUtilsMessengerEXT"));
    if (!pfnVkDestroyDebugUtilsMessengerEXT)
    {
        std::cout << "GetInstanceProcAddr: Unable to find pfnVkDestroyDebugUtilsMessengerEXT function." << std::endl;
        exit(1);
    }

    vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo
        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError
    );
    vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
        | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
        | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
    );
    auto debug_info = vk::DebugUtilsMessengerCreateInfoEXT({}, severityFlags, messageTypeFlags, &debugMessageFunc);
    auto debug = inst->createDebugUtilsMessengerEXTUnique(debug_info);
}
