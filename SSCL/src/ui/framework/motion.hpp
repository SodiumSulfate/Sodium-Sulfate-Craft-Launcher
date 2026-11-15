#pragma once

// 动效模块（D-081）：统一时长、缓动、位移与缩放，全部受「动效强度」控制。
//
// 参照 PCL 2.99 `ModAnimation`：
//   * 全局 `AniSpeed` 倍率（0.1×~3×）——本模块对应 `level_scale()`；
//   * `AniControlEnabled` 计数抑制——本模块对应 `motion::Guard`；
//   * In/Out Fluent 缓动——本模块对应 `ease_out()` / `ease_in()`。
//
// 强度四档（负责人 2026-11-07 定）：完整 1.0 / 柔和 0.6 / 精简 0.3 / 关闭 0。
// 时长与幅度同时按倍率缩放；关闭档一律瞬时到位（不产生任何中间帧）。
// **约定**：新增动画一律走本模块，不得再写死毫秒数或自建缓动。

#include <d2d1.h>
#include <string>
#include <vector>

namespace sscl::ui::motion {

enum class Level {
    kFull = 0,     // 完整
    kSoft = 1,     // 柔和
    kReduced = 2,  // 精简
    kOff = 3,      // 关闭
};

// 动效类别：决定基准时长与基准位移。
enum class Kind {
    kHover,      // 悬停变色 90ms
    kPress,      // 按下 60ms
    kFade,       // 内容切换 / 淡入 120ms
    kPop,        // 弹层弹出 160ms（缩放 + 上移）
    kPage,       // 页面切换 180ms（上移淡入）
    kScrollbar,  // 滚动条粗细过渡 90ms
};

Level level();
void set_level(Level value);

// 配置键与档位互转（"full" / "soft" / "reduced" / "off"；非法值按完整处理）。
const char* level_key(Level value);
Level level_from_key(const std::string& key);
const wchar_t* level_label(Level value);

// 当前倍率：1.0 / 0.6 / 0.3 / 0。
float level_scale();

// 当前强度下的时长（毫秒）；关闭档返回 0。
float duration(Kind kind);
// 当前强度下的位移（DIP）：kPop=8、kPage=6、其它 0。
float offset(Kind kind);
// 弹层缩放起点：完整 0.96 / 柔和 0.976 / 精简 0.988 / 关闭 1。
float pop_scale();
// 弹层遮罩目标不透明度（PCL 式深遮罩）。
float dim_opacity();

// 缓动（0..1 输入，钳制）。
float ease_out(float t);
float ease_in(float t);

// 进程内抑制（对应 PCL `AniControlEnabled`）：批量重建、程序改值时瞬时到位。
class Guard {
public:
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

bool suppressed();

// 平滑逼近：时长语义为 95% 收敛时间；返回是否仍在变化。
bool tween(float& value, float target, float dt_ms, Kind kind);

// 线性推进（0..1 进度），供绘制端自行套 ease_out；关闭档直接置终点。
bool advance(float& value, float target, float dt_ms, Kind kind);

// 进度动画（0..1）：显式时长 + 缓出，用于弹层/页面这类需要取中间值的过渡。
class Progress {
public:
    void to(float target, Kind kind);
    void jump(float value);
    bool update(float dt_ms);
    bool active() const { return m_active; }
    float raw() const { return m_value; }
    float eased() const { return ease_out(m_value); }

private:
    float m_value = 0.0f;
    float m_from = 0.0f;
    float m_target = 0.0f;
    float m_elapsed = 0.0f;
    float m_duration = 0.0f;
    bool m_active = false;
};

// 弹层过渡：开 = 淡入 + 0.96→1 缩放 + 上移 8px；关 = 反向。
class Pop {
public:
    void open();
    void close();
    void set_open(bool value, int tag = 0);
    bool visible_for(int tag) const { return m_tag == tag && visible(); }
    void jump_open();
    void jump_closed();

    bool update(float dt_ms);
    // 是否还需要绘制（关闭动画未收敛时仍为 true）。
    bool visible() const { return m_progress.raw() > 0.001f || m_progress.active(); }
    float opacity() const { return m_progress.eased(); }
    float scale() const;
    float offset_y() const;
    float dim() const;
    bool closing() const { return m_closing; }

private:
    Progress m_progress;
    bool m_closing = false;
    int m_tag = 0;  // 菜单归属（同一时刻只允许一个浮层，关闭淡出期仍按 tag 绘制）
};

// 弹层绘制助手：围绕卡片中心做「透明度 + 缩放 + 上移」的图层。
// 返回 true 表示已压层，绘制完卡片后必须调用 pop_pop() 平衡。
bool push_pop(ID2D1RenderTarget* target, const Pop& pop, const D2D1_RECT_F& card);
void pop_pop(ID2D1RenderTarget* target);

// 无需卡片中心的通用浮层（下拉/菜单）：透明度 + 上移，成对调用。
bool push_fade(ID2D1RenderTarget* target, float opacity, float offset_y);
void pop_layer(ID2D1RenderTarget* target);

// 语义色线性混合（0..1）；供所有页面统一做 hover/pressed 渐变。
D2D1_COLOR_F blend(D2D1_COLOR_F from, D2D1_COLOR_F to, float t);
bool rect_valid(const D2D1_RECT_F& rect);
bool rect_same(const D2D1_RECT_F& a, const D2D1_RECT_F& b);

// 统一的按钮/列表行三态配色：所有自绘页调用同一函数，保证逐页语言一致。
struct StateColors {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F text;
    D2D1_COLOR_F border;
    float hover = 0.0f;
    float press = 0.0f;
};

StateColors button_colors(bool primary, bool enabled, const D2D1_RECT_F& rect);
StateColors row_colors(D2D1_COLOR_F base, D2D1_COLOR_F hover_color,
                       D2D1_COLOR_F pressed_color, const D2D1_RECT_F& rect);

// 列表行 / tab / 图标按钮：在已绘制的底色上叠加悬停与按下渐变，并登记命中矩形。
bool draw_hot_overlay(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                      D2D1_COLOR_F hover_color, D2D1_COLOR_F pressed_color);

// ——— 加载指示器（D-102）———
// 所有「前台等待」（获取版本列表 / 搜索 Mod / 解析前置 / 修复文件…）共用同一相位与画法。
// 相位由 AppShell::tick 单点推进（页面不得再调 tick_spinner，否则转速翻倍）；
// 忙碌页面通过 Page::is_busy() 保持帧循环，文案由 Page::busy_text() 提供。
// 转速随「动效强度」缩放；关闭档不转动（保留静态弧，避免看起来"没在动"）。
void tick_spinner(float dt_ms);
float spinner_angle();  // 弧度，0..2π
// 画一个 16 段渐隐圆环（尾部透明、头部实色），radius 为圆心到笔画中线的距离。
void draw_spinner(ID2D1RenderTarget* target, D2D1_POINT_2F center, float radius,
                  D2D1_COLOR_F color, float alpha = 1.0f, float thickness = 2.0f);

// ——— 全局动画状态层（D-083）———
// 所有页面共用，AppShell::tick 单点驱动，保证每一页的交互反馈完全一致。
// 以「矩形」为命中键：页面 mouse_move 把当前命中矩形交给 set_hover，
// 绘制时用 hover_weight(rect)/press_weight(rect) 混色，调用点无需额外参数。
class HotState {
public:
    void set_hover(const D2D1_RECT_F& rect);   // 无效矩形 = 无悬停
    void set_pressed(const D2D1_RECT_F& rect);
    void begin_frame();                        // AppShell::draw 开头：清空本帧候选矩形
    void observe(const D2D1_RECT_F& rect);     // 绘制可交互元素时登记（DIP 坐标）
    void pick(float x_dip, float y_dip);       // AppShell::mouse_move：取最小命中矩形
    const D2D1_RECT_F& hover_rect() const { return m_hover; }
    float hover_weight(const D2D1_RECT_F& rect) const;
    float press_weight(const D2D1_RECT_F& rect) const;
    void clear();
    bool update(float dt_ms);

private:
    D2D1_RECT_F m_hover{};
    D2D1_RECT_F m_prev_hover{};
    D2D1_RECT_F m_press{};
    D2D1_RECT_F m_prev_press{};
    bool m_has_hover = false;
    bool m_has_prev_hover = false;
    bool m_has_press = false;
    bool m_has_prev_press = false;
    float m_in = 0.0f;
    float m_out = 0.0f;
    float m_press_in = 0.0f;
    float m_press_out = 0.0f;
    std::vector<D2D1_RECT_F> m_candidates;
};

// 内容/分类/tab 切换淡入（统一 120ms，页面只需 restart）。
class ContentFade {
public:
    void restart() {
        m_progress = duration(Kind::kFade) <= 0.0f ? 1.0f : 0.0f;  // 关闭档不淡入
    }
    void jump() { m_progress = 1.0f; }
    bool update(float dt_ms) { return advance(m_progress, 1.0f, dt_ms, Kind::kFade); }
    bool active() const { return m_progress < 0.999f; }
    float opacity() const { return ease_out(m_progress); }

private:
    float m_progress = 1.0f;
};

HotState& hot();
ContentFade& content();
Pop& modal();   // 模态弹层（全局同一时刻只有一个）
Pop& menu();    // 下拉 / 右键菜单（全局同一时刻只有一个）
bool update_all(float dt_ms);  // AppShell::tick 单点驱动，返回是否需要下一帧

}  // namespace sscl::ui::motion
