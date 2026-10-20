/* Cooks the P6 PPM sources under content/render/textures into KTX2 files with
 * UASTC Basis payloads. The runtime transcoder turns them back into RGBA8;
 * compressed GPU formats slot in behind the same container later. */

#include <basisu/encoder/basisu_comp.h>
#include <basisu/encoder/basisu_enc.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

bool readPpm(const std::filesystem::path& path, std::uint32_t& out_width, std::uint32_t& out_height,
             std::vector<std::uint8_t>& out_rgba)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    std::size_t position = 0;
    const auto token = [&data, &position]() -> std::string
    {
        while (position < data.size())
        {
            const char c = data[position];
            if (c == '#')
            {
                while (position < data.size() && data[position] != '\n')
                {
                    ++position;
                }
            }
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++position;
            }
            else
            {
                break;
            }
        }
        const std::size_t start = position;
        while (position < data.size() && data[position] != ' ' && data[position] != '\t' &&
               data[position] != '\r' && data[position] != '\n')
        {
            ++position;
        }
        return std::string(data.begin() + static_cast<std::ptrdiff_t>(start),
                           data.begin() + static_cast<std::ptrdiff_t>(position));
    };

    const std::string magic = token();
    const int width = std::atoi(token().c_str());
    const int height = std::atoi(token().c_str());
    const int maxval = std::atoi(token().c_str());
    if (magic != "P6" || width <= 0 || height <= 0 || maxval != 255)
    {
        return false;
    }
    ++position; /* the separator after maxval */
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (position + pixels * 3U > data.size())
    {
        return false;
    }

    out_width = static_cast<std::uint32_t>(width);
    out_height = static_cast<std::uint32_t>(height);
    out_rgba.resize(pixels * 4U);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const auto* rgb =
            reinterpret_cast<const std::uint8_t*>(data.data() + position + pixel * 3U);
        out_rgba[pixel * 4U] = rgb[0];
        out_rgba[pixel * 4U + 1U] = rgb[1];
        out_rgba[pixel * 4U + 2U] = rgb[2];
        out_rgba[pixel * 4U + 3U] = 255U;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: texcook <source-directory> <output-directory>\n");
        return 1;
    }
    basisu::basisu_encoder_init(false);
    const std::filesystem::path source_directory = argv[1];
    const std::filesystem::path output_directory = argv[2];
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);

    int cooked = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(source_directory, error))
    {
        if (entry.path().extension() != ".ppm")
        {
            continue;
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
        if (!readPpm(entry.path(), width, height, rgba))
        {
            std::fprintf(stderr, "texcook: %s: not a readable P6 PPM\n",
                         entry.path().string().c_str());
            return 1;
        }

        basisu::basis_compressor_params params;
        params.m_source_images.push_back(basisu::image(rgba.data(), width, height, 4));
        params.m_uastc = true;
        params.m_create_ktx2_file = true;
        params.m_perceptual = false;
        params.m_status_output = false;
        params.m_write_output_basis_files = true;
        params.m_out_filename =
            (output_directory / (entry.path().stem().string() + ".ktx2")).string();

        basisu::job_pool pool(1);
        params.m_pJob_pool = &pool;
        basisu::basis_compressor compressor;
        if (!compressor.init(params) ||
            compressor.process() != basisu::basis_compressor::cECSuccess)
        {
            std::fprintf(stderr, "texcook: %s: basis encoding failed\n",
                         entry.path().string().c_str());
            return 1;
        }
        std::printf("texcook: %s -> %s (%ux%u)\n", entry.path().filename().string().c_str(),
                    params.m_out_filename.c_str(), width, height);
        ++cooked;
    }
    std::printf("texcook: %d texture(s) -> %s\n", cooked, output_directory.string().c_str());
    return 0;
}
