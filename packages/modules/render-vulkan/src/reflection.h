#pragma once

#include <cstdint>
#include <string>

namespace resonate::render_vulkan {

/* The descriptor contract every shader module must match (see context.h):
   descriptors live in set 0 only, the storage tables sit at bindings 0-4, the
   sampled-image table, the frame block and the shadow table follow, each with
   the engine's element count. */
struct ShaderContract
{
    std::uint32_t storage_bindings_begin = 0U;
    std::uint32_t storage_bindings_end = 0U; /* exclusive */
    std::uint32_t storage_table_size = 0U;
    std::uint32_t sampled_table_binding = 0U;
    std::uint32_t sampled_table_size = 0U;
    std::uint32_t frame_binding = 0U;
    std::uint32_t shadow_table_binding = 0U;
    std::uint32_t shadow_table_size = 0U;
    std::uint32_t sampler_table_binding = 0U;
    std::uint32_t sampler_table_size = 0U;
    std::uint32_t max_push_constants = 0U;
};

enum class ShaderStageKind
{
    Vertex,
    Fragment,
    Compute,
};

/* Scans one SPIR-V module and answers false, naming the offence in `error`,
   when it breaks the contract: a descriptor outside set 0, an unknown binding,
   a table whose declared length disagrees with the engine's, a push block past
   the limit, or no entry point of the caller's stage. The scan is structural —
   it reads headers, types, constants, decorations and variables — so it runs
   before the driver sees the module. */
bool reflectShader(const std::uint32_t* words, std::uint64_t word_count,
                   ShaderStageKind stage, const ShaderContract& contract,
                   std::string& error);

} // namespace resonate::render_vulkan
