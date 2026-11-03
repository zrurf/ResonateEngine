#include "context.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <xxhash.h>

#ifdef RESONATE_PLATFORM_ANDROID
#    include <android/native_window.h>
#endif

#include <resonate/pal/io.h>
#include <vma/vk_mem_alloc.h>

namespace resonate::render_vulkan
{
namespace
{

constexpr const char* kAppName = "Resonate";


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
        case RESONATE_RENDER_LAYOUT_GENERAL:
            return VK_IMAGE_LAYOUT_GENERAL;
        case RESONATE_RENDER_LAYOUT_UNDEFINED:
        default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

} // namespace

Context::Context(bool vsync, bool prefer_srgb, ResonateRenderCanvas* canvas)
    : vsync_(vsync), prefer_srgb_(prefer_srgb), canvas_(canvas)
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
        discardMsaa();
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
        if (debug_canvas_buffer_ != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator_, debug_canvas_buffer_, debug_canvas_allocation_);
        }
        if (trace_buffer_ != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator_, trace_buffer_, trace_allocation_);
        }
        for (VkQueryPool& pool : profile_pool_)
        {
            if (pool != VK_NULL_HANDLE)
            {
                vkDestroyQueryPool(device_, pool, nullptr);
                pool = VK_NULL_HANDLE;
            }
        }
        if (table_dummy_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, table_dummy_view_, nullptr);
        }
        if (table_dummy_image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, table_dummy_image_, table_dummy_image_allocation_);
        }
        if (table_sampler_linear_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device_, table_sampler_linear_, nullptr);
        }
        if (shadow_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device_, shadow_sampler_, nullptr);
        }
        if (staging_buffer_ != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(allocator_, staging_buffer_, staging_allocation_);
            staging_buffer_ = VK_NULL_HANDLE;
            staging_allocation_ = nullptr;
        }
        if (shadow_dummy_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, shadow_dummy_view_, nullptr);
        }
        if (shadow_dummy_image_ != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, shadow_dummy_image_, shadow_dummy_allocation_);
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

namespace
{

/* A timestamp query is only meaningful when the queue family reports valid
   bits; anything above zero gives a usable clock. */
constexpr uint32_t kTimestampBits = 1U;

/* RESONATE_FRAME_PROFILE: the GPU's own frame duration, summed across slots. */
double gpu_time_ms = 0.0;
unsigned gpu_time_frames = 0U;

/* Read on first use: the platform sets the environment after the process
   starts (Android passes it as a system property). */
bool profileEnabled()
{
    static const bool enabled = std::getenv("RESONATE_FRAME_PROFILE") != nullptr;
    return enabled;
}

} // namespace

void Context::trace(const char* format, ...) const
{
    static const bool enabled = std::getenv("RESONATE_FRAME_TRACE") != nullptr;
    if (!enabled)
    {
        return;
    }
    std::fprintf(stderr, "render-vulkan: frame %llu ", static_cast<unsigned long long>(frame_));
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

void Context::tracePresentedTexel(VkCommandBuffer command)
{
    static const bool enabled = std::getenv("RESONATE_FRAME_TRACE_TEXEL") != nullptr;
    if (!enabled || offscreen_ || swapchain_ == VK_NULL_HANDLE || swapchain_images_.empty())
    {
        return;
    }
    if (trace_buffer_ == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = 64U;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(allocator_, &buffer, &allocation, &trace_buffer_, &trace_allocation_,
                            nullptr) != VK_SUCCESS)
        {
            return;
        }
    }
    VkImageMemoryBarrier to_copy{};
    to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = swapchain_images_[image_index_];
    to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_copy.subresourceRange.levelCount = 1;
    to_copy.subresourceRange.layerCount = 1;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &to_copy);
    VkBufferImageCopy region{};
    region.bufferOffset = (frame_ % kFramesInFlight) * 16U;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = VkOffset3D{static_cast<int32_t>(extent_.width / 2U),
                                    static_cast<int32_t>(extent_.height / 2U), 0};
    region.imageExtent = VkExtent3D{1U, 1U, 1U};
    vkCmdCopyImageToBuffer(command, swapchain_images_[image_index_],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, trace_buffer_, 1U, &region);
    VkImageMemoryBarrier to_present = to_copy;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &to_present);
    trace_frames_[frame_ % kFramesInFlight] = frame_;
}

void Context::profileTimestamp(VkCommandBuffer command)
{
    if (timestamp_period_ns_ <= 0.0F)
    {
        return;
    }
    const std::size_t slot = frame_ % kFramesInFlight;
    if (profile_pool_[slot] == VK_NULL_HANDLE ||
        profile_count_[slot] >= kMaxPassTimestamps)
    {
        return;
    }
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile_pool_[slot],
                        profile_count_[slot]);
    ++profile_count_[slot];
}

/* Reads the previous frame of this slot: the timestamps are the frame start and
   one per pass boundary, so the deltas are the passes themselves. */
void Context::profileReport(std::size_t slot)
{
    if (timestamp_period_ns_ <= 0.0F || profile_pool_[slot] == VK_NULL_HANDLE ||
        profile_reported_[slot] < 2U)
    {
        return;
    }
    std::uint64_t stamps[kMaxPassTimestamps] = {};
    const std::uint32_t count = profile_reported_[slot] < kMaxPassTimestamps
                                    ? profile_reported_[slot]
                                    : kMaxPassTimestamps;
    const VkResult read = vkGetQueryPoolResults(
        device_, profile_pool_[slot], 0, count,
        static_cast<std::size_t>(count) * sizeof(std::uint64_t), stamps, sizeof(std::uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    profile_reported_[slot] = 0U;
    if (read != VK_SUCCESS)
    {
        return;
    }
    std::fprintf(stderr, "render-vulkan: gpu passes");
    for (std::uint32_t index = 1U; index < count; ++index)
    {
        const double ms =
            static_cast<double>(stamps[index] - stamps[index - 1U]) * timestamp_period_ns_ /
            1.0e6;
        std::fprintf(stderr, " %.2f", ms);
    }
    std::fputc('\n', stderr);
}

void Context::tracePreviousTexel()
{
    if (trace_buffer_ == VK_NULL_HANDLE)
    {
        return;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, trace_allocation_, &mapped) != VK_SUCCESS)
    {
        return;
    }
    /* The GPU wrote the trace buffer; host memory is not necessarily
       coherent. */
    (void)vmaInvalidateAllocation(allocator_, trace_allocation_, 0, VK_WHOLE_SIZE);
    const std::size_t slot = frame_ % kFramesInFlight;
    const auto* texel = static_cast<const std::uint8_t*>(mapped) + slot * 16U;
    std::fprintf(stderr, "render-vulkan: frame %u texel %u,%u,%u\n", trace_frames_[slot],
                 texel[0], texel[1], texel[2]);
    vmaUnmapMemory(allocator_, trace_allocation_);
}

std::string Context::pipelineCachePath() const
{
    const char* directory = resonate_pal_io_executable_directory();
    return std::string(directory != nullptr ? directory : ".") + "/resonate.pipeline-cache";
}

namespace
{

/* The cache file wraps the driver blob in a digest container: a process kill
   mid-save must not hand truncated bytes to vkCreatePipelineCache. */
constexpr uint32_t kCacheMagic = 0x43505652U; /* 'RVPC' little-endian */
constexpr uint32_t kCacheVersion = 1U;
constexpr std::size_t kCacheHeaderSize = 4U + 4U + 8U + 16U;

#pragma pack(push, 1)
struct CacheHeader
{
    uint32_t magic;
    uint32_t version;
    uint64_t payload_size;
    XXH128_hash_t digest;
};
#pragma pack(pop)

static_assert(sizeof(CacheHeader) == kCacheHeaderSize, "the cache header is tightly packed");

} // namespace

void Context::loadPipelineCache()
{
    std::vector<char> data;
    ResonateFileHandle file{};
    if (resonate_pal_io_open_file(&file, pipelineCachePath().c_str(), RESONATE_FILE_READ) ==
        RESONATE_PAL_OK)
    {
        uint64_t size = 0;
        if (resonate_pal_io_size(&file, &size) == RESONATE_PAL_OK && size > kCacheHeaderSize &&
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

    std::vector<char> payload;
    if (!data.empty())
    {
        CacheHeader header{};
        std::memcpy(&header, data.data(), sizeof(header));
        const std::size_t payload_size = data.size() - sizeof(header);
        if (header.magic == kCacheMagic && header.version == kCacheVersion &&
            header.payload_size == payload_size &&
            XXH128_isEqual(XXH3_128bits(data.data() + sizeof(header), payload_size),
                           header.digest))
        {
            payload.assign(data.begin() + static_cast<std::ptrdiff_t>(sizeof(header)),
                           data.end());
        }
        else
        {
            std::fprintf(stderr, "render-vulkan: pipeline cache discarded (integrity)\n");
        }
    }

    VkPipelineCacheCreateInfo cache{};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache.initialDataSize = payload.size();
    cache.pInitialData = payload.empty() ? nullptr : payload.data();
    if (vkCreatePipelineCache(device_, &cache, nullptr, &pipeline_cache_) != VK_SUCCESS)
    {
        pipeline_cache_ = VK_NULL_HANDLE;
        return;
    }
    if (!payload.empty())
    {
        std::fprintf(stderr, "render-vulkan: pipeline cache %zu bytes loaded\n", payload.size());
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

    CacheHeader header{};
    header.magic = kCacheMagic;
    header.version = kCacheVersion;
    header.payload_size = data.size();
    header.digest = XXH3_128bits(data.data(), data.size());

    ResonateFileHandle file{};
    if (resonate_pal_io_open_file(&file, pipelineCachePath().c_str(),
                                  RESONATE_FILE_WRITE | RESONATE_FILE_CREATE |
                                      RESONATE_FILE_TRUNCATE) != RESONATE_PAL_OK)
    {
        return;
    }
    uint64_t written = 0;
    resonate_pal_io_write(&file, &header, sizeof(header), &written);
    resonate_pal_io_write(&file, data.data(), data.size(), &written);
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
#elif defined(RESONATE_PLATFORM_ANDROID)
    surface_extension = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
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

    /* Descriptor indexing is core in Vulkan 1.2. Drivers past that promote it
       and often stop advertising the extension, so the instance asks for the
       newer core version when the loader carries it. */
    auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_version != nullptr)
    {
        uint32_t loader_version = VK_API_VERSION_1_1;
        if (enumerate_version(&loader_version) == VK_SUCCESS &&
            loader_version >= VK_API_VERSION_1_2)
        {
            app.apiVersion = VK_API_VERSION_1_2;
        }
    }
    instance_api_version_ = app.apiVersion;

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
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_, &properties);

    /* Every shader's image table is declared at kImageTableSize, so the
       per-stage sampler limit has to cover it: a smaller device would read
       past its layout. Refuse it with the reason instead. */
    if (properties.limits.maxPerStageDescriptorSamplers < kImageTableSize)
    {
        std::fprintf(stderr,
                     "render-vulkan: %s offers %u samplers per stage; the renderer's image "
                     "table needs %u\n",
                     properties.deviceName,
                     properties.limits.maxPerStageDescriptorSamplers, kImageTableSize);
        return RESONATE_E_UNSUPPORTED;
    }

    const bool core_indexing = instance_api_version_ >= VK_API_VERSION_1_2 &&
                               properties.apiVersion >= VK_API_VERSION_1_2;
    const bool extension_indexing =
        hasExtension(extensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

    /* The table shader indexes descriptor arrays with push-constant slots, a
       dynamically uniform index the base features cover. Descriptor indexing
       adds partially bound slots and the per-pixel shadow lookup; a device
       without it runs the compat scene shader instead, with every slot bound. */
    VkPhysicalDeviceVulkan11Features vulkan11{};
    vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    if (core_indexing)
    {
        vulkan12.pNext = &vulkan11;
        features.pNext = &vulkan12;
    }
    else if (extension_indexing)
    {
        indexing.pNext = &vulkan11;
        features.pNext = &indexing;
    }
    else
    {
        features.pNext = &vulkan11;
    }
    vkGetPhysicalDeviceFeatures2(physical_, &features);

    bool partially_bound = false;
    bool non_uniform = false;
    const bool uab_sampled =
        core_indexing
            ? vulkan12.descriptorBindingSampledImageUpdateAfterBind != 0U
            : (extension_indexing ? indexing.descriptorBindingSampledImageUpdateAfterBind != 0U
                                  : false);
    const bool uab_storage =
        core_indexing
            ? vulkan12.descriptorBindingStorageBufferUpdateAfterBind != 0U
            : (extension_indexing ? indexing.descriptorBindingStorageBufferUpdateAfterBind != 0U
                                  : false);
    if (core_indexing)
    {
        partially_bound = vulkan12.descriptorBindingPartiallyBound;
        non_uniform = vulkan12.shaderSampledImageArrayNonUniformIndexing;
    }
    else if (extension_indexing)
    {
        partially_bound = indexing.descriptorBindingPartiallyBound;
        non_uniform = indexing.shaderSampledImageArrayNonUniformIndexing;
    }
    /* RESONATE_FORCE_COMPAT exercises the compat shaders on a capable device. */
    bindless_ = partially_bound && non_uniform &&
                std::getenv("RESONATE_FORCE_COMPAT") == nullptr;
    /* A table slot is rewritten while frames may still sample the set; only a
       device with update-after-bind lets the write skip the idle wait. */
    update_after_bind_ = bindless_ && uab_sampled && uab_storage;

    const bool dynamic_buffers = features.features.shaderStorageBufferArrayDynamicIndexing;
    const bool dynamic_images = features.features.shaderSampledImageArrayDynamicIndexing;
    if (!dynamic_buffers || !dynamic_images)
    {
        const std::string detail =
            "device lacks the table features (" + std::string(properties.deviceName) +
            ", Vulkan " + std::to_string(VK_VERSION_MAJOR(properties.apiVersion)) + "." +
            std::to_string(VK_VERSION_MINOR(properties.apiVersion)) +
            ", dynamicBuffers=" + std::to_string(dynamic_buffers != 0U) +
            ", dynamicImages=" + std::to_string(dynamic_images != 0U) + ")";
        note(device_failed_, detail.c_str());
        return RESONATE_E_UNSUPPORTED;
    }

    VkPhysicalDeviceVulkan11Features enabled11{};
    enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    /* No shader reads the base-vertex builtin since the fullscreen triangle
       moved to a vertex buffer, but enable it where the driver has it. */
    enabled11.shaderDrawParameters =
        vulkan11.shaderDrawParameters != 0U ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceDescriptorIndexingFeatures enabled_indexing{};
    enabled_indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
    void* interface_chain = &enabled11;
    if (bindless_ && core_indexing)
    {
        enabled12.descriptorBindingPartiallyBound = VK_TRUE;
        enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        if (update_after_bind_)
        {
            enabled12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            enabled12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        }
        enabled12.pNext = &enabled11;
        interface_chain = &enabled12;
    }
    else if (bindless_)
    {
        enabled_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        enabled_indexing.descriptorBindingPartiallyBound = VK_TRUE;
        if (update_after_bind_)
        {
            enabled_indexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            enabled_indexing.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        }
        enabled_indexing.pNext = &enabled11;
        interface_chain = &enabled_indexing;
    }

    VkPhysicalDeviceFeatures2 enabled_features{};
    enabled_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled_features.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    enabled_features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    enabled_features.pNext = interface_chain;

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
    if (bindless_ && !core_indexing)
    {
        device_extensions[device_extension_count++] = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
    }
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled_features;
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

    /* Timestamp queries are the only honest per-pass GPU timer; a device whose
       queue family has no valid timestamp bits gets none. */
    if (profileEnabled())
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        uint32_t family_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
        if (queue_family_ < family_count && properties.limits.timestampPeriod > 0.0F &&
            families[queue_family_].timestampValidBits >= kTimestampBits)
        {
            timestamp_period_ns_ = properties.limits.timestampPeriod;
        }
        std::fprintf(stderr,
                     "render-vulkan: timestamps period=%.2f ns, valid bits=%u, enabled=%d\n",
                     static_cast<double>(properties.limits.timestampPeriod),
                     queue_family_ < family_count ? families[queue_family_].timestampValidBits : 0U,
                     timestamp_period_ns_ > 0.0F ? 1 : 0);
    }

    /* The table's per-stage size is what decides whether the shaders can be
       compiled at all; a mobile driver with small limits refuses the pipeline
       rather than the layout. */
    VkFormatProperties color_props{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R16G16B16A16_SFLOAT, &color_props);
    VkFormatProperties depth_props{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D32_SFLOAT, &depth_props);
    VkFormatProperties packed_props{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                        &packed_props);
    VkFormatProperties d16_props{};
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_D16_UNORM, &d16_props);
    std::fprintf(stderr,
                 "render-vulkan: %s, Vulkan %u.%u, %s table=%u, samplers/stage=%u, "
                 "storage/stage=%u, resources/stage=%u, set samplers=%u set images=%u, "
                 "rgba16f color=%u, d32 depth=%u, b10g11r11 color=%u, d16 depth=%u\n",
                 properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
                 VK_VERSION_MINOR(properties.apiVersion), bindless_ ? "bindless" : "compat",
                 kImageTableSize, properties.limits.maxPerStageDescriptorSamplers,
                 properties.limits.maxPerStageDescriptorStorageBuffers,
                 properties.limits.maxPerStageResources,
                 properties.limits.maxDescriptorSetSamplers,
                 properties.limits.maxDescriptorSetSampledImages,
                 (color_props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U,
                 (depth_props.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U,
                 (packed_props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0U,
                 (d16_props.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U);

    /* Which of the scene formats support 2x/4x multisampling here: an MSAA
       attempt has to pick a sample count the images and the pass agree on, and
       a tile GPU is narrow about 16-bit HDR formats. One line, once. */
    {
        const auto samples_of = [&](VkFormat format, VkImageUsageFlags usage)
        {
            VkImageFormatProperties properties{};
            if (vkGetPhysicalDeviceImageFormatProperties(
                    physical_, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0,
                    &properties) != VK_SUCCESS)
            {
                return VkSampleCountFlags{0};
            }
            return properties.sampleCounts;
        };
        const VkImageUsageFlags color_usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        const auto describe = [](VkSampleCountFlags bits) -> const char*
        {
            if ((bits & VK_SAMPLE_COUNT_4_BIT) != 0U)
            {
                return (bits & VK_SAMPLE_COUNT_2_BIT) != 0U ? "2,4" : "4";
            }
            return (bits & VK_SAMPLE_COUNT_2_BIT) != 0U ? "2" : "-";
        };
        std::fprintf(stderr, "render-vulkan: samples b10g11r11=%s rgba16f=%s depth16=%s depth32=%s\n",
                     describe(samples_of(VK_FORMAT_B10G11R11_UFLOAT_PACK32, color_usage)),
                     describe(samples_of(VK_FORMAT_R16G16B16A16_SFLOAT, color_usage)),
                     describe(samples_of(VK_FORMAT_D16_UNORM, depth_usage)),
                     describe(samples_of(VK_FORMAT_D32_SFLOAT, depth_usage)));
    }

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

    /* Table slots start low and are handed out in creation order; the handle's
       pool index is independent, so vertex and index buffers do not consume
       them. */
    free_storage_slots_.clear();
    for (uint32_t slot = kStorageTableSize; slot-- > 0U;)
    {
        free_storage_slots_.push_back(slot);
    }
    free_image_slots_.clear();
    for (uint32_t slot = kImageTableSize; slot-- > 0U;)
    {
        free_image_slots_.push_back(slot);
    }

    /* The resource tables: set 0, bindings 0-4 = kStorageTableSize storage
       buffers each, binding 5 = kImageTableSize combined image samplers. The
       storage slots are dummy-filled, so an index nobody wrote reads defined
       zeros; the image slots are partially bound when the device supports it
       and dummy-filled otherwise. */
    VkDescriptorSetLayoutBinding table_bindings[kTableBindingCount] = {};
    for (uint32_t binding = 0; binding < kStorageTableBindings; ++binding)
    {
        table_bindings[binding].binding = binding;
        table_bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        table_bindings[binding].descriptorCount = kStorageTableSize;
        table_bindings[binding].stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        table_bindings[binding].pImmutableSamplers = nullptr;
    }
    /* R13: the image table carries images only — the sampler is the shader's
       pick from the palette at binding 8, not a property of the slot. */
    table_bindings[kSampledTableBinding].binding = kSampledTableBinding;
    table_bindings[kSampledTableBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    table_bindings[kSampledTableBinding].descriptorCount = kImageTableSize;
    table_bindings[kSampledTableBinding].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    table_bindings[kShadowTableBinding].binding = kShadowTableBinding;
    table_bindings[kShadowTableBinding].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    table_bindings[kShadowTableBinding].descriptorCount = kShadowTableSize;
    table_bindings[kShadowTableBinding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    table_bindings[kSamplerTableBinding].binding = kSamplerTableBinding;
    table_bindings[kSamplerTableBinding].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    table_bindings[kSamplerTableBinding].descriptorCount = kSamplerTableSize;
    table_bindings[kSamplerTableBinding].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    /* The frame block, outside the table so the vertex stage does not depend
       on a storage-array read. */
    table_bindings[kFrameBinding].binding = kFrameBinding;
    table_bindings[kFrameBinding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    table_bindings[kFrameBinding].descriptorCount = 1;
    table_bindings[kFrameBinding].stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorBindingFlags table_flags = 0U;
    if (bindless_)
    {
        table_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        if (update_after_bind_)
        {
            table_flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        }
    }
    VkDescriptorBindingFlags binding_flags[kTableBindingCount] = {};
    for (uint32_t binding = 0; binding < kTableBindingCount; ++binding)
    {
        /* The frame block is written once at setup; only the tables are
           rewritten while frames are in flight. */
        binding_flags[binding] = binding == kFrameBinding ? 0U : table_flags;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
    binding_flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    binding_flags_info.bindingCount = kTableBindingCount;
    binding_flags_info.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutCreateInfo table_layout{};
    table_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    table_layout.pNext = bindless_ ? &binding_flags_info : nullptr;
    table_layout.bindingCount = kTableBindingCount;
    table_layout.pBindings = table_bindings;
    if (vkCreateDescriptorSetLayout(device_, &table_layout, nullptr, &table_layout_) != VK_SUCCESS)
    {
        note(device_failed_, "table layout creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkDescriptorPoolSize table_pool_sizes[5] = {};
    table_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    table_pool_sizes[0].descriptorCount = kStorageTableBindings * kStorageTableSize;
    table_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    table_pool_sizes[1].descriptorCount = kShadowTableSize;
    table_pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    table_pool_sizes[2].descriptorCount = kImageTableSize;
    table_pool_sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    table_pool_sizes[3].descriptorCount = kSamplerTableSize;
    table_pool_sizes[4].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    table_pool_sizes[4].descriptorCount = 1;
    VkDescriptorPoolCreateInfo table_pool{};
    table_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    table_pool.flags =
        update_after_bind_ ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0U;
    table_pool.maxSets = 1;
    table_pool.poolSizeCount = 5;
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
    for (uint32_t slot = 0; slot < kStorageTableSize; ++slot)
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
    /* Linear transitions between levels: an uploaded texture carries a mip
       chain (R8-class minification quality) and a level-zero-only image simply
       clamps to its single level. */
    table_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    table_sampler.maxLod = VK_LOD_CLAMP_NONE;
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
    /* The shadow sampler compares on read: a filtered tap is four comparisons
       (2x2), and LESS_OR_EQUAL is "receiver no farther than the stored
       depth", which returns 1 (lit) for a visible surface. */
    table_sampler.minFilter = VK_FILTER_LINEAR;
    table_sampler.magFilter = VK_FILTER_LINEAR;
    table_sampler.compareEnable = VK_TRUE;
    table_sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(device_, &table_sampler, nullptr, &shadow_sampler_) != VK_SUCCESS)
    {
        note(device_failed_, "shadow sampler creation failed");
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

    if (!bindless_)
    {
        if (ResonateStatus status = createTableImageDummy(); status != RESONATE_OK)
        {
            return status;
        }
    }
    if (ResonateStatus status = createShadowDummy(); status != RESONATE_OK)
    {
        return status;
    }
    if (ResonateStatus status = createSamplerTable(); status != RESONATE_OK)
    {
        return status;
    }
    return RESONATE_OK;
}

ResonateStatus Context::createTableImageDummy()
{
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = VkExtent3D{1U, 1U, 1U};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &image, &allocation, &table_dummy_image_,
                       &table_dummy_image_allocation_, nullptr) != VK_SUCCESS)
    {
        note(device_failed_, "table dummy image creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = table_dummy_image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view, nullptr, &table_dummy_view_) != VK_SUCCESS)
    {
        vmaDestroyImage(allocator_, table_dummy_image_, table_dummy_image_allocation_);
        table_dummy_image_ = VK_NULL_HANDLE;
        note(device_failed_, "table dummy image view creation failed");
        return RESONATE_E_INTERNAL;
    }

    /* The dummy is white and ready to sample from the first frame: a draw that
       names a slot before its real image exists gets a neutral albedo. */
    VkCommandBufferAllocateInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = command_pool_;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &command, &buffer) != VK_SUCCESS)
    {
        note(device_failed_, "table dummy image transition failed");
        return RESONATE_E_INTERNAL;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(buffer, &begin);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = table_dummy_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &barrier);
    const VkClearColorValue white{{1.0F, 1.0F, 1.0F, 1.0F}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    vkCmdClearColorImage(buffer, table_dummy_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white,
                         1U, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0U, 0U, nullptr, 0U, nullptr, 1U, &barrier);
    vkEndCommandBuffer(buffer);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer;
    const VkResult submitted = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &buffer);
    if (submitted != VK_SUCCESS)
    {
        note(device_failed_, "table dummy image transition failed");
        return RESONATE_E_INTERNAL;
    }

    for (uint32_t slot = 0; slot < kImageTableSize; ++slot)
    {
        writeImageSlot(slot, table_dummy_view_, false);
    }
    return RESONATE_OK;
}

ResonateStatus Context::ensureStaging(VkDeviceSize bytes)
{
    if (staging_buffer_ != VK_NULL_HANDLE && staging_capacity_ >= bytes)
    {
        return RESONATE_OK;
    }
    if (staging_buffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator_, staging_buffer_, staging_allocation_);
        staging_buffer_ = VK_NULL_HANDLE;
        staging_allocation_ = nullptr;
        staging_mapped_ = nullptr;
        staging_capacity_ = 0U;
    }

    /* Growth is doubling from 4 MB, so a run's largest upload sets the high
       water mark and every later upload reuses it. */
    VkDeviceSize capacity = staging_capacity_ == 0U ? 4U * 1024U * 1024U : staging_capacity_ * 2U;
    while (capacity < bytes)
    {
        capacity *= 2U;
    }

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = capacity;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(allocator_, &info, &allocation, &staging_buffer_, &staging_allocation_,
                        &allocation_info) != VK_SUCCESS)
    {
        return RESONATE_E_INTERNAL;
    }
    staging_mapped_ = allocation_info.pMappedData;
    staging_capacity_ = capacity;
    ++staging_allocations_;
    std::fprintf(stderr, "render-vulkan: staging buffer %.1f MB (allocation #%u)\n",
                 static_cast<double>(capacity) / (1024.0 * 1024.0), staging_allocations_);
    return RESONATE_OK;
}

/* The sampler palette is written once at setup: it is a fixed set, and the
   shaders pick an entry at the sampling site (R13). */
ResonateStatus Context::createSamplerTable()
{
    for (uint32_t slot = 0; slot < kSamplerTableSize; ++slot)
    {
        writeSamplerSlot(slot, slot == 1U ? table_sampler_nearest_ : table_sampler_linear_);
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

    if (surface_ != VK_NULL_HANDLE && attached_surface_ != native_surface)
    {
        /* The platform replaced the window (Android does this on rotation); the
           surface and everything built from it belong to the old one. */
        trace("surface replaced");
        if (device_ != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device_);
        }
        destroySwapchainObjects();
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
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
#elif defined(RESONATE_PLATFORM_ANDROID)
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = reinterpret_cast<ANativeWindow*>(const_cast<void*>(native_surface));
        if (vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS)
        {
            note(surface_failed_, "surface creation failed");
            return RESONATE_E_INTERNAL;
        }
#else
        note(platform_unsupported_, "no surface backend on this platform yet");
        return RESONATE_E_UNSUPPORTED;
#endif
    }
    attached_surface_ = native_surface;

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

uint32_t Context::featureBits() const
{
    return bindless_ ? RESONATE_RENDER_FEATURE_DESCRIPTOR_INDEXING : 0U;
}

ResonateRenderFormat Context::surfaceFormat() const
{
    switch (surface_format_.format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB:
            return RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return RESONATE_RENDER_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return RESONATE_RENDER_FORMAT_B8G8R8A8_UNORM;
        default:
            return RESONATE_RENDER_FORMAT_UNKNOWN;
    }
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
    /* The shaders end the frame in linear light; an sRGB swapchain encodes on
       write. A linear swapchain would show the scene unencoded and dark unless
       the final pass encodes itself — the configured color space decides who
       owns the transfer function. */
    if (prefer_srgb_)
    {
        bool srgb_surface = false;
        for (const VkSurfaceFormatKHR& format : formats)
        {
            const bool srgb = format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                              format.format == VK_FORMAT_R8G8B8A8_SRGB;
            if (srgb && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                surface_format_ = format;
                srgb_surface = true;
                break;
            }
        }
        if (!srgb_surface)
        {
            std::fprintf(stderr, "render-vulkan: no sRGB surface format; the final pass encodes\n");
        }
    }
    std::fprintf(stderr, "render-vulkan: swapchain format %u\n",
                 static_cast<unsigned>(surface_format_.format));

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
       frame of latency for never tearing, immediate is the uncapped fallback.
       A named present mode wins over the vsync bool; a preference the surface
       cannot do falls to the next one and FIFO is the floor. */
    VkPresentModeKHR preferences[2] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR};
    uint32_t preference_count = 1U;
    if (present_mode_override_ >= 0)
    {
        preferences[0] = present_mode_override_ == 1   ? VK_PRESENT_MODE_MAILBOX_KHR
                         : present_mode_override_ == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                       : VK_PRESENT_MODE_FIFO_KHR;
    }
    else if (!vsync_)
    {
        preferences[0] = VK_PRESENT_MODE_MAILBOX_KHR;
        preferences[1] = VK_PRESENT_MODE_IMMEDIATE_KHR;
        preference_count = 2U;
    }
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    for (uint32_t preference = 0U; preference < preference_count; ++preference)
    {
        if (std::find(modes.begin(), modes.end(), preferences[preference]) != modes.end())
        {
            present_mode_ = preferences[preference];
            break;
        }
    }

    /* The compositor rotates the image to the display; claiming the current
       transform would say the image is already rotated, which it is not. */
    const VkSurfaceTransformFlagBitsKHR pre_transform =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0U
            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
            : caps.currentTransform;

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format_.format;
    info.imageColorSpace = surface_format_.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    /* TRANSFER_SRC is what the debug canvas and texel trace read back. */
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = pre_transform;
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
    swapchain_images_ = images;
    discardMsaa();

    if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN, 1, 0,
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
    discardMsaa();

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

    if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN, 1, 0,
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
    swapchain_images_.clear();
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
        /* The canvas pass is cached like any other; drop its entry by value so
           the destructor's loop below does not destroy the object twice. */
        for (auto entry = pass_cache_.begin(); entry != pass_cache_.end(); ++entry)
        {
            if (entry->second == render_pass_)
            {
                pass_cache_.erase(entry);
                break;
            }
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

bool Context::recreateSwapchain()
{
    trace("recreate swapchain");
    vkDeviceWaitIdle(device_);
    destroySwapchainObjects();
    /* A failed recreation leaves no swapchain behind; the caller turns that
       into E_STATE instead of acquiring from a null handle. */
    return createSwapchain(canvas_width_, canvas_height_) == RESONATE_OK &&
           swapchain_ != VK_NULL_HANDLE;
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
        case RESONATE_RENDER_FORMAT_R32G32B32A32_SFLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case RESONATE_RENDER_FORMAT_B10G11R11_UFLOAT:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case RESONATE_RENDER_FORMAT_D16_UNORM:
            return VK_FORMAT_D16_UNORM;
        case RESONATE_RENDER_FORMAT_D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case RESONATE_RENDER_FORMAT_UNKNOWN:
        default:
            return surface_format_.format;
    }
}

/* The pass's sample count: 0 and 1 read as 1x; anything the format survey
   never answered falls back to 1x rather than failing the pass. Shared with
   the pipeline path in rhi.cpp. */
VkSampleCountFlagBits sampleCountOf(uint32_t samples)
{
    switch (samples)
    {
        case 2U:
            return VK_SAMPLE_COUNT_2_BIT;
        case 4U:
            return VK_SAMPLE_COUNT_4_BIT;
        case 8U:
            return VK_SAMPLE_COUNT_8_BIT;
        default:
            return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkSampleCountFlags Context::sampleCountsOf(VkFormat format, bool depth_usage)
{
    const uint32_t key = static_cast<uint32_t>(format) << 1U | (depth_usage ? 1U : 0U);
    if (const auto found = sample_counts_.find(key); found != sample_counts_.end())
    {
        return found->second;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    usage |= depth_usage ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageFormatProperties properties{};
    VkSampleCountFlags bits = 0U;
    if (vkGetPhysicalDeviceImageFormatProperties(physical_, format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                 &properties) == VK_SUCCESS)
    {
        bits = properties.sampleCounts;
    }
    sample_counts_.emplace(key, bits);
    return bits;
}

uint32_t Context::clampedSamples(ResonateRenderFormat color, ResonateRenderFormat depth,
                                 uint32_t requested)
{
    if (requested <= 1U)
    {
        return 1U;
    }
    if (color == RESONATE_RENDER_FORMAT_UNKNOWN)
    {
        color = surfaceFormat();
    }
    if (depth == RESONATE_RENDER_FORMAT_UNKNOWN)
    {
        depth = RESONATE_RENDER_FORMAT_NONE;
    }
    for (const uint32_t candidate : {8U, 4U, 2U})
    {
        if (candidate > requested)
        {
            continue;
        }
        const VkSampleCountFlags bit = static_cast<VkSampleCountFlags>(candidate);
        if (color != RESONATE_RENDER_FORMAT_NONE &&
            (sampleCountsOf(formatOf(color), false) & bit) == 0U)
        {
            continue;
        }
        if (depth != RESONATE_RENDER_FORMAT_NONE &&
            (sampleCountsOf(formatOf(depth), true) & bit) == 0U)
        {
            continue;
        }
        if (candidate != requested && !msaa_clamped_noted_)
        {
            msaa_clamped_noted_ = true;
            std::fprintf(stderr,
                         "render-vulkan: %ux MSAA is unsupported for these formats; using %ux\n",
                         requested, candidate);
        }
        return candidate;
    }
    if (!msaa_clamped_noted_)
    {
        msaa_clamped_noted_ = true;
        std::fprintf(stderr,
                     "render-vulkan: %ux MSAA is unsupported for these formats; using 1x\n",
                     requested);
    }
    return 1U;
}

ResonateStatus Context::renderPassFor(ResonateRenderFormat color, ResonateRenderFormat depth,
                                      uint32_t samples, uint32_t ops, VkRenderPass* out)
{
    samples = sampleCountOf(samples);
    const uint64_t key = static_cast<uint64_t>(color) | (static_cast<uint64_t>(depth) << 8) |
                         (static_cast<uint64_t>(samples) << 16) | (static_cast<uint64_t>(ops & 0xFFU) << 24);
    const auto found = pass_cache_.find(key);
    if (found != pass_cache_.end())
    {
        *out = found->second;
        return RESONATE_OK;
    }

    /* Depth-only: color NONE and a depth format make a pass with no color
       attachment (shadow maps). Shadow passes stay 1x: their depth is an
       explicit persistent image, not a transient. */
    const bool colorless = color == RESONATE_RENDER_FORMAT_NONE;
    const bool multisampled = samples > VK_SAMPLE_COUNT_1_BIT && !colorless;
    /* The pass's attachments all render at one sample count; the actions pick
       bits of the ops mask. What an attachment loads and stores is outside
       render-pass compatibility, so pipelines may share a pass whose actions
       differ (IGL I3). */
    const VkSampleCountFlagBits sample_flags = static_cast<VkSampleCountFlagBits>(samples);
    const bool color_loads = (ops & 1U) != 0U;
    const bool color_transient = (ops & 2U) != 0U;
    const bool depth_loads = (ops & 4U) != 0U;
    const bool depth_transient = (ops & 8U) != 0U;

    /* A multisampled pass is the Filament shape: the device's transient color
       at `samples` stores nothing; the 1x color target is the resolve and
       stores; the D32F depth clears and stores nothing. The ops mask does not
       reach this shape. */
    VkAttachmentDescription attachments[3] = {};
    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    /* Outside the conditional: the subpass points at it until the render pass
       is created. */
    VkAttachmentReference depth_reference{};
    VkAttachmentReference resolve_reference{};
    resolve_reference.attachment = 1;
    resolve_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    if (multisampled)
    {
        attachments[0].format = formatOf(color);
        attachments[0].samples = sample_flags;
        /* The engine's contract is that a pass clears, so the transient color
           clears too: a pass that draws nothing still resolves the clear. */
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        attachments[1].format = formatOf(color);
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        /* Only a swapchain image ends a pass in the present layout; an
           offscreen target stays a color attachment, which is where readback
           picks it up. */
        attachments[1].finalLayout = color == RESONATE_RENDER_FORMAT_UNKNOWN && !offscreen_
                                         ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        attachments[2].format = VK_FORMAT_D32_SFLOAT;
        attachments[2].samples = sample_flags;
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    if (!colorless && !multisampled)
    {
        attachments[0].format = formatOf(color);
        attachments[0].samples = sample_flags;
        attachments[0].loadOp = color_loads ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp =
            color_transient ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
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
        if (multisampled)
        {
            subpass.pResolveAttachments = &resolve_reference;
        }
        attachment_count = multisampled ? 2U : 1U;
    }

    if (multisampled)
    {
        depth_reference.attachment = 2;
        depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.pDepthStencilAttachment = &depth_reference;
        attachment_count = 3;
    }
    else if (depth != RESONATE_RENDER_FORMAT_UNKNOWN)
    {
        depth_reference.attachment = colorless ? 0 : 1;
        depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        const uint32_t depth_index = colorless ? 0 : 1;
        attachments[depth_index].format = formatOf(depth);
        attachments[depth_index].samples = sample_flags;
        attachments[depth_index].loadOp =
            depth_loads ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[depth_index].storeOp =
            depth_transient ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
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
    /* A pass that ends in the present layout feeds the presentation engine,
       which is external to the pass: the dependency makes the colour writes
       and the final transition available before that read. The Mali-G51 drops
       the last pass's fragment work without it (a frame shows the clear
       colour alone) while the same pass keeps drawing on Adreno and desktop
       drivers, so the dependency is not optional. */
    VkSubpassDependency present_dependency{};
    if (!colorless && color == RESONATE_RENDER_FORMAT_UNKNOWN && !offscreen_)
    {
        present_dependency.srcSubpass = 0;
        present_dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        present_dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        present_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        present_dependency.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        present_dependency.dstAccessMask = 0;
        pass.dependencyCount = 1;
        pass.pDependencies = &present_dependency;
    }
    VkRenderPass handle = VK_NULL_HANDLE;
    const VkResult created = vkCreateRenderPass(device_, &pass, nullptr, &handle);
    if (created != VK_SUCCESS)
    {
        std::fprintf(stderr, "render-vulkan: render pass creation failed (%d, color %d, depth %d)\n",
                     static_cast<int>(created), static_cast<int>(color),
                     static_cast<int>(depth));
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

bool Context::acquireTableSlot(std::vector<uint32_t>& free_slots, uint32_t& out_slot)
{
    if (free_slots.empty())
    {
        return false;
    }
    out_slot = free_slots.back();
    free_slots.pop_back();
    return true;
}

void Context::releaseTableSlot(std::vector<uint32_t>& free_slots, uint32_t slot)
{
    free_slots.push_back(slot);
}

uint32_t Context::bufferSlot(ResonateRenderBuffer handle) const
{
    const BufferSlot* slot =
        handle.value != 0
            ? buffers_.live(handleIndex(handle.value), handleGeneration(handle.value))
            : nullptr;
    return slot != nullptr && slot->in_table ? slot->table_slot : 0U;
}

ResonateStatus Context::bindImageSlot(uint32_t slot, ResonateRenderImage handle, bool nearest)
{
    if (device_ == VK_NULL_HANDLE || slot >= kImageTableSize)
    {
        return RESONATE_E_INVALID;
    }
    const ImageSlot* image =
        handle.value != 0 ? images_.live(handleIndex(handle.value), handleGeneration(handle.value))
                          : nullptr;
    if (image == nullptr || !image->in_table)
    {
        return RESONATE_E_INVALID;
    }
    writeImageSlot(slot, image->view, nearest);
    return RESONATE_OK;
}

uint32_t Context::imageSlot(ResonateRenderImage handle) const
{
    const ImageSlot* slot =
        handle.value != 0 ? images_.live(handleIndex(handle.value), handleGeneration(handle.value))
                          : nullptr;
    return slot != nullptr && slot->in_table ? slot->table_slot : 0U;
}

ResonateStatus Context::nativeImage(ResonateRenderImage handle,
                                    ResonateRenderNativeImage* out) const
{
    if (out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    const ImageSlot* slot =
        handle.value != 0 ? images_.live(handleIndex(handle.value), handleGeneration(handle.value))
                          : nullptr;
    if (slot == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    out->image = reinterpret_cast<std::uint64_t>(slot->image);
    out->format = static_cast<std::uint32_t>(slot->format);
    /* The RHI leaves sampled images in this layout after creation and upload;
       the caller owns the transition from here. */
    out->layout = static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    out->width = slot->width;
    out->height = slot->height;
    return RESONATE_OK;
}

void Context::updateTable(const VkWriteDescriptorSet* writes, uint32_t count)
{
    if (device_ == VK_NULL_HANDLE || writes == nullptr || count == 0U)
    {
        return;
    }
    if (!update_after_bind_)
    {
        /* Without update-after-bind the set must not be written while any frame
           may read it; table writes are rare, so the idle wait is the honest
           price. */
        vkDeviceWaitIdle(device_);
    }
    vkUpdateDescriptorSets(device_, count, writes, 0, nullptr);
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
    updateTable(writes, kStorageTableBindings);
}

void Context::writeImageSlot(uint32_t slot, VkImageView view, bool nearest)
{
    /* R13 moved the sampler choice into the shaders, so the slot carries an
       image only; `nearest` is accepted and ignored for ABI stability. */
    (void)nearest;
    VkDescriptorImageInfo image_info{};
    image_info.imageView = view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = table_set_;
    write.dstBinding = kSampledTableBinding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    updateTable(&write, 1);
}

void Context::writeSamplerSlot(uint32_t slot, VkSampler sampler)
{
    VkDescriptorImageInfo image_info{};
    image_info.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = table_set_;
    write.dstBinding = kSamplerTableBinding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image_info;
    updateTable(&write, 1);
}

void Context::writeShadowSlot(uint32_t slot, VkImageView view)
{
    VkDescriptorImageInfo image_info{};
    image_info.sampler = shadow_sampler_;
    image_info.imageView = view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = table_set_;
    write.dstBinding = kShadowTableBinding;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    updateTable(&write, 1);
}

ResonateStatus Context::setShadowTable(const ResonateRenderImage* images, uint32_t count)
{
    if (device_ == VK_NULL_HANDLE || table_set_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (images == nullptr || count == 0U || count > kShadowTableSize)
    {
        return RESONATE_E_INVALID;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        const ImageSlot* image = images[index].value != 0
                                     ? images_.live(handleIndex(images[index].value),
                                                    handleGeneration(images[index].value))
                                     : nullptr;
        if (image == nullptr || !image->depth)
        {
            return RESONATE_E_INVALID;
        }
    }
    for (uint32_t index = count; index < kShadowTableSize; ++index)
    {
        writeShadowSlot(index, shadow_dummy_view_);
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        const ImageSlot* image = images_.live(handleIndex(images[index].value),
                                              handleGeneration(images[index].value));
        writeShadowSlot(index, image->view);
    }
    return RESONATE_OK;
}

ResonateStatus Context::createShadowDummy()
{
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D32_SFLOAT;
    image.extent = VkExtent3D{1U, 1U, 1U};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &image, &allocation, &shadow_dummy_image_,
                       &shadow_dummy_allocation_, nullptr) != VK_SUCCESS)
    {
        note(device_failed_, "shadow dummy image creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = shadow_dummy_image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_D32_SFLOAT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view, nullptr, &shadow_dummy_view_) != VK_SUCCESS)
    {
        vmaDestroyImage(allocator_, shadow_dummy_image_, shadow_dummy_allocation_);
        shadow_dummy_image_ = VK_NULL_HANDLE;
        note(device_failed_, "shadow dummy view creation failed");
        return RESONATE_E_INTERNAL;
    }

    /* Cleared to the far plane: a stub compare reads "not shadowed". */
    VkCommandBufferAllocateInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = command_pool_;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &command, &buffer) != VK_SUCCESS)
    {
        note(device_failed_, "shadow dummy transition allocation failed");
        return RESONATE_E_INTERNAL;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(buffer, &begin);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = shadow_dummy_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &barrier);
    const VkClearDepthStencilValue far_plane{1.0F, 0U};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U};
    vkCmdClearDepthStencilImage(buffer, shadow_dummy_image_,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far_plane, 1U, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                         &barrier);
    vkEndCommandBuffer(buffer);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer;
    const VkResult submitted = vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &buffer);
    if (submitted != VK_SUCCESS)
    {
        note(device_failed_, "shadow dummy transition failed");
        return RESONATE_E_INTERNAL;
    }

    for (uint32_t slot = 0; slot < kShadowTableSize; ++slot)
    {
        writeShadowSlot(slot, shadow_dummy_view_);
    }
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

ResonateStatus Context::setPresentMode(uint32_t mode)
{
    VkPresentModeKHR requested = VK_PRESENT_MODE_FIFO_KHR;
    switch (mode)
    {
    case RESONATE_RENDER_PRESENT_FIFO:
        requested = VK_PRESENT_MODE_FIFO_KHR;
        break;
    case RESONATE_RENDER_PRESENT_MAILBOX:
        requested = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
    case RESONATE_RENDER_PRESENT_IMMEDIATE:
        requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
        break;
    default:
        return RESONATE_E_INVALID;
    }
    const int named = static_cast<int>(mode);
    if (named != present_mode_override_)
    {
        present_mode_override_ = named;
        /* The swapchain owns the mode: the next frame builds a new one. */
        recreate_pending_ = true;
    }
    return RESONATE_OK;
}

ResonateStatus Context::beginFrame()
{
    if (device_lost_)
    {
        return RESONATE_E_STATE;
    }
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
        if (width != canvas_width_ || height != canvas_height_)
        {
            trace("resize %ux%u -> %ux%u", canvas_width_, canvas_height_, width, height);
        }
        canvas_width_ = width;
        canvas_height_ = height;
        recreate_pending_ = false;
        if (!recreateSwapchain())
        {
            note(surface_failed_, "the swapchain could not be recreated");
            return RESONATE_E_STATE;
        }
    }

    Frame& frame = frames_[frame_ % kFramesInFlight];
    const std::size_t slot = frame_ % kFramesInFlight;
    if (frame.fence_pending)
    {
        /* A submission that never signals its fence must not hang the render
           thread forever: five seconds and the device is written off, the same
           way a failed submit is. */
        const VkResult waited = vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE,
                                                5ULL * 1000ULL * 1000ULL * 1000ULL);
        if (waited == VK_TIMEOUT)
        {
            /* The command buffer may still be executing; the device is written
               off so no later frame rewrites it under the GPU. */
            device_lost_ = true;
            note(device_failed_, "the GPU did not finish the previous frame within five seconds");
            return RESONATE_E_STATE;
        }
    }
    if (trace_submit_ns_[slot] != 0U)
    {
        /* From that slot's submit to its fence: how long the GPU held the
           frame. Sampled after the wait, so it is the real duration. */
        const std::uint64_t done = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        gpu_time_ms += static_cast<double>(done - trace_submit_ns_[slot]) / 1.0e6;
        ++gpu_time_frames;
        if (profileEnabled() && gpu_time_frames >= 30U)
        {
            std::fprintf(stderr, "render-vulkan: gpu frame %.2f ms\n",
                         gpu_time_ms / static_cast<double>(gpu_time_frames));
            gpu_time_ms = 0.0;
            gpu_time_frames = 0U;
        }
        trace_submit_ns_[slot] = 0U;
    }
    drainGraveyard(frame.graveyard);
    tracePreviousTexel();

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
            vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.image_available,
                                  VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            trace("acquire out of date");
            if (!recreateSwapchain())
            {
                note(surface_failed_, "the swapchain could not be recreated");
                return RESONATE_E_STATE;
            }
            acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                             frame.image_available, VK_NULL_HANDLE, &image_index);
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            trace("acquire failed %d", static_cast<int>(acquired));
            note(surface_failed_, "image acquisition failed");
            return RESONATE_E_STATE;
        }
        /* A suboptimal acquire is a hint, not a reason to drain and rebuild the
           swapchain every frame; the present path schedules the rebuild when
           the surface is actually out of date. */
        recreate_pending_ = false;
        image_index_ = image_index;
        trace("acquire result=%d image=%u", static_cast<int>(acquired), image_index);
    }

    vkResetFences(device_, 1, &frame.in_flight);
    vkResetCommandBuffer(frame.command, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(frame.command, &begin);
    profileReport(slot);
    if (timestamp_period_ns_ > 0.0F)
    {
        if (profile_pool_[slot] == VK_NULL_HANDLE)
        {
            VkQueryPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            pool.queryType = VK_QUERY_TYPE_TIMESTAMP;
            pool.queryCount = kMaxPassTimestamps;
            if (vkCreateQueryPool(device_, &pool, nullptr, &profile_pool_[slot]) != VK_SUCCESS)
            {
                profile_pool_[slot] = VK_NULL_HANDLE;
                timestamp_period_ns_ = 0.0F;
            }
        }
        if (profile_pool_[slot] != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(frame.command, profile_pool_[slot], 0, kMaxPassTimestamps);
            profile_count_[slot] = 0U;
            profileTimestamp(frame.command);
        }
    }

    if (kFramesInFlight > 1U)
    {
        /* With more than one frame in flight the frame starts after everything
           submitted before it: the graph's barriers name a pass's own resources,
           which covers the passes inside one frame but not the frame in flight.
           Measured on the Mali-G51 with an outside submitter on the queue: ~2
           dropped passes per thousand frames without this dependency, none with
           it. */
        VkMemoryBarrier frame_start{};
        frame_start.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        frame_start.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        frame_start.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 1U, &frame_start, 0U,
                             nullptr, 0U, nullptr);
    }

    /* Every pipeline reads the resource table as set 0, so the frame binds it
       once at each bind point. The table and its layout come with the first
       pipeline; a frame that runs no pipeline binds nothing. */
    if (pipeline_layout_ != VK_NULL_HANDLE)
    {
        VkDescriptorSet set = table_set_;
        vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                1, &set, 0, nullptr);
        vkCmdBindDescriptorSets(frame.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0,
                                1, &set, 0, nullptr);
    }
    frame_open_ = true;
    return RESONATE_OK;
}

/* The transient MSAA target for a samples>1 pass: color at the target's
   format, depth at D32F, both TRANSIENT so the allocator can tile them away. */
Context::MsaaTarget& Context::ensureMsaa(uint32_t width, uint32_t height,
                                         ResonateRenderFormat color, uint32_t samples)
{
    const uint64_t key = static_cast<uint64_t>(width) | (static_cast<uint64_t>(height) << 16) |
                         (static_cast<uint64_t>(color) << 32) |
                         (static_cast<uint64_t>(sampleCountOf(samples)) << 40);
    const auto found = msaa_cache_.find(key);
    if (found != msaa_cache_.end())
    {
        return found->second;
    }

    MsaaTarget target;
    target.width = width;
    target.height = height;

    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = formatOf(color);
    image.extent = VkExtent3D{width, height, 1U};
    image.mipLevels = 1U;
    image.arrayLayers = 1U;
    image.samples = sampleCountOf(samples);
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    if (vmaCreateImage(allocator_, &image, &allocation, &target.color, &target.color_allocation,
                       nullptr) != VK_SUCCESS)
    {
        target.color = VK_NULL_HANDLE;
    }
    else
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = target.color;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1U;
        view.subresourceRange.layerCount = 1U;
        if (vkCreateImageView(device_, &view, nullptr, &target.color_view) != VK_SUCCESS)
        {
            target.color_view = VK_NULL_HANDLE;
        }
    }

    image.format = VK_FORMAT_D32_SFLOAT;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (vmaCreateImage(allocator_, &image, &allocation, &target.depth, &target.depth_allocation,
                       nullptr) != VK_SUCCESS)
    {
        target.depth = VK_NULL_HANDLE;
    }
    else
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = target.depth;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = image.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view.subresourceRange.levelCount = 1U;
        view.subresourceRange.layerCount = 1U;
        if (vkCreateImageView(device_, &view, nullptr, &target.depth_view) != VK_SUCCESS)
        {
            target.depth_view = VK_NULL_HANDLE;
        }
    }

    return msaa_cache_.emplace(key, target).first->second;
}

void Context::discardMsaa()
{
    /* Every cached framebuffer may attach one of the views this call destroys;
       a graph-image framebuffer carries source == 0 just like a canvas one.
       They all go through the graveyard, so a frame still holding one finishes
       first. The cache is per extent and pass, so recreating them is cheap. */
    {
        Frame& frame = frames_[frame_ % kFramesInFlight];
        for (const auto& entry : framebuffer_cache_)
        {
            frame.graveyard.push_back(
                Retired{kRetiredFramebuffer, static_cast<void*>(entry.second), nullptr});
        }
        framebuffer_cache_.clear();
    }
    for (auto& entry : msaa_cache_)
    {
        MsaaTarget& target = entry.second;
        if (target.color_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, target.color_view, nullptr);
        }
        if (target.depth_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device_, target.depth_view, nullptr);
        }
        if (target.color != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, target.color, target.color_allocation);
        }
        if (target.depth != VK_NULL_HANDLE)
        {
            vmaDestroyImage(allocator_, target.depth, target.depth_allocation);
        }
    }
    msaa_cache_.clear();
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
    /* The pass tails: a sample count above 1 runs the pass on the device's
       transient MSAA attachments and resolves into the color target; the
       action bits shape the 1x path. Zero reads as 1x. The request is clamped
       to what the pass's formats support here, so a count this device cannot
       do falls back instead of failing the pass (R1 tail). */
    uint32_t requested_samples = 0U;
    if (desc.struct_size >= offsetof(ResonateRenderPassDesc, samples) + sizeof(uint32_t))
    {
        requested_samples = desc.samples;
    }
    ResonateRenderFormat pass_color = RESONATE_RENDER_FORMAT_NONE;
    ResonateRenderFormat pass_depth = RESONATE_RENDER_FORMAT_NONE;
    if (desc.color.value != 0)
    {
        if (const ImageSlot* color = images_.live(handleIndex(desc.color.value),
                                                  handleGeneration(desc.color.value));
            color != nullptr)
        {
            pass_color = color->format_id;
        }
    }
    else if (!colorless)
    {
        pass_color = surfaceFormat();
    }
    if (desc.depth.value != 0)
    {
        if (const ImageSlot* depth = images_.live(handleIndex(desc.depth.value),
                                                  handleGeneration(desc.depth.value));
            depth != nullptr)
        {
            pass_depth = depth->format_id;
        }
    }
    const uint32_t samples =
        static_cast<uint32_t>(sampleCountOf(clampedSamples(pass_color, pass_depth,
                                                           requested_samples)));
    const uint32_t ops =
        desc.struct_size >= offsetof(ResonateRenderPassDesc, attachment_ops) + sizeof(uint32_t)
            ? desc.attachment_ops
            : 0U;
    const bool multisampled = samples > 1U && !colorless;
    /* The framebuffer must be rebuilt when the pass identity changes, because
       a VkFramebuffer is created against one VkRenderPass. */
    const uint64_t pass_identity = static_cast<uint64_t>(samples) | (static_cast<uint64_t>(ops) << 8);
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkExtent2D pass_extent{};

    if (desc.color.value == 0 && !colorless)
    {
        if (renderPassFor(RESONATE_RENDER_FORMAT_UNKNOWN, RESONATE_RENDER_FORMAT_UNKNOWN, samples,
                          ops, &pass) !=
            RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        if (!multisampled)
        {
            framebuffer = framebuffers_[image_index_];
        }
        else
        {
            /* The 1x canvas image is the resolve; the transient attachments
               come from the device. One framebuffer per swapchain image. */
            const MsaaTarget& msaa = ensureMsaa(extent_.width, extent_.height,
                                                RESONATE_RENDER_FORMAT_UNKNOWN, samples);
            if (msaa.color_view == VK_NULL_HANDLE || msaa.depth_view == VK_NULL_HANDLE)
            {
                return RESONATE_E_INTERNAL;
            }
            const uint64_t identity = pass_identity |
                                      (static_cast<uint64_t>(extent_.width) << 16) |
                                      (static_cast<uint64_t>(extent_.height) << 32) |
                                      (static_cast<uint64_t>(image_index_) << 48);
            const FramebufferKey key{0U, 0U, identity, 1U};
            const auto found = framebuffer_cache_.find(key);
            if (found != framebuffer_cache_.end())
            {
                framebuffer = found->second;
            }
            else
            {
                /* The render pass reads color-at-samples, the 1x resolve, the
                   device's depth - in that attachment order. */
                VkImageView attachments[3] = {msaa.color_view, views_[image_index_],
                                              msaa.depth_view};
                VkFramebufferCreateInfo created{};
                created.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                created.renderPass = pass;
                created.attachmentCount = 3U;
                created.pAttachments = attachments;
                created.width = extent_.width;
                created.height = extent_.height;
                created.layers = 1U;
                VkFramebuffer handle = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device_, &created, nullptr, &handle) != VK_SUCCESS)
                {
                    note(device_failed_, "framebuffer creation failed");
                    return RESONATE_E_INTERNAL;
                }
                framebuffer_cache_.emplace(key, handle);
                framebuffer = handle;
            }
        }
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

        if (renderPassFor(RESONATE_RENDER_FORMAT_NONE, depth->format_id, samples, ops, &pass) !=
            RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        const FramebufferKey key{0U, desc.depth.value, pass_identity, 0U};
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
        if (renderPassFor(image->format_id, depth_format, samples, ops, &pass) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        /* On a multisampled pass the device's transient attachments render and
           the 1x image resolves; the explicit depth is ignored. */
        VkImageView attachments[3] = {image->view,
                                      depth != nullptr ? depth->view : VK_NULL_HANDLE,
                                      VK_NULL_HANDLE};
        uint32_t attachment_count = depth != nullptr ? 2U : 1U;
        if (multisampled)
        {
            const MsaaTarget& msaa =
                ensureMsaa(image->width, image->height, image->format_id, samples);
            if (msaa.color_view == VK_NULL_HANDLE || msaa.depth_view == VK_NULL_HANDLE)
            {
                return RESONATE_E_INTERNAL;
            }
            attachments[0] = msaa.color_view;
            attachments[1] = image->view;
            attachments[2] = msaa.depth_view;
            attachment_count = 3U;
        }
        const uint64_t identity =
            pass_identity | (static_cast<uint64_t>(image->width) << 16) |
            (static_cast<uint64_t>(image->height) << 32);
        const FramebufferKey key{desc.color.value, desc.depth.value, identity, 0U};
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
    /* The clear covers the whole attachment; the viewport may be a sub-rectangle
       of it (dynamic-resolution rendering), so the draw only fills that part. */
    VkExtent2D draw_extent = pass_extent;
    if (desc.struct_size >=
        offsetof(ResonateRenderPassDesc, viewport_width) + sizeof(uint32_t) * 2)
    {
        if (desc.viewport_width != 0U && desc.viewport_height != 0U &&
            desc.viewport_width <= pass_extent.width &&
            desc.viewport_height <= pass_extent.height)
        {
            draw_extent = VkExtent2D{desc.viewport_width, desc.viewport_height};
        }
    }
    /* One clear value per cleared attachment: a depth-only pass's sole
       attachment is the depth image, a color pass's second is depth, and a
       multisampled pass clears only its device-owned depth at attachment 2. */
    VkClearValue clears[3] = {};
    uint32_t clear_count = 1U;
    if (colorless)
    {
        clears[0].depthStencil = {1.0f, 0U};
    }
    else if (multisampled)
    {
        clears[0].color = {
            {desc.clear_color.x, desc.clear_color.y, desc.clear_color.z, desc.clear_color.w}};
        clears[2].depthStencil = {1.0f, 0U};
        clear_count = 3U;
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
    profileTimestamp(command);
    vkCmdBeginRenderPass(command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(draw_extent.width);
    viewport.height = static_cast<float>(draw_extent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = draw_extent;
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
    profileTimestamp(frames_[frame_ % kFramesInFlight].command);
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

ResonateStatus Context::barrierBatch(const ResonateRenderBarrierDesc* descs, uint32_t count)
{
    if (!frame_open_)
    {
        return RESONATE_E_STATE;
    }
    if (descs == nullptr || count == 0U)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;

    /* Consecutive descriptors moving in the same direction share one
       submission; a direction flip flushes the run, so a read-after-write
       never merges with the write it depends on. Each entry keeps its own
       access masks; only the stage scopes are the run's union, because Vulkan
       carries them per call. */
    constexpr uint32_t kWrites =
        RESONATE_RENDER_ACCESS_SHADER_WRITE | RESONATE_RENDER_ACCESS_COLOR_WRITE |
        RESONATE_RENDER_ACCESS_DEPTH_WRITE | RESONATE_RENDER_ACCESS_TRANSFER_WRITE;
    uint32_t index = 0U;
    while (index < count)
    {
        const bool writing = (descs[index].after_access & kWrites) != 0U;
        VkPipelineStageFlags before_stages = 0;
        VkPipelineStageFlags after_stages = 0;
        batch_image_barriers_.clear();
        batch_buffer_barriers_.clear();
        uint32_t end = index;
        for (; end < count; ++end)
        {
            const ResonateRenderBarrierDesc& desc = descs[end];
            if (((desc.after_access & kWrites) != 0U) != writing)
            {
                break;
            }
            const VkPipelineStageFlags before = stagesToVulkan(desc.before_stages);
            const VkPipelineStageFlags after = stagesToVulkan(desc.after_stages);
            before_stages |= before != 0 ? before : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            after_stages |= after != 0 ? after : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            const VkAccessFlags src = accessToVulkan(desc.before_access);
            const VkAccessFlags dst = accessToVulkan(desc.after_access);
            if (desc.image.value != 0)
            {
                const ImageSlot* slot =
                    images_.live(handleIndex(desc.image.value),
                                 handleGeneration(desc.image.value));
                if (slot == nullptr)
                {
                    return RESONATE_E_INVALID;
                }
                VkImageMemoryBarrier image_barrier{};
                image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                image_barrier.srcAccessMask = src;
                image_barrier.dstAccessMask = dst;
                image_barrier.oldLayout = layoutToVulkan(desc.before_layout);
                image_barrier.newLayout = layoutToVulkan(desc.after_layout);
                image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image_barrier.image = slot->image;
                image_barrier.subresourceRange.aspectMask =
                    slot->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                image_barrier.subresourceRange.levelCount = 1;
                image_barrier.subresourceRange.layerCount = 1;
                batch_image_barriers_.push_back(image_barrier);
            }
            else if (desc.buffer.value != 0)
            {
                VkBufferMemoryBarrier buffer_barrier{};
                buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                buffer_barrier.srcAccessMask = src;
                buffer_barrier.dstAccessMask = dst;
                buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                buffer_barrier.buffer = resolveBuffer(desc.buffer);
                buffer_barrier.size = VK_WHOLE_SIZE;
                batch_buffer_barriers_.push_back(buffer_barrier);
            }
            else
            {
                return RESONATE_E_INVALID;
            }
        }
        vkCmdPipelineBarrier(command, before_stages, after_stages, 0, 0, nullptr,
                             static_cast<uint32_t>(batch_buffer_barriers_.size()),
                             batch_buffer_barriers_.data(),
                             static_cast<uint32_t>(batch_image_barriers_.size()),
                             batch_image_barriers_.data());
        index = end;
    }
    return RESONATE_OK;
}

ResonateStatus Context::waitIdle()
{
    if (device_ == VK_NULL_HANDLE || frame_open_)
    {
        return RESONATE_E_STATE;
    }
    vkDeviceWaitIdle(device_);
    /* Nothing is in flight any more, so the deferred destructions can happen
       now instead of on a later frame, when a submission may be in flight
       again. */
    for (Frame& frame : frames_)
    {
        drainGraveyard(frame.graveyard);
    }
    return RESONATE_OK;
}

ResonateStatus Context::present()
{
    if (!frame_open_ || (swapchain_ == VK_NULL_HANDLE && !offscreen_))
    {
        return RESONATE_E_STATE;
    }
    Frame& frame = frames_[frame_ % kFramesInFlight];

    /* RESONATE_DEBUG_CANVAS: copy the presented image's centre texel so a
       device run can tell a drawn frame from a cleared one. */
    const bool debug_canvas =
        !offscreen_ && swapchain_ != VK_NULL_HANDLE && !swapchain_images_.empty() &&
        debug_canvas_frames_ < 5U && std::getenv("RESONATE_DEBUG_CANVAS") != nullptr;
    if (debug_canvas && debug_canvas_buffer_ == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = 4U;
        buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vmaCreateBuffer(allocator_, &buffer, &allocation, &debug_canvas_buffer_,
                        &debug_canvas_allocation_, nullptr);
    }
    if (debug_canvas && debug_canvas_buffer_ != VK_NULL_HANDLE)
    {
        VkImageMemoryBarrier to_copy{};
        to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_copy.image = swapchain_images_[image_index_];
        to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_copy.subresourceRange.levelCount = 1;
        to_copy.subresourceRange.layerCount = 1;
        to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                             &to_copy);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = VkOffset3D{static_cast<int32_t>(extent_.width / 2U),
                                        static_cast<int32_t>(extent_.height / 2U), 0};
        region.imageExtent = VkExtent3D{1U, 1U, 1U};
        vkCmdCopyImageToBuffer(frame.command, swapchain_images_[image_index_],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, debug_canvas_buffer_, 1U,
                               &region);

        VkImageMemoryBarrier to_present = to_copy;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                             &to_present);
    }

    /* The frame's colour writes must be available to the presentation engine
       before the image leaves this submission. On Mali-G51 the semaphore alone
       is not enough: without this barrier some frames present the final pass's
       clear colour and drop its draw. */
    if (!offscreen_ && swapchain_ != VK_NULL_HANDLE &&
        image_index_ < static_cast<uint32_t>(swapchain_images_.size()))
    {
        VkImageMemoryBarrier to_present{};
        to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_present.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_present.image = swapchain_images_[image_index_];
        to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_present.subresourceRange.levelCount = 1;
        to_present.subresourceRange.layerCount = 1;
        to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, nullptr, 0U, nullptr, 1U,
                             &to_present);
    }
    tracePresentedTexel(frame.command);
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
    profile_reported_[frame_ % kFramesInFlight] = profile_count_[frame_ % kFramesInFlight];
    const std::uint64_t submitted_at = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const VkResult submitted = vkQueueSubmit(queue_, 1, &submit, frame.in_flight);
    trace_submit_ns_[frame_ % kFramesInFlight] = submitted_at;
    frame_open_ = false;
    pass_open_ = false;
    bound_pipeline_ = VK_NULL_HANDLE;
    bound_vertex_ = VK_NULL_HANDLE;
    bound_index_ = VK_NULL_HANDLE;
    frame_++;
    if (submitted != VK_SUCCESS)
    {
        /* A submission that failed never signals its fence, so this slot must not
           be waited on next time: that wait would block the render thread until
           the process is killed. */
        frame.fence_pending = false;
        std::fprintf(stderr, "render-vulkan: command submission failed (%d)\n",
                     static_cast<int>(submitted));
        note(surface_failed_, "command submission failed");
        return RESONATE_E_INTERNAL;
    }
    frame.fence_pending = true;
    if (debug_canvas && debug_canvas_buffer_ != VK_NULL_HANDLE)
    {
        vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
        void* mapped = nullptr;
        if (vmaMapMemory(allocator_, debug_canvas_allocation_, &mapped) == VK_SUCCESS)
        {
            (void)vmaInvalidateAllocation(allocator_, debug_canvas_allocation_, 0, VK_WHOLE_SIZE);
            const auto* pixel = static_cast<const std::uint8_t*>(mapped);
            std::fprintf(stderr, "render-vulkan: canvas centre %u,%u,%u,%u\n", pixel[0],
                         pixel[1], pixel[2], pixel[3]);
            vmaUnmapMemory(allocator_, debug_canvas_allocation_);
        }
        ++debug_canvas_frames_;
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
    trace("present result=%d image=%u", static_cast<int>(presented), image_index_);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR)
    {
        recreate_pending_ = true;
    }
    else if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
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
    ResonateStatus status = submitAndRun(aux_command_);
    if (status == RESONATE_OK)
    {
        /* The staging allocation is host-visible, not necessarily coherent; the
           copy's writes become visible to the CPU only through an invalidate. */
        if (vmaInvalidateAllocation(allocator_, allocation, 0, tight) != VK_SUCCESS)
        {
            status = RESONATE_E_INTERNAL;
        }
        else
        {
            std::memcpy(data, allocation_info.pMappedData, static_cast<std::size_t>(tight));
            *out_width = extent_.width;
            *out_height = extent_.height;
        }
    }
    vmaDestroyBuffer(allocator_, staging, allocation);
    return status;
}

} // namespace resonate::render_vulkan
