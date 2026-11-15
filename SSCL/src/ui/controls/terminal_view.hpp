#pragma once

// 终端风格滚动日志（批次 42）：apt 风格纯文本输出。
// - 等宽字体（Cascadia Mono → Consolas → Courier New 回退），彩色语义行；
// - 环形缓冲：超过 max_lines 丢弃最旧行（只保留用户可能回看的窗口），
//   绘制时只遍历可见行，不创建逐行控件；
// - 滚轮滚动 + 底部跟随（用户向上翻后停止自动跟随）；
// - 滚轮动画由调用方 tick 驱动（窗口帧循环在 tick 返回 false 后自动停止）。

#include <cstddef>
#include <deque>
#include <string>

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace sscl::ui {

enum class TerminalTone {
    kDefault,  // 正文（theme_text_primary）
    kDim,      // 次要/说明（theme_text_secondary）
    kAccent,   // 阶段/提示（theme_primary）
    kSuccess,  // 完成（theme_success）
    kError,    // 错误（theme_danger）
};

class TerminalView {
public:
    void set_bounds(const D2D1_RECT_F& bounds);
    const D2D1_RECT_F& bounds() const { return m_bounds; }

    void clear();
    void append(std::wstring text, TerminalTone tone = TerminalTone::kDefault);

    // 运行中保留的滚动窗口；超出丢弃最旧行并计入 trimmed_count。
    void set_max_lines(std::size_t max_lines);
    // 立即收缩到最近 keep 行（页面隐藏/任务结束时释放内存）。
    void trim_to(std::size_t keep);

    std::size_t line_count() const { return m_lines.size(); }
    std::size_t trimmed_count() const { return m_trimmed; }

    // 滚轮（WHEEL_DELTA 单位，正 = 向上）；返回是否产生滚动。
    bool on_wheel(float delta);
    // 滚动趋近动画；返回是否仍在动画（驱动窗口帧循环）。
    bool tick(float dt_ms);
    void draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    bool hit(float x, float y) const;

private:
    struct Line {
        std::wstring text;
        TerminalTone tone = TerminalTone::kDefault;
    };

    float content_height() const;
    float max_scroll() const;
    void clamp_scroll();
    void follow_bottom();

    std::deque<Line> m_lines;
    std::size_t m_trimmed = 0;
    std::size_t m_max_lines = 2000;
    D2D1_RECT_F m_bounds{0.0f, 0.0f, 0.0f, 0.0f};
    float m_scroll = 0.0f;         // 像素偏移（0 = 顶部）
    float m_target_scroll = 0.0f;
    bool m_follow = true;          // 新行到达时自动滚到底

    // 等宽字体缓存：字体族解析一次，TextFormat 按工厂缓存（避免每帧创建）。
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    IDWriteFactory* m_format_factory = nullptr;
    std::wstring m_font_family;
};

}  // namespace sscl::ui
