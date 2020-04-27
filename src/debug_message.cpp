#include "pch.h"
#include <sstream>

VkBool32 debugMessageFunc(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData, void* /*pUserData*/)
{
    std::ostringstream message;

    message << vk::to_string(static_cast<vk::DebugUtilsMessageSeverityFlagBitsEXT>(messageSeverity)) << 
        ": " << vk::to_string(static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(messageTypes)) << ":\n";
    if (pCallbackData->pMessageIdName)
        message << "\t" << "messageIDName   = <" << pCallbackData->pMessageIdName << ">\n";
    message << "\t" << "messageIdNumber = " << pCallbackData->messageIdNumber << "\n";
    if (pCallbackData->pMessage)
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

vk::UniqueDebugUtilsMessengerEXT init_debug_message(const vk::UniqueInstance& inst)
{
    vk::DebugUtilsMessageSeverityFlagsEXT severityFlags(
        //vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning
        | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError
    );
    vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags(
        vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
        | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance
        | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
    );
    auto debug_info = vk::DebugUtilsMessengerCreateInfoEXT({}, severityFlags, messageTypeFlags, &debugMessageFunc);
    return inst->createDebugUtilsMessengerEXTUnique(debug_info);
}
