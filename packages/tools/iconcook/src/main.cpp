/* Cooks the Lucide SVG sources under content/ui/icons into one alpha atlas and
   the name-to-UV map the UI resolves Icon nodes through. The icons are
   stroke-only: each one is rasterized as the distance field of its centre
   lines, which gives round caps and joins for free and antialiases the edge. */

#include "png.h"
#include "svg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

/* Atlas geometry: a 48 px icon inside a 56 px cell, so linear sampling near an
   icon's bounds never reaches its neighbour. */
constexpr int kIconPixels = 48;
constexpr int kCellPixels = 56;
constexpr int kMarginPixels = 4;
constexpr int kColumns = 4;

float distanceToSegment(resonate::iconcook::Point point, resonate::iconcook::Point from,
                        resonate::iconcook::Point to)
{
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length_squared = dx * dx + dy * dy;
    float t = 0.0F;
    if (length_squared > 0.0F)
    {
        t = ((point.x - from.x) * dx + (point.y - from.y) * dy) / length_squared;
        t = std::clamp(t, 0.0F, 1.0F);
    }
    const float nearest_x = from.x + t * dx;
    const float nearest_y = from.y + t * dy;
    return std::sqrt((point.x - nearest_x) * (point.x - nearest_x) +
                     (point.y - nearest_y) * (point.y - nearest_y));
}

bool readFile(const std::filesystem::path& path, std::string& out_text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    out_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: iconcook <icons-dir> <atlas-png> <map-file>\n");
        return 2;
    }
    const std::filesystem::path icons_directory(argv[1]);
    const std::filesystem::path atlas_path(argv[2]);
    const std::filesystem::path map_path(argv[3]);

    std::vector<std::filesystem::path> files;
    std::error_code scan_error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(icons_directory, scan_error))
    {
        if (entry.is_regular_file(scan_error) && entry.path().extension() == ".svg")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
    {
        std::fprintf(stderr, "iconcook: no .svg files under %s\n", icons_directory.string().c_str());
        return 1;
    }

    const int rows = (static_cast<int>(files.size()) + kColumns - 1) / kColumns;
    const int atlas_width = kColumns * kCellPixels;
    const int atlas_height = rows * kCellPixels;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(atlas_width) * atlas_height * 4U,
                                     0U);

    std::string map;
    map += "# name u0 v0 u1 v1: fractions of the atlas, written by xmake icons\n";
    for (std::size_t index = 0U; index < files.size(); ++index)
    {
        std::string text;
        if (!readFile(files[index], text))
        {
            std::fprintf(stderr, "iconcook: cannot read %s\n", files[index].string().c_str());
            return 1;
        }
        resonate::iconcook::Outline outline;
        std::string error;
        if (!resonate::iconcook::parseSvg(text, outline, error))
        {
            std::fprintf(stderr, "iconcook: %s: %s\n", files[index].filename().string().c_str(),
                         error.c_str());
            return 1;
        }

        const float scale = static_cast<float>(kIconPixels) / outline.view_box;
        const float half_width = outline.stroke_width * scale * 0.5F;
        const auto cell = static_cast<int>(index);
        const int origin_x = (cell % kColumns) * kCellPixels + kMarginPixels;
        const int origin_y = (cell / kColumns) * kCellPixels + kMarginPixels;

        for (const std::vector<resonate::iconcook::Point>& polyline : outline.polylines)
        {
            for (std::size_t step = 1U; step < polyline.size(); ++step)
            {
                const resonate::iconcook::Point from{polyline[step - 1U].x * scale,
                                                     polyline[step - 1U].y * scale};
                const resonate::iconcook::Point to{polyline[step].x * scale,
                                                   polyline[step].y * scale};
                /* Only the segment's neighbourhood can be covered. */
                const int min_x = std::max(
                    0, static_cast<int>(std::floor(std::min(from.x, to.x) - half_width - 1.0F)));
                const int max_x = std::min(
                    kIconPixels, static_cast<int>(std::ceil(std::max(from.x, to.x) + half_width + 1.0F)));
                const int min_y = std::max(
                    0, static_cast<int>(std::floor(std::min(from.y, to.y) - half_width - 1.0F)));
                const int max_y = std::min(
                    kIconPixels, static_cast<int>(std::ceil(std::max(from.y, to.y) + half_width + 1.0F)));
                for (int y = min_y; y < max_y; ++y)
                {
                    for (int x = min_x; x < max_x; ++x)
                    {
                        const float distance =
                            distanceToSegment(resonate::iconcook::Point{static_cast<float>(x) + 0.5F,
                                                                        static_cast<float>(y) + 0.5F},
                                              from, to);
                        const float coverage =
                            std::clamp(half_width + 0.5F - distance, 0.0F, 1.0F);
                        const auto alpha = static_cast<std::uint8_t>(coverage * 255.0F + 0.5F);
                        const std::size_t offset =
                            (static_cast<std::size_t>(origin_y + y) * atlas_width +
                             static_cast<std::size_t>(origin_x + x)) *
                            4U;
                        if (alpha > pixels[offset + 3U])
                        {
                            pixels[offset] = 255U;
                            pixels[offset + 1U] = 255U;
                            pixels[offset + 2U] = 255U;
                            pixels[offset + 3U] = alpha;
                        }
                    }
                }
            }
        }

        char line[256] = {};
        const float u0 = static_cast<float>(origin_x) / static_cast<float>(atlas_width);
        const float v0 = static_cast<float>(origin_y) / static_cast<float>(atlas_height);
        const float u1 = static_cast<float>(origin_x + kIconPixels) / static_cast<float>(atlas_width);
        const float v1 =
            static_cast<float>(origin_y + kIconPixels) / static_cast<float>(atlas_height);
        std::snprintf(line, sizeof(line), "%s %.6f %.6f %.6f %.6f\n",
                      files[index].stem().string().c_str(), u0, v0, u1, v1);
        map += line;
    }

    std::string error;
    if (!resonate::iconcook::writePng(atlas_path.string(), static_cast<std::uint32_t>(atlas_width),
                                      static_cast<std::uint32_t>(atlas_height), pixels, error))
    {
        std::fprintf(stderr, "iconcook: %s\n", error.c_str());
        return 1;
    }
    std::ofstream map_out(map_path, std::ios::binary);
    if (!map_out.is_open())
    {
        std::fprintf(stderr, "iconcook: cannot write %s\n", map_path.string().c_str());
        return 1;
    }
    map_out << map;
    std::printf("iconcook: %zu icon(s) -> %dx%d atlas\n", files.size(), atlas_width,
                atlas_height);
    return 0;
}
