#ifndef RESONATE_RENDERER_TEXTURE_H
#define RESONATE_RENDERER_TEXTURE_H

/* The runtime texture container: a 2D KTX2. The loader takes either the v1
 * uncompressed RGBA8 layout (one mip level, no arrays or cubes) or a KTX2
 * carrying a Basis Universal payload, which it transcodes to RGBA8. The cook
 * tool writes UASTC; compressed GPU payloads slot in behind the same entry
 * point when the RHI grows those formats.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace resonate::renderer
{

struct TextureImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /* True when the payload is sRGB-encoded (the device linearizes it on
       sampling); false for a plain linear multiplier. */
    bool srgb = false;
    std::vector<std::uint8_t> rgba; /* tightly packed, four bytes per texel */
};

/* The direct (uncompressed RGBA8) path; false with a named error otherwise. */
bool decodeKtx2Rgba8(const std::vector<std::uint8_t>& bytes, TextureImage& out_image,
                     std::string& out_error);

/* Direct when the container is uncompressed RGBA8, transcoded when it holds a
   Basis payload. False with a named error for anything else. */
bool loadTexture(const std::string& path, TextureImage& out_image, std::string& out_error);

} // namespace resonate::renderer

#endif /* RESONATE_RENDERER_TEXTURE_H */
