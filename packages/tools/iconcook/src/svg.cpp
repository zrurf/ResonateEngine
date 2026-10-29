#include "svg.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace resonate::iconcook
{
namespace
{

constexpr float kPi = 3.14159265358979323846F;
/* Curve and arc budgets: the icons are drawn at 48 px, so a quarter turn at
   eight samples is already finer than a pixel. */
constexpr int kArcSamplesPerTurn = 32;
constexpr int kCubicSamples = 16;
constexpr int kQuadraticSamples = 12;

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1U);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1U);
    }
    return text;
}

/* The value of `name` on the element text `tag`, or an empty view. */
std::string_view attribute(std::string_view tag, std::string_view name)
{
    const std::string pattern = " " + std::string(name) + "=";
    const std::size_t at = tag.find(pattern);
    if (at == std::string_view::npos)
    {
        return {};
    }
    std::size_t position = at + pattern.size();
    if (position >= tag.size() || (tag[position] != '"' && tag[position] != '\''))
    {
        return {};
    }
    const char quote = tag[position++];
    const std::size_t end = tag.find(quote, position);
    if (end == std::string_view::npos)
    {
        return {};
    }
    return tag.substr(position, end - position);
}

float floatAttribute(std::string_view tag, std::string_view name, float fallback)
{
    const std::string_view value = attribute(tag, name);
    if (value.empty())
    {
        return fallback;
    }
    return static_cast<float>(std::atof(std::string(value).c_str()));
}

bool addLine(std::vector<std::vector<Point>>& out, Point from, Point to)
{
    if (std::fabs(from.x - to.x) < 1.0e-5F && std::fabs(from.y - to.y) < 1.0e-5F)
    {
        return false;
    }
    out.push_back({from, to});
    return true;
}

/* SVG arc endpoint parameterization to a sampled polyline (spec F.6.5). */
void appendArc(Point from, float rx, float ry, float rotation_degrees, bool large_arc, bool sweep,
               Point to, std::vector<Point>& out)
{
    if (rx == 0.0F || ry == 0.0F)
    {
        out.push_back(to);
        return;
    }
    const float phi = rotation_degrees * kPi / 180.0F;
    const float cos_phi = std::cos(phi);
    const float sin_phi = std::sin(phi);
    const float dx = (from.x - to.x) * 0.5F;
    const float dy = (from.y - to.y) * 0.5F;
    const float x1 = cos_phi * dx + sin_phi * dy;
    const float y1 = -sin_phi * dx + cos_phi * dy;
    rx = std::fabs(rx);
    ry = std::fabs(ry);

    const float lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1.0F)
    {
        const float scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }

    const float denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float coefficient = 0.0F;
    if (denominator > 0.0F)
    {
        const float numerator =
            std::max(rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, 0.0F);
        coefficient = std::sqrt(numerator / denominator);
        if (large_arc == sweep)
        {
            coefficient = -coefficient;
        }
    }
    const float cx1 = coefficient * rx * y1 / ry;
    const float cy1 = -coefficient * ry * x1 / rx;
    const float cx = cos_phi * cx1 - sin_phi * cy1 + (from.x + to.x) * 0.5F;
    const float cy = sin_phi * cx1 + cos_phi * cy1 + (from.y + to.y) * 0.5F;

    const auto angle_between = [](float ux, float uy, float vx, float vy)
    { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const float ux = (x1 - cx1) / rx;
    const float uy = (y1 - cy1) / ry;
    const float vx = (-x1 - cx1) / rx;
    const float vy = (-y1 - cy1) / ry;
    const float start = angle_between(1.0F, 0.0F, ux, uy);
    float sweep_angle = angle_between(ux, uy, vx, vy);
    if (!sweep && sweep_angle > 0.0F)
    {
        sweep_angle -= 2.0F * kPi;
    }
    else if (sweep && sweep_angle < 0.0F)
    {
        sweep_angle += 2.0F * kPi;
    }

    const int steps = std::clamp(
        static_cast<int>(std::ceil(std::fabs(sweep_angle) / (2.0F * kPi) * kArcSamplesPerTurn)), 2,
        kArcSamplesPerTurn);
    for (int index = 1; index <= steps; ++index)
    {
        const float t = start + sweep_angle * static_cast<float>(index) / static_cast<float>(steps);
        const float x = cx + cos_phi * rx * std::cos(t) - sin_phi * ry * std::sin(t);
        const float y = cy + sin_phi * rx * std::cos(t) + cos_phi * ry * std::sin(t);
        out.push_back(Point{x, y});
    }
}

/* Numbers and commands of one path's `d` attribute. Flags are single
   characters: "a2 2 0 01 3 3" is legal, so they cannot go through the
   number scanner. */
class PathReader
{
  public:
    explicit PathReader(std::string_view text) : text_(text) {}

    void skipSeparators()
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',')
            {
                ++position_;
                continue;
            }
            break;
        }
    }

    bool done()
    {
        skipSeparators();
        return position_ >= text_.size();
    }

    bool peekCommand()
    {
        skipSeparators();
        return position_ < text_.size() &&
               std::isalpha(static_cast<unsigned char>(text_[position_])) != 0;
    }

    char takeCommand()
    {
        return text_[position_++];
    }

    bool number(float& out)
    {
        skipSeparators();
        if (position_ >= text_.size())
        {
            return false;
        }
        const std::string rest(text_.substr(position_));
        char* end = nullptr;
        const float value = std::strtof(rest.c_str(), &end);
        if (end == rest.c_str())
        {
            return false;
        }
        position_ += static_cast<std::size_t>(end - rest.c_str());
        out = value;
        return true;
    }

    bool flag(bool& out)
    {
        skipSeparators();
        if (position_ >= text_.size() || (text_[position_] != '0' && text_[position_] != '1'))
        {
            return false;
        }
        out = text_[position_++] == '1';
        return true;
    }

  private:
    std::string_view text_;
    std::size_t position_ = 0U;
};

bool parsePath(std::string_view data, std::vector<std::vector<Point>>& out, std::string& error)
{
    PathReader reader(data);
    Point current{0.0F, 0.0F};
    Point start{0.0F, 0.0F};
    std::vector<Point> polyline;
    char command = 0;
    Point last_control{0.0F, 0.0F};
    bool last_was_cubic = false;
    bool last_was_quadratic = false;

    const auto flush = [&out, &polyline]()
    {
        if (polyline.size() > 1U)
        {
            out.push_back(polyline);
        }
        polyline.clear();
    };
    const auto push = [&polyline](Point point) { polyline.push_back(point); };

    while (!reader.done())
    {
        char next = command;
        if (reader.peekCommand())
        {
            next = reader.takeCommand();
        }
        else if (command == 'M')
        {
            /* Repeated pairs after a move are implicit line-tos. */
            next = 'L';
        }
        else if (command == 'm')
        {
            next = 'l';
        }
        if (next == 0)
        {
            error = "path data starts without a command";
            return false;
        }

        const bool relative = std::islower(static_cast<unsigned char>(next)) != 0;
        const bool cubic = next == 'C' || next == 'c' || next == 'S' || next == 's';
        const bool quadratic = next == 'Q' || next == 'q' || next == 'T' || next == 't';
        float values[7] = {};
        int count = 0;
        switch (std::toupper(static_cast<unsigned char>(next)))
        {
        case 'M':
        case 'L':
        case 'T':
            count = 2;
            break;
        case 'H':
        case 'V':
            count = 1;
            break;
        case 'C':
            count = 6;
            break;
        case 'S':
        case 'Q':
            count = 4;
            break;
        case 'A':
            count = 7;
            break;
        case 'Z':
            count = 0;
            break;
        default:
            error = std::string("unsupported path command '") + next + "'";
            return false;
        }
        for (int index = 0; index < count; ++index)
        {
            bool ok = false;
            if (std::toupper(static_cast<unsigned char>(next)) == 'A' &&
                (index == 3 || index == 4))
            {
                bool flag = false;
                ok = reader.flag(flag);
                values[index] = flag ? 1.0F : 0.0F;
            }
            else
            {
                ok = reader.number(values[index]);
            }
            if (!ok)
            {
                error = std::string("'") + next + "' is missing arguments";
                return false;
            }
        }

        const Point from = current;
        switch (std::toupper(static_cast<unsigned char>(next)))
        {
        case 'M':
            flush();
            current = Point{relative ? current.x + values[0] : values[0],
                            relative ? current.y + values[1] : values[1]};
            start = current;
            push(current);
            last_control = current;
            break;
        case 'L':
            current = Point{relative ? current.x + values[0] : values[0],
                            relative ? current.y + values[1] : values[1]};
            push(current);
            last_control = current;
            break;
        case 'H':
            current = Point{relative ? current.x + values[0] : values[0], current.y};
            push(current);
            last_control = current;
            break;
        case 'V':
            current = Point{current.x, relative ? current.y + values[0] : values[0]};
            push(current);
            last_control = current;
            break;
        case 'C':
        case 'S':
        {
            Point control_one = last_control;
            int base = 0;
            if (std::toupper(static_cast<unsigned char>(next)) == 'C')
            {
                control_one = Point{relative ? current.x + values[0] : values[0],
                                    relative ? current.y + values[1] : values[1]};
                base = 2;
            }
            else if (!last_was_cubic)
            {
                control_one = current;
            }
            const Point control_two{relative ? current.x + values[base] : values[base],
                                    relative ? current.y + values[base + 1] : values[base + 1]};
            const Point end{relative ? current.x + values[base + 2] : values[base + 2],
                            relative ? current.y + values[base + 3] : values[base + 3]};
            for (int step = 1; step <= kCubicSamples; ++step)
            {
                const float t = static_cast<float>(step) / static_cast<float>(kCubicSamples);
                const float u = 1.0F - t;
                push(Point{u * u * u * from.x + 3.0F * u * u * t * control_one.x +
                               3.0F * u * t * t * control_two.x + t * t * t * end.x,
                           u * u * u * from.y + 3.0F * u * u * t * control_one.y +
                               3.0F * u * t * t * control_two.y + t * t * t * end.y});
            }
            last_control = control_two;
            current = end;
            break;
        }
        case 'Q':
        case 'T':
        {
            Point control = last_control;
            int base = 0;
            if (std::toupper(static_cast<unsigned char>(next)) == 'Q')
            {
                control = Point{relative ? current.x + values[0] : values[0],
                                relative ? current.y + values[1] : values[1]};
                base = 2;
            }
            else if (!last_was_quadratic)
            {
                control = current;
            }
            const Point end{relative ? current.x + values[base] : values[base],
                            relative ? current.y + values[base + 1] : values[base + 1]};
            for (int step = 1; step <= kQuadraticSamples; ++step)
            {
                const float t = static_cast<float>(step) / static_cast<float>(kQuadraticSamples);
                const float u = 1.0F - t;
                push(Point{u * u * from.x + 2.0F * u * t * control.x + t * t * end.x,
                           u * u * from.y + 2.0F * u * t * control.y + t * t * end.y});
            }
            last_control = control;
            current = end;
            break;
        }
        case 'A':
        {
            Point end{relative ? current.x + values[5] : values[5],
                      relative ? current.y + values[6] : values[6]};
            if (polyline.empty())
            {
                push(current);
            }
            appendArc(current, values[0], values[1], values[2], values[3] >= 0.5F,
                      values[4] >= 0.5F, end, polyline);
            current = end;
            last_control = current;
            break;
        }
        case 'Z':
            /* The stroke's round caps make an open run of coincident ends
               indistinguishable from a closed one, so closing just restarts. */
            push(start);
            flush();
            current = start;
            last_control = current;
            break;
        default:
            break;
        }
        last_was_cubic = cubic;
        last_was_quadratic = quadratic;
        command = next;
    }
    flush();
    return true;
}

/* `<circle>`, `<line>`, `<rect>` and `<polygon>`/`<polyline>` as polylines. */
bool parseElement(std::string_view tag, std::string_view name, Outline& outline,
                  std::string& error)
{
    if (name == "path")
    {
        const std::string_view data = attribute(tag, "d");
        if (data.empty())
        {
            error = "a path has no d attribute";
            return false;
        }
        return parsePath(data, outline.polylines, error);
    }
    if (name == "circle")
    {
        const Point centre{floatAttribute(tag, "cx", 0.0F), floatAttribute(tag, "cy", 0.0F)};
        const float radius = floatAttribute(tag, "r", 0.0F);
        std::vector<Point> points;
        for (int step = 0; step <= kArcSamplesPerTurn; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(kArcSamplesPerTurn) *
                            2.0F * kPi;
            points.push_back(Point{centre.x + radius * std::cos(t), centre.y + radius * std::sin(t)});
        }
        outline.polylines.push_back(std::move(points));
        return true;
    }
    if (name == "line")
    {
        std::vector<std::vector<Point>>& out = outline.polylines;
        addLine(out,
                Point{floatAttribute(tag, "x1", 0.0F), floatAttribute(tag, "y1", 0.0F)},
                Point{floatAttribute(tag, "x2", 0.0F), floatAttribute(tag, "y2", 0.0F)});
        return true;
    }
    if (name == "rect")
    {
        const float x = floatAttribute(tag, "x", 0.0F);
        const float y = floatAttribute(tag, "y", 0.0F);
        const float width = floatAttribute(tag, "width", 0.0F);
        const float height = floatAttribute(tag, "height", 0.0F);
        const float radius = std::min(floatAttribute(tag, "rx", 0.0F),
                                      std::min(width, height) * 0.5F);
        std::vector<Point> points;
        const auto corner = [&points, radius](Point centre, float from, float to)
        {
            constexpr int kSteps = 8;
            for (int step = 0; step <= kSteps; ++step)
            {
                const float t = from + (to - from) * static_cast<float>(step) /
                                          static_cast<float>(kSteps);
                points.push_back(Point{centre.x + radius * std::cos(t),
                                       centre.y + radius * std::sin(t)});
            }
        };
        if (radius <= 0.0F)
        {
            points = {{x, y}, {x + width, y}, {x + width, y + height}, {x, y + height}, {x, y}};
        }
        else
        {
            corner(Point{x + width - radius, y + radius}, -0.5F * kPi, 0.0F);
            corner(Point{x + width - radius, y + height - radius}, 0.0F, 0.5F * kPi);
            corner(Point{x + radius, y + height - radius}, 0.5F * kPi, kPi);
            corner(Point{x + radius, y + radius}, kPi, 1.5F * kPi);
            points.push_back(points.front());
        }
        outline.polylines.push_back(std::move(points));
        return true;
    }
    if (name == "polygon" || name == "polyline")
    {
        const std::string_view data = attribute(tag, "points");
        PathReader reader(data);
        std::vector<Point> points;
        float x = 0.0F;
        float y = 0.0F;
        while (true)
        {
            if (!reader.number(x) || !reader.number(y))
            {
                break;
            }
            points.emplace_back(Point{x, y});
        }
        if (name == "polygon" && !points.empty())
        {
            points.push_back(points.front());
        }
        if (points.size() > 1U)
        {
            outline.polylines.push_back(std::move(points));
        }
        return true;
    }
    error = "unsupported element <" + std::string(name) + ">";
    return false;
}

} // namespace

bool parseSvg(const std::string& text, Outline& outline, std::string& error)
{
    outline = Outline{};
    const std::string_view root(text);
    const std::string_view stroke_width = attribute(root, "stroke-width");
    if (!stroke_width.empty())
    {
        outline.stroke_width = static_cast<float>(std::atof(std::string(stroke_width).c_str()));
    }
    const std::string_view view_box = attribute(root, "viewBox");
    if (!view_box.empty())
    {
        float values[4] = {};
        PathReader reader(view_box);
        for (float& value : values)
        {
            if (!reader.number(value))
            {
                error = "the viewBox needs four numbers";
                return false;
            }
        }
        outline.view_box = std::max(values[2], values[3]);
    }

    std::size_t position = 0U;
    while ((position = text.find('<', position)) != std::string::npos)
    {
        const std::size_t end = text.find('>', position);
        if (end == std::string::npos)
        {
            error = "an element is not closed";
            return false;
        }
        const std::string_view tag = trim(std::string_view(text).substr(position + 1U, end - position - 1U));
        position = end + 1U;
        if (tag.empty() || tag.front() == '?' || tag.front() == '!' || tag.front() == '/')
        {
            continue;
        }
        const std::size_t name_end = tag.find_first_of(" \t\r\n/");
        const std::string_view name = tag.substr(0U, name_end);
        if (name == "svg" || name == "g")
        {
            continue;
        }
        if (!parseElement(tag, name, outline, error))
        {
            return false;
        }
    }
    if (outline.polylines.empty())
    {
        error = "the icon has no drawable elements";
        return false;
    }
    return true;
}

} // namespace resonate::iconcook
