#include <resonate/renderer/texture.h>

#include <cstring>
#include <fstream>
#include <iterator>

namespace resonate::renderer
{
namespace
{

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint64_t>(readU32(bytes, offset)) |
           (static_cast<std::uint64_t>(readU32(bytes, offset + 4U)) << 32U);
}

/* The KTX2 identifier: «KTX 20» with the two byte marks. */
constexpr std::uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                          0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

constexpr std::uint32_t kHeaderSize = 80U;
constexpr std::uint32_t kLevelIndexSize = 24U;
constexpr std::uint32_t kVkFormatRgba8Unorm = 37U;
constexpr std::uint32_t kVkFormatRgba8Srgb = 43U;
constexpr std::uint32_t kSupercompressionNone = 0U;

} // namespace

bool decodeKtx2Rgba8(const std::vector<std::uint8_t>& bytes, TextureImage& out_image,
                     std::string& out_error)
{
    out_error.clear();
    if (bytes.size() < kHeaderSize + kLevelIndexSize)
    {
        out_error = "shorter than the KTX2 header";
        return false;
    }
    if (std::memcmp(bytes.data(), kIdentifier, sizeof(kIdentifier)) != 0)
    {
        out_error = "not a KTX2 file";
        return false;
    }

    const std::uint32_t vk_format = readU32(bytes, 12U);
    const std::uint32_t type_size = readU32(bytes, 16U);
    const std::uint32_t width = readU32(bytes, 20U);
    const std::uint32_t height = readU32(bytes, 24U);
    const std::uint32_t depth = readU32(bytes, 28U);
    const std::uint32_t layers = readU32(bytes, 32U);
    const std::uint32_t faces = readU32(bytes, 36U);
    const std::uint32_t levels = readU32(bytes, 40U);
    const std::uint32_t supercompression = readU32(bytes, 44U);
    const std::uint32_t dfd_offset = readU32(bytes, 48U);
    const std::uint32_t dfd_length = readU32(bytes, 52U);
    const std::uint32_t kvd_offset = readU32(bytes, 56U);
    const std::uint32_t kvd_length = readU32(bytes, 60U);
    const std::uint64_t sgd_offset = readU64(bytes, 64U);
    const std::uint64_t sgd_length = readU64(bytes, 72U);

    if (vk_format != kVkFormatRgba8Unorm && vk_format != kVkFormatRgba8Srgb)
    {
        out_error = "the v1 container only holds RGBA8 UNORM or sRGB";
        return false;
    }
    if (type_size != 1U || depth != 0U || layers != 0U || faces != 1U || levels != 1U ||
        supercompression != kSupercompressionNone)
    {
        out_error = "the v1 container is one 2D mip level, uncompressed";
        return false;
    }
    if (width == 0U || height == 0U)
    {
        out_error = "a zero extent";
        return false;
    }
    if (static_cast<std::uint64_t>(dfd_offset) + dfd_length > bytes.size() ||
        static_cast<std::uint64_t>(kvd_offset) + kvd_length > bytes.size() ||
        sgd_offset + sgd_length > bytes.size())
    {
        out_error = "a header section past the file end";
        return false;
    }

    const std::uint64_t level_offset = readU64(bytes, 80U);
    const std::uint64_t level_length = readU64(bytes, 88U);
    const std::uint64_t uncompressed_length = readU64(bytes, 96U);
    const std::uint64_t expected =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4U;
    if (level_length != expected || uncompressed_length != expected)
    {
        out_error = "a level that does not match the extent's RGBA8 size";
        return false;
    }
    if (level_offset > bytes.size() || level_length > bytes.size() - level_offset)
    {
        out_error = "level data past the file end";
        return false;
    }

    out_image.width = width;
    out_image.height = height;
    out_image.srgb = vk_format == kVkFormatRgba8Srgb;
    out_image.rgba.assign(bytes.begin() + static_cast<std::ptrdiff_t>(level_offset),
                          bytes.begin() + static_cast<std::ptrdiff_t>(level_offset + level_length));
    return true;
}

bool loadKtx2Rgba8(const std::string& path, TextureImage& out_image, std::string& out_error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        out_error = "cannot open " + path;
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    return decodeKtx2Rgba8(bytes, out_image, out_error);
}

} // namespace resonate::renderer
