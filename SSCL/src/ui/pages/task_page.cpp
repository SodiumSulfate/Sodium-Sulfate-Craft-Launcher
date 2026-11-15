#include "ui/pages/task_page.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <thread>
#include <utility>
#include <vector>

#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

// PCL StringUtils.FormatByteSize 对照：B/KB/MB/GB 两位小数。
std::wstring format_bytes(std::uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[64];
    if (unit == 0) {
        swprintf(buffer, 64, L"%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    } else {
        swprintf(buffer, 64, L"%.2f %s", value, units[unit]);
    }
    return buffer;
}

std::wstring format_speed(double bytes_per_second) {
    const double value = bytes_per_second < 0.0 ? 0.0 : bytes_per_second;
    return format_bytes(static_cast<std::uint64_t>(value)) + L"/s";
}

std::wstring format_percent(float fraction) {
    const float percent = (std::max)(0.0f, (std::min)(1.0f, fraction)) * 100.0f;
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%.1f%%", static_cast<double>(percent));
    return buffer;
}

std::wstring format_eta(double seconds) {
    if (!(seconds > 0.0) || seconds > 35999.0) {
        return L"—";
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int minutes = total / 60;
    const int secs = total % 60;
    wchar_t buffer[32];
    if (minutes > 0) {
        swprintf(buffer, 32, L"%d 分 %d 秒", minutes, secs);
    } else {
        swprintf(buffer, 32, L"%d 秒", secs);
    }
    return buffer;
}

// 右栏布局常量（layout 与 draw 共用，避免文字/进度条位置漂移）。
constexpr float kZonePaddingDip = 14.0f;
constexpr float kZoneStepTopDip = 10.0f;
constexpr float kZoneOverallTopDip = 32.0f;
constexpr float kZoneBarTopDip = 56.0f;
constexpr float kZoneBarHeightDip = 8.0f;
constexpr float kZoneRowsTopDip = 72.0f;
constexpr float kFileRowHeightDip = 22.0f;
constexpr int kMaxFileRows = 6;

}  // namespace

TaskPage::TaskPage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service)
    : m_window(window), m_service(std::move(service)) {
    auto title = std::make_unique<Label>(L"下载任务", 22.0f, theme_text_primary(), true);

    auto progress_caption = std::make_unique<Label>(L"总进度", 12.0f, theme_text_secondary());
    auto progress_value = std::make_unique<Label>(L"0.0%", 20.0f, theme_text_primary(), true);
    auto speed_caption = std::make_unique<Label>(L"下载速度", 12.0f, theme_text_secondary());
    auto speed_value = std::make_unique<Label>(L"0 B/s", 20.0f, theme_text_primary(), true);
    auto files_caption = std::make_unique<Label>(L"剩余文件", 12.0f, theme_text_secondary());
    auto files_value = std::make_unique<Label>(L"0", 20.0f, theme_text_primary(), true);
    auto status = std::make_unique<Label>(L"暂无下载任务", 11.0f, theme_text_secondary());

    auto cancel = std::make_unique<Button>(L"取消下载", 13.0f, false);
    auto back = std::make_unique<Button>(L"返回下载页", 13.0f, true);

    m_title = title.get();
    m_progress_caption = progress_caption.get();
    m_progress_value = progress_value.get();
    m_speed_caption = speed_caption.get();
    m_speed_value = speed_value.get();
    m_files_caption = files_caption.get();
    m_files_value = files_value.get();
    m_status_label = status.get();
    m_cancel = cancel.get();
    m_back = back.get();
    cancel->icon = "close";
    back->icon = "arrow_back";

    // 左侧简略系统信息：CPU 占用率 + 迷你历史柱状图、RAM 使用量/占用率（图标自绘）。
    auto cpu_label = std::make_unique<Label>(L"CPU", 10.0f, theme_text_primary());
    auto cpu_percent = std::make_unique<Label>(L"—", 10.0f, theme_text_primary());
    auto ram_label = std::make_unique<Label>(L"RAM —", 10.0f, theme_text_secondary());
    auto ram_percent = std::make_unique<Label>(L"—", 10.0f, theme_text_primary());
    cpu_label->visible = false;
    cpu_percent->visible = false;
    ram_label->visible = false;
    ram_percent->visible = false;
    cpu_label->vertical_center = true;
    cpu_percent->vertical_center = true;
    ram_label->vertical_center = true;
    ram_percent->vertical_center = true;
    m_cpu_label = cpu_label.get();
    m_cpu_percent_label = cpu_percent.get();
    m_ram_label = ram_label.get();
    m_ram_percent_label = ram_percent.get();

    cancel->on_click = [this]() {
        if (m_running) {
            m_stop.request_stop();
            m_status = L"正在取消…";
            append_log(L"-- 正在取消 --", TerminalTone::kAccent);
            update_labels();
            if (request_repaint) {
                request_repaint();
            }
        } else if (m_bg_job != 0 &&
                   sscl::core::TaskCenter::instance().cancel(m_bg_job)) {
            // D-098：后台任务同样可取消（任务真正结束以工作体返回为准）。
            m_status = L"正在取消…";
            append_log(L"-- 正在取消 --", TerminalTone::kAccent);
            update_labels();
            if (request_repaint) {
                request_repaint();
            }
        }
    };
    back->on_click = [this]() {
        if (on_back) {
            on_back();
        }
    };

    m_root.add(std::move(title));
    m_root.add(std::move(progress_caption));
    m_root.add(std::move(progress_value));
    m_root.add(std::move(speed_caption));
    m_root.add(std::move(speed_value));
    m_root.add(std::move(files_caption));
    m_root.add(std::move(files_value));
    m_root.add(std::move(status));
    m_root.add(std::move(cpu_label));
    m_root.add(std::move(cpu_percent));
    m_root.add(std::move(ram_label));
    m_root.add(std::move(ram_percent));
    m_root.add(std::move(cancel));
    m_root.add(std::move(back));

    // 右栏终端标题/计数
    auto terminal_title =
        std::make_unique<Label>(L"下载输出", 11.0f, theme_text_primary(), true);
    auto terminal_meta = std::make_unique<Label>(L"", 9.0f, theme_text_secondary());
    m_terminal_title = terminal_title.get();
    m_terminal_meta = terminal_meta.get();
    m_root.add(std::move(terminal_title));
    m_root.add(std::move(terminal_meta));

    // 底部实时区文本
    auto step = std::make_unique<Label>(L"步骤：等待任务…", 11.0f, theme_text_secondary());
    auto overall = std::make_unique<Label>(L"0.0%", 11.5f, theme_text_primary(), true);
    auto speed2 = std::make_unique<Label>(L"", 10.0f, theme_text_secondary());
    auto more = std::make_unique<Label>(L"", 9.0f, theme_text_muted());
    m_step_label = step.get();
    m_overall_label = overall.get();
    m_speed_label = speed2.get();
    m_more_label = more.get();
    m_root.add(std::move(step));
    m_root.add(std::move(overall));
    m_root.add(std::move(speed2));
    m_root.add(std::move(more));

    for (int i = 0; i < kMaxFileRows; ++i) {
        auto name = std::make_unique<Label>(L"", 9.5f, theme_text_primary());
        auto stats = std::make_unique<Label>(L"", 9.5f, theme_text_secondary());
        name->visible = false;
        stats->visible = false;
        m_file_name[i] = name.get();
        m_file_stats[i] = stats.get();
        m_root.add(std::move(name));
        m_root.add(std::move(stats));
    }

    m_terminal.set_max_lines(2000);
    update_labels();
    update_system_labels();
}

void TaskPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    const float w = m_width_dip;
    const float h = m_height_dip;
    const float margin = kSidebarMarginDip;

    // —— 左栏（沿用旧页面的 PCL PageSpeedLeft 结构）——
    m_title->bounds = rect_of(margin, kNavHeightDip + 20.0f, kSidebarWidthDip - margin,
                              kNavHeightDip + 56.0f);
    // 高度不足时压缩三个大数字的间距，为下方系统信息留空间（0.1s 固定刷新）。
    const float metric_gap = h < 660.0f ? 76.0f : 86.0f;
    float y = kNavHeightDip + 92.0f;
    m_progress_caption->bounds = rect_of(margin, y, kSidebarWidthDip - margin, y + 20.0f);
    m_progress_value->bounds = rect_of(margin, y + 24.0f, kSidebarWidthDip - margin, y + 58.0f);
    y += metric_gap;
    m_speed_caption->bounds = rect_of(margin, y, kSidebarWidthDip - margin, y + 20.0f);
    m_speed_value->bounds = rect_of(margin, y + 24.0f, kSidebarWidthDip - margin, y + 58.0f);
    y += metric_gap;
    m_files_caption->bounds = rect_of(margin, y, kSidebarWidthDip - margin, y + 20.0f);
    m_files_value->bounds = rect_of(margin, y + 24.0f, kSidebarWidthDip - margin, y + 58.0f);

    // 系统信息块（CPU/RAM）：空间不足（小窗口）时隐藏，避免与状态/按钮重叠。
    const float files_bottom = y + 58.0f;
    const float status_top = h - 196.0f;
    m_system_top = files_bottom + 6.0f;
    m_system_visible = (m_system_top + 58.0f) <= (status_top - 4.0f);
    if (m_system_visible) {
        const float cpu_top = m_system_top;
        const float ram_top = m_system_top + 28.0f;
        // 两行共用列：图标 | 文本 | 百分比（同 x） | 统计图（同 x/宽）。
        const float chart_width = 56.0f;
        const float chart_left = kSidebarWidthDip - margin - chart_width;
        const float percent_width = 32.0f;
        const float percent_left = chart_left - 6.0f - percent_width;
        m_cpu_label->bounds = rect_of(margin + 24.0f, cpu_top, percent_left - 6.0f,
                                      cpu_top + 28.0f);
        m_cpu_percent_label->bounds = rect_of(percent_left, cpu_top, chart_left - 6.0f,
                                              cpu_top + 28.0f);
        m_ram_label->bounds = rect_of(margin + 24.0f, ram_top, percent_left - 6.0f,
                                      ram_top + 28.0f);
        m_ram_percent_label->bounds = rect_of(percent_left, ram_top, chart_left - 6.0f,
                                              ram_top + 28.0f);
    }
    m_cpu_label->visible = m_system_visible;
    m_cpu_percent_label->visible = m_system_visible;
    m_ram_label->visible = m_system_visible;
    m_ram_percent_label->visible = m_system_visible;

    m_status_label->bounds = rect_of(margin, h - 196.0f, kSidebarWidthDip - margin, h - 156.0f);
    m_cancel->bounds = rect_of(margin, h - 150.0f, kSidebarWidthDip - margin, h - 115.0f);
    m_back->bounds = rect_of(margin, h - 100.0f, kSidebarWidthDip - margin, h - 65.0f);

    // —— 右栏 ——
    m_panel = rect_of(kSidebarWidthDip + 24.0f, kNavHeightDip + 20.0f, w - 24.0f, h - 20.0f);
    const float panel_height = (std::max)(0.0f, m_panel.bottom - m_panel.top);
    const float zone_height =
        (std::min)(268.0f, (std::max)(196.0f, panel_height * 0.38f));
    m_zone = rect_of(m_panel.left, m_panel.bottom - zone_height, m_panel.right, m_panel.bottom);

    const float terminal_bottom = m_zone.top - 10.0f;
    const float terminal_top = m_panel.top;
    m_terminal_title->bounds =
        rect_of(m_panel.left + kZonePaddingDip, terminal_top + 8.0f,
                m_panel.right - kZonePaddingDip - 180.0f, terminal_top + 28.0f);
    m_terminal_meta->bounds = rect_of(m_panel.right - kZonePaddingDip - 180.0f,
                                      terminal_top + 8.0f, m_panel.right - kZonePaddingDip,
                                      terminal_top + 28.0f);
    m_terminal.set_bounds(rect_of(m_panel.left + kZonePaddingDip, terminal_top + 32.0f,
                                  m_panel.right - kZonePaddingDip,
                                  (std::max)(terminal_top + 40.0f, terminal_bottom - 10.0f)));

    const float zone_left = m_zone.left + kZonePaddingDip;
    const float zone_right = m_zone.right - kZonePaddingDip;
    m_step_label->bounds =
        rect_of(zone_left, m_zone.top + kZoneStepTopDip, zone_right,
                m_zone.top + kZoneStepTopDip + 20.0f);
    m_overall_label->bounds =
        rect_of(zone_left, m_zone.top + kZoneOverallTopDip, zone_left + (zone_right - zone_left) * 0.56f,
                m_zone.top + kZoneOverallTopDip + 20.0f);
    m_speed_label->bounds =
        rect_of(zone_left + (zone_right - zone_left) * 0.56f + 8.0f,
                m_zone.top + kZoneOverallTopDip + 1.0f, zone_right,
                m_zone.top + kZoneOverallTopDip + 21.0f);

    const int rows = static_cast<int>((zone_height - 98.0f) / kFileRowHeightDip);
    m_file_rows = (std::max)(1, (std::min)(kMaxFileRows, rows));

    const float name_width = (zone_right - zone_left) * 0.38f;
    const float stats_left = zone_right - 190.0f;
    for (int i = 0; i < kMaxFileRows; ++i) {
        const float row_top = m_zone.top + kZoneRowsTopDip + static_cast<float>(i) * kFileRowHeightDip;
        const bool used = i < m_file_rows;
        m_file_name[i]->bounds =
            rect_of(zone_left, row_top, zone_left + name_width, row_top + kFileRowHeightDip - 4.0f);
        m_file_stats[i]->bounds = rect_of(stats_left, row_top, zone_right,
                                          row_top + kFileRowHeightDip - 4.0f);
        if (!used) {
            m_file_name[i]->visible = false;
            m_file_stats[i]->visible = false;
        }
    }
    m_more_label->bounds =
        rect_of(zone_left,
                m_zone.top + kZoneRowsTopDip + static_cast<float>(m_file_rows) * kFileRowHeightDip + 2.0f,
                zone_right, m_zone.bottom - 6.0f);

    // 行数/尺寸变化后按新布局刷新可见性（不依赖下一次进度回调）。
    update_labels();
    update_system_labels();
}

float TaskPage::total_fraction() const {
    return (std::min)(1.0f, (std::max)(0.0f, m_report.fraction));
}

std::size_t TaskPage::visible_file_rows() const {
    return static_cast<std::size_t>((std::max)(1, m_file_rows));
}

bool TaskPage::step_active() const {
    return m_report.step_done > 0 || m_report.step_total > 0 || m_report.step_indeterminate;
}

bool TaskPage::step_animating() const {
    return m_visible && m_running && m_report.step_indeterminate;
}

float TaskPage::progress_fraction() const {
    if (!m_running) {
        // D-098：前台没有版本安装时，悬浮窗显示后台任务（附加页 Mod 下载）。
        const sscl::core::JobSnapshot active = sscl::core::TaskCenter::instance().active();
        if (active.valid()) {
            return (std::min)(1.0f, (std::max)(0.0f, active.progress.fraction()));
        }
    }
    return total_fraction();
}

std::wstring TaskPage::activity_title() const {
    if (m_running) {
        return L"正在下载 " + widen(m_entry_id);
    }
    const sscl::core::JobSnapshot active = sscl::core::TaskCenter::instance().active();
    if (active.valid()) {
        return widen(active.title);
    }
    return m_status;
}

std::wstring TaskPage::activity_detail() const {
    float fraction = total_fraction();
    if (!m_running) {
        const sscl::core::JobSnapshot active = sscl::core::TaskCenter::instance().active();
        if (active.valid()) {
            fraction = (std::min)(1.0f, (std::max)(0.0f, active.progress.fraction()));
        }
    }
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%.0f %%", static_cast<double>(fraction) * 100.0);
    return buffer;
}

std::wstring TaskPage::eta_text() const {
    if (step_active()) {
        return L"处理中…";  // 处理器/Java 检测：无法用下载速度估算
    }
    if (!(m_speed_bps > 1.0) || m_report.bytes_total <= m_report.bytes_done) {
        return L"剩余 —";
    }
    const std::uint64_t remaining = m_report.bytes_total - m_report.bytes_done;
    const double seconds = static_cast<double>(remaining) / m_speed_bps;
    return L"剩余 " + format_eta(seconds);
}

void TaskPage::draw_progress_bar(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                                 float fraction, float height, bool strong) const {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    if (FAILED(target->CreateSolidColorBrush(theme_border(), track.GetAddressOf()))) {
        return;
    }
    const float radius = height * 0.5f;
    target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), track.Get());
    if (fraction <= 0.001f) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    const D2D1_COLOR_F color = strong ? theme_primary() : theme_text_secondary();
    if (FAILED(target->CreateSolidColorBrush(color, fill.GetAddressOf()))) {
        return;
    }
    fill->SetOpacity(strong ? 1.0f : 0.75f);
    const float width = (rect.right - rect.left) * (std::min)(1.0f, fraction);
    D2D1_RECT_F filled = rect;
    filled.right = filled.left + (std::max)(height, width);
    if (filled.right > rect.right) {
        filled.right = rect.right;
    }
    target->FillRoundedRectangle(D2D1::RoundedRect(filled, radius, radius), fill.Get());
}

void TaskPage::draw_step_bar(ID2D1RenderTarget* target) const {
    const float left = m_zone.left + kZonePaddingDip;
    const float right = m_zone.right - kZonePaddingDip;
    const D2D1_RECT_F bar = rect_of(left, m_zone.top + kZoneRowsTopDip, right,
                                    m_zone.top + kZoneRowsTopDip + 12.0f);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    if (FAILED(target->CreateSolidColorBrush(theme_border(), track.GetAddressOf()))) {
        return;
    }
    const float radius = 6.0f;
    target->FillRoundedRectangle(D2D1::RoundedRect(bar, radius, radius), track.Get());

    // 粗进度（处理器 i/N）：有总数才画已完成部分。
    float fraction = 0.0f;
    if (m_report.step_total > 0) {
        fraction = (std::min)(
            1.0f, static_cast<float>(static_cast<double>(m_report.step_done) /
                                     static_cast<double>(m_report.step_total)));
    }
    if (fraction > 0.001f) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_primary(), fill.GetAddressOf()))) {
            fill->SetOpacity(0.45f);
            D2D1_RECT_F filled = bar;
            filled.right = filled.left + (bar.right - bar.left) * fraction;
            target->FillRoundedRectangle(D2D1::RoundedRect(filled, radius, radius), fill.Get());
        }
    }

    // 无细进度 / 当前处理器运行中：循环扫动高光，证明任务仍在工作（不做假死）。
    if (m_report.step_indeterminate) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shimmer;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_primary(), shimmer.GetAddressOf()))) {
            shimmer->SetOpacity(0.85f);
            const float width = bar.right - bar.left;
            const float segment = (std::max)(28.0f, width * 0.22f);
            const float phase = m_activity_phase / 1400.0f;
            const float pingpong = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
            const float x = bar.left + (width - segment) * pingpong;
            const D2D1_RECT_F moving = rect_of(x, bar.top + 2.0f, x + segment, bar.bottom - 2.0f);
            target->FillRoundedRectangle(D2D1::RoundedRect(moving, 4.0f, 4.0f), shimmer.Get());
        }
    }
}

void TaskPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), background.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip + 64.0f, m_height_dip + 64.0f),
                              background.Get());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sidebar;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_sidebar_background(),
                                                sidebar.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, kNavHeightDip, kSidebarWidthDip, m_height_dip),
                              sidebar.Get());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> divider;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), divider.GetAddressOf()))) {
        divider->SetOpacity(0.6f);
        target->DrawLine(D2D1::Point2F(kSidebarWidthDip, kNavHeightDip),
                         D2D1::Point2F(kSidebarWidthDip, m_height_dip), divider.Get(), 1.0f);
    }

    // 右栏两张卡片：终端输出 + 实时状态。
    const float radius = theme_radius();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> terminal_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> zone_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    target->CreateSolidColorBrush(theme_container(), terminal_bg.GetAddressOf());
    target->CreateSolidColorBrush(theme_surface(), zone_bg.GetAddressOf());
    target->CreateSolidColorBrush(theme_border(), border.GetAddressOf());
    const D2D1_RECT_F terminal_panel = rect_of(m_panel.left, m_panel.top, m_panel.right,
                                               m_zone.top - 10.0f);
    if (terminal_bg != nullptr) {
        target->FillRoundedRectangle(D2D1::RoundedRect(terminal_panel, radius, radius),
                                     terminal_bg.Get());
    }
    if (zone_bg != nullptr) {
        target->FillRoundedRectangle(D2D1::RoundedRect(m_zone, radius, radius), zone_bg.Get());
    }
    if (border != nullptr) {
        border->SetOpacity(0.8f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(terminal_panel, radius, radius),
                                     border.Get(), 1.0f);
        target->DrawRoundedRectangle(D2D1::RoundedRect(m_zone, radius, radius), border.Get(),
                                     1.0f);
    }

    m_root.draw(target, dwrite);

    if (m_system_visible) {
        draw_system_icons(target);  // CPU/RAM 图标 + CPU 迷你历史柱状图（主题色）
    }

    // 总进度条
    const D2D1_RECT_F overall_bar =
        rect_of(m_zone.left + kZonePaddingDip, m_zone.top + kZoneBarTopDip,
                m_zone.right - kZonePaddingDip, m_zone.top + kZoneBarTopDip + kZoneBarHeightDip);
    draw_progress_bar(target, overall_bar, total_fraction(), kZoneBarHeightDip, true);

    if (step_active()) {
        // 安装器处理器 / Java 检测：没有文件级进度，用步骤条 + 循环扫光表示仍在工作。
        draw_step_bar(target);
    } else {
        // 逐文件进度条（只画可见行；名称/百分比标签由 m_root 绘制）
        const float zone_left = m_zone.left + kZonePaddingDip;
        const float zone_right = m_zone.right - kZonePaddingDip;
        const float name_width = (zone_right - zone_left) * 0.38f;
        const float stats_left = zone_right - 190.0f;
        const std::size_t rows = visible_file_rows();
        for (std::size_t i = 0; i < rows && i < m_file_order.size(); ++i) {
            const auto it = m_files.find(m_file_order[i]);
            if (it == m_files.end()) {
                continue;
            }
            const ActiveFile& file = it->second;
            const float fraction =
                file.total > 0
                    ? static_cast<float>(static_cast<double>(file.bytes) /
                                         static_cast<double>(file.total))
                    : 0.0f;
            const float row_top =
                m_zone.top + kZoneRowsTopDip + static_cast<float>(i) * kFileRowHeightDip;
            const D2D1_RECT_F bar = rect_of(zone_left + name_width + 8.0f, row_top + 7.0f,
                                            stats_left - 8.0f, row_top + 13.0f);
            if (bar.right <= bar.left) {
                continue;
            }
            draw_progress_bar(target, bar, fraction, 6.0f, false);
        }
    }

    // 终端文本在最后绘制（卡片背景已铺好）。
    m_terminal.draw(target, dwrite);
}

bool TaskPage::tick(float dt_ms) {
    bool active = m_root.tick(dt_ms);
    if (m_terminal.tick(dt_ms)) {
        active = true;
    }
    if (m_visible && !m_running) {
        // D-098：后台任务由任务中心驱动，这里按 ≈16Hz 轮询并复用既有进度链路。
        m_bg_poll_ms += dt_ms;
        if (m_bg_poll_ms >= 60.0f) {
            m_bg_poll_ms = 0.0f;
            poll_background();
        }
    }
    if (step_animating()) {
        // 循环扫光相位：帧循环由 tick 返回 true 维持；步骤结束（indeterminate=false）
        // 后下一次 tick 返回 false，渲染循环自动停止，不空转。
        m_activity_phase += dt_ms;
        if (!(m_activity_phase < 1400.0f)) {
            m_activity_phase = std::fmod(m_activity_phase, 1400.0f);
        }
        active = true;
    }
    return active;
}

bool TaskPage::mouse_move(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_move(x * m_to_dip, y * m_to_dip);
    return m_root.state_changed;
}

bool TaskPage::mouse_down(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_down(x * m_to_dip, y * m_to_dip);
    return m_root.state_changed;
}

bool TaskPage::mouse_up(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_up(x * m_to_dip, y * m_to_dip);
    return m_root.state_changed;
}

bool TaskPage::mouse_leave() {
    m_root.state_changed = false;
    m_root.on_mouse_leave();
    return m_root.state_changed;
}

bool TaskPage::mouse_wheel(float x, float y, float delta) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (!m_terminal.hit(x_dip, y_dip)) {
        return false;
    }
    if (!m_terminal.on_wheel(delta)) {
        return false;
    }
    if (request_repaint) {
        request_repaint();
    }
    return true;
}

void TaskPage::append_log(std::wstring text, TerminalTone tone) {
    if (!m_visible) {
        m_skipped_while_hidden = true;
        return;
    }
    m_terminal.append(std::move(text), tone);
}

void TaskPage::reset_task_state() {
    m_report = sscl::core::InstallProgress{};
    m_files.clear();
    m_terminal.clear();
    m_terminal.set_max_lines(2000);
    m_terminal.set_bounds(m_terminal.bounds());  // 重新贴底
    m_get_seq = 0;
    m_last_phase.clear();
    m_last_detail.clear();
    m_speed_bps = 0.0;
    m_last_bytes = 0;
    m_has_sample = false;
    m_activity_phase = 0.0f;
    m_skipped_while_hidden = false;
    m_last_hidden_repaint = {};
    m_started_at = std::chrono::steady_clock::now();
    for (int i = 0; i < kMaxFileRows; ++i) {
        m_file_name[i]->text.clear();
        m_file_stats[i]->text.clear();
        m_file_name[i]->visible = false;
        m_file_stats[i]->visible = false;
    }
    update_labels();
}

void TaskPage::sync_active_files(const sscl::core::InstallProgress& report) {
    std::map<std::uint64_t, ActiveFile> next;
    for (const sscl::core::ActiveDownloadInfo& info : report.active) {
        ActiveFile file;
        const auto it = m_files.find(info.id);
        if (it != m_files.end()) {
            file = it->second;  // 保留速度采样
        }
        file.name = widen(info.name);
        file.bytes = info.bytes;
        if (info.total > 0) {
            file.total = info.total;
        }

        const auto now = std::chrono::steady_clock::now();
        if (file.has_sample) {
            const double dt = std::chrono::duration<double>(now - file.last_time).count();
            if (dt > 0.05 && info.bytes >= file.last_bytes) {
                const double instant =
                    static_cast<double>(info.bytes - file.last_bytes) / dt;
                file.speed_bps =
                    file.speed_bps <= 0.0 ? instant : file.speed_bps * 0.6 + instant * 0.4;
            }
        }
        file.last_bytes = info.bytes;
        file.last_time = now;
        file.has_sample = true;
        next.emplace(info.id, std::move(file));
    }
    m_files.swap(next);
}

void TaskPage::on_progress(const sscl::core::InstallProgress& report) {
    const auto now = std::chrono::steady_clock::now();
    if (report.bytes_done < m_last_bytes) {
        // D-117：新任务/分段基线会让累计字节回退。旧样本必须丢弃，
        // 否则下一次"增量"会算出离谱的瞬时速度（几十 MB/s 的假峰值）。
        m_has_sample = false;
        m_speed_bps = 0.0;
    }
    // 总速度 EMA（PCL 0.1s 平滑对照）
    if (m_has_sample) {
        const double dt = std::chrono::duration<double>(now - m_last_sample).count();
        if (dt > 0.001 && report.bytes_done >= m_last_bytes) {
            const double instant = static_cast<double>(report.bytes_done - m_last_bytes) / dt;
            m_speed_bps = (m_speed_bps <= 0.0) ? instant : (m_speed_bps * 0.7 + instant * 0.3);
        }
    }
    m_last_sample = now;
    const bool moved = report.bytes_done > m_last_bytes;
    m_last_bytes = report.bytes_done;
    m_has_sample = true;

    if (report.step_indeterminate && !moved) {
        // 处理器/Java 检测期间没有下载字节：速度归零（ETA 显示"处理中…"），
        // 避免沿用上一次下载速度造成"卡住但速度还在"的误导。
        // 但处理器阶段仍可能在下载映射文件（有真实字节增量）——那种情况保留速度。
        m_speed_bps = 0.0;
        m_has_sample = false;
    }

    if (!m_visible) {
        // 页面不可见：只保留悬浮窗需要的标量进度，丢弃日志/活动文件/文本更新；
        // 重绘降到 5Hz（悬浮窗仍能大致跟随），避免在其它页面全页重绘 10Hz。
        m_report = report;
        m_report.events.clear();
        m_report.active.clear();
        if (!report.events.empty()) {
            m_skipped_while_hidden = true;
        }
        if (m_last_hidden_repaint.time_since_epoch().count() == 0 ||
            now - m_last_hidden_repaint >= std::chrono::milliseconds(200)) {
            m_last_hidden_repaint = now;
            if (request_repaint) {
                request_repaint();
            }
        }
        return;
    }

    if (!report.phase.empty() && widen(report.phase) != m_last_phase) {
        m_last_phase = widen(report.phase);
        append_log(L"-- 阶段：" + m_last_phase + L" --", TerminalTone::kAccent);
    }
    if (!report.detail.empty()) {
        const std::wstring detail = widen(report.detail);
        if (detail != m_last_detail) {
            m_last_detail = detail;
            append_log(detail, TerminalTone::kDim);
        }
    }

    for (const sscl::core::DownloadEvent& event : report.events) {
        switch (event.kind) {
            case sscl::core::DownloadEventKind::kStarted: {
                ++m_get_seq;
                std::wstring line = L"Get:" + std::to_wstring(m_get_seq) + L" ";
                line += widen(event.name);
                if (event.bytes > 0) {
                    line += L" [" + format_bytes(event.bytes) + L"]";
                }
                m_terminal.append(std::move(line), TerminalTone::kDefault);
                break;
            }
            case sscl::core::DownloadEventKind::kFailed:
                append_log(L"E: 下载失败 " + widen(event.name), TerminalTone::kError);
                break;
            case sscl::core::DownloadEventKind::kFinished:
                break;  // 完成行不刷屏：进度由底部实时区呈现
        }
    }
    sync_active_files(report);

    m_report = report;
    // 事件/活动列表已经消费完：立即释放副本，避免大任务在 UI 侧多留一份。
    m_report.events.clear();
    m_report.active.clear();

    update_labels();
    if (request_repaint) {
        request_repaint();
    }
}

void TaskPage::update_labels() {
    // Java 检测等「总量未知」的步骤：百分比会长时间停在段起点，
    // 用"处理中…"替代一个不动的数字（步骤条本身在循环扫光）。
    m_progress_value->text = (step_animating() && m_report.step_total == 0)
                                 ? std::wstring(L"处理中…")
                                 : format_percent(total_fraction());
    m_speed_value->text = format_speed(m_speed_bps);
    const std::uint64_t remain =
        m_report.total_files > m_report.completed_files
            ? m_report.total_files - m_report.completed_files
            : 0;
    // 处理器/Java 检测阶段没有文件级进度：不要再显示一个不动的"剩余文件"。
    m_files_value->text =
        step_active() ? std::wstring(L"—") : std::to_wstring(remain);
    m_status_label->text = m_status;

    const std::wstring& phase = m_last_phase;
    const std::wstring& detail = m_last_detail;
    std::wstring step = L"步骤：" + (phase.empty() ? std::wstring(L"—") : phase);
    if (!detail.empty() && detail != phase) {
        step += L" — " + detail;
    }
    m_step_label->text = step;

    std::wstring overall = format_percent(total_fraction());
    if (m_report.bytes_total > 0) {
        overall += L"  ·  " + format_bytes(m_report.bytes_done) + L" / " +
                   format_bytes(m_report.bytes_total);
    } else if (m_report.bytes_done > 0) {
        overall += L"  ·  " + format_bytes(m_report.bytes_done);
    }
    m_overall_label->text = overall;
    m_speed_label->text = format_speed(m_speed_bps) + L"  ·  " + eta_text();

    if (m_terminal.trimmed_count() > 0) {
        m_terminal_meta->text =
            L"已省略 " + std::to_wstring(m_terminal.trimmed_count()) + L" 行";
    } else {
        m_terminal_meta->text = std::to_wstring(m_terminal.line_count()) + L" 行";
    }

    // 活动文件排序：剩余字节多的优先（未知大小排后面），最多显示 m_file_rows 行。
    std::vector<std::pair<std::uint64_t, const ActiveFile*>> ordered;
    ordered.reserve(m_files.size());
    for (const auto& [id, file] : m_files) {
        ordered.emplace_back(id, &file);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        const ActiveFile* left = a.second;
        const ActiveFile* right = b.second;
        const std::uint64_t left_remaining =
            left->total > left->bytes ? left->total - left->bytes : 0;
        const std::uint64_t right_remaining =
            right->total > right->bytes ? right->total - right->bytes : 0;
        if (left_remaining != right_remaining) {
            return left_remaining > right_remaining;
        }
        return left->bytes > right->bytes;
    });
    m_file_order.clear();
    m_file_order.reserve(ordered.size());
    for (const auto& entry : ordered) {
        m_file_order.push_back(entry.first);
    }

    const std::size_t rows = visible_file_rows();
    if (step_active()) {
        // 步骤阶段（处理器/Java 检测）没有文件级进度：隐藏逐文件行，底部改画步骤条。
        for (std::size_t i = 0; i < static_cast<std::size_t>(kMaxFileRows); ++i) {
            m_file_name[i]->visible = false;
            m_file_stats[i]->visible = false;
        }
        m_more_label->visible = false;
    } else {
        for (std::size_t i = 0; i < rows; ++i) {
            if (i < ordered.size()) {
                const ActiveFile& file = *ordered[i].second;
                m_file_name[i]->text = file.name;
                const float fraction =
                    file.total > 0 ? static_cast<float>(static_cast<double>(file.bytes) /
                                                        static_cast<double>(file.total))
                                   : 0.0f;
                std::wstring stats = format_percent(fraction);
                if (file.total > 0) {
                    stats += L"  ·  " + format_bytes(file.bytes) + L" / " +
                             format_bytes(file.total);
                } else {
                    stats += L"  ·  " + format_bytes(file.bytes);
                }
                m_file_stats[i]->text = stats;
                m_file_name[i]->visible = true;
                m_file_stats[i]->visible = true;
            } else {
                m_file_name[i]->visible = false;
                m_file_stats[i]->visible = false;
            }
        }
        for (std::size_t i = rows; i < static_cast<std::size_t>(kMaxFileRows); ++i) {
            m_file_name[i]->visible = false;
            m_file_stats[i]->visible = false;
        }

        if (ordered.size() > rows) {
            m_more_label->text = L"还有 " + std::to_wstring(ordered.size() - rows) +
                                 L" 个文件下载中（见上方输出）";
            m_more_label->visible = true;
        } else {
            m_more_label->visible = false;
        }
    }

    m_cancel->enabled = m_running || m_bg_active;
}

void TaskPage::update_system_labels() {
    if (!m_system_visible) {
        return;  // 小窗口隐藏系统信息块
    }
    wchar_t buffer[64];
    m_cpu_label->text = L"CPU";
    swprintf(buffer, 64, L"%.0f%%", m_cpu_percent);
    m_cpu_percent_label->text = buffer;
    if (m_ram_total_bytes > 0) {
        m_ram_label->text = format_bytes(m_ram_used_bytes) + L" / " +
                            format_bytes(m_ram_total_bytes);
        swprintf(buffer, 64, L"%.0f%%", m_ram_percent);
        m_ram_percent_label->text = buffer;
    } else {
        m_ram_label->text = L"RAM —";
        m_ram_percent_label->text = L"—";
    }
}

void TaskPage::draw_system_icons(ID2D1RenderTarget* target) const {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary;
    if (FAILED(target->CreateSolidColorBrush(theme_primary(), primary.GetAddressOf()))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary;
    target->CreateSolidColorBrush(theme_text_secondary(), secondary.GetAddressOf());

    const float x = kSidebarMarginDip;

    // —— CPU 图标：芯片本体 + 四边引脚（18x18，垂直中心 = 行中心）——
    const float cpu_y = m_system_top + 5.0f;
    primary->SetOpacity(0.65f);
    for (int i = 0; i < 3; ++i) {
        const float offset = 5.25f + static_cast<float>(i) * 3.0f;  // 中心 6 / 9 / 12
        target->FillRectangle(rect_of(x + offset, cpu_y, x + offset + 1.5f, cpu_y + 3.0f),
                              primary.Get());
        target->FillRectangle(
            rect_of(x + offset, cpu_y + 15.0f, x + offset + 1.5f, cpu_y + 18.0f),
            primary.Get());
        target->FillRectangle(rect_of(x, cpu_y + offset, x + 3.0f, cpu_y + offset + 1.5f),
                              primary.Get());
        target->FillRectangle(
            rect_of(x + 15.0f, cpu_y + offset, x + 18.0f, cpu_y + offset + 1.5f),
            primary.Get());
    }
    primary->SetOpacity(0.85f);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect_of(x + 3.0f, cpu_y + 3.0f, x + 15.0f, cpu_y + 15.0f), 2.5f,
                          2.5f),
        primary.Get());

    // CPU / RAM 统计图表：同一宽度、同一 x 区间；柱高严格按百分比线性映射。
    const float chart_width = 56.0f;
    const float chart_left = kSidebarWidthDip - kSidebarMarginDip - chart_width;
    const float chart_height = 22.0f;
    draw_history_graph(target, m_cpu_history,
                       rect_of(chart_left, m_system_top + 3.0f, chart_left + chart_width,
                               m_system_top + 3.0f + chart_height));

    // —— RAM 图标：内存条（本体/开槽/金手指均以 9 为中心对称，垂直中心 = 行中心）——
    const float ram_y = m_system_top + 28.0f + 3.0f;
    primary->SetOpacity(0.85f);
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect_of(x + 2.0f, ram_y + 7.0f, x + 16.0f, ram_y + 13.0f), 2.0f,
                          2.0f),
        primary.Get());
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> notch;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_sidebar_background(),
                                                notch.GetAddressOf()))) {
        for (int i = 0; i < 3; ++i) {
            const float notch_x = x + 5.25f + static_cast<float>(i) * 3.0f;  // 中心 6/9/12
            target->FillRectangle(
                rect_of(notch_x, ram_y + 8.0f, notch_x + 1.5f, ram_y + 10.0f), notch.Get());
        }
    }
    if (secondary != nullptr) {
        secondary->SetOpacity(0.9f);
        for (int i = 0; i < 4; ++i) {
            const float pin_x = x + 3.5f + static_cast<float>(i) * 3.0f;  // 中心 4.5..13.5
            target->FillRectangle(
                rect_of(pin_x, ram_y + 13.0f, pin_x + 2.0f, ram_y + 15.0f), secondary.Get());
        }
    }
    draw_history_graph(target, m_ram_history,
                       rect_of(chart_left, m_system_top + 28.0f + 3.0f,
                               chart_left + chart_width,
                               m_system_top + 28.0f + 3.0f + chart_height));
}

void TaskPage::draw_history_graph(ID2D1RenderTarget* target,
                                  const std::deque<float>& history,
                                  const D2D1_RECT_F& rect) const {
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    constexpr std::size_t kCapacity = 20;
    const float bar_width = width / static_cast<float>(kCapacity);

    // 20 个槽位的底轨：低占用时也能看清图表结构（不改变柱高的百分比比例）。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), track.GetAddressOf()))) {
        track->SetOpacity(0.35f);
        for (std::size_t i = 0; i < kCapacity; ++i) {
            const float slot_x = rect.left + static_cast<float>(i) * bar_width;
            target->FillRectangle(
                rect_of(slot_x + 0.4f, rect.top, slot_x + bar_width - 0.4f, rect.bottom),
                track.Get());
        }
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bars;
    if (FAILED(target->CreateSolidColorBrush(theme_primary(), bars.GetAddressOf()))) {
        return;
    }
    const std::size_t count = history.size();
    if (count > 0) {
        bars->SetOpacity(0.95f);
        for (std::size_t i = 0; i < count; ++i) {
            const float percent = (std::max)(0.0f, (std::min)(100.0f, history[i]));
            // 严格线性比例：45% 的柱高 = 图高 45%，3% = 3%；
            // 非零值至少画 1px（否则低百分比在屏幕上完全不可见，但比例不失真）。
            float bar_height = height * percent / 100.0f;
            if (percent > 0.0f && bar_height < 1.0f) {
                bar_height = 1.0f;
            }
            if (bar_height <= 0.0f) {
                continue;
            }
            const float bar_x = rect.left + width - static_cast<float>(count - i) * bar_width;
            target->FillRectangle(
                rect_of(bar_x + 0.4f, rect.bottom - bar_height,
                        bar_x + bar_width - 0.4f, rect.bottom),
                bars.Get());
        }
    }

    // 50% 参考线：帮助判断柱高是否与百分比一致。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> guide;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_text_secondary(),
                                                guide.GetAddressOf()))) {
        guide->SetOpacity(0.22f);
        const float middle = rect.top + height * 0.5f;
        target->DrawLine(D2D1::Point2F(rect.left, middle), D2D1::Point2F(rect.right, middle),
                         guide.Get(), 1.0f);
    }
}

void TaskPage::set_system_stats(const sscl::platform::SystemLoad& load) {
    m_cpu_percent = (std::max)(0.0, (std::min)(100.0, load.cpu_percent));
    m_ram_percent = (std::max)(0.0, (std::min)(100.0, load.memory_percent));
    m_ram_used_bytes = load.memory_used_bytes;
    m_ram_total_bytes = load.memory_total_bytes;

    m_cpu_history.push_back(static_cast<float>(m_cpu_percent));
    while (m_cpu_history.size() > 20u) {
        m_cpu_history.pop_front();
    }
    m_ram_history.push_back(static_cast<float>(m_ram_percent));
    while (m_ram_history.size() > 20u) {
        m_ram_history.pop_front();
    }

    update_system_labels();
    if (m_visible && request_repaint) {
        request_repaint();
    }
}

void TaskPage::set_visible(bool visible) {
    if (m_visible == visible) {
        return;
    }
      m_visible = visible;
      if (visible) {
          if (m_skipped_while_hidden) {
              m_terminal.append(L"-- 已恢复显示：隐藏期间的输出已省略 --", TerminalTone::kDim);
              m_skipped_while_hidden = false;
          }
          m_terminal.set_max_lines(2000);
          // D-098：进入页面立刻接管正在跑的后台任务，不等下一次轮询。
          m_bg_poll_ms = 0.0f;
          poll_background();
      } else {
        // 用户再也看不到的数据：日志收缩到最近 200 行，活动文件表释放。
        m_terminal.set_max_lines(200);
        m_terminal.trim_to(200);
        m_files.clear();
        for (int i = 0; i < kMaxFileRows; ++i) {
            m_file_name[i]->visible = false;
            m_file_stats[i]->visible = false;
        }
    }
    update_labels();
    if (request_repaint) {
        request_repaint();
    }
}

void TaskPage::start_install(const sscl::core::InstallJob& job) {
    if (m_running || m_service == nullptr) {
        return;
    }
    m_running = true;
    m_state = TaskState::kRunning;
    m_entry_id = job.minecraft.id;
    m_stop = std::stop_source();
    reset_task_state();

    const std::string loader_name =
        job.loader.kind == sscl::core::LoaderKind::kNone
            ? std::string()
            : std::string(" + ") + sscl::core::loader_kind_display(job.loader.kind) + " " +
                  job.loader.entry.version;
    m_status = L"正在安装 " + widen(job.minecraft.id) + widen(loader_name) + L" …";
    std::wstring header = L"-- SSCL 下载任务：" + widen(job.minecraft.id);
    if (!loader_name.empty()) {
        header += widen(loader_name);
    }
    header += L" --";
    m_terminal.append(std::move(header), TerminalTone::kAccent);
    update_labels();
    m_cancel->enabled = true;
    if (on_state_changed) {
        on_state_changed();  // 悬浮窗弹出动画（D-020）
    }
    if (request_repaint) {
        request_repaint();
    }

    // 值捕获（避免后台线程读 UI 状态）；shared_from_this 保活（§5.1）。
    std::stop_token token = m_stop.get_token();
    std::shared_ptr<TaskPage> self = shared_from_this();
    std::thread([self, job, token]() {
        const std::string display_id = job.minecraft.id;
        const auto started = std::chrono::steady_clock::now();
        sscl::util::Result<sscl::core::VersionJson> result =
            self->m_service->install_with_loader(
                job, token, [self](const sscl::core::InstallProgress& report) {
                    self->m_window->post_to_ui([self, report]() { self->on_progress(report); });
                });
        const auto finished = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(finished - started).count();

        self->m_window->post_to_ui([self, display_id, result, elapsed]() {
            self->m_running = false;
            self->m_cancel->enabled = false;
            const std::uint64_t finished_bytes = self->m_report.bytes_done;
            self->m_speed_bps = 0.0;
            if (result.is_ok()) {
                self->m_state = TaskState::kDone;
                self->m_status = L"安装完成：" + widen(display_id);
                // 收尾信息只有几行：无论页面是否可见都保留（用户返回时能看到结果）。
                const double speed =
                    elapsed > 0.0 ? static_cast<double>(finished_bytes) / elapsed : 0.0;
                wchar_t buffer[128];
                swprintf(buffer, 128, L"Fetched %s in %.1fs (%s)",
                         format_bytes(finished_bytes).c_str(), elapsed,
                         format_speed(speed).c_str());
                self->m_terminal.append(buffer, TerminalTone::kSuccess);
                self->m_terminal.append(L"Done", TerminalTone::kSuccess);
                self->m_terminal.set_max_lines(300);
                self->m_terminal.trim_to(300);
                self->m_files.clear();
                sscl::util::log_info("[ui] installed " + display_id);
                // D-047：完成后自动退回上一页面（用户停留在下载界面时不再停在任务页）
                if (self->on_task_done) {
                    self->on_task_done();
                }
            } else if (result.error().code == "download.cancelled") {
                self->m_state = TaskState::kCancelled;
                self->m_status = L"已取消";
                self->m_terminal.append(L"-- 已取消 --", TerminalTone::kError);
                self->m_files.clear();
                // D-023：确认取消后返回上一页面
                if (self->on_task_cancelled) {
                    self->on_task_cancelled();
                }
            } else {
                self->m_state = TaskState::kFailed;
                self->m_status = L"安装失败：" + widen(result.error().message);
                self->m_terminal.append(L"E: " + widen(result.error().message),
                                        TerminalTone::kError);
                if (!result.error().detail.empty()) {
                    self->m_terminal.append(widen(result.error().detail), TerminalTone::kDim);
                }
                self->m_files.clear();
                self->m_terminal.set_max_lines(600);
                sscl::util::log_error("[ui] install failed " + display_id + ": " +
                                      result.error().code + " / " + result.error().detail);
            }
            self->update_labels();
            if (self->request_repaint) {
                self->request_repaint();
            }
            if (self->on_state_changed) {
                self->on_state_changed();  // 悬浮窗收起动画（D-020）
            }
        });
    }).detach();
}

// ————————————————————————— 后台任务（D-098）—————————————————————————

bool TaskPage::has_background_job() const {
    return sscl::core::TaskCenter::instance().has_active();
}

void TaskPage::adopt_background_job(const sscl::core::JobSnapshot& snapshot) {
    m_bg_job = snapshot.id;
    m_bg_active = true;
    m_bg_last_file.clear();
    m_bg_files_seen.clear();
    reset_task_state();
    m_state = TaskState::kRunning;
    m_status = L"正在处理 " + widen(snapshot.title) + L" …";
    std::wstring header = L"-- SSCL 后台任务：" + widen(snapshot.title);
    if (!snapshot.subtitle.empty()) {
        header += L"（" + widen(snapshot.subtitle) + L"）";
    }
    header += L" --";
    m_terminal.append(std::move(header), TerminalTone::kAccent);
    m_cancel->enabled = true;
    update_labels();
    if (request_repaint) {
        request_repaint();
    }
}

void TaskPage::publish_background_progress(const sscl::core::JobSnapshot& snapshot) {
    // 把任务中心的通用进度合成成 InstallProgress，直接复用既有
    // 「速度 EMA / 隐藏降频 / 终端输出 / 实时区」链路（D-098）。
    sscl::core::InstallProgress report;
    report.phase = snapshot.progress.phase.empty() ? std::string("下载 Mod")
                                                   : snapshot.progress.phase;
    report.detail = snapshot.progress.current_file;
    report.bytes_done = snapshot.progress.done_bytes;
    report.bytes_total = snapshot.progress.total_bytes;
    report.completed_files = static_cast<std::uint64_t>((std::max)(0, snapshot.progress.done_files));
    report.total_files = static_cast<std::uint64_t>((std::max)(0, snapshot.progress.total_files));
    report.fraction = snapshot.progress.fraction();

    if (!snapshot.progress.current_file.empty()) {
        m_bg_last_file = snapshot.progress.current_file;
        // D-100：并行下载时 current_file 会在多个文件间跳动，
        // 按「首次出现」去重，保证每个文件只输出一行 Get:。
        if (std::find(m_bg_files_seen.begin(), m_bg_files_seen.end(),
                      snapshot.progress.current_file) == m_bg_files_seen.end()) {
            m_bg_files_seen.push_back(snapshot.progress.current_file);
            if (m_bg_files_seen.size() > 64) {
                m_bg_files_seen.erase(m_bg_files_seen.begin());
            }
            sscl::core::DownloadEvent event;
            event.kind = sscl::core::DownloadEventKind::kStarted;
            event.name = snapshot.progress.current_file;
            report.events.push_back(std::move(event));
        }
        sscl::core::ActiveDownloadInfo active;
        active.id = snapshot.progress.done_files > 0
                        ? static_cast<std::uint64_t>(snapshot.progress.done_files)
                        : 1;
        active.name = snapshot.progress.current_file;
        active.bytes = snapshot.progress.current_done;
        active.total = snapshot.progress.current_total;
        report.active.push_back(std::move(active));
    }

    m_status = L"正在处理 " + widen(snapshot.title) + L" …";
    on_progress(report);
}

void TaskPage::finish_background_job(const sscl::core::JobSnapshot& snapshot) {
    m_bg_job = 0;
    m_bg_active = false;
    m_status.clear();
    m_speed_bps = 0.0;
    m_files.clear();
    switch (snapshot.state) {
        case sscl::core::JobState::kSucceeded:
            m_state = TaskState::kDone;
            m_status = L"完成：" + widen(snapshot.title);
            // 成功即 100%：即使最后一个文件的字节回调被节流丢弃，也不会停在 90.x%。
            m_report.fraction = 1.0f;
            if (m_report.bytes_total > 0) {
                m_report.bytes_done = m_report.bytes_total;
            }
            m_report.completed_files = m_report.total_files;
            m_terminal.append(L"Done", TerminalTone::kSuccess);
            break;
        case sscl::core::JobState::kCancelled:
            m_state = TaskState::kCancelled;
            m_status = L"已取消：" + widen(snapshot.title);
            m_terminal.append(L"-- 已取消 --", TerminalTone::kError);
            break;
        case sscl::core::JobState::kFailed:
        default:
            m_state = TaskState::kFailed;
            m_status = L"失败：" + widen(snapshot.title);
            if (!snapshot.message.empty()) {
                m_status += L" — " + widen(snapshot.message);
                m_terminal.append(widen(snapshot.message), TerminalTone::kError);
            }
            break;
    }
    m_terminal.set_max_lines(300);
    m_terminal.trim_to(300);
    update_labels();
    if (request_repaint) {
        request_repaint();
    }
    if (on_state_changed) {
        on_state_changed();
    }
}

void TaskPage::poll_background() {
    if (m_running) {
        return;  // 前台版本安装优先显示
    }
    sscl::core::TaskCenter& center = sscl::core::TaskCenter::instance();
    if (m_bg_job != 0) {
        const sscl::core::JobSnapshot snapshot = center.job(m_bg_job);
        if (!snapshot.valid()) {
            m_bg_job = 0;
            m_bg_active = false;
            return;
        }
        if (snapshot.finished()) {
            // D-100：先把最终快照推一次（任务中心成功时已把 done 拉到 total），
            // 再收尾 —— 否则进度条会停在最后一个 10Hz 采样点（如 90.9%）。
            publish_background_progress(snapshot);
            finish_background_job(snapshot);
            return;
        }
        publish_background_progress(snapshot);
        return;
    }
    const sscl::core::JobSnapshot active = center.active();
    if (!active.valid()) {
        return;
    }
    adopt_background_job(active);
    publish_background_progress(active);
}

}  // namespace sscl::ui
