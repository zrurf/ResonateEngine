#ifndef RESONATE_RENDERER_TEXTURE_H
#define RESONATE_RENDERER_TEXTURE_H

/* The runtime texture container v1: an uncompressed 2D RGBA8 KTX2
 * (VK_FORMAT_R8G8B8A8_UNORM, one mip level, no arrays, cubes or
 * supercompression). The cook task writes it; the renderer uploads what this
 * decodes. Compressed Basis/BC payloads replace the reader when the asset
 * pipeline lands, behind the same entry point.
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
    std::vector<std::uint8_t> rgba; /* tightly packed, four bytes per texel */
};

/* False with a named error for anything the v1 container does not cover. */
bool decodeKtx2Rgba8(const std::vector<std::uint8_t>& bytes, TextureImage& out_image,
                     std::string& out_error);
bool loadKtx2Rgba8(const std::string& path, TextureImage& out_image, std::string& out_error);

} // namespace resonate::renderer

#endif /* RESONATE_RENDERER_TEXTURE_H */
