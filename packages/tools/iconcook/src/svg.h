#ifndef RESONATE_ICONCOOK_SVG_H
#define RESONATE_ICONCOOK_SVG_H

#include <string>
#include <vector>

namespace resonate::iconcook
{

struct Point
{
    float x = 0.0F;
    float y = 0.0F;
};

/* One icon's outline, flattened into polylines in viewBox units. The shipped
   icons are stroke-only, so every element contributes its centre line: paths
   (with curves and arcs sampled), circles, lines, rects and polygons. */
struct Outline
{
    std::vector<std::vector<Point>> polylines;
    float stroke_width = 2.0F;
    float view_box = 24.0F;
};

/* Answers false with `error` for an unsupported element or a malformed path. */
bool parseSvg(const std::string& text, Outline& outline, std::string& error);

} // namespace resonate::iconcook

#endif /* RESONATE_ICONCOOK_SVG_H */
