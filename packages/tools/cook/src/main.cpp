/* The cook step's command line: <content-directory> <output-directory>. The
 * xmake target runs it after linking, so a build leaves the cooked tree
 * current; a packaging step copies that tree beside the executable. */

#include <cstdio>
#include <string>

#include <resonate/cook/cook.h>

namespace
{

void report(void*, const char* message)
{
    std::fprintf(stderr, "cook: %s\n", message != nullptr ? message : "");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: cook <content-directory> <output-directory>\n");
        return 1;
    }
    resonate::cook::Summary summary;
    const std::string source = argv[1];
    const std::string out = argv[2];
    if (!resonate::cook::run(source, out, summary, nullptr, &report))
    {
        return 1;
    }
    std::printf("cook: %u document(s), %u clip(s), %u string table(s), %u copied, %u skipped"
                " -> %s\n",
                summary.documents, summary.clips, summary.strings, summary.copies, summary.skipped,
                out.c_str());
    return 0;
}
