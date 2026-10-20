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
#include <resonate/render/canvas.h>
#include <resonate/render/device.h>

#include <cstdint>
#include <map>
#include <string>
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

    const Slot* live(uint32_t index, uint32_t generation) const
    {
        if (index >= slots_.size())
        {
            return nullptr;
        }
        const Slot& slot = slots_[index];
        return slot.alive && slot.generation == generation ? &slot : nullptr;
    }

    Slot* live(uint32_t index, uint32_t generation)
    {
        return const_cast<Slot*>(static_cast<const Pool*>(this)->live(index, generation));
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
       configuration at attach; restart-to-apply. The canvas is where frame
       sizes come from. */
    Context(bool vsync, ResonateRenderCanvas* canvas);
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
    ResonateStatus createComputePipeline(ResonateRenderShader shader, ResonateRenderPipeline* out);

    ResonateStatus uploadBuffer(ResonateRenderBuffer handle, uint64_t offset, const void* data,
                                uint64_t size_bytes);
    ResonateStatus uploadImage(ResonateRenderImage handle, const void* data, uint64_t size_bytes,
                               uint64_t row_pitch_bytes);
    ResonateStatus updateBuffer(ResonateRenderBuffer handle, uint64_t offset, const void* data,
                                uint64_t size_bytes);

    /* Acquires the canvas's image and opens the frame's command buffer; every
       pass and barrier lands in it, and present submits it. */
    ResonateStatus beginFrame();
    ResonateStatus beginPass(const ResonateRenderPassDesc& desc);
    ResonateStatus endPass();
    ResonateStatus barrier(const ResonateRenderBarrierDesc* desc);
    ResonateStatus bindPipeline(ResonateRenderPipeline handle);
    ResonateStatus bindVertexBuffer(ResonateRenderBuffer handle, uint64_t offset);
    ResonateStatus bindIndexBuffer(ResonateRenderBuffer handle, uint64_t offset);
    ResonateStatus setConstants(const void* data, uint32_t size_bytes);
    ResonateStatus draw(uint32_t vertex_count, uint32_t instance_count);
    ResonateStatus drawIndexed(uint32_t index_count, uint32_t instance_count);
    ResonateStatus dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
    ResonateStatus present();
    /* Waits for every submitted frame; used before resource surgery between
       frames. An open frame refuses. */
    ResonateStatus waitIdle();

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
        bool compute = false;
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
        VkFence in_flight = VK_NULL_HANDLE;
        std::vector<Retired> graveyard;
    };

    ResonateStatus ensureInstance();
    ResonateStatus ensureDevice();
    ResonateStatus createSwapchain(uint32_t width, uint32_t height);
    VkBuffer resolveBuffer(ResonateRenderBuffer handle) const;
    void destroySwapchainObjects();
    void recreateSwapchain();
    ResonateStatus renderPassFor(ResonateRenderFormat color, ResonateRenderFormat depth,
                                 VkRenderPass* out);
    VkFormat formatOf(ResonateRenderFormat format) const;
    ResonateStatus submitAndRun(VkCommandBuffer command); /* one-shot sync submit */
    void drainGraveyard(std::vector<Retired>& graveyard);
    void note(bool& once, const char* what);

    /* The persistent pipeline cache: loaded at device creation, written at
       teardown. The driver keys its entries by pipeline state and shader code,
       so a cache from an older shader set is simply not hit. */
    std::string pipelineCachePath() const;
    void loadPipelineCache();
    void savePipelineCache();

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
    /* The swapchain owns the images; the views and framebuffers are ours. One
       present-wait semaphore per image: reusing one across images would signal
       it while the presentation engine still holds it. */
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> present_semaphores_;

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
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE; /* shared, lazy */
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    std::map<uint32_t, VkRenderPass> pass_cache_; /* key: color | depth<<8 */
    /* Keyed by the full color and depth handles, generations included: a
       recreated image must not find the framebuffer of its predecessor. */
    std::map<std::pair<uint64_t, uint64_t>, VkFramebuffer> framebuffer_cache_;

    /* The resource tables: set 0, bindings 0-4 = 64 storage buffers each,
       binding 5 = 64 combined image samplers, all indexed by handle slot. A
       storage buffer's slot is written to every storage binding, so any typed
       array declaration can name it. Dummies fill every storage slot at device
       creation, so an unwritten index reads defined zeros; the sampled slots
       are partially bound, so an unwritten one is never accessed. Sampled
       images share one device-owned linear-clamp sampler. */
    static constexpr uint32_t kTableSize = 64;
    static constexpr uint32_t kStorageTableBindings = 5;
    static constexpr uint32_t kSampledTableBinding = 5;
    static constexpr uint32_t kTableBindingCount = kSampledTableBinding + 1;
    void writeBufferSlot(uint32_t slot, VkBuffer buffer, VkDeviceSize size);
    void writeImageSlot(uint32_t slot, VkImageView view);
    VkDescriptorSetLayout table_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool table_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet table_set_ = VK_NULL_HANDLE;
    VkBuffer table_dummy_ = VK_NULL_HANDLE;
    VmaAllocation_T* table_dummy_allocation_ = nullptr;
    VkSampler table_sampler_ = VK_NULL_HANDLE;

    /* The open frame and pass, and the state their commands use. One command
       buffer per frame: begin_frame opens it, present closes and submits it. */
    VkCommandBuffer aux_command_ = VK_NULL_HANDLE; /* sync work: uploads */
    bool frame_open_ = false;
    bool pass_open_ = false;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    bool bound_compute_ = false;
    VkBuffer bound_vertex_ = VK_NULL_HANDLE;
    VkBuffer bound_index_ = VK_NULL_HANDLE;

    uint32_t image_index_ = 0;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool vsync_ = true;
    bool recreate_pending_ = false;
    ResonateRenderCanvas* canvas_ = nullptr; /* frame sizes come from here */
    uint32_t canvas_width_ = 0;
    uint32_t canvas_height_ = 0;

    /* One stderr line per failure path, however often the path runs. */
    bool surface_failed_ = false;
    bool device_failed_ = false;
    bool platform_unsupported_ = false;
};

} // namespace resonate::render_vulkan

#endif /* RESONATE_RENDER_VULKAN_CONTEXT_H */
