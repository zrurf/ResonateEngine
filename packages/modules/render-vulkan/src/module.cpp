#include "capability.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/module/module.hpp>
#include <resonate/render/canvas.h>

#ifdef RESONATE_PLATFORM_LINUX
#    include <dlfcn.h>
#endif

namespace
{

using resonate::render_vulkan::State;

constexpr ResonateVec4 kClearColor{0.08f, 0.09f, 0.11f, 1.0f};

/* The shaders the slang toolchain emits (`xmake slang`) sit in a `shaders`
   directory above the plugin's own, so the walk starts from this module's
   location, wherever the build tree put it. */
std::string shadersDirectory()
{
    std::string start;
#ifdef RESONATE_PLATFORM_WINDOWS
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&shadersDirectory), &module);
    wchar_t wide[MAX_PATH];
    if (module == nullptr || GetModuleFileNameW(module, wide, MAX_PATH) == 0U)
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    start.resize(static_cast<std::size_t>(size) - 1U);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, start.data(), size, nullptr, nullptr);
#elif defined(RESONATE_PLATFORM_LINUX)
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&shadersDirectory), &info) == 0 || info.dli_fname == nullptr)
    {
        return {};
    }
    start = info.dli_fname;
#endif

    std::filesystem::path directory = std::filesystem::path(start).parent_path();
    for (int level = 0; level < 4; ++level)
    {
        const std::filesystem::path candidate = directory / "shaders";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate.string();
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

std::vector<char> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/* The demo draw: one Slang-built triangle from the vertex id, no vertex
   buffers, created on the first surface. */
struct Triangle
{
    ResonateRenderShader vertex{};
    ResonateRenderShader fragment{};
    ResonateRenderPipeline pipeline{};
    bool ready = false;
};

Triangle g_triangle;

/* One stderr line per outcome, so a silent clear-only frame is distinguishable
   from a drawn one in the log. */
bool g_triangle_ready_noted = false;
bool g_triangle_failed_noted = false;

void noteTriangleFailure(const char* why)
{
    if (!g_triangle_failed_noted)
    {
        g_triangle_failed_noted = true;
        std::fprintf(stderr, "render-vulkan: demo triangle unavailable: %s\n", why);
    }
}

void renderFrame(void* context, float)
{
    auto* state = static_cast<State*>(context);
    ResonateRenderCanvas* canvas = state->canvas;
    if (canvas == nullptr)
    {
        return;
    }
    const uint32_t width = canvas->width(canvas->self);
    const uint32_t height = canvas->height(canvas->self);
    if (width == 0U || height == 0U)
    {
        return;
    }

    if (!state->context->hasSurface())
    {
        const void* native = canvas->is_window_backed(canvas->self) != 0U
                                 ? canvas->native_handle(canvas->self)
                                 : nullptr;
        if (native == nullptr)
        {
            return;
        }
        if (state->context->attachSurface(native, width, height) != RESONATE_OK)
        {
            return;
        }
    }

    if (!g_triangle.ready)
    {
        const std::string directory = shadersDirectory();
        if (directory.empty())
        {
            noteTriangleFailure("no shaders directory above the plugin");
            return;
        }
        const std::vector<char> vertex_spirv =
            readFile(std::filesystem::path(directory) / "triangle.vertexMain.spv");
        const std::vector<char> fragment_spirv =
            readFile(std::filesystem::path(directory) / "triangle.fragmentMain.spv");
        if (vertex_spirv.empty() || fragment_spirv.empty())
        {
            noteTriangleFailure("shader files missing; run `xmake slang`");
            return;
        }

        using resonate::render_vulkan::Context;
        if (state->context->createShader(RESONATE_RENDER_SHADER_VERTEX, vertex_spirv.data(),
                                         vertex_spirv.size(), &g_triangle.vertex) != RESONATE_OK ||
            state->context->createShader(RESONATE_RENDER_SHADER_FRAGMENT, fragment_spirv.data(),
                                         fragment_spirv.size(),
                                         &g_triangle.fragment) != RESONATE_OK)
        {
            noteTriangleFailure("shader creation failed");
            return;
        }

        ResonateRenderGraphicsPipelineDesc pipeline{};
        pipeline.struct_size = sizeof(pipeline);
        pipeline.vertex_shader = g_triangle.vertex;
        pipeline.fragment_shader = g_triangle.fragment;
        pipeline.stream_count = 0U;
        pipeline.color_format = RESONATE_RENDER_FORMAT_UNKNOWN; /* the swapchain */
        pipeline.depth_format = RESONATE_RENDER_FORMAT_UNKNOWN;
        if (state->context->createPipeline(pipeline, &g_triangle.pipeline) != RESONATE_OK)
        {
            noteTriangleFailure("pipeline creation failed");
            return;
        }
        g_triangle.ready = true;
        if (!g_triangle_ready_noted)
        {
            g_triangle_ready_noted = true;
            std::fprintf(stderr, "render-vulkan: demo triangle ready\n");
        }
    }

    ResonateRenderPassDesc pass{};
    pass.struct_size = sizeof(pass);
    pass.color = ResonateRenderImage{}; /* the swapchain */
    pass.clear_color = kClearColor;
    pass.depth = ResonateRenderImage{};
    if (state->context->beginPass(pass) != RESONATE_OK)
    {
        return;
    }

    if (state->context->bindPipeline(g_triangle.pipeline) == RESONATE_OK)
    {
        state->context->draw(3U, 1U);
    }
    state->context->endPass();
}

void presentFrame(void* context, float)
{
    static_cast<State*>(context)->context->present();
}

ResonateSystemDesc systemDesc(ResonateStage stage, ResonateSystemFn run, void* context,
                              const char* name)
{
    ResonateSystemDesc desc = {};
    desc.struct_size = sizeof(ResonateSystemDesc);
    desc.stage = stage;
    desc.run = run;
    desc.context = context;
    desc.name = name;
    return desc;
}

class RenderVulkanModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        /* A canvas rather than a window: the same code draws into a window and
           into an offscreen target, and this module cannot tell which it got.
           No canvas at all — a server run — skips the backend cleanly. */
        canvas_ = host.query<ResonateRenderCanvas>();
        host_ = host.raw();
        if (!canvas_)
        {
            return RESONATE_E_UNSUPPORTED;
        }

        /* One device per run: the configuration names its backend, and a second
           one to attach is a skip, not a collision. Both checks answer before
           anything is taken. */
        const std::optional<std::string> selected = host.readConfig("resonate.render", "backend");
        if (selected.has_value() && *selected != "vulkan")
        {
            return RESONATE_E_UNSUPPORTED;
        }

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        state_->canvas = &*canvas_;
        /* Vsync comes from the run's configuration, read once here; the
           swapchain it decides is created on the first window. */
        state_->context =
            new resonate::render_vulkan::Context(host.readBool("resonate.render", "vsync", true));

        ResonateRenderDevice& published = resonate::render_vulkan::device();
        published.self = state_;
        if (host.publish(published) != RESONATE_OK)
        {
            /* Another device provider is live: leave nothing behind and skip. */
            delete state_->context;
            state_->context = nullptr;
            host.deallocate(state_);
            canvas_ = {};
            state_ = nullptr;
            return RESONATE_E_UNSUPPORTED;
        }

        if (host.addSystem(systemDesc(RESONATE_STAGE_RENDER, &renderFrame, state_,
                                      "render-vulkan.frame")) != RESONATE_OK ||
            host.addSystem(systemDesc(RESONATE_STAGE_PRESENT, &presentFrame, state_,
                                      "render-vulkan.present")) != RESONATE_OK)
        {
            detachWork(host, published);
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    }

    void onDetach() override
    {
        resonate::Host host(host_);
        ResonateRenderDevice& published = resonate::render_vulkan::device();
        detachWork(host, published);
    }

  private:
    /* Undoes what onAttach published and registered; safe only before the host
       has begun its own teardown of this module's records. */
    void detachWork(resonate::Host& host, ResonateRenderDevice& published)
    {
        host.removeSystem("render-vulkan.frame");
        host.removeSystem("render-vulkan.present");
        host.unpublish(published);
        published.self = nullptr;
        delete state_->context;
        state_->context = nullptr;
        host.deallocate(state_);
        canvas_ = {};
        state_ = nullptr;
    }

    resonate::CapabilityRef<ResonateRenderCanvas> canvas_;
    State* state_ = nullptr;

    /* Kept because onDetach receives no host. */
    const ResonateHostApi* host_ = nullptr;
};

} // namespace

RESONATE_DEFINE_MODULE(RenderVulkanModule, "resonate.render-vulkan", "Render Vulkan", "0.1.0")
