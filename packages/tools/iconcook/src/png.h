#ifndef RESONATE_ICONCOOK_PNG_H
#define RESONATE_ICONCOOK_PNG_H

#include <cstdint>
#include <string>
#include <vector>

namespace resonate::iconcook
{

/* Writes a straight-alpha RGBA8, non-interlaced PNG. The pixel payload rides
   uncompressed deflate blocks, so the file is larger than a compressed one but
   the tool needs no zlib; the atlas is written once and committed. */
bool writePng(const std::string& path, std::uint32_t width, std::uint32_t height,
              const std::vector<std::uint8_t>& rgba, std::string& error);

} // namespace resonate::iconcook

#endif /* RESONATE_ICONCOOK_PNG_H */
