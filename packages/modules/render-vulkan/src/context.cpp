#include "context.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <resonate/pal/io.h>
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

/* Offscreen runs need no present support: any graphics family will do. */
bool graphicsQueueFamily(VkPhysicalDevice physical, uint32_t* family_out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
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

VkPipelineStageFlags stagesToVulkan(uint32_t stages)
{
    VkPipelineStageFlags flags = 0;
    if ((stages & RESONATE_RENDER_STAGE_VERTEX) != 0U)
    {
        flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    }
    if ((stages & RESONATE_RENDER_STAGE_FRAGMENT) != 0U)
    {
        flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if ((stages & RESONATE_RENDER_STAGE_COLOR_OUTPUT) != 0U)
    {
        flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if ((stages & RESONATE_RENDER_STAGE_DEPTH_OUTPUT) != 0U)
    {
        flags |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    if ((stages & RESONATE_RENDER_STAGE_COMPUTE) != 0U)
    {
        flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if ((stages & RESONATE_RENDER_STAGE_TRANSFER) != 0U)
    {
        flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    return flags;
}

VkAccessFlags accessToVulkan(uint32_t access)
{
    VkAccessFlags flags = 0;
    if ((access & RESONATE_RENDER_ACCESS_SHADER_READ) != 0U)
    {
        flags |= VK_ACCESS_SHADER_READ_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_SHADER_WRITE) != 0U)
    {
        flags |= VK_ACCESS_SHADER_WRITE_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_COLOR_WRITE) != 0U)
    {
        flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_DEPTH_READ) != 0U)
    {
        flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_DEPTH_WRITE) != 0U)
    {
        flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_TRANSFER_READ) != 0U)
    {
        flags |= VK_ACCESS_TRANSFER_READ_BIT;
    }
    if ((access & RESONATE_RENDER_ACCESS_TRANSFER_WRITE) != 0U)
    {
        flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    return flags;
}

VkImageLayout layoutToVulkan(ResonateRenderImageLayout layout)
{
    switch (layout)
    {
        case RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case RESONATE_RENDER_LAYOUT_DEPTH_ATTACHMENT:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case RESONATE_RENDER_LAYOUT_TRANSFER_SRC:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case RESONATE_RENDER_LAYOUT_TRANSFER_DST:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case RESONATE_RENDER_LAYOUT_PRESENT:
            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case RESONATE_RENDER_LAYOUT_UNDEFINED:
        default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

} // namespace

Context::Context(bool vsync, ResonateRenderCanvas* canvas) : vsync_(vsync), canvas_(canvas)
{
}

Context::~Context()
{
    if (device_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device_);
        /* Pipelines are all created by here; persist what the driver compiled. */
        savePipelineCache();
        /* The swapchain owns its render pass and framebuffers; taking them down
           first also removes the pass from the cache the loop below drains. */
        destroySwapchainObjects();
        /* Framebuffers reference image views, so they go before the graveyard's
           image views are destroyed. */
        for (const auto& entry : framebuffer_cache_)
        {
            vkDestroyFramebuffer(device_, entry.second, nullptr);
        }
        for (Frame& frame : frames_)
        {
            drainGraveyard(frame.graveyard);
        }
        for (const auto& entry : pass_cache_)
        {
            vkDestroyRenderPass(device_, entry.second, nullptr);
        }
        pass_cache_.clear();
        framebuffer_cache_.clear();
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
        if (pipeline_cache_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            pipeline_cache_ = VK_NULL_HANDLE;
        }
        if (table_dummy_ != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator_, table_dummy_, table_dummy_allocation_);
        }
        if (table_sampler_linear_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device_, table_sampler_linear_, nullptr);
        }
        if (table_sampler_nearest_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device_, table_sampler_nearest_, nullptr);
        }
        if (table_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device_, table_pool_, nullptr);
        }
        if (table_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device_, table_layout_, nullptr);
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

std::string Context::pipelineCachePath() const
{
    const char* directory = resonate_pal_io_executable_directory();
    return std::string(directory != nullptr ? directory : ".") + "/resonate.pipeline-cache";
}

void Context::loadPipelineCache()
{
    std::vector<char> data;
    ResonateFileHandle file{};
    if (resonate_pal_io_open_file(&file, pipelineCachePath().c_str(), RESONATE_FILE_READ) ==
        RESONATE_PAL_OK)
    {
        uint64_t size = 0;
        if (resonate_pal_io_size(&file, &size) == RESONATE_PAL_OK && size > 0U &&
            size <= 64U * 1024U * 1024U)
        {
            data.resize(static_cast<std::size_t>(size));
            uint64_t read = 0;
            if (resonate_pal_io_read(&file, data.data(), size, &read) == RESONATE_PAL_OK)
            {
                data.resize(static_cast<std::size_t>(read));
            }
            else
            {
                data.clear();
            }
        }
        resonate_pal_io_close_file(&file);
    }

    VkPipelineCacheCreateInfo cache{};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache.initialDataSize = data.size();
    cache.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &cache, nullptr, &pipeline_cache_) != VK_SUCCESS)
    {
        pipeline_cache_ = VK_NULL_HANDLE;
        return;
    }
    if (!data.empty())
    {
        std::fprintf(stderr, "render-vulkan: pipeline cache %zu bytes loaded\n", data.size());
    }
}

void Context::savePipelineCache()
{
    if (pipeline_cache_ == VK_NULL_HANDLE)
    {
        return;
    }
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS ||
        size == 0U)
    {
        return;
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS)
    {
        return;
    }

    ResonateFileHandle file{};
    if (resonate_pal_io_open_file(&file, pipelineCachePath().c_str(),
                                  RESONATE_FILE_WRITE | RESONATE_FILE_CREATE |
                                      RESONATE_FILE_TRUNCATE) != RESONATE_PAL_OK)
    {
        return;
    }
    uint64_t written = 0;
    resonate_pal_io_write(&file, data.data(), size, &written);
    resonate_pal_io_close_file(&file);
}

ResonateStatus Context::ensureInstance(bool needs_surface)
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

    /* The surface extensions are only for a windowed target; an offscreen run
       asks for neither. */
    const char* extensions[3];
    uint32_t enabled_extensions = 0;
    if (needs_surface)
    {
        extensions[enabled_extensions++] = VK_KHR_SURFACE_EXTENSION_NAME;
        extensions[enabled_extensions++] = surface_extension;
    }
    if (debug_utils)
    {
        extensions[enabled_extensions++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = kAppName;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = enabled_extensions;
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
    const bool windowed = surface_ != VK_NULL_HANDLE;
    for (VkPhysicalDevice candidate : devices)
    {
        uint32_t candidate_family = UINT32_MAX;
        const bool eligible =
            windowed ? combinedGraphicsAndPresent(candidate, surface_, &candidate_family)
                     : graphicsQueueFamily(candidate, &candidate_family);
        if (!eligible)
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
        note(device_failed_, windowed ? "no Vulkan device with a graphics+present queue"
                                      : "no Vulkan device with a graphics queue");
        return RESONATE_E_UNSUPPORTED;
    }
    queue_family_ = family;

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extension_count, extensions.data());
    if (windowed && !hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        note(device_failed_, "device has no swapchain extension");
        return RESONATE_E_UNSUPPORTED;
    }
    if (!hasExtension(extensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
    {
        note(device_failed_, "device has no descriptor indexing extension");
        return RESONATE_E_UNSUPPORTED;
    }

    /* The resource table needs partially bound arrays, and the scene shader
       picks a shadow map with a per-pixel index (a non-uniform sampled-image
       lookup). Slang's vertex-id path also reads a base-vertex builtin, so
       shaderDrawParameters is required. */
    VkPhysicalDeviceVulkan11Features vulkan11{};
    vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    indexing.pNext = &vulkan11;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &indexing;
    vkGetPhysicalDeviceFeatures2(physical_, &features);
    if (!indexing.shaderSampledImageArrayNonUniformIndexing ||
        !indexing.descriptorBindingPartiallyBound || !vulkan11.shaderDrawParameters)
    {
        note(device_failed_, "device lacks the resource-table features");
        return RESONATE_E_UNSUPPORTED;
    }

    VkPhysicalDeviceVulkan11Features enabled11{};
    enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    enabled11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceDescriptorIndexingFeatures enabled_indexing{};
    enabled_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    enabled_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    enabled_indexing.descriptorBindingPartiallyBound = VK_TRUE;
    enabled_indexing.pNext = &enabled11;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    const char* device_extensions[2];
    uint32_t device_extension_count = 0;
    if (windowed)
    {
        device_extensions[device_extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    device_extensions[device_extension_count++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled_indexing;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = device_extension_count;
    info.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS)
    {
        note(device_failed_, "device creation failed");
        return RESONATE_E_INTERNAL;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    /* The pipeline cache is per machine, not per run: it is what keeps a
       second launch from recompiling every pipeline. */
    loadPipelineCache();

    VmaAllocatorCreateInfo allocator{};
    allocator.physicalDevice = physical_;
    allocator.device = device_;
    allocator.instance = instance_;
    if (vmaCreateAllocator(&allocator, &allocator_) != VK_SUCCESS)
    {
        note(device_failed_, "allocator creation failed");
        return RESONATE_E_INTERNAL;
    }

    /* The resource tables: set 0, bindings 0-4 = kTableSize storage buffers
       each, binding 5 = kTableSize combined image samplers. Every binding is
       partially bound, so a slot nobody created may be absent from the array.
       Dummies fill the storage slots at creation, so an index nobody wrote
       reads defined zeros. */
    VkDescriptorSetLayoutBinding table_bindings[kTableBindingCount] = {};
    for (uint32_t binding = 0; binding < kStorageTableBindings; ++binding)
    {
        table_bindings[binding].binding = binding;
        table_bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        table_bindings[binding].descriptorCount = kTableSize;
        table_bindings[binding].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        table_bindings[binding].pImmutableSamplers = nullptr;
    }
    table_bindings[kSampledTableBinding].binding = kSampledTableBinding;
    table_bindings[kSampledTableBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    table_bindings[kSampledTableBinding].descriptorCount = kTableSize;
    table_bindings[kSampledTableBinding].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorBindingFlags binding_flags[kTableBindingCount] = {};
    for (VkDescriptorBindingFlags& flags : binding_flags)
    {
        flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
    binding_flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    binding_flags_info.bindingCount = kTableBindingCount;
    binding_flags_info.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo table_layout{};
    table_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    table_layout.pNext = &binding_flags_info;
    table_layout.bindingCount = kTableBindingCount;
    table_layout.pBindings = table_bindings;
    if (vkCreateDescriptorSetLayout(device_, &table_layout, nullptr, &table_layout_) != VK_SUCCESS)
    {
        note(device_failed_, "table layout creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkDescriptorPoolSize table_pool_sizes[2] = {};
    table_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    table_pool_sizes[0].descriptorCount = kStorageTableBindings * kTableSize;
    table_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    table_pool_sizes[1].descriptorCount = kTableSize;
    VkDescriptorPoolCreateInfo table_pool{};
    table_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    table_pool.maxSets = 1;
    table_pool.poolSizeCount = 2;
    table_pool.pPoolSizes = table_pool_sizes;
    if (vkCreateDescriptorPool(device_, &table_pool, nullptr, &table_pool_) != VK_SUCCESS)
    {
        note(device_failed_, "table pool creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkDescriptorSetAllocateInfo table_set{};
    table_set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    table_set.descriptorPool = table_pool_;
    table_set.descriptorSetCount = 1;
    table_set.pSetLayouts = &table_layout_;
    if (vkAllocateDescriptorSets(device_, &table_set, &table_set_) != VK_SUCCESS)
    {
        note(device_failed_, "table allocation failed");
        return RESONATE_E_INTERNAL;
    }

    VkBufferCreateInfo dummy_info{};
    dummy_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    dummy_info.size = 256U;
    dummy_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    dummy_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo dummy_allocation{};
    dummy_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateBuffer(allocator_, &dummy_info, &dummy_allocation, &table_dummy_,
                        &table_dummy_allocation_, nullptr) != VK_SUCCESS)
    {
        note(device_failed_, "table dummy buffer creation failed");
        return RESONATE_E_INTERNAL;
    }
    VkDescriptorBufferInfo dummy_buffer_info{};
    dummy_buffer_info.buffer = table_dummy_;
    dummy_buffer_info.offset = 0;
    dummy_buffer_info.range = 256U;
    for (uint32_t slot = 0; slot < kTableSize; ++slot)
    {
        VkWriteDescriptorSet writes[kStorageTableBindings]{};
        for (uint32_t binding = 0; binding < kStorageTableBindings; ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = table_set_;
            writes[binding].dstBinding = binding;
            writes[binding].dstArrayElement = slot;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &dummy_buffer_info;
        }
        vkUpdateDescriptorSets(device_, kStorageTableBindings, writes, 0, nullptr);
    }

    /* Two clamp samplers back the sampled-image slots: linear for the post
       chain and shadow reads (full-screen or texel-offset taps), nearest for
       albedo texels the image asks to keep crisp. */
    VkSamplerCreateInfo table_sampler{};
    table_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    table_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    table_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    table_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    table_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    table_sampler.minFilter = VK_FILTER_LINEAR;
    table_sampler.magFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(device_, &table_sampler, nullptr, &table_sampler_linear_) != VK_SUCCESS)
    {
        note(device_failed_, "table sampler creation failed");
        return RESONATE_E_INTERNAL;
    }
    table_sampler.minFilter = VK_FILTER_NEAREST;
    table_sampler.magFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_, &table_sampler, nullptr, &table_sampler_nearest_) != VK_SUCCESS)
    {
        note(device_failed_, "table sampler creation failed");
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
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_available) != VK_SUCCESS)
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
    if (native_surface == nullptr || offscreen_)
    {
        return RESONATE_E_INVALID;
    }
    if (ResonateStatus status = ensureInstance(true); status != RESONATE_OK)
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
    return swapchain_ != VK_NULL_HANDLE || offscreen_;
}

ResonateStatus Context::attachOffscreen(uint32_t width, uint32_t height)
{
    if (width == 0U || height == 0U || surface_ != VK_NULL_HANDLE)
    {
        return RESONATE_E_INVALID;
    }
    if (ResonateStatus status = ensureInstance(false); status != RESONATE_OK)
    {
        return status;
    }
    if (ResonateStatus status = ensureDevice(); status != RESONATE_OK)
    {
        return status;
    }

    /* Re-attaching resizes an existing target; a window target is never
       replaced. */
    offscreen_ = true;
    canvas_width_ = width;
    canvas_height_ = height;
    destroySwapchainObjects();
    return createOffscreen(width, height);
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

    /* One render-done semaphore per image: the presentation engine may hold a
       semaphore after its image was presented, so it cannot be shared across
       images. */
    present_semaphores_.reserve(views_.size());
    for (std::size_t index = 0; index < views_.size(); ++index)
    {
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore created = VK_NULL_HANDLE;
        if (vkCreateSemaphore(device_, &semaphore, nullptr, &created) != VK_SUCCESS)
        {
            note(surface_failed_, "present semaphore creation failed");
            return RESONATE_E_INTERNAL;
        }
        present_semaphores_.push_back(created);
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

ResonateStatus Context::createOffscreen(uint32_t width, uint32_t height)
{
    extent_ = VkExtent2D{width, height};
    surface_format_.format = VK_FORMAT_R8G8B8A8_SRGB;
    surface_format_.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

    /* The image index shares the frame slot: the fence waited at begin_frame
       then guarantees the image is free as well. */
    constexpr uint32_t kOffscreenImages = kFramesInFlight;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = surface_format_.format;
    image_info.extent = VkExtent3D{width, height, 1U};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = surface_format_.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;

    offscreen_images_.reserve(kOffscreenImages);
    views_.reserve(kOffscreenImages);
    for (uint32_t index = 0; index < kOffscreenImages; ++index)
    {
        OwnedImage owned;
        if (vmaCreateImage(allocator_, &image_info, &allocation, &owned.image, &owned.allocation,
                           nullptr) != VK_SUCCESS)
        {
            note(device_failed_, "offscreen image creation failed");
            return RESONATE_E_INTERNAL;
        }
        view.image = owned.image;
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &view, nullptr, &handle) != VK_SUCCESS)
        {
            vmaDestroyImage(allocator_, owned.image, owned.allocation);
            note(device_failed_, "offscreen view creation failed");
            return RESONATE_E_INTERNAL;
        }
        offscreen_images_.push_back(owned);
        views_.push_back(handle);
    }

    if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN,
                      &render_pass_) != RESONATE_OK)
    {
        note(device_failed_, "render pass creation failed");
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
            note(device_failed_, "framebuffer creation failed");
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
    for (VkSemaphore semaphore : present_semaphores_)
    {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    present_semaphores_.clear();
    /* Offscreen images are ours, unlike swapchain images; the views above are
       already gone. */
    for (OwnedImage& owned : offscreen_images_)
    {
        vmaDestroyImage(allocator_, owned.image, owned.allocation);
    }
    offscreen_images_.clear();
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

    /* Depth-only: color NONE and a depth format make a pass with no color
       attachment (shadow maps). */
    const bool colorless = color == RESONATE_RENDER_FORMAT_NONE;

    VkAttachmentDescription attachments[2] = {};
    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (!colorless)
    {
        attachments[0].format = formatOf(color);
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        /* Only a swapchain image ends a pass in the present layout; an
           offscreen target stays a color attachment, which is where readback
           picks it up. */
        attachments[0].finalLayout = color == RESONATE_RENDER_FORMAT_UNKNOWN && !offscreen_
                                         ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t attachment_count = 0;

    if (!colorless)
    {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        attachment_count = 1;
    }

    if (depth != RESONATE_RENDER_FORMAT_UNKNOWN)
    {
        VkAttachmentReference depth_reference{};
        depth_reference.attachment = colorless ? 0 : 1;
        depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        const uint32_t depth_index = colorless ? 0 : 1;
        attachments[depth_index].format = formatOf(depth);
        attachments[depth_index].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[depth_index].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[depth_index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[depth_index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[depth_index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[depth_index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[depth_index].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        subpass.pDepthStencilAttachment = &depth_reference;
        attachment_count = colorless ? 1 : 2;
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

VkBuffer Context::resolveBuffer(ResonateRenderBuffer handle) const
{
    const BufferSlot* slot =
        buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    return slot != nullptr ? slot->buffer : VK_NULL_HANDLE;
}

void Context::writeBufferSlot(uint32_t slot, VkBuffer buffer, VkDeviceSize size)
{
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = buffer;
    buffer_info.offset = 0;
    buffer_info.range = size;
    VkWriteDescriptorSet writes[kStorageTableBindings]{};
    for (uint32_t binding = 0; binding < kStorageTableBindings; ++binding)
    {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = table_set_;
        writes[binding].dstBinding = binding;
        writes[binding].dstArrayElement = slot;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &buffer_info;
    }
    vkUpdateDescriptorSets(device_, kStorageTableBindings, writes, 0, nullptr);
}

void Context::writeImageSlot(uint32_t slot, VkImageView view, bool nearest)
{
    VkDescriptorImageInfo image_info{};
    image_info.sampler = nearest ? table_sampler_nearest_ : table_sampler_linear_;
    image_info.imageView = view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = table_set_;
    write.dstBinding = kSampledTableBinding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
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

ResonateStatus Context::beginFrame()
{
    if ((swapchain_ == VK_NULL_HANDLE && !offscreen_) || frame_open_)
    {
        return RESONATE_E_STATE;
    }
    /* The swapchain recreates itself when the canvas changes size; an offscreen
       target is re-attached by its owner instead. */
    const uint32_t width = canvas_ != nullptr ? canvas_->width(canvas_->self) : canvas_width_;
    const uint32_t height = canvas_ != nullptr ? canvas_->height(canvas_->self) : canvas_height_;
    if (offscreen_)
    {
        canvas_width_ = width;
        canvas_height_ = height;
    }
    else if (recreate_pending_ || width != canvas_width_ || height != canvas_height_)
    {
        canvas_width_ = width;
        canvas_height_ = height;
        recreate_pending_ = false;
        recreateSwapchain();
    }

    Frame& frame = frames_[frame_ % kFramesInFlight];
    vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
    drainGraveyard(frame.graveyard);

    if (offscreen_)
    {
        /* The image shares the frame slot, so the fence above guarantees the
           image is free as well. */
        image_index_ = frame_ % kFramesInFlight;
    }
    else
    {
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
    }

    vkResetFences(device_, 1, &frame.in_flight);
    vkResetCommandBuffer(frame.command, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(frame.command, &begin);

    /* Every pipeline reads the resource table as set 0, so the frame binds it
       once at each bind point. */
    VkDescriptorSet set = table_set_;
    vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                            &set, 0, nullptr);
    vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1,
                            &set, 0, nullptr);
    frame_open_ = true;
    return RESONATE_OK;
}

ResonateStatus Context::beginPass(const ResonateRenderPassDesc& desc)
{
    if (!frame_open_ || pass_open_)
    {
        return RESONATE_E_STATE;
    }

    const bool colorless =
        desc.struct_size >= offsetof(ResonateRenderPassDesc, colorless) + sizeof(uint32_t) &&
        desc.colorless != 0U;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D pass_extent{};

    if (desc.color.value == 0 && !colorless)
    {
        if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN, &pass) !=
            RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        framebuffer = framebuffers_[image_index_];
        pass_extent = extent_;
    }
    else if (colorless)
    {
        /* Depth-only (a shadow pass): the depth image is the sole target. */
        if (desc.depth.value == 0)
        {
            return RESONATE_E_INVALID;
        }
        ImageSlot* depth =
            images_.live(handleIndex(desc.depth.value), handleGeneration(desc.depth.value));
        if (depth == nullptr || !depth->depth)
        {
            return RESONATE_E_INVALID;
        }

        if (renderPassFor(RESONATE_RENDER_FORMAT_NONE, depth->format_id, &pass) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        const std::pair<uint64_t, uint64_t> key{0U, desc.depth.value};
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
            created.pAttachments = &depth->view;
            created.width = depth->width;
            created.height = depth->height;
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
        pass_extent = VkExtent2D{depth->width, depth->height};
    }
    else
    {
        /* Render-to-image passes run in the frame's command buffer like any
           other; the depth image, when given, joins the framebuffer. */
        ImageSlot* image =
            images_.live(handleIndex(desc.color.value), handleGeneration(desc.color.value));
        if (image == nullptr || image->depth)
        {
            return RESONATE_E_INVALID;
        }
        ImageSlot* depth = nullptr;
        if (desc.depth.value != 0)
        {
            depth = images_.live(handleIndex(desc.depth.value), handleGeneration(desc.depth.value));
            if (depth == nullptr || !depth->depth)
            {
                return RESONATE_E_INVALID;
            }
        }

        const ResonateRenderFormat depth_format =
            depth != nullptr ? depth->format_id : RESONATE_RENDER_FORMAT_UNKNOWN;
        if (renderPassFor(image->format_id, depth_format, &pass) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        VkImageView attachments[2] = {image->view, depth != nullptr ? depth->view : VK_NULL_HANDLE};
        const uint32_t attachment_count = depth != nullptr ? 2U : 1U;
        const std::pair<uint64_t, uint64_t> key{desc.color.value, desc.depth.value};
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
            created.attachmentCount = attachment_count;
            created.pAttachments = attachments;
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
        pass_extent = VkExtent2D{image->width, image->height};
    }

    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    /* One clear value per cleared attachment: a depth-only pass's sole
       attachment is the depth image, a color pass's second is depth. */
    VkClearValue clears[2] = {};
    uint32_t clear_count = 1U;
    if (colorless)
    {
        clears[0].depthStencil = {1.0f, 0U};
    }
    else
    {
        clears[0].color = {
            {desc.clear_color.x, desc.clear_color.y, desc.clear_color.z, desc.clear_color.w}};
        if (desc.depth.value != 0)
        {
            clears[1].depthStencil = {1.0f, 0U};
            clear_count = 2U;
        }
    }
    VkRenderPassBeginInfo pass_begin{};
    pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea.extent = pass_extent;
    pass_begin.clearValueCount = clear_count;
    pass_begin.pClearValues = clears;
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(pass_extent.width);
    viewport.height = static_cast<float>(pass_extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = pass_extent;
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);

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
    vkCmdEndRenderPass(frames_[frame_ % kFramesInFlight].command);
    pass_open_ = false;
    return RESONATE_OK;
}

ResonateStatus Context::barrier(const ResonateRenderBarrierDesc* desc)
{
    if (!frame_open_)
    {
        return RESONATE_E_STATE;
    }
    if (desc == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;

    VkPipelineStageFlags before_stages = stagesToVulkan(desc->before_stages);
    if (before_stages == 0)
    {
        before_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    VkPipelineStageFlags after_stages = stagesToVulkan(desc->after_stages);
    if (after_stages == 0)
    {
        after_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    const VkAccessFlags before_access = accessToVulkan(desc->before_access);
    const VkAccessFlags after_access = accessToVulkan(desc->after_access);

    if (desc->image.value != 0)
    {
        const ImageSlot* slot =
            images_.live(handleIndex(desc->image.value), handleGeneration(desc->image.value));
        if (slot == nullptr)
        {
            return RESONATE_E_INVALID;
        }
        VkImageMemoryBarrier image_barrier{};
        image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image_barrier.srcAccessMask = before_access;
        image_barrier.dstAccessMask = after_access;
        image_barrier.oldLayout = layoutToVulkan(desc->before_layout);
        image_barrier.newLayout = layoutToVulkan(desc->after_layout);
        image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image = slot->image;
        /* The aspect belongs to the image, not to either layout: a shadow map
           transitions from depth attachment to shader-read. */
        image_barrier.subresourceRange.aspectMask =
            slot->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier.subresourceRange.levelCount = 1;
        image_barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, before_stages, after_stages, 0, 0, nullptr, 0, nullptr, 1,
                             &image_barrier);
        return RESONATE_OK;
    }
    if (desc->buffer.value != 0)
    {
        VkBufferMemoryBarrier buffer_barrier{};
        buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        buffer_barrier.srcAccessMask = before_access;
        buffer_barrier.dstAccessMask = after_access;
        buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buffer_barrier.buffer = resolveBuffer(desc->buffer);
        buffer_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, before_stages, after_stages, 0, 0, nullptr, 1,
                             &buffer_barrier, 0, nullptr);
        return RESONATE_OK;
    }
    return RESONATE_E_INVALID;
}

ResonateStatus Context::waitIdle()
{
    if (device_ == VK_NULL_HANDLE || frame_open_)
    {
        return RESONATE_E_STATE;
    }
    vkDeviceWaitIdle(device_);
    return RESONATE_OK;
}

ResonateStatus Context::present()
{
    if (!frame_open_ || (swapchain_ == VK_NULL_HANDLE && !offscreen_))
    {
        return RESONATE_E_STATE;
    }
    Frame& frame = frames_[frame_ % kFramesInFlight];
    vkEndCommandBuffer(frame.command);

    /* An offscreen frame waits on and signals nothing: there is no acquire and
       no presentation engine. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_done =
        present_semaphores_.empty() ? VK_NULL_HANDLE : present_semaphores_[image_index_];
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (!offscreen_)
    {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.image_available;
        submit.pWaitDstStageMask = &wait_stage;
    }
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    if (!offscreen_)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_done;
    }
    const VkResult submitted = vkQueueSubmit(queue_, 1, &submit, frame.in_flight);
    frame_open_ = false;
    pass_open_ = false;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_vertex_ = VK_NULL_HANDLE;
    bound_index_ = VK_NULL_HANDLE;
    frame_++;
    if (submitted != VK_SUCCESS)
    {
        note(surface_failed_, "command submission failed");
        return RESONATE_E_INTERNAL;
    }
    if (offscreen_)
    {
        return RESONATE_OK;
    }

    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_done;
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

ResonateStatus Context::readbackCanvas(void* data, uint64_t size_bytes, uint32_t* out_width,
                                       uint32_t* out_height)
{
    if (!offscreen_ || data == nullptr || out_width == nullptr || out_height == nullptr ||
        frame_open_ || frame_ == 0U || offscreen_images_.empty())
    {
        return RESONATE_E_STATE;
    }
    const uint64_t tight = static_cast<uint64_t>(extent_.width) * extent_.height * 4U;
    if (size_bytes < tight)
    {
        return RESONATE_E_INVALID;
    }
    vkDeviceWaitIdle(device_);

    VkBufferCreateInfo staging_info{};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = tight;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo staging_allocation{};
    staging_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    staging_allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(allocator_, &staging_info, &staging_allocation, &staging, &allocation,
                        &allocation_info) != VK_SUCCESS)
    {
        return RESONATE_E_INTERNAL;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkResetCommandBuffer(aux_command_, 0);
    vkBeginCommandBuffer(aux_command_, &begin);

    VkImageMemoryBarrier to_copy{};
    to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = offscreen_images_[image_index_].image;
    to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_copy.subresourceRange.levelCount = 1;
    to_copy.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(aux_command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = VkExtent3D{extent_.width, extent_.height, 1U};
    vkCmdCopyImageToBuffer(aux_command_, to_copy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &copy);

    /* Back to the attachment layout, so a second readback sees the same state
       and the next frame's render pass starts from it. */
    VkImageMemoryBarrier to_color = to_copy;
    to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_color.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(aux_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_color);

    vkEndCommandBuffer(aux_command_);
    const ResonateStatus status = submitAndRun(aux_command_);
    if (status == RESONATE_OK)
    {
        std::memcpy(data, allocation_info.pMappedData, static_cast<std::size_t>(tight));
        *out_width = extent_.width;
        *out_height = extent_.height;
    }
    vmaDestroyBuffer(allocator_, staging, allocation);
    return status;
}

} // namespace resonate::render_vulkan
