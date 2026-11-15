#include "ui/framework/icon_manager.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

float number_value(std::string_view text, std::size_t* pos) {
    const char* begin = text.data() + *pos;
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    if (end == begin) {
        return 0.0f;
    }
    *pos = static_cast<std::size_t>(end - text.data());
    return value;
}

bool extract_viewbox(const std::string& svg, float* x, float* y, float* w, float* h) {
    const std::size_t key = svg.find("viewBox=\"");
    if (key == std::string::npos) {
        return false;
    }
    const std::size_t begin = key + 9;
    const std::size_t end = svg.find('"', begin);
    if (end == std::string::npos) {
        return false;
    }
    const std::string value = svg.substr(begin, end - begin);
    float vx = 0.0f;
    float vy = 0.0f;
    float vw = 24.0f;
    float vh = 24.0f;
    std::istringstream stream(value);
    if (!(stream >> vx >> vy >> vw >> vh) || vw <= 0.0f || vh <= 0.0f) {
        return false;
    }
    *x = vx;
    *y = vy;
    *w = vw;
    *h = vh;
    return true;
}

class PathParser {
public:
    PathParser(std::string_view data, ID2D1GeometrySink* sink)
        : m_data(data), m_sink(sink) {}

    bool parse() {
        while (skip_separators(), m_pos < m_data.size()) {
            char command = 0;
            if (is_alpha(m_data[m_pos])) {
                command = m_data[m_pos++];
                m_command = command;
            } else if (m_command != 0) {
                command = m_command;
            } else {
                return false;
            }
            if (!parse_command(command)) {
                return false;
            }
        }
        close_figure();
        return true;
    }

private:
    static bool is_alpha(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    void skip_separators() {
        while (m_pos < m_data.size()) {
            const char c = m_data[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool has_number() {
        skip_separators();
        if (m_pos >= m_data.size()) {
            return false;
        }
        const char c = m_data[m_pos];
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
    }

    bool read(float* value) {
        skip_separators();
        if (m_pos >= m_data.size()) {
            return false;
        }
        const std::size_t before = m_pos;
        *value = number_value(m_data, &m_pos);
        return m_pos != before;
    }

    bool read(Point* point, bool relative) {
        float x = 0.0f;
        float y = 0.0f;
        if (!read(&x) || !read(&y)) {
            return false;
        }
        point->x = relative ? m_current.x + x : x;
        point->y = relative ? m_current.y + y : y;
        return true;
    }

    void begin_figure(const Point& point) {
        close_figure();
        m_sink->BeginFigure(D2D1::Point2F(point.x, point.y), D2D1_FIGURE_BEGIN_FILLED);
        m_figure_open = true;
        m_start = point;
        m_current = point;
    }

    void close_figure() {
        if (m_figure_open) {
            m_sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            m_figure_open = false;
        }
    }

    void line_to(const Point& point) {
        if (!m_have_point) {
            return;
        }
        m_sink->AddLine(D2D1::Point2F(point.x, point.y));
        m_current = point;
    }

    bool parse_command(char command) {
        const bool relative = command >= 'a' && command <= 'z';
        switch (command == 'z' ? 'Z' : (relative ? static_cast<char>(command - 32) : command)) {
            case 'M':
                if (!read(&m_current, relative)) {
                    return false;
                }
                begin_figure(m_current);
                m_have_point = true;
                while (has_number()) {
                    Point point;
                    if (!read(&point, relative)) {
                        return false;
                    }
                    line_to(point);
                }
                return true;
            case 'L':
                while (has_number()) {
                    Point point;
                    if (!read(&point, relative)) {
                        return false;
                    }
                    line_to(point);
                }
                return true;
            case 'H':
                while (has_number()) {
                    float x = 0.0f;
                    if (!read(&x)) {
                        return false;
                    }
                    Point point{m_current.x + (relative ? x : 0.0f), m_current.y};
                    if (!relative) {
                        point.x = x;
                    }
                    line_to(point);
                }
                return true;
            case 'V':
                while (has_number()) {
                    float y = 0.0f;
                    if (!read(&y)) {
                        return false;
                    }
                    Point point{m_current.x, m_current.y + (relative ? y : 0.0f)};
                    if (!relative) {
                        point.y = y;
                    }
                    line_to(point);
                }
                return true;
            case 'C':
                while (has_number()) {
                    Point c1;
                    Point c2;
                    Point end;
                    if (!read(&c1, relative) || !read(&c2, relative) || !read(&end, relative)) {
                        return false;
                    }
                    m_sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(c1.x, c1.y), D2D1::Point2F(c2.x, c2.y),
                        D2D1::Point2F(end.x, end.y)));
                    m_last_control = c2;
                    m_current = end;
                }
                return true;
            case 'S':
                while (has_number()) {
                    Point c2;
                    Point end;
                    if (!read(&c2, relative) || !read(&end, relative)) {
                        return false;
                    }
                    const Point c1{2.0f * m_current.x - m_last_control.x,
                                   2.0f * m_current.y - m_last_control.y};
                    m_sink->AddBezier(D2D1::BezierSegment(
                        D2D1::Point2F(c1.x, c1.y), D2D1::Point2F(c2.x, c2.y),
                        D2D1::Point2F(end.x, end.y)));
                    m_last_control = c2;
                    m_current = end;
                }
                return true;
            case 'Q':
                while (has_number()) {
                    Point control;
                    Point end;
                    if (!read(&control, relative) || !read(&end, relative)) {
                        return false;
                    }
                    m_sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                        D2D1::Point2F(control.x, control.y), D2D1::Point2F(end.x, end.y)));
                    m_last_control = control;
                    m_current = end;
                }
                return true;
            case 'T':
                while (has_number()) {
                    Point end;
                    if (!read(&end, relative)) {
                        return false;
                    }
                    const Point control{2.0f * m_current.x - m_last_control.x,
                                        2.0f * m_current.y - m_last_control.y};
                    m_sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                        D2D1::Point2F(control.x, control.y), D2D1::Point2F(end.x, end.y)));
                    m_last_control = control;
                    m_current = end;
                }
                return true;
            case 'A':
                // Material Symbols 的单色图标不使用圆弧；为通用性退化为直线。
                while (has_number()) {
                    float rx = 0.0f;
                    float ry = 0.0f;
                    float rotation = 0.0f;
                    float large = 0.0f;
                    float sweep = 0.0f;
                    Point end;
                    if (!read(&rx) || !read(&ry) || !read(&rotation) || !read(&large) ||
                        !read(&sweep) || !read(&end, relative)) {
                        return false;
                    }
                    (void)rotation;
                    (void)large;
                    (void)sweep;
                    line_to(end);
                }
                return true;
            case 'Z':
                close_figure();
                m_current = m_start;
                m_have_point = true;
                return true;
            default:
                return false;
        }
    }

    std::string_view m_data;
    ID2D1GeometrySink* m_sink = nullptr;
    std::size_t m_pos = 0;
    char m_command = 0;
    bool m_figure_open = false;
    bool m_have_point = false;
    Point m_current{};
    Point m_start{};
    Point m_last_control{};
};

bool parse_path_data(ID2D1GeometrySink* sink, std::string_view data) {
    PathParser parser(data, sink);
    return parser.parse();
}

// 属性名前必须带空格，避免 "x=" 误命中 "rx="。
float attr_number(const std::string& tag, const char* key, float fallback) {
    const std::string pattern = std::string(" ") + key + "=\"";
    const std::size_t pos = tag.find(pattern);
    if (pos == std::string::npos) {
        return fallback;
    }
    const char* begin = tag.c_str() + pos + pattern.size();
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    return end == begin ? fallback : value;
}

void append_rounded_rect(ID2D1GeometrySink* sink, float x, float y, float w, float h,
                         float radius) {
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    const float r = (std::max)(0.0f, (std::min)(radius, (std::min)(w, h) * 0.5f));
    sink->BeginFigure(D2D1::Point2F(x + r, y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w - r, y));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
        D2D1::Point2F(x + w, y), D2D1::Point2F(x + w, y + r)));
    sink->AddLine(D2D1::Point2F(x + w, y + h - r));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
        D2D1::Point2F(x + w, y + h), D2D1::Point2F(x + w - r, y + h)));
    sink->AddLine(D2D1::Point2F(x + r, y + h));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
        D2D1::Point2F(x, y + h), D2D1::Point2F(x, y + h - r)));
    sink->AddLine(D2D1::Point2F(x, y + r));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
        D2D1::Point2F(x, y), D2D1::Point2F(x + r, y)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

void append_circle(ID2D1GeometrySink* sink, float cx, float cy, float r) {
    if (r <= 0.0f) {
        return;
    }
    constexpr float kKappa = 0.5522847498f;
    const float k = r * kKappa;
    sink->BeginFigure(D2D1::Point2F(cx + r, cy), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + r, cy + k),
                                        D2D1::Point2F(cx + k, cy + r),
                                        D2D1::Point2F(cx, cy + r)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - k, cy + r),
                                        D2D1::Point2F(cx - r, cy + k),
                                        D2D1::Point2F(cx - r, cy)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - r, cy - k),
                                        D2D1::Point2F(cx - k, cy - r),
                                        D2D1::Point2F(cx, cy - r)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + k, cy - r),
                                        D2D1::Point2F(cx + r, cy - k),
                                        D2D1::Point2F(cx + r, cy)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

}  // namespace

IconManager& IconManager::instance() {
    static IconManager manager;
    return manager;
}

bool IconManager::has(const std::string& name) const {
    for (const auto& set : m_sets) {
        if (set->icons.find(name) != set->icons.end()) {
            return true;
        }
    }
    return false;
}

// D-119：按工厂取分桶。窗口销毁后其工厂不再出现，分桶自然失效；
// 超过上限时淘汰「非当前」的一桶（几何随工厂引用一起释放，不会悬挂）。
IconManager::IconSet* IconManager::set_for(ID2D1RenderTarget* target) {
    if (target == nullptr) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    if (factory == nullptr) {
        return nullptr;
    }
    for (const auto& set : m_sets) {
        if (set->factory.Get() == factory.Get()) {
            m_active = set.get();
            return set.get();
        }
    }
    if (m_sets.size() >= kMaxSets) {
        for (auto it = m_sets.begin(); it != m_sets.end(); ++it) {
            if (it->get() != m_active) {
                m_sets.erase(it);
                break;
            }
        }
    }
    auto set = std::make_unique<IconSet>();
    set->factory = factory;
    m_sets.push_back(std::move(set));
    m_active = m_sets.back().get();
    return m_active;
}

const IconManager::Icon* IconManager::get_in(IconSet* set, const std::string& name) {
    if (set == nullptr) {
        return nullptr;
    }
    if (set->missing.find(name) != set->missing.end()) {
        return nullptr;
    }
    auto found = set->icons.find(name);
    if (found != set->icons.end()) {
        return &found->second;
    }
    Icon icon;
    if (!load(set, name, &icon)) {
        set->missing[name] = true;
        return nullptr;
    }
    auto inserted = set->icons.emplace(name, std::move(icon));
    return &inserted.first->second;
}

bool IconManager::load(IconSet* set, const std::string& name, Icon* icon) {
    if (set == nullptr || set->factory == nullptr || icon == nullptr) {
        return false;
    }
    std::wstring wide = sscl::util::utf8_to_wide(name);
    for (wchar_t& ch : wide) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    HRSRC resource = ::FindResourceW(instance, wide.c_str(), RT_RCDATA);
    if (resource == nullptr) {
        util::log_warn("[icons] 缺少资源：" + name);
        return false;
    }
    HGLOBAL loaded = ::LoadResource(instance, resource);
    const void* data = loaded != nullptr ? ::LockResource(loaded) : nullptr;
    const DWORD size = data != nullptr ? ::SizeofResource(instance, resource) : 0;
    if (data == nullptr || size == 0) {
        util::log_warn("[icons] 资源为空：" + name);
        return false;
    }
    const std::string svg(static_cast<const char*>(data), static_cast<std::size_t>(size));
    if (!extract_viewbox(svg, &icon->view_x, &icon->view_y, &icon->view_w, &icon->view_h)) {
        util::log_warn("[icons] viewBox 无效：" + name);
        return false;
    }
    if (set->stroke_style == nullptr) {
        // 描边型图标（version-*.svg）要求 round cap/join；填充型图标不受影响。
        D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        set->factory->CreateStrokeStyle(&properties, nullptr, 0,
                                        set->stroke_style.GetAddressOf());
    }
    if (!parse_svg(set, svg, icon)) {
        util::log_warn("[icons] SVG 图元解析失败：" + name);
        return false;
    }
    return true;
}

bool IconManager::parse_svg(IconSet* set, const std::string& svg, Icon* icon) {
    const std::size_t root_end = svg.find('>');
    if (root_end == std::string::npos) {
        return false;
    }
    const std::string root_tag = svg.substr(0, root_end + 1);
    // 现有 Material Symbols：根标签无 fill/stroke → 按 SVG 默认「填充、不描边」。
    // 版本类型图标：根标签 fill="none" stroke="currentColor" → 「描边、不填充」。
    const bool root_fill_none = root_tag.find("fill=\"none\"") != std::string::npos;
    const bool root_stroke = root_tag.find("stroke=\"currentColor\"") != std::string::npos;
    const float root_width = attr_number(root_tag, "stroke-width", 2.0f);

    std::size_t pos = 0;
    while ((pos = svg.find('<', pos)) != std::string::npos) {
        const std::size_t tag_end = svg.find('>', pos);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string tag = svg.substr(pos, tag_end - pos + 1);
        pos = tag_end + 1;

        int element = 0;  // 1=path, 2=rect, 3=circle
        if (tag.rfind("<path", 0) == 0) {
            element = 1;
        } else if (tag.rfind("<rect", 0) == 0) {
            element = 2;
        } else if (tag.rfind("<circle", 0) == 0) {
            element = 3;
        } else {
            continue;
        }

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
        if (FAILED(set->factory->CreatePathGeometry(geometry.GetAddressOf()))) {
            continue;
        }
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geometry->Open(sink.GetAddressOf())) || sink == nullptr) {
            continue;
        }
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        bool parsed = false;
        if (element == 1) {
            const std::size_t d_key = tag.find(" d=\"");
            if (d_key != std::string::npos) {
                const std::size_t d_begin = d_key + 4;
                const std::size_t d_end = tag.find('"', d_begin);
                if (d_end != std::string::npos) {
                    parsed = parse_path_data(
                        sink.Get(), std::string_view(tag).substr(d_begin, d_end - d_begin));
                }
            }
        } else if (element == 2) {
            append_rounded_rect(sink.Get(), attr_number(tag, "x", 0.0f),
                                attr_number(tag, "y", 0.0f),
                                attr_number(tag, "width", 0.0f),
                                attr_number(tag, "height", 0.0f),
                                attr_number(tag, "rx", 0.0f));
            parsed = true;
        } else {
            append_circle(sink.Get(), attr_number(tag, "cx", 0.0f),
                          attr_number(tag, "cy", 0.0f), attr_number(tag, "r", 0.0f));
            parsed = true;
        }
        sink->Close();
        if (!parsed) {
            continue;
        }

        bool fill = !root_fill_none;
        bool stroke = root_stroke;
        float width = attr_number(tag, "stroke-width", root_width);
        // 子元素可覆盖根属性：neoforge 的眼睛/嘴是 fill="currentColor" stroke="none"。
        if (tag.find("fill=\"currentColor\"") != std::string::npos) {
            fill = true;
        }
        if (tag.find("fill=\"none\"") != std::string::npos) {
            fill = false;
        }
        if (tag.find("stroke=\"currentColor\"") != std::string::npos) {
            stroke = true;
        }
        if (tag.find("stroke=\"none\"") != std::string::npos) {
            stroke = false;
        }
        if (fill) {
            icon->shapes.push_back(Shape{geometry, false, width});
        }
        if (stroke) {
            icon->shapes.push_back(Shape{geometry, true, width});
        }
    }
    return !icon->shapes.empty();
}

bool IconManager::draw(ID2D1RenderTarget* target, const std::string& name,
                       const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float opacity) {
    if (target == nullptr || rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    IconSet* set = set_for(target);
    const Icon* icon = get_in(set, name);
    if (set == nullptr || icon == nullptr || icon->shapes.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf())) || brush == nullptr) {
        return false;
    }
    brush->SetOpacity((std::max)(0.0f, (std::min)(1.0f, opacity)));

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float scale = (std::min)(width / icon->view_w, height / icon->view_h);
    const float tx = rect.left + (width - icon->view_w * scale) * 0.5f - icon->view_x * scale;
    const float ty = rect.top + (height - icon->view_h * scale) * 0.5f - icon->view_y * scale;
    D2D1_MATRIX_3X2_F old{};
    target->GetTransform(&old);
    // strokeWidth 是 viewBox 单位：transform 的 Scale 会同时缩放几何与笔触宽度。
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(tx, ty));
    for (const Shape& shape : icon->shapes) {
        if (shape.stroke) {
            target->DrawGeometry(shape.geometry.Get(), brush.Get(), shape.stroke_width,
                                 set->stroke_style.Get());
        } else {
            target->FillGeometry(shape.geometry.Get(), brush.Get());
        }
    }
    target->SetTransform(old);
    return true;
}

}  // namespace sscl::ui
