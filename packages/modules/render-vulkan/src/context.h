#ifndef RESONATE_RENDER_VULKAN_CONTEXT_H
#define RESONATE_RENDER_VULKAN_CONTEXT_H

/* The Vulkan implementation behind the RHI device capability.
 *
 * Creation is lazy and staged: the instance exists once the first surface is
 * requested, because a headless run never needs it and creation must not
 * determine whether the module loads. Until then every RHI call answers
 * RESONATE_E_STATE.
 *
 * Calls arrive serialised — the stage barrier separates the frame's render
 * work — so the context holds no internal lock. Resource destruction is
 * deferred: the Vk objects move to the destroying frame's graveyard, drained
 * once that frame's submission is known complete.
 */

#ifdef RESONATE_PLATFORM_WINDOWS
/* windows.h must precede the Vulkan headers: the win32 surface extension
   declares against its types without including them. The platform define comes
   public from the Resonate.Vulkan target. */
#    include <windows.h>
#endif
#include <vulkan/vulkan.h>

#include <resonate/module/abi.h>
#include <resonate/module/types.h>
#include <resonate/render/device.h>

#include <cstdint>
#include <map>
#include <vector>

struct VmaAllocator_T;
struct VmaAllocation_T;

namespace resonate::render_vulkan
{

/* Opaque handle = generation in the high half, slot index in the low half;
   {0} is null. The index half is the resource's slot in the device's resource
   table. */
constexpr uint64_t handleValue(uint32_t generation, uint32_t index)
{
    return (static_cast<uint64_t>(generation) << 32) | index;
}
constexpr uint32_t handleIndex(uint64_t value)
{
    return static_cast<uint32_t>(value & 0xFFFFFFFFull);
}
constexpr uint32_t handleGeneration(uint64_t value)
{
    return static_cast<uint32_t>(value >> 32);
}

/* Generation-checked slot pool: acquire hands a slot out, retire bumps the
   generation and frees the index, so a stale handle finds its slot's
   generation changed and validates against it. */
template <typename Slot> class Pool
{
  public:
    uint32_t acquire(Slot** out)
    {
        if (!free_.empty())
        {
            const uint32_t index = free_.back();
            free_.pop_back();
            *out = &slots_[index];
            return index;
        }
        slots_.emplace_back();
        *out = &slots_.back();
        return static_cast<uint32_t>(slots_.size() - 1U);
    }

    Slot* live(uint32_t index, uint32_t generation)
    {
        if (index >= slots_.size())
        {
            return nullptr;
        }
        Slot& slot = slots_[index];
        return slot.alive && slot.generation == generation ? &slot : nullptr;
    }

    void retire(uint32_t index)
    {
        Slot& slot = slots_[index];
        slot.alive = false;
        slot.generation += 1;
        free_.push_back(index);
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
};

class Context
{
  public:
    /* `vsync` picks the presentation mode: FIFO when on, MAILBOX or IMMEDIATE
       when off, FIFO when neither is offered. Read once from the run's
       configuration at attach; restart-to-apply. */
    explicit Context(bool vsync);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /* Creates the surface and, on the first window, the instance, device and
       swapchain. `native_surface` is the platform window handle the canvas
       reported (an HWND on Windows). Returns RESONATE_E_UNSUPPORTED when
       Vulkan is unavailable and RESONATE_E_INTERNAL on creation failure. */
    ResonateStatus attachSurface(const void* native_surface, uint32_t width, uint32_t height);

    /* True once a swapchain exists to draw into. */
    bool hasSurface() const;

    /* --- RHI surface --- */

    ResonateStatus createBuffer(const ResonateRenderBufferDesc& desc, ResonateRenderBuffer* out);
    void destroyBuffer(ResonateRenderBuffer handle);
    ResonateStatus createImage(const ResonateRenderImageDesc& desc, ResonateRenderImage* out);
    void destroyImage(ResonateRenderImage handle);
    ResonateStatus createSampler(const ResonateRenderSamplerDesc& desc, ResonateRenderSampler* out);
    void destroySampler(ResonateRenderSampler handle);
    ResonateStatus createShader(ResonateRenderShaderStage stage, const void* spirv,
                                uint64_t size_bytes, ResonateRenderShader* out);
    void destroyShader(ResonateRenderShader handle);
    ResonateStatus createPipeline(const ResonateRenderGraphicsPipelineDesc& desc,
                                  ResonateRenderPipeline* out);
    void destroyPipeline(ResonateRenderPipeline handle);

    ResonateStatus uploadBuffer(ResonateRenderBuffer handle, uint64_t offset, const void* data,
                                uint64_t size_bytes);
    ResonateStatus uploadImage(ResonateRenderImage handle, const void* data, uint64_t size_bytes,
                               uint64_t row_pitch_bytes);

    ResonateStatus beginPass(const ResonateRenderPassDesc& desc);
    ResonateStatus endPass();
    ResonateStatus bindPipeline(ResonateRenderPipeline handle);
    ResonateStatus bindVertexBuffer(ResonateRenderBuffer handle, uint64_t offset);
    ResonateStatus bindIndexBuffer(ResonateRenderBuffer handle, uint64_t offset);
    ResonateStatus setConstants(const void* data, uint32_t size_bytes);
    ResonateStatus draw(uint32_t vertex_count, uint32_t instance_count);
    ResonateStatus drawIndexed(uint32_t index_count, uint32_t instance_count);
    ResonateStatus present();

  private:
    struct BufferSlot
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        void* mapped = nullptr; /* host-visible only */
        uint64_t size = 0;
        uint32_t generation = 1;
        bool alive = false;
    };

    struct ImageSlot
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation_T* allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        ResonateRenderFormat format_id = RESONATE_RENDER_FORMAT_UNKNOWN;
        uint32_t bytes_per_pixel = 4;
        uint32_t width = 0;
        uint32_t height = 0;
        bool depth = false;
        uint32_t generation = 1;
        bool alive = false;
    };

    struct SamplerSlot
    {
        VkSampler sampler = VK_NULL_HANDLE;
        uint32_t generation = 1;
        bool alive = false;
    };

    struct ShaderSlot
    {
        VkShaderModule module = VK_NULL_HANDLE;
        uint32_t generation = 1;
        bool alive = false;
    };

    struct PipelineSlot
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t generation = 1;
        bool alive = false;
    };

    /* One retired Vk object: kind names the destroy call, `a` the object,
       `b` its VMA allocation when it has one. */
    struct Retired
    {
        uint8_t kind = 0;
        void* a = nullptr;
        void* b = nullptr;
    };

    enum RetiredKind : uint8_t
    {
        kRetiredBuffer = 1,
        kRetiredImage,
        kRetiredImageView,
        kRetiredFramebuffer,
        kRetiredSampler,
        kRetiredShader,
        kRetiredPipeline,
        kRetiredRenderPass,
        kRetiredPipelineLayout,
    };

    struct Frame
    {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore render_done = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        std::vector<Retired> graveyard;
    };

    ResonateStatus ensureInstance();
    ResonateStatus ensureDevice();
    ResonateStatus createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchainObjects();
    void recreateSwapchain();
    ResonateStatus renderPassFor(ResonateRenderFormat color, ResonateRenderFormat depth,
                                 VkRenderPass* out);
    VkFormat formatOf(ResonateRenderFormat format) const;
    ResonateStatus submitAndRun(VkCommandBuffer command); /* one-shot sync submit */
    void drainGraveyard(std::vector<Retired>& graveyard);
    void note(bool& once, const char* what);

    VkInstance instance_ = VK_NULL_HANDLE;
    /* The loader's import library exports core and WSI entry points only; the
       debug utils ones resolve through vkGetInstanceProcAddr. */
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger_ = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger_ = nullptr;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator_T* allocator_ = nullptr;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format_{};
    VkExtent2D extent_{};
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    /* The swapchain owns the images; the views and framebuffers are ours. */
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;

    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kMaxConstants = 128;
    Frame frames_[kFramesInFlight];
    uint32_t frame_ = 0;

    /* Resources. */
    Pool<BufferSlot> buffers_;
    Pool<ImageSlot> images_;
    Pool<SamplerSlot> samplers_;
    Pool<ShaderSlot> shaders_;
    Pool<PipelineSlot> pipelines_;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;   /* shared, lazy */
    std::map<uint32_t, VkRenderPass> pass_cache_;         /* key: color | depth<<8 */
    std::map<uint64_t, VkFramebuffer> framebuffer_cache_; /* key: image<<16 | pass key */

    /* The open pass and the state its draws use. */
    VkCommandBuffer aux_command_ = VK_NULL_HANDLE; /* sync work: uploads, image passes */
    bool pass_open_ = false;
    bool pass_on_swapchain_ = false;
    ResonateRenderImage pass_color_{};
    VkRenderPass open_pass_ = VK_NULL_HANDLE;
    VkFramebuffer open_framebuffer_ = VK_NULL_HANDLE;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    VkBuffer bound_vertex_ = VK_NULL_HANDLE;
    VkBuffer bound_index_ = VK_NULL_HANDLE;

    uint32_t image_index_ = 0;
    VkSemaphore last_render_done_ = VK_NULL_HANDLE;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool vsync_ = true;
    bool recreate_pending_ = false;
    uint32_t canvas_width_ = 0;
    uint32_t canvas_height_ = 0;

    /* One stderr line per failure path, however often the path runs. */
    bool surface_failed_ = false;
    bool device_failed_ = false;
    bool platform_unsupported_ = false;
};

} // namespace resonate::render_vulkan

#endif /* RESONATE_RENDER_VULKAN_CONTEXT_H */
