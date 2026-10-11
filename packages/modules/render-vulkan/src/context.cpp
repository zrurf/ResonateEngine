#include "context.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <vma/vk_mem_alloc.h>

namespace resonate::render_vulkan
{
namespace
{

constexpr const char* kAppName = "Resonate";
constexpr uint64_t kAcquireTimeoutNs = 1000000000ull;

/* One combined graphics+present family keeps the frame path free of queue
   ownership transfers; the RHI work gives queues their own selection rules. */
bool combinedGraphicsAndPresent(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                uint32_t* family_out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
        {
            continue;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (present)
        {
            *family_out = i;
            return true;
        }
    }
    return false;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const VkExtensionProperties& entry : list)
    {
        if (std::strcmp(entry.extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*)
{
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        return VK_FALSE;
    }
    std::fprintf(stderr, "render-vulkan: %s: %s\n",
                 severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data->pMessage);
    return VK_FALSE;
}

} // namespace

Context::Context(bool vsync) : vsync_(vsync)
{
}

Context::~Context()
{
    if (device_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device_);
        for (Frame& frame : frames_)
        {
            drainGraveyard(frame.graveyard);
        }
        for (const auto& entry : framebuffer_cache_)
        {
            vkDestroyFramebuffer(device_, entry.second, nullptr);
        }
        for (const auto& entry : pass_cache_)
        {
            vkDestroyRenderPass(device_, entry.second, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        }
        for (BufferSlot& slot : buffers_.slots_)
        {
            if (slot.alive)
            {
                vmaDestroyBuffer(allocator_, slot.buffer, slot.allocation);
            }
        }
        for (ImageSlot& slot : images_.slots_)
        {
            if (slot.alive)
            {
                vkDestroyImageView(device_, slot.view, nullptr);
                vmaDestroyImage(allocator_, slot.image, slot.allocation);
            }
        }
        for (SamplerSlot& slot : samplers_.slots_)
        {
            if (slot.alive)
            {
                vkDestroySampler(device_, slot.sampler, nullptr);
            }
        }
        for (ShaderSlot& slot : shaders_.slots_)
        {
            if (slot.alive)
            {
                vkDestroyShaderModule(device_, slot.module, nullptr);
            }
        }
        for (PipelineSlot& slot : pipelines_.slots_)
        {
            if (slot.alive)
            {
                vkDestroyPipeline(device_, slot.pipeline, nullptr);
            }
        }
        if (allocator_ != nullptr)
        {
            vmaDestroyAllocator(allocator_);
        }
    }
    destroySwapchainObjects();
    if (surface_ != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    for (Frame& frame : frames_)
    {
        if (frame.image_available != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, frame.image_available, nullptr);
        }
        if (frame.render_done != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, frame.render_done, nullptr);
        }
        if (frame.in_flight != VK_NULL_HANDLE)
        {
            vkDestroyFence(device_, frame.in_flight, nullptr);
        }
    }
    if (command_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE)
    {
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE && destroy_messenger_ != nullptr)
    {
        destroy_messenger_(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance_, nullptr);
    }
}

void Context::note(bool& once, const char* what)
{
    if (!once)
    {
        once = true;
        std::fprintf(stderr, "render-vulkan: %s\n", what);
    }
}

ResonateStatus Context::ensureInstance()
{
    if (instance_ != VK_NULL_HANDLE)
    {
        return RESONATE_OK;
    }

    const char* surface_extension = VK_KHR_SURFACE_EXTENSION_NAME;
#ifdef RESONATE_PLATFORM_WINDOWS
    surface_extension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif

    uint32_t available_layer_count = 0;
    vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(available_layer_count);
    vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data());
    bool validation = false;
#ifdef RESONATE_DEBUG
    for (const VkLayerProperties& layer : available_layers)
    {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
        {
            validation = true;
            break;
        }
    }
#endif

    uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data());
    bool debug_utils = hasExtension(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, surface_extension,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = kAppName;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = debug_utils ? 3u : 2u;
    info.ppEnabledExtensionNames = extensions;
    info.enabledLayerCount = validation ? 1u : 0u;
    info.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS)
    {
        note(surface_failed_, "instance creation failed");
        return RESONATE_E_INTERNAL;
    }

    if (debug_utils)
    {
        create_messenger_ = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        destroy_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    }

    if (validation && create_messenger_ != nullptr)
    {
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = &debugCallback;
        create_messenger_(instance_, &messenger, nullptr, &messenger_);
    }
    return RESONATE_OK;
}

ResonateStatus Context::ensureDevice()
{
    if (device_ != VK_NULL_HANDLE)
    {
        return RESONATE_OK;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int best = -1;
    uint32_t family = UINT32_MAX;
    for (VkPhysicalDevice candidate : devices)
    {
        uint32_t candidate_family = UINT32_MAX;
        if (!combinedGraphicsAndPresent(candidate, surface_, &candidate_family))
        {
            continue;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        const int score =
            properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 100;
        if (score > best)
        {
            best = score;
            physical_ = candidate;
            family = candidate_family;
        }
    }
    if (physical_ == VK_NULL_HANDLE)
    {
        note(device_failed_, "no Vulkan device with a graphics+present queue");
        return RESONATE_E_UNSUPPORTED;
    }
    queue_family_ = family;

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, extensions.data());
    if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        note(device_failed_, "device has no swapchain extension");
        return RESONATE_E_UNSUPPORTED;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS)
    {
        note(device_failed_, "device creation failed");
        return RESONATE_E_INTERNAL;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VmaAllocatorCreateInfo allocator{};
    allocator.physicalDevice = physical_;
    allocator.device = device_;
    allocator.instance = instance_;
    if (vmaCreateAllocator(&allocator, &allocator_) != VK_SUCCESS)
    {
        note(device_failed_, "allocator creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS)
    {
        note(device_failed_, "command pool creation failed");
        return RESONATE_E_INTERNAL;
    }

    for (Frame& frame : frames_)
    {
        VkCommandBufferAllocateInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool = command_pool_;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device_, &command, &frame.command) != VK_SUCCESS)
        {
            note(device_failed_, "command buffer allocation failed");
            return RESONATE_E_INTERNAL;
        }

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_available) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semaphore, nullptr, &frame.render_done) != VK_SUCCESS)
        {
            note(device_failed_, "semaphore creation failed");
            return RESONATE_E_INTERNAL;
        }

        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device_, &fence, nullptr, &frame.in_flight) != VK_SUCCESS)
        {
            note(device_failed_, "fence creation failed");
            return RESONATE_E_INTERNAL;
        }
    }

    /* Sync work — uploads and image-target passes — shares one command buffer,
       which is safe because the calls arrive serialised. */
    VkCommandBufferAllocateInfo aux{};
    aux.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    aux.commandPool = command_pool_;
    aux.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    aux.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &aux, &aux_command_) != VK_SUCCESS)
    {
        note(device_failed_, "command buffer allocation failed");
        return RESONATE_E_INTERNAL;
    }
    return RESONATE_OK;
}

ResonateStatus Context::attachSurface(const void* native_surface, uint32_t width, uint32_t height)
{
    if (native_surface == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    if (ResonateStatus status = ensureInstance(); status != RESONATE_OK)
    {
        return status;
    }

    if (surface_ == VK_NULL_HANDLE)
    {
#ifdef RESONATE_PLATFORM_WINDOWS
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = reinterpret_cast<HWND>(const_cast<void*>(native_surface));
        if (vkCreateWin32SurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS)
        {
            note(surface_failed_, "surface creation failed");
            return RESONATE_E_INTERNAL;
        }
#else
        note(platform_unsupported_, "no surface backend on this platform yet");
        return RESONATE_E_UNSUPPORTED;
#endif
    }

    if (ResonateStatus status = ensureDevice(); status != RESONATE_OK)
    {
        return status;
    }

    canvas_width_ = width;
    canvas_height_ = height;
    destroySwapchainObjects();
    return createSwapchain(width, height);
}

bool Context::hasSurface() const
{
    return swapchain_ != VK_NULL_HANDLE;
}

ResonateStatus Context::createSwapchain(uint32_t width, uint32_t height)
{
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS)
    {
        note(surface_failed_, "surface capability query failed");
        return RESONATE_E_INTERNAL;
    }

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
    surface_format_ = formats.empty() ? surface_format_ : formats[0];
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            surface_format_ = format;
            break;
        }
    }

    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX)
    {
        extent_.width = width < caps.minImageExtent.width   ? caps.minImageExtent.width
                        : width > caps.maxImageExtent.width ? caps.maxImageExtent.width
                                                            : width;
        extent_.height = height < caps.minImageExtent.height   ? caps.minImageExtent.height
                         : height > caps.maxImageExtent.height ? caps.maxImageExtent.height
                                                               : height;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
    {
        image_count = caps.maxImageCount;
    }

    /* FIFO is the only mode the spec guarantees. Vsync off: mailbox trades a
       frame of latency for never tearing, immediate is the uncapped fallback. */
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_)
    {
        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
        for (const VkPresentModeKHR mode : modes)
        {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR || mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
            {
                present_mode_ = mode;
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                {
                    break;
                }
            }
        }
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format_.format;
    info.imageColorSpace = surface_format_.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode_;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS)
    {
        note(surface_failed_, "swapchain creation failed");
        return RESONATE_E_INTERNAL;
    }

    uint32_t image_count_actual = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_actual, nullptr);
    std::vector<VkImage> images(image_count_actual);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_actual, images.data());

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = surface_format_.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    views_.reserve(images.size());
    for (VkImage image : images)
    {
        view.image = image;
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &view, nullptr, &handle) != VK_SUCCESS)
        {
            note(surface_failed_, "image view creation failed");
            return RESONATE_E_INTERNAL;
        }
        views_.push_back(handle);
    }

    if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN,
                      &render_pass_) != RESONATE_OK)
    {
        note(surface_failed_, "render pass creation failed");
        return RESONATE_E_INTERNAL;
    }

    framebuffers_.reserve(views_.size());
    for (VkImageView image_view : views_)
    {
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = render_pass_;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &image_view;
        framebuffer.width = extent_.width;
        framebuffer.height = extent_.height;
        framebuffer.layers = 1;
        VkFramebuffer created = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device_, &framebuffer, nullptr, &created) != VK_SUCCESS)
        {
            note(surface_failed_, "framebuffer creation failed");
            return RESONATE_E_INTERNAL;
        }
        framebuffers_.push_back(created);
    }
    return RESONATE_OK;
}

void Context::destroySwapchainObjects()
{
    if (device_ == VK_NULL_HANDLE)
    {
        return;
    }
    for (VkFramebuffer framebuffer : framebuffers_)
    {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView view : views_)
    {
        vkDestroyImageView(device_, view, nullptr);
    }
    views_.clear();
    /* The cached pass object for the swapchain key is retired like any other;
       recreateSwapchain rebuilds it on demand. */
    if (render_pass_ != VK_NULL_HANDLE)
    {
        const uint32_t key = static_cast<uint32_t>(RESONATE_RENDER_FORMAT_UNKNOWN);
        auto found = pass_cache_.find(key);
        if (found != pass_cache_.end() && found->second == render_pass_)
        {
            pass_cache_.erase(found);
        }
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (swapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Context::recreateSwapchain()
{
    vkDeviceWaitIdle(device_);
    destroySwapchainObjects();
    createSwapchain(canvas_width_, canvas_height_);
}

VkFormat Context::formatOf(ResonateRenderFormat format) const
{
    switch (format)
    {
        case RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case RESONATE_RENDER_FORMAT_B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case RESONATE_RENDER_FORMAT_R32G32_SFLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case RESONATE_RENDER_FORMAT_D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case RESONATE_RENDER_FORMAT_UNKNOWN:
        default:
            return surface_format_.format;
    }
}

ResonateStatus Context::renderPassFor(ResonateRenderFormat color, ResonateRenderFormat depth,
                                      VkRenderPass* out)
{
    const uint32_t key = static_cast<uint32_t>(color) | (static_cast<uint32_t>(depth) << 8);
    const auto found = pass_cache_.find(key);
    if (found != pass_cache_.end())
    {
        *out = found->second;
        return RESONATE_OK;
    }

    VkAttachmentDescription attachments[2] = {};
    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    attachments[0].format = formatOf(color);
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = color == RESONATE_RENDER_FORMAT_UNKNOWN
                                     ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    uint32_t attachment_count = 1;

    if (depth != RESONATE_RENDER_FORMAT_UNKNOWN)
    {
        VkAttachmentReference depth_reference{};
        depth_reference.attachment = 1;
        depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        attachments[1].format = formatOf(depth);
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        subpass.pDepthStencilAttachment = &depth_reference;
        attachment_count = 2;
    }

    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = attachment_count;
    pass.pAttachments = attachments;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    VkRenderPass handle = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device_, &pass, nullptr, &handle) != VK_SUCCESS)
    {
        return RESONATE_E_INTERNAL;
    }
    pass_cache_.emplace(key, handle);
    *out = handle;
    return RESONATE_OK;
}

ResonateStatus Context::submitAndRun(VkCommandBuffer command)
{
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        note(device_failed_, "sync submission failed");
        return RESONATE_E_INTERNAL;
    }
    vkQueueWaitIdle(queue_);
    return RESONATE_OK;
}

void Context::drainGraveyard(std::vector<Retired>& graveyard)
{
    /* The SDK's handles are pointer-typed on this platform, so the retired
       void* casts straight back. */
    for (const Retired& retired : graveyard)
    {
        switch (retired.kind)
        {
            case kRetiredBuffer:
                vmaDestroyBuffer(allocator_, static_cast<VkBuffer>(retired.a),
                                 static_cast<VmaAllocation_T*>(retired.b));
                break;
            case kRetiredImage:
                vmaDestroyImage(allocator_, static_cast<VkImage>(retired.a),
                                static_cast<VmaAllocation_T*>(retired.b));
                break;
            case kRetiredImageView:
                vkDestroyImageView(device_, static_cast<VkImageView>(retired.a), nullptr);
                break;
            case kRetiredFramebuffer:
                vkDestroyFramebuffer(device_, static_cast<VkFramebuffer>(retired.a), nullptr);
                break;
            case kRetiredSampler:
                vkDestroySampler(device_, static_cast<VkSampler>(retired.a), nullptr);
                break;
            case kRetiredShader:
                vkDestroyShaderModule(device_, static_cast<VkShaderModule>(retired.a), nullptr);
                break;
            case kRetiredPipeline:
                vkDestroyPipeline(device_, static_cast<VkPipeline>(retired.a), nullptr);
                break;
            case kRetiredRenderPass:
                vkDestroyRenderPass(device_, static_cast<VkRenderPass>(retired.a), nullptr);
                break;
            case kRetiredPipelineLayout:
                vkDestroyPipelineLayout(device_, static_cast<VkPipelineLayout>(retired.a), nullptr);
                break;
            default:
                break;
        }
    }
    graveyard.clear();
}

ResonateStatus Context::beginPass(const ResonateRenderPassDesc& desc)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (pass_open_)
    {
        return RESONATE_E_STATE;
    }

    const bool swapchain_target = desc.color.value == 0;
    if (swapchain_target && swapchain_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }

    VkCommandBuffer command = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D pass_extent{};

    if (swapchain_target)
    {
        if (recreate_pending_ || extent_.width != canvas_width_ || extent_.height != canvas_height_)
        {
            recreate_pending_ = false;
            recreateSwapchain();
        }

        Frame& frame = frames_[frame_ % kFramesInFlight];
        vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
        drainGraveyard(frame.graveyard);

        uint32_t image_index = 0;
        VkResult acquired =
            vkAcquireNextImageKHR(device_, swapchain_, kAcquireTimeoutNs, frame.image_available,
                                  VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapchain();
            acquired = vkAcquireNextImageKHR(device_, swapchain_, kAcquireTimeoutNs,
                                             frame.image_available, VK_NULL_HANDLE, &image_index);
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            note(surface_failed_, "image acquisition failed");
            return RESONATE_E_STATE;
        }
        recreate_pending_ = acquired == VK_SUBOPTIMAL_KHR;
        image_index_ = image_index;

        vkResetFences(device_, 1, &frame.in_flight);
        vkResetCommandBuffer(frame.command, 0);
        command = frame.command;
        if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN, &pass) !=
            RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        framebuffer = framebuffers_[image_index_];
        pass_extent = extent_;
    }
    else
    {
        /* Render-to-image passes run synchronously on the aux command buffer.
           Depth targets are refused. */
        if (desc.depth.value != 0)
        {
            return RESONATE_E_UNSUPPORTED;
        }
        ImageSlot* image =
            images_.live(handleIndex(desc.color.value), handleGeneration(desc.color.value));
        if (image == nullptr || image->depth)
        {
            return RESONATE_E_INVALID;
        }

        if (renderPassFor(image->format_id, RESONATE_RENDER_FORMAT_UNKNOWN, &pass) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        const uint64_t key =
            (desc.color.value << 16) | (static_cast<uint64_t>(image->format_id) << 8);
        const auto found = framebuffer_cache_.find(key);
        if (found != framebuffer_cache_.end())
        {
            framebuffer = found->second;
        }
        else
        {
            VkFramebufferCreateInfo created{};
            created.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            created.renderPass = pass;
            created.attachmentCount = 1;
            created.pAttachments = &image->view;
            created.width = image->width;
            created.height = image->height;
            created.layers = 1;
            VkFramebuffer handle = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device_, &created, nullptr, &handle) != VK_SUCCESS)
            {
                note(device_failed_, "framebuffer creation failed");
                return RESONATE_E_INTERNAL;
            }
            framebuffer_cache_.emplace(key, handle);
            framebuffer = handle;
        }

        vkResetCommandBuffer(aux_command_, 0);
        command = aux_command_;
        pass_extent = VkExtent2D{image->width, image->height};
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(command, &begin);

    VkClearValue clear{};
    clear.color = {
        {desc.clear_color.x, desc.clear_color.y, desc.clear_color.z, desc.clear_color.w}};
    VkRenderPassBeginInfo pass_begin{};
    pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea.extent = pass_extent;
    pass_begin.clearValueCount = 1;
    pass_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(pass_extent.width);
    viewport.height = static_cast<float>(pass_extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = pass_extent;
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);

    open_pass_ = pass;
    open_framebuffer_ = framebuffer;
    pass_on_swapchain_ = swapchain_target;
    pass_color_ = desc.color;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_vertex_ = VK_NULL_HANDLE;
    bound_index_ = VK_NULL_HANDLE;
    pass_open_ = true;
    return RESONATE_OK;
}

ResonateStatus Context::endPass()
{
    if (!pass_open_)
    {
        return RESONATE_E_STATE;
    }
    const bool swapchain_target = pass_on_swapchain_;
    VkCommandBuffer command =
        swapchain_target ? frames_[frame_ % kFramesInFlight].command : aux_command_;
    vkCmdEndRenderPass(command);
    vkEndCommandBuffer(command);
    open_pass_ = VK_NULL_HANDLE;
    open_framebuffer_ = VK_NULL_HANDLE;
    pass_open_ = false;

    if (!swapchain_target)
    {
        if (submitAndRun(command) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    }

    Frame& frame = frames_[frame_ % kFramesInFlight];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.render_done;
    if (vkQueueSubmit(queue_, 1, &submit, frame.in_flight) != VK_SUCCESS)
    {
        note(surface_failed_, "command submission failed");
        return RESONATE_E_INTERNAL;
    }
    last_render_done_ = frame.render_done;
    frame_++;
    return RESONATE_OK;
}

ResonateStatus Context::present()
{
    if (swapchain_ == VK_NULL_HANDLE || last_render_done_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &last_render_done_;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index_;
    VkResult presented = vkQueuePresentKHR(queue_, &info);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
    {
        recreate_pending_ = true;
    }
    else if (presented != VK_SUCCESS)
    {
        note(surface_failed_, "present failed");
        return RESONATE_E_INTERNAL;
    }
    return RESONATE_OK;
}

} // namespace resonate::render_vulkan
