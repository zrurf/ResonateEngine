#include "png.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace resonate::iconcook
{
namespace
{

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    static std::uint32_t table[256] = {};
    static bool ready = false;
    if (!ready)
    {
        for (std::uint32_t index = 0U; index < 256U; ++index)
        {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
            {
                value = (value & 1U) != 0U ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
            }
            table[index] = value;
        }
        ready = true;
    }
    std::uint32_t value = 0xFFFFFFFFU;
    for (std::size_t index = 0U; index < size; ++index)
    {
        value = table[(value ^ data[index]) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t low = 1U;
    std::uint32_t high = 0U;
    for (std::size_t index = 0U; index < size; ++index)
    {
        low = (low + data[index]) % 65521U;
        high = (high + low) % 65521U;
    }
    return (high << 16U) | low;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void putChunk(std::vector<std::uint8_t>& out, const char type[4],
              const std::vector<std::uint8_t>& payload)
{
    put32(out, static_cast<std::uint32_t>(payload.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

} // namespace

bool writePng(const std::string& path, std::uint32_t width, std::uint32_t height,
              const std::vector<std::uint8_t>& rgba, std::string& error)
{
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4U)
    {
        error = "the pixel buffer does not match the image size";
        return false;
    }

    /* Raw scanlines: a filter byte per row, then the pixels. */
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(width) * 4U + 1U) * height);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        raw.push_back(0U);
        const std::size_t offset = static_cast<std::size_t>(row) * width * 4U;
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(offset),
                   rgba.begin() + static_cast<std::ptrdiff_t>(offset + width * 4U));
    }

    /* A zlib stream of stored deflate blocks. */
    std::vector<std::uint8_t> zlib;
    zlib.push_back(0x78U);
    zlib.push_back(0x01U);
    std::size_t position = 0U;
    while (position < raw.size())
    {
        const std::size_t size = std::min<std::size_t>(65535U, raw.size() - position);
        zlib.push_back(position + size >= raw.size() ? 0x01U : 0x00U);
        zlib.push_back(static_cast<std::uint8_t>(size & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>((~size) & 0xFFU));
        zlib.push_back(static_cast<std::uint8_t>(((~size) >> 8U) & 0xFFU));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(position),
                    raw.begin() + static_cast<std::ptrdiff_t>(position + size));
        position += size;
    }
    put32(zlib, adler32(raw.data(), raw.size()));

    std::vector<std::uint8_t> png = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    std::vector<std::uint8_t> header;
    put32(header, width);
    put32(header, height);
    header.push_back(8U); /* bit depth */
    header.push_back(6U); /* RGBA */
    header.push_back(0U); /* deflate */
    header.push_back(0U); /* filter */
    header.push_back(0U); /* no interlace */
    putChunk(png, "IHDR", header);
    putChunk(png, "IDAT", zlib);
    putChunk(png, "IEND", {});

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        error = "cannot open " + path;
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1U, png.size(), file) == png.size();
    std::fclose(file);
    if (!ok)
    {
        error = "cannot write " + path;
    }
    return ok;
}

} // namespace resonate::iconcook
