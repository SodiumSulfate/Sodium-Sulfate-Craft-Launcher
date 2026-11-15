#include "ui/framework/motion.hpp"

#include <algorithm>
#include <cmath>

#include <d2d1helper.h>
#include <wrl/client.h>

#include "ui/theme/theme.hpp"

namespace sscl::ui::motion {

namespace {

Level g_level = Level::kFull;
int g_guard = 0;
constexpr float kTau = 6.28318530718f;
float g_spinner_angle = 0.0f;  // D-102：全局加载指示器相位（弧度）

constexpr float kBaseHover = 90.0f;
constexpr float kBasePress = 60.0f;
constexpr float kBaseFade = 120.0f;
constexpr float kBasePop = 160.0f;
constexpr float kBasePage = 180.0f;
constexpr float kBaseScrollbar = 90.0f;

constexpr float kPopOffset = 8.0f;
constexpr float kPageOffset = 6.0f;

float base_ms(Kind kind) {
    switch (kind) {
        case Kind::kHover:
            return kBaseHover;
        case Kind::kPress:
            return kBasePress;
        case Kind::kFade:
            return kBaseFade;
        case Kind::kPop:
            return kBasePop;
        case Kind::kPage:
            return kBasePage;
        case Kind::kScrollbar:
            return kBaseScrollbar;
    }
    return kBaseHover;
}

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

Level level() {
    return g_level;
}

void set_level(Level value) {
    g_level = value;
}

const char* level_key(Level value) {
    switch (value) {
        case Level::kFull:
            return "full";
        case Level::kSoft:
            return "soft";
        case Level::kReduced:
            return "reduced";
        case Level::kOff:
            return "off";
    }
    return "full";
}

Level level_from_key(const std::string& key) {
    if (key == "soft") {
        return Level::kSoft;
    }
    if (key == "reduced") {
        return Level::kReduced;
    }
    if (key == "off") {
        return Level::kOff;
    }
    return Level::kFull;
}

const wchar_t* level_label(Level value) {
    switch (value) {
        case Level::kFull:
            return L"完整";
        case Level::kSoft:
            return L"柔和";
        case Level::kReduced:
            return L"精简";
        case Level::kOff:
            return L"关闭";
    }
    return L"完整";
}

float level_scale() {
    switch (g_level) {
        case Level::kFull:
            return 1.0f;
        case Level::kSoft:
            return 0.6f;
        case Level::kReduced:
            return 0.3f;
        case Level::kOff:
            return 0.0f;
    }
    return 1.0f;
}

float duration(Kind kind) {
    const float scale = level_scale();
    if (scale <= 0.0f) {
        return 0.0f;
    }
    return base_ms(kind) * scale;
}

float offset(Kind kind) {
    const float scale = level_scale();
    if (scale <= 0.0f) {
        return 0.0f;
    }
    if (kind == Kind::kPop) {
        return kPopOffset * scale;
    }
    if (kind == Kind::kPage) {
        return kPageOffset * scale;
    }
    return 0.0f;
}

float pop_scale() {
    // 0.96 起步，随强度收敛到 1。
    return 1.0f - 0.04f * level_scale();
}

float dim_opacity() {
    return 0.72f;
}

float ease_out(float t) {
    const float c = clamp01(t);
    return 1.0f - (1.0f - c) * (1.0f - c);
}

float ease_in(float t) {
    const float c = clamp01(t);
    return c * c;
}

Guard::Guard() {
    ++g_guard;
}

Guard::~Guard() {
    if (g_guard > 0) {
        --g_guard;
    }
}

bool suppressed() {
    return g_guard > 0;
}

bool tween(float& value, float target, float dt_ms, Kind kind) {
    if (suppressed()) {
        value = target;
        return false;
    }
    const float ms = duration(kind);
    if (ms <= 0.0f || dt_ms <= 0.0f) {
        value = target;
        return false;
    }
    // 指数趋近：3τ ≈ 95% 收敛，起停比线性步进柔和（PCL In/Out Fluent 的简化）。
    const float factor = 1.0f - std::exp(-dt_ms / (ms / 3.0f));
    value += (target - value) * factor;
    if (std::fabs(target - value) < 0.0005f) {
        value = target;
        return false;
    }
    return true;
}

bool advance(float& value, float target, float dt_ms, Kind kind) {
    if (suppressed()) {
        value = target;
        return false;
    }
    const float ms = duration(kind);
    if (ms <= 0.0f || dt_ms <= 0.0f) {
        value = target;
        return false;
    }
    const float step = dt_ms / ms;
    if (value < target) {
        value = target < value + step ? target : value + step;
    } else if (value > target) {
        value = target > value - step ? target : value - step;
    }
    return value != target;
}

void Progress::to(float target, Kind kind) {
    const float clamped = clamp01(target);
    if (suppressed() || duration(kind) <= 0.0f) {
        jump(clamped);
        return;
    }
    if (m_active && clamped == m_target) {
        return;
    }
    if (!m_active && clamped == m_value) {
        return;
    }
    m_from = m_value;
    m_target = clamped;
    m_elapsed = 0.0f;
    m_duration = duration(kind);
    m_active = true;
}

void Progress::jump(float value) {
    m_value = clamp01(value);
    m_from = m_value;
    m_target = m_value;
    m_elapsed = 0.0f;
    m_duration = 0.0f;
    m_active = false;
}

bool Progress::update(float dt_ms) {
    if (!m_active) {
        return false;
    }
    m_elapsed += (std::max)(0.0f, dt_ms);
    const float t = m_duration > 0.0f ? m_elapsed / m_duration : 1.0f;
    if (t >= 1.0f) {
        m_value = m_target;
        m_active = false;
        return true;  // 最后一帧仍需重绘到终点
    }
    m_value = m_from + (m_target - m_from) * t;
    return true;
}

void Pop::open() {
    m_closing = false;
    m_progress.to(1.0f, Kind::kPop);
}

void Pop::close() {
    m_closing = true;
    m_progress.to(0.0f, Kind::kPop);
}

void Pop::set_open(bool value, int tag) {
    if (value) {
        m_tag = tag;
        open();
    } else {
        close();
    }
}

void Pop::jump_open() {
    m_closing = false;
    m_progress.jump(1.0f);
}

void Pop::jump_closed() {
    m_closing = false;
    m_progress.jump(0.0f);
}

bool Pop::update(float dt_ms) {
    return m_progress.update(dt_ms);
}

float Pop::scale() const {
    return 1.0f - (1.0f - pop_scale()) * (1.0f - m_progress.eased());
}

float Pop::offset_y() const {
    return offset(Kind::kPop) * (1.0f - m_progress.eased());
}

float Pop::dim() const {
    return dim_opacity() * m_progress.eased();
}

bool push_pop(ID2D1RenderTarget* target, const Pop& pop, const D2D1_RECT_F& card) {
    if (target == nullptr || card.right <= card.left || card.bottom <= card.top) {
        return false;
    }
    const float scale = pop.scale();
    const float dy = pop.offset_y();
    const D2D1_POINT_2F center =
        D2D1::Point2F((card.left + card.right) * 0.5f, (card.top + card.bottom) * 0.5f);
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Translation(-center.x, -center.y) *
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(center.x, center.y + dy);
    // 透明度直接用图层 opacity（不建画刷，避免 PopLayer 前画刷生命周期问题）。
    const D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters(
        D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, transform,
        clamp01(pop.opacity()), nullptr);
    target->PushLayer(&params, nullptr);
    return true;
}

void pop_pop(ID2D1RenderTarget* target) {
    if (target != nullptr) {
        target->PopLayer();
    }
}

bool push_fade(ID2D1RenderTarget* target, float opacity, float offset_y) {
    if (target == nullptr || opacity >= 0.999f) {
        return false;
    }
    const D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Translation(0.0f, offset_y);
    const D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters(
        D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, transform,
        clamp01(opacity), nullptr);
    target->PushLayer(&params, nullptr);
    return true;
}

void pop_layer(ID2D1RenderTarget* target) {
    if (target != nullptr) {
        target->PopLayer();
    }
}

D2D1_COLOR_F blend(D2D1_COLOR_F from, D2D1_COLOR_F to, float t) {
    const float k = clamp01(t);
    D2D1_COLOR_F out{};
    out.r = from.r + (to.r - from.r) * k;
    out.g = from.g + (to.g - from.g) * k;
    out.b = from.b + (to.b - from.b) * k;
    out.a = from.a + (to.a - from.a) * k;
    return out;
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool rect_same(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return std::fabs(a.left - b.left) < 0.6f && std::fabs(a.top - b.top) < 0.6f &&
           std::fabs(a.right - b.right) < 0.6f && std::fabs(a.bottom - b.bottom) < 0.6f;
}

StateColors button_colors(bool primary, bool enabled, const D2D1_RECT_F& rect) {
    StateColors out{};
    hot().observe(rect);
    const float hw = enabled ? hot().hover_weight(rect) : 0.0f;
    const float pw = enabled ? hot().press_weight(rect) : 0.0f;
    const float state = (std::max)(hw, pw);
    out.hover = hw;
    out.press = pw;
    if (!enabled) {
        out.fill = theme_surface();
        out.text = theme_text_disabled();
        out.border = theme_border();
    } else if (primary) {
        out.fill = blend(theme_primary(), theme_primary_hover(), hw);
        out.fill = blend(out.fill, theme_primary_pressed(), pw);
        out.text = theme_text_on_primary();
        out.border = out.fill;
    } else {
        out.fill = blend(theme_surface(), theme_hover_background(), hw);
        out.fill = blend(out.fill, theme_primary_container(), pw);
        out.text = blend(theme_text_primary(), theme_primary(), state);
        out.border = blend(theme_border(), theme_primary(), state);
    }
    return out;
}

StateColors row_colors(D2D1_COLOR_F base, D2D1_COLOR_F hover_color,
                       D2D1_COLOR_F pressed_color, const D2D1_RECT_F& rect) {
    StateColors out{};
    hot().observe(rect);
    out.hover = hot().hover_weight(rect);
    out.press = hot().press_weight(rect);
    out.fill = blend(base, hover_color, out.hover);
    out.fill = blend(out.fill, pressed_color, out.press);
    out.text = blend(theme_text_primary(), theme_primary(), (std::max)(out.hover, out.press));
    out.border = blend(theme_border(), theme_primary(), (std::max)(out.hover, out.press));
    return out;
}

bool draw_hot_overlay(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                      D2D1_COLOR_F hover_color, D2D1_COLOR_F pressed_color) {
    if (target == nullptr || !rect_valid(rect)) {
        return false;
    }
    hot().observe(rect);
    const float hw = hot().hover_weight(rect);
    const float pw = hot().press_weight(rect);
    if (hw <= 0.001f && pw <= 0.001f) {
        return false;
    }
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (hw > 0.001f &&
        SUCCEEDED(target->CreateSolidColorBrush(hover_color, brush.GetAddressOf()))) {
        brush->SetOpacity(hw);
        target->FillRoundedRectangle(rounded, brush.Get());
        brush.Reset();
    }
    if (pw > 0.001f &&
        SUCCEEDED(target->CreateSolidColorBrush(pressed_color, brush.GetAddressOf()))) {
        brush->SetOpacity(pw);
        target->FillRoundedRectangle(rounded, brush.Get());
    }
    return true;
}

void HotState::begin_frame() {
    m_candidates.clear();
}

void HotState::observe(const D2D1_RECT_F& rect) {
    if (rect_valid(rect)) {
        m_candidates.push_back(rect);
    }
}

void HotState::pick(float x_dip, float y_dip) {
    // 取面积最小的命中矩形（图标按钮嵌在行内时优先图标）。
    const D2D1_RECT_F* best = nullptr;
    float best_area = 0.0f;
    for (const D2D1_RECT_F& rect : m_candidates) {
        if (x_dip < rect.left || x_dip >= rect.right || y_dip < rect.top ||
            y_dip >= rect.bottom) {
            continue;
        }
        const float area = (rect.right - rect.left) * (rect.bottom - rect.top);
        if (best == nullptr || area < best_area) {
            best = &rect;
            best_area = area;
        }
    }
    set_hover(best != nullptr ? *best : D2D1_RECT_F{});
}

void HotState::set_hover(const D2D1_RECT_F& rect) {
    const bool valid = rect_valid(rect);
    if (m_has_hover && valid && rect_same(m_hover, rect)) {
        return;
    }
    if (!m_has_hover && !valid) {
        return;
    }
    // 鼠标移回刚离开的控件：交换进度，避免闪一下再淡入。
    if (valid && m_has_prev_hover && rect_same(m_prev_hover, rect)) {
        m_in = m_out;
        m_out = 0.0f;
        m_has_prev_hover = false;
        m_hover = rect;
        m_has_hover = true;
        return;
    }
    if (m_has_hover) {
        m_prev_hover = m_hover;
        m_has_prev_hover = true;
        m_out = m_in;
    } else {
        m_has_prev_hover = false;
        m_out = 0.0f;
    }
    m_hover = rect;
    m_has_hover = valid;
    m_in = 0.0f;
}

void HotState::set_pressed(const D2D1_RECT_F& rect) {
    const bool valid = rect_valid(rect);
    if (m_has_press && valid && rect_same(m_press, rect)) {
        return;
    }
    if (!m_has_press && !valid) {
        return;
    }
    if (m_has_press) {
        m_prev_press = m_press;
        m_has_prev_press = true;
        m_press_out = m_press_in;
    } else {
        m_has_prev_press = false;
        m_press_out = 0.0f;
    }
    m_press = rect;
    m_has_press = valid;
    m_press_in = 0.0f;
}

float HotState::hover_weight(const D2D1_RECT_F& rect) const {
    if (!rect_valid(rect)) {
        return 0.0f;
    }
    if (m_has_hover && rect_same(m_hover, rect)) {
        return m_in;
    }
    if (m_has_prev_hover && rect_same(m_prev_hover, rect)) {
        return m_out;
    }
    return 0.0f;
}

float HotState::press_weight(const D2D1_RECT_F& rect) const {
    if (!rect_valid(rect)) {
        return 0.0f;
    }
    if (m_has_press && rect_same(m_press, rect)) {
        return m_press_in;
    }
    if (m_has_prev_press && rect_same(m_prev_press, rect)) {
        return m_press_out;
    }
    return 0.0f;
}

void HotState::clear() {
    m_candidates.clear();
    m_hover = {};
    m_prev_hover = {};
    m_press = {};
    m_prev_press = {};
    m_has_hover = false;
    m_has_prev_hover = false;
    m_has_press = false;
    m_has_prev_press = false;
    m_in = 0.0f;
    m_out = 0.0f;
    m_press_in = 0.0f;
    m_press_out = 0.0f;
}

bool HotState::update(float dt_ms) {
    bool active = false;
    if (tween(m_in, m_has_hover ? 1.0f : 0.0f, dt_ms, Kind::kHover)) {
        active = true;
    }
    if (tween(m_out, 0.0f, dt_ms, Kind::kHover)) {
        active = true;
    }
    if (tween(m_press_in, m_has_press ? 1.0f : 0.0f, dt_ms, Kind::kPress)) {
        active = true;
    }
    if (tween(m_press_out, 0.0f, dt_ms, Kind::kPress)) {
        active = true;
    }
    if (m_out <= 0.0f) {
        m_has_prev_hover = false;
    }
    if (m_press_out <= 0.0f) {
        m_has_prev_press = false;
    }
    return active;
}

HotState& hot() {
    static HotState state;
    return state;
}

ContentFade& content() {
    static ContentFade state;
    return state;
}

Pop& modal() {
    static Pop state;
    return state;
}

Pop& menu() {
    static Pop state;
    return state;
}

bool update_all(float dt_ms) {
    bool active = false;
    if (hot().update(dt_ms)) {
        active = true;
    }
    if (content().update(dt_ms)) {
        active = true;
    }
    if (modal().update(dt_ms)) {
        active = true;
    }
    if (menu().update(dt_ms)) {
        active = true;
    }
    return active;
}

// ————————————————————— 加载指示器（D-102）—————————————————————

void tick_spinner(float dt_ms) {
    if (!(dt_ms > 0.0f)) {
        return;
    }
    // 关闭档不转动：静态弧也能表达「忙」，但不产生任何动画帧。
    const float scale = level_scale();
    if (scale <= 0.0f) {
        return;
    }
    g_spinner_angle += kTau * (dt_ms / 900.0f) * scale;
    if (g_spinner_angle >= kTau) {
        g_spinner_angle = std::fmod(g_spinner_angle, kTau);
    }
}

float spinner_angle() {
    return g_spinner_angle;
}

void draw_spinner(ID2D1RenderTarget* target, D2D1_POINT_2F center, float radius,
                  D2D1_COLOR_F color, float alpha, float thickness) {
    if (target == nullptr || radius <= 0.5f || alpha <= 0.001f) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    constexpr int kSegments = 16;
    const float step = kTau / static_cast<float>(kSegments);
    for (int i = 0; i < kSegments; ++i) {
        // t：0 = 尾端（最淡），1 = 头端（最实）；平方让尾部更快消失。
        const float t = static_cast<float>(i) / static_cast<float>(kSegments - 1);
        brush->SetOpacity(alpha * (0.07f + 0.93f * t * t));
        const float a0 = g_spinner_angle + static_cast<float>(i) * step;
        const float a1 = a0 + step * 0.62f;
        target->DrawLine(
            D2D1::Point2F(center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius),
            D2D1::Point2F(center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius),
            brush.Get(), thickness);
    }
}

}  // namespace sscl::ui::motion
