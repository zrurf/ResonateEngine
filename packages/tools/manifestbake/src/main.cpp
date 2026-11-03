/* Bakes one TOML manifest into the runtime's binary form: packaged builds ship
   the baked file and skip the text parse (and the source paths a manifest
   carries never travel with the build). The source tree keeps the TOML. */

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <resonate/asset/baked_manifest.h>

#include <toml++/toml.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: manifestbake <in.toml> <out.rman>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in.is_open())
    {
        std::cerr << "manifestbake: cannot open " << argv[1] << "\n";
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const toml::parse_result result = toml::parse(text);
    if (!result)
    {
        std::cerr << "manifestbake: " << argv[1] << " does not parse\n";
        return 1;
    }
    std::vector<std::uint8_t> baked;
    std::string error;
    if (!resonate::asset::bakeTable(result.table(), baked, error))
    {
        std::cerr << "manifestbake: " << error << "\n";
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    if (!out.is_open())
    {
        std::cerr << "manifestbake: cannot write " << argv[2] << "\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(baked.data()),
              static_cast<std::streamsize>(baked.size()));
    if (!out.good())
    {
        std::cerr << "manifestbake: writing " << argv[2] << " failed\n";
        return 1;
    }
    std::cout << "manifestbake: " << argv[1] << " -> " << argv[2] << " (" << baked.size()
              << " bytes)\n";
    return 0;
}
