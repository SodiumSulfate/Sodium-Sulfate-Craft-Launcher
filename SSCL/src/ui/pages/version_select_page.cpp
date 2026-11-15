#include "ui/pages/version_select_page.hpp"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

#include "core/config/build_config.hpp"
#include "core/config/secret_store.hpp"
#include "core/modpack/modpack_importer.hpp"
#include "core/modplatform/mod_catalog.hpp"
#include "platform/path_utils.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/framework/version_icon.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

// 版本列表卡片几何（D-094）：rebuild_hits / draw_cards 共用一份，避免两处漂移。
// 标题行 36；内容上留白 2、行高 46/节距 50、下留白 6；行左右内缩 6；卡片间距 12。
constexpr float kListHeaderH = 36.0f;
constexpr float kListBodyTop = 2.0f;
constexpr float kListBodyBottom = 6.0f;
constexpr float kListRowH = 46.0f;
constexpr float kListRowPitch = 50.0f;
constexpr float kListRowInset = 6.0f;
constexpr float kListCardGap = 12.0f;

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

bool contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

struct Ctx {
    ID2D1RenderTarget* target = nullptr;
    IDWriteFactory* dwrite = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_pressed;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> primary_container;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> on_primary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> elevated;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> danger;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> warning;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f10;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f11;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f12;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f13b;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f14b;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> f16b;
};

Microsoft::WRL::ComPtr<IDWriteTextFormat> make_format(IDWriteFactory* dwrite, float size,
                                                       DWRITE_FONT_WEIGHT weight) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (dwrite == nullptr) {
        return format;
    }
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", format.GetAddressOf()))) {
        format.Reset();
    }
    return format;
}

bool make_ctx(Ctx& ctx, ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return false;
    }
    ctx.target = target;
    ctx.dwrite = dwrite;
    if (FAILED(target->CreateSolidColorBrush(theme_primary(), ctx.primary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_hover(),
                                             ctx.primary_hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_pressed(),
                                             ctx.primary_pressed.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_hover_background(),
                                             ctx.hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_primary_container(),
                                             ctx.primary_container.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_on_primary(),
                                             ctx.on_primary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_primary(), ctx.text.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_secondary(),
                                             ctx.secondary.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_text_muted(), ctx.muted.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), ctx.border.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_surface(), ctx.surface.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_elevated_surface(),
                                             ctx.elevated.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_danger(), ctx.danger.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_warning(), ctx.warning.GetAddressOf()))) {
        return false;
    }
    ctx.f9 = make_format(dwrite, 9.5f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f10 = make_format(dwrite, 10.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f11 = make_format(dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f12 = make_format(dwrite, 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    ctx.f13b = make_format(dwrite, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ctx.f14b = make_format(dwrite, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ctx.f16b = make_format(dwrite, 16.0f, DWRITE_FONT_WEIGHT_BOLD);
    return ctx.f9 && ctx.f10 && ctx.f11 && ctx.f12 && ctx.f13b && ctx.f14b && ctx.f16b;
}

void draw_text(Ctx& ctx, const std::wstring& text, const D2D1_RECT_F& rect,
               const Microsoft::WRL::ComPtr<IDWriteTextFormat>& format, ID2D1Brush* brush,
               DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (format.Get() == nullptr || brush == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(valign);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ctx.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect,
                          brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void fill_round(Ctx& ctx, const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush) {
    if (brush == nullptr || !rect_valid(rect)) {
        return;
    }
    ctx.target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
}

void stroke_round(Ctx& ctx, const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush,
                  float width = 1.0f) {
    if (brush == nullptr || !rect_valid(rect)) {
        return;
    }
    ctx.target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, width);
}

void draw_gear(Ctx& ctx, float cx, float cy, float radius, ID2D1Brush* brush) {
    if (brush == nullptr) {
        return;
    }
    ctx.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius * 0.45f, radius * 0.45f),
                            brush, 1.2f);
    for (int i = 0; i < 8; ++i) {
        const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
        const float x0 = cx + std::cos(angle) * radius * 0.55f;
        const float y0 = cy + std::sin(angle) * radius * 0.55f;
        const float x1 = cx + std::cos(angle) * radius * 0.9f;
        const float y1 = cy + std::sin(angle) * radius * 0.9f;
        ctx.target->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, 1.2f);
    }
}

void draw_cross(Ctx& ctx, float cx, float cy, float radius, ID2D1Brush* brush) {
    if (brush == nullptr) {
        return;
    }
    ctx.target->DrawLine(D2D1::Point2F(cx - radius, cy - radius),
                         D2D1::Point2F(cx + radius, cy + radius), brush, 1.2f);
    ctx.target->DrawLine(D2D1::Point2F(cx + radius, cy - radius),
                         D2D1::Point2F(cx - radius, cy + radius), brush, 1.2f);
}

void draw_button(Ctx& ctx, const D2D1_RECT_F& rect, const std::wstring& label, bool primary,
                 bool enabled = true, const char* icon = nullptr, bool hovered = false,
                 bool pressed = false) {
    (void)hovered;
    (void)pressed;
    // 三态与首页 Button 控件一致：主按钮 primary → hover → pressed；
    // 次按钮 surface → Hover Background + Primary 描边 → PrimaryContainer。
    // D-083：全部自绘按钮统一取全局 hover/pressed 渐变权重，逐页完全一致。
    const motion::StateColors state = motion::button_colors(primary, enabled, rect);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(ctx.target->CreateSolidColorBrush(state.fill, fill_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.border, stroke_brush.GetAddressOf())) ||
        FAILED(ctx.target->CreateSolidColorBrush(state.text, text_brush.GetAddressOf()))) {
        return;
    }
    ID2D1Brush* background = fill_brush.Get();
    ID2D1Brush* foreground = text_brush.Get();
    fill_round(ctx, rect, theme_radius(), background);
    if (!primary) {
        stroke_round(ctx, rect, theme_radius(), stroke_brush.Get());
    }
    D2D1_RECT_F text_rect = rect;
    if (icon != nullptr && *icon != '\0') {
        float text_width = 0.0f;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(ctx.dwrite->CreateTextLayout(
                label.c_str(), static_cast<UINT32>(label.size()), ctx.f12.Get(), 10000.0f, 40.0f,
                layout.GetAddressOf())) &&
            layout != nullptr) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                text_width = metrics.widthIncludingTrailingWhitespace;
            }
        }
        const float icon_size = 18.0f;
        const float gap = 6.0f;
        const float total = icon_size + gap + text_width;
        float start = rect.left + ((rect.right - rect.left) - total) * 0.5f;
        if (start < rect.left + 5.0f) {
            start = rect.left + 5.0f;
        }
        const float center_y = (rect.top + rect.bottom) * 0.5f;
        IconManager::instance().draw(
            ctx.target, icon,
            rect_of(start, center_y - icon_size * 0.5f, start + icon_size,
                    center_y + icon_size * 0.5f),
            state.text);
        text_rect.left = start + icon_size + gap;
        text_rect.right = rect.right - 3.0f;
    }
    draw_text(ctx, label, text_rect, ctx.f12, foreground,
              icon != nullptr && *icon != '\0' ? DWRITE_TEXT_ALIGNMENT_LEADING
                                               : DWRITE_TEXT_ALIGNMENT_CENTER);
}

}  // namespace

VersionSelectPage::VersionSelectPage(AppWindow* window,
                                     std::shared_ptr<sscl::core::GameService> service)
    : m_window(window), m_service(std::move(service)) {}

void VersionSelectPage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    // 顶部留白 24（与侧栏标题同基线），底部预留 48 给状态条与滚动条。
    m_list_top = kNavHeightDip + 24.0f;
    m_list_bottom = (std::max)(m_list_top + 80.0f, m_height_dip - 48.0f);
    m_scroll.layout(m_list_top, m_list_bottom, 0.0f, m_width_dip);
    rebuild_hits();
}

void VersionSelectPage::set_folders(std::vector<sscl::core::McFolderInfo> folders) {
    m_folders = std::move(folders);
    rebuild_hits();
    if (request_repaint) {
        request_repaint();
    }
}

void VersionSelectPage::set_message(std::wstring message) {
    m_message = std::move(message);
    if (request_repaint) {
        request_repaint();
    }
}

// D-089：整合包导入。解析 → 匹配已安装实例 → 下载落盘；失败给出可执行提示。
void VersionSelectPage::start_import_modpack() {
    if (m_importing || m_service == nullptr || m_window == nullptr) {
        return;
    }
    std::vector<std::wstring> picked;
    if (!sscl::platform::open_file_dialog(
            m_window->hwnd(), L"选择整合包",
            L"整合包 (*.mrpack;*.zip)\0*.mrpack;*.zip\0所有文件\0*.*\0\0", &picked) ||
        picked.empty()) {
        return;
    }
    const std::filesystem::path archive(picked.front());
    m_importing = true;
    m_import_stop = std::stop_source{};
    set_message(L"正在解析整合包…");

    std::shared_ptr<VersionSelectPage> self = shared_from_this();
    const std::stop_token stop = m_import_stop.get_token();
    std::thread([self, archive, stop]() {
        util::Result<sscl::core::ModpackIndex> parsed = sscl::core::parse_modpack(archive);
        if (parsed.is_error()) {
            const std::wstring message =
                L"整合包解析失败：" + widen(parsed.error().message);
            self->m_window->post_to_ui([self, message]() {
                self->m_importing = false;
                self->set_message(message);
            });
            return;
        }
        sscl::core::ModpackIndex index = std::move(parsed.value());
        const std::string version_id = sscl::core::match_modpack_instance(
            index, self->m_service->version_summaries());
        if (version_id.empty()) {
            std::wstring want = widen(index.minecraft_version);
            if (!index.loader.empty()) {
                want += L" / " + widen(index.loader);
            }
            const std::wstring message =
                L"未找到匹配实例（需要 " + want + L"）。请先安装该版本后再导入。";
            self->m_window->post_to_ui([self, message]() {
                self->m_importing = false;
                self->set_message(message);
            });
            return;
        }
        util::Result<sscl::core::InstanceTarget> target =
            self->m_service->resolve_target(version_id);
        if (target.is_error()) {
            const std::wstring message =
                L"无法解析目标实例：" + widen(target.error().message);
            self->m_window->post_to_ui([self, message]() {
                self->m_importing = false;
                self->set_message(message);
            });
            return;
        }
        const std::filesystem::path instance_dir = target.value().run_dir;

        // CurseForge manifest 只有 projectID/fileID，必须经 API 换成直链。
        if (index.format == sscl::core::ModpackFormat::kCurseForge) {
            const std::filesystem::path data_dir = sscl::platform::data_dir();
            std::string key = std::string(sscl::core::curseforge_api_key());
            util::Result<std::string> stored = sscl::core::load_secret_value(
                data_dir / L"secrets.dat", "curseforge_api_key");
            if (stored.is_ok() && !stored.value().empty()) {
                key = stored.value();
            }
            auto catalog = std::make_shared<sscl::core::ModCatalog>(
                sscl::core::make_mod_catalog_options(data_dir, key, true));
            auto resolver = [catalog, stop](const std::string& project_id,
                                            const std::string& file_id)
                -> util::Result<std::string> {
                util::Result<sscl::core::ModVersion> version = catalog->version(
                    sscl::core::ModPlatform::kCurseForge, project_id, file_id, stop);
                if (version.is_error()) {
                    return util::Result<std::string>::fail(version.error());
                }
                const sscl::core::ModFile* file = sscl::core::select_primary_file(version.value());
                if (file == nullptr || file->url.empty()) {
                    return util::Result<std::string>::fail(util::Error::make(
                        "modpack.file_unavailable", "文件没有下载地址",
                        project_id + "/" + file_id));
                }
                return util::Result<std::string>::ok(file->url);
            };
            util::Result<void> resolved =
                sscl::core::resolve_curseforge_urls(&index, resolver, stop);
            if (resolved.is_error()) {
                const std::wstring message =
                    L"CurseForge 整合包需要可用 API Key：" + widen(resolved.error().message);
                self->m_window->post_to_ui([self, message]() {
                    self->m_importing = false;
                    self->set_message(message);
                });
                return;
            }
        }

        sscl::core::ModpackInstallOptions options;
        const sscl::core::ModpackProgressFn progress =
            [self](const sscl::core::ModpackInstallProgress& current) {
                self->m_window->post_to_ui([self, current]() {
                    self->set_message(L"导入中 " + std::to_wstring(current.completed_files) +
                                      L"/" + std::to_wstring(current.total_files) + L"：" +
                                      widen(current.current_file));
                });
            };
        util::Result<int> installed = sscl::core::install_modpack_files(
            index, instance_dir, self->m_service->scheduler(), options, progress, stop);
        std::wstring message;
        if (installed.is_error()) {
            message = L"整合包导入失败：" + widen(installed.error().message);
        } else {
            message = L"整合包导入完成：写入 " + std::to_wstring(installed.value()) +
                      L" 个文件到 " + widen(version_id);
            if (!index.warnings.empty()) {
                message += L"（" + std::to_wstring(index.warnings.size()) + L" 条警告）";
            }
        }
        self->m_window->post_to_ui([self, message, version_id]() {
            self->m_importing = false;
            self->set_message(message);
            self->refresh(self->m_selected_id.empty() ? version_id : self->m_selected_id);
        });
    }).detach();
}

void VersionSelectPage::refresh(const std::string& selected_id) {
    m_selected_id = selected_id;
    rebuild_cards();
    m_scroll.jump_to(0.0f);
    rebuild_hits();
    if (request_repaint) {
        request_repaint();
    }
}

void VersionSelectPage::rebuild_cards() {
    m_cards = m_service ? m_service->instance_cards(m_show_hidden)
                        : std::vector<sscl::core::InstanceCard>{};
    const bool force_expand = m_cards.size() == 1;
    for (const auto& card : m_cards) {
        const int key = static_cast<int>(card.category);
        if (m_expanded.find(key) == m_expanded.end()) {
            const bool default_expanded =
                card.category == sscl::core::InstanceCategory::kStar ||
                card.category == sscl::core::InstanceCategory::kApi ||
                card.category == sscl::core::InstanceCategory::kOriginalLike;
            m_expanded[key] = force_expand || default_expanded;
        }
        // 首次出现直接落到目标高度（不播放入场手风琴）；后续切换才做过渡。
        if (m_expand_anim.find(key) == m_expand_anim.end()) {
            m_expand_anim[key] = m_expanded[key] ? 1.0f : 0.0f;
        }
    }
}

std::wstring VersionSelectPage::state_text(sscl::core::InstanceState state) const {
    switch (state) {
        case sscl::core::InstanceState::kSnapshot:
            return L"快照";
        case sscl::core::InstanceState::kOld:
            return L"旧版";
        case sscl::core::InstanceState::kFool:
            return L"愚人节";
        case sscl::core::InstanceState::kForge:
            return L"Forge";
        case sscl::core::InstanceState::kNeoForge:
            return L"NeoForge";
        case sscl::core::InstanceState::kFabric:
            return L"Fabric";
        case sscl::core::InstanceState::kQuilt:
            return L"Quilt";
        case sscl::core::InstanceState::kError:
            return L"错误";
        case sscl::core::InstanceState::kOriginal:
        default:
            return L"正式版";
    }
}

std::wstring VersionSelectPage::version_info_text(
    const sscl::core::VersionSummary& summary) const {
    if (!summary.custom_info.empty()) {
        return widen(summary.custom_info);
    }
    std::wstring text = state_text(summary.state);
    if (!summary.minecraft_version.empty()) {
        text += L" " + widen(summary.minecraft_version);
    }
    if (!summary.loader.empty()) {
        text += L", " + widen(summary.loader);
        if (!summary.loader_version.empty()) {
            text += L" " + widen(summary.loader_version);
        }
    }
    return text;
}

void VersionSelectPage::rebuild_folder_hits() {
    m_folder_hits.clear();
    const float margin = kSidebarMarginDip;
    // 侧栏刻度（D-094）：标题 24 起；行高 46、节距 50（4px 行距）；
    // 分区间距 20；操作项 32 高、间距 8；底部返回距底 16。
    float y = kNavHeightDip + 60.0f;
    for (const auto& folder : m_folders) {
        FolderHit hit;
        hit.path = folder.location;
        hit.row = rect_of(margin, y, kSidebarWidthDip - margin, y + 46.0f);
        // 与列表行同一规则（D-095）：图标框右缘距行右缘 4px，热区不外扩；
        // 此前框右缘与行描边重合，图标看起来像被行边框切掉。
        hit.gear = rect_of(kSidebarWidthDip - margin - 28.0f, y + 11.0f,
                           kSidebarWidthDip - margin - 4.0f, y + 35.0f);
        m_folder_hits.push_back(std::move(hit));
        y += 50.0f;
    }
    const float section_y = y + 16.0f;  // 上一行底 + 20
    m_add_folder_rect = rect_of(margin, section_y + 24.0f, kSidebarWidthDip - margin,
                                section_y + 56.0f);
    m_import_rect = rect_of(margin, section_y + 64.0f, kSidebarWidthDip - margin,
                            section_y + 96.0f);
    m_back_rect = rect_of(margin, m_height_dip - 52.0f, kSidebarWidthDip - margin,
                          m_height_dip - 16.0f);
    m_download_rect = rect_of(0, 0, 0, 0);
}

void VersionSelectPage::rebuild_hits() {
    rebuild_folder_hits();

    // D-093：展开进度 → 卡片内容区可见高度；收起时高度渐变到 0，下方卡片同步上移。
    const auto reveal_of = [this](int category) {
        const auto it = m_expand_anim.find(category);
        const float value = it == m_expand_anim.end()
                                ? (m_expanded[category] ? 1.0f : 0.0f)
                                : it->second;
        return (std::max)(0.0f, (std::min)(1.0f, value));
    };
    const auto body_of = [&](const sscl::core::InstanceCard& card) {
        return static_cast<float>(card.versions.size()) * kListRowPitch *
               reveal_of(static_cast<int>(card.category));
    };

    const float list_left = kSidebarWidthDip + 24.0f;
    const float list_right = m_width_dip - 24.0f;
    float content_y = 0.0f;
    for (const auto& card : m_cards) {
        content_y +=
            kListHeaderH + kListBodyTop + body_of(card) + kListBodyBottom + kListCardGap;
    }
    m_scroll.layout(m_list_top, m_list_bottom, content_y, m_width_dip);
    const float offset = m_scroll.offset();

    m_card_layout.clear();
    m_card_hits.clear();
    float y = 0.0f;
    for (const auto& card : m_cards) {
        CardLayout layout;
        layout.category = static_cast<int>(card.category);
        layout.expanded = m_expanded[layout.category];
        layout.reveal = reveal_of(layout.category);
        layout.header = rect_of(list_left, m_list_top + y - offset, list_right,
                                m_list_top + y + kListHeaderH - offset);
        m_card_hits.push_back(CardHit{layout.category, layout.header});
        y += kListHeaderH;
        const float body = body_of(card);
        const float body_top = y + kListBodyTop;
        layout.body_bottom = m_list_top + body_top - offset + body;
        if (layout.expanded || layout.reveal > 0.001f) {
            for (std::size_t i = 0; i < card.versions.size(); ++i) {
                // 行顶进入可见区即登记（绘制由内容区裁剪负责），保证展开过程连贯。
                if (static_cast<float>(i) * kListRowPitch >= body) {
                    break;
                }
                const auto& summary = card.versions[i];
                RowHit row;
                row.id = summary.version.id;
                const float top =
                    m_list_top + body_top + static_cast<float>(i) * kListRowPitch - offset;
                row.row = rect_of(list_left + kListRowInset, top, list_right - kListRowInset,
                                  top + kListRowH);
                // 图标列：行内右侧留 4px、图标间距 6；热区即图标框，不再外扩，
                // 否则悬停底色会越出行/卡片的圆角边界（2026-11-12 溢出事故）。
                row.gear = rect_of(list_right - kListRowInset - 28.0f, top + 11.0f,
                                   list_right - kListRowInset - 4.0f, top + 35.0f);
                row.del = rect_of(list_right - kListRowInset - 58.0f, top + 11.0f,
                                  list_right - kListRowInset - 34.0f, top + 35.0f);
                row.star = rect_of(list_right - kListRowInset - 88.0f, top + 11.0f,
                                   list_right - kListRowInset - 64.0f, top + 35.0f);
                layout.rows.push_back(std::move(row));
            }
        }
        y = body_top + body + kListBodyBottom + kListCardGap;
        m_card_layout.push_back(std::move(layout));
    }
}

void VersionSelectPage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    rebuild_hits();

    // 与首页/任务页同一套页面底色与侧栏色（Background / Secondary Surface）。
    // 此前这里是 Surface(#08060A) + Elevated(#17101A)，左侧栏比首页亮一档；
    // Elevated 按规范仅用于弹窗/下拉/浮层。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> page_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sidebar_bg;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> divider;
    if (FAILED(target->CreateSolidColorBrush(theme_background(), page_bg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_sidebar_background(),
                                             sidebar_bg.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), divider.GetAddressOf()))) {
        return;
    }
    divider->SetOpacity(0.6f);
    ctx.target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip), page_bg.Get());
    ctx.target->FillRectangle(rect_of(0, kNavHeightDip, kSidebarWidthDip, m_height_dip),
                              sidebar_bg.Get());
    ctx.target->DrawLine(D2D1::Point2F(kSidebarWidthDip, kNavHeightDip),
                         D2D1::Point2F(kSidebarWidthDip, m_height_dip), divider.Get(), 1.0f);

    draw_folder_list(target, dwrite);
    draw_cards(target, dwrite);
    m_scroll.draw(target);

    if (!m_message.empty()) {
        draw_text(ctx, m_message, rect_of(kSidebarWidthDip + 24.0f, m_height_dip - 34.0f,
                                          m_width_dip - 24.0f, m_height_dip - 14.0f),
                  ctx.f11, ctx.muted.Get());
    }
    motion::menu().set_open(m_folder_menu_open, 2);
    if (m_folder_menu_open || motion::menu().visible_for(2)) {
        const bool menu_layer = motion::push_fade(target, motion::menu().opacity(),
                                                  motion::menu().offset_y());
        draw_folder_menu(target, dwrite);
        if (menu_layer) {
            motion::pop_layer(target);
        }
    }
    if (m_overlay != OverlayKind::kNone || motion::modal().visible()) {
        draw_overlay(target, dwrite);
    }
}

void VersionSelectPage::draw_folder_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    const float margin = kSidebarMarginDip;
    draw_text(ctx, L"文件夹列表",
              rect_of(margin, kNavHeightDip + 24.0f, kSidebarWidthDip - margin,
                      kNavHeightDip + 46.0f),
              ctx.f11, ctx.muted.Get());

    for (std::size_t i = 0; i < m_folders.size() && i < m_folder_hits.size(); ++i) {
        const auto& folder = m_folders[i];
        const auto& hit = m_folder_hits[i];
        const int folder_index = static_cast<int>(i);
        const bool folder_pressed = (m_pressed == Hot::kFolder && m_hover_folder == folder_index);
        if (folder.selected || folder_pressed) {
            fill_round(ctx, hit.row, 6.0f, ctx.primary_container.Get());
            stroke_round(ctx, hit.row, 6.0f, ctx.primary.Get());
        }
        motion::draw_hot_overlay(ctx.target, hit.row, 6.0f, theme_hover_background(),
                             theme_primary_container());
        const float radio_x = margin + 10.0f;
        const float radio_y = hit.row.top + 23.0f;
        if (folder.selected) {
            ctx.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(radio_x, radio_y), 6.0f, 6.0f),
                                    ctx.primary.Get());
        } else {
            ctx.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(radio_x, radio_y), 6.0f, 6.0f),
                                    ctx.border.Get(), 1.2f);
        }
        draw_text(ctx, widen(folder.name),
                  rect_of(margin + 24.0f, hit.row.top + 4.0f, hit.gear.left - 10.0f,
                          hit.row.top + 25.0f),
                  ctx.f12, ctx.text.Get());
        draw_text(ctx, widen(sscl::util::wide_to_utf8(folder.location.wstring())),
                  rect_of(margin + 24.0f, hit.row.top + 25.0f, hit.gear.left - 10.0f,
                          hit.row.top + 45.0f),
                  ctx.f10, ctx.muted.Get());
        const bool gear_hovered =
            (m_hover == Hot::kFolderGear && m_hover_folder == folder_index);
        motion::draw_hot_overlay(ctx.target, hit.gear, 6.0f, theme_hover_background(),
                                 theme_primary_container());
        IconManager::instance().draw(
            ctx.target, "settings",
            rect_of(hit.gear.left + 2.0f, hit.gear.top + 2.0f, hit.gear.right - 2.0f,
                    hit.gear.bottom - 2.0f),
            gear_hovered ? theme_primary() : theme_text_secondary());
    }

    const float section_y = m_folder_hits.empty()
                                ? kNavHeightDip + 76.0f
                                : m_folder_hits.back().row.bottom + 18.0f;
    draw_text(ctx, L"添加或导入", rect_of(margin, section_y, kSidebarWidthDip - margin,
                                          section_y + 20.0f),
              ctx.f11, ctx.muted.Get());
    const bool add_hovered = (m_hover == Hot::kAddFolder);
    const bool import_hovered = (m_hover == Hot::kImport);
    const bool back_hovered = (m_hover == Hot::kBack);
    motion::draw_hot_overlay(ctx.target, m_add_folder_rect, 6.0f, theme_hover_background(),
                         theme_primary_container());
    IconManager::instance().draw(
        ctx.target, "folder_open",
        rect_of(m_add_folder_rect.left + 8.0f, m_add_folder_rect.top + 7.0f,
                m_add_folder_rect.left + 26.0f, m_add_folder_rect.bottom - 7.0f),
        add_hovered ? theme_primary() : theme_text_primary());
    draw_text(ctx, L"添加已有文件夹",
              rect_of(m_add_folder_rect.left + 30.0f, m_add_folder_rect.top,
                      m_add_folder_rect.right, m_add_folder_rect.bottom),
              ctx.f12, add_hovered ? ctx.primary.Get() : ctx.text.Get());
    motion::draw_hot_overlay(ctx.target, m_import_rect, 6.0f, theme_hover_background(),
                         theme_primary_container());
    IconManager::instance().draw(
        ctx.target, "upload_file",
        rect_of(m_import_rect.left + 8.0f, m_import_rect.top + 7.0f,
                m_import_rect.left + 26.0f, m_import_rect.bottom - 7.0f),
        import_hovered ? theme_primary() : theme_text_muted());
    draw_text(ctx, L"导入整合包（后续）",
              rect_of(m_import_rect.left + 30.0f, m_import_rect.top, m_import_rect.right,
                      m_import_rect.bottom),
              ctx.f12, import_hovered ? ctx.primary.Get() : ctx.muted.Get());
    motion::draw_hot_overlay(ctx.target, m_back_rect, 6.0f, theme_hover_background(),
                         theme_primary_container());
    IconManager::instance().draw(
        ctx.target, "arrow_back",
        rect_of(m_back_rect.left + 8.0f, m_back_rect.top + 9.0f, m_back_rect.left + 26.0f,
                m_back_rect.bottom - 9.0f),
        back_hovered ? theme_primary() : theme_text_secondary());
    draw_text(ctx, L"返回启动页",
              rect_of(m_back_rect.left + 30.0f, m_back_rect.top, m_back_rect.right,
                      m_back_rect.bottom),
              ctx.f12, back_hovered ? ctx.primary.Get() : ctx.secondary.Get());
}

void VersionSelectPage::draw_cards(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    const float list_left = kSidebarWidthDip + 24.0f;
    const float list_right = m_width_dip - 24.0f;

    if (m_cards.empty()) {
        const float card_w = (std::min)(420.0f, list_right - list_left);
        const float card_h = 180.0f;
        const D2D1_RECT_F card = rect_of(list_left + (list_right - list_left - card_w) * 0.5f,
                                          m_list_top + 90.0f,
                                          list_left + (list_right - list_left + card_w) * 0.5f,
                                          m_list_top + 90.0f + card_h);
        fill_round(ctx, card, theme_radius(), ctx.surface.Get());
        stroke_round(ctx, card, theme_radius(), ctx.border.Get());
        draw_text(ctx, m_show_hidden ? L"没有隐藏的版本" : L"没有找到任何版本",
                  rect_of(card.left + 20.0f, card.top + 28.0f, card.right - 20.0f,
                          card.top + 60.0f),
                  ctx.f16b, ctx.text.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(ctx,
                  m_show_hidden ? L"按 F11 退出隐藏版本模式"
                                : L"可以在左侧添加已有文件夹，或下载一个游戏版本。",
                  rect_of(card.left + 24.0f, card.top + 66.0f, card.right - 24.0f,
                          card.top + 96.0f),
                  ctx.f11, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        m_download_rect = rect_of(card.left + (card_w - 150.0f) * 0.5f, card.top + 112.0f,
                                  card.left + (card_w + 150.0f) * 0.5f, card.top + 144.0f);
        draw_button(ctx, m_download_rect, m_show_hidden ? L"退出隐藏模式" : L"下载游戏", true,
                    true, "download", m_hover == Hot::kDownload,
                    m_pressed == Hot::kDownload);
        return;
    }
    m_download_rect = rect_of(0, 0, 0, 0);

    target->PushAxisAlignedClip(rect_of(list_left, m_list_top, list_right, m_list_bottom),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t ci = 0; ci < m_card_layout.size() && ci < m_cards.size(); ++ci) {
        const auto& card = m_cards[ci];
        const auto& layout = m_card_layout[ci];
        const float body =
            static_cast<float>(card.versions.size()) * kListRowPitch * layout.reveal;
        // 与 rebuild_hits 同一套刻度：内容上留白 2、下留白 6。
        float bottom = layout.header.bottom;
        if (body > 0.5f) {
            bottom += kListBodyTop + body + kListBodyBottom;
        }
        const D2D1_RECT_F card_rect = rect_of(list_left, layout.header.top, list_right, bottom);
        if (card_rect.bottom < m_list_top || card_rect.top > m_list_bottom) {
            continue;
        }
        fill_round(ctx, card_rect, theme_radius(), ctx.surface.Get());
        // 浅色下 Surface=#FFFFFF 与页面 #FFF7FA 极接近，必须有 1px Outline 才有卡片边界。
        stroke_round(ctx, card_rect, theme_radius(), ctx.border.Get());
        motion::draw_hot_overlay(ctx.target, layout.header, theme_radius(), theme_hover_background(),
                             theme_primary_container());
        draw_text(ctx, widen(card.title) + L"  (" + std::to_wstring(card.versions.size()) + L")",
                  rect_of(layout.header.left + 16.0f, layout.header.top,
                          layout.header.right - 30.0f, layout.header.bottom),
                  ctx.f14b, ctx.text.Get());
        draw_text(ctx, layout.expanded ? L"▾" : L"▸",
                  rect_of(layout.header.right - 28.0f, layout.header.top,
                          layout.header.right - 8.0f, layout.header.bottom),
                  ctx.f11, ctx.muted.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);

        if (body <= 0.5f) {
            continue;
        }
        // 展开/收起过渡：只显示内容区前 body 高度，行按固定 50px 节距排列。
        const D2D1_RECT_F body_clip =
            rect_of(list_left, layout.header.bottom + 1.0f, list_right,
                    layout.header.bottom + kListBodyTop + body);
        target->PushAxisAlignedClip(body_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (std::size_t ri = 0; ri < layout.rows.size() && ri < card.versions.size(); ++ri) {
            const auto& row = layout.rows[ri];
            const auto& summary = card.versions[ri];
            if (row.row.bottom < m_list_top || row.row.top > m_list_bottom) {
                continue;
            }
            const bool row_selected = (summary.version.id == m_selected_id);
            const bool row_pressed =
                (m_pressed == Hot::kRow && m_hover_row_id == summary.version.id);
            if (row_selected || row_pressed) {
                // 行高亮用“紧凑层”圆角 6（与下载页列表行一致），并因 6px 内缩不压卡片描边。
                fill_round(ctx, row.row, 6.0f, ctx.primary_container.Get());
                stroke_round(ctx, row.row, 6.0f, ctx.primary.Get());
            }
            motion::draw_hot_overlay(ctx.target, row.row, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            const std::string row_id = summary.version.id;
            const bool star_hovered =
                (m_hover == Hot::kRowStar && m_hover_row_id == row_id);
            const bool del_hovered =
                (m_hover == Hot::kRowDelete && m_hover_row_id == row_id);
            const bool row_gear_hovered =
                (m_hover == Hot::kRowGear && m_hover_row_id == row_id);
            motion::draw_hot_overlay(ctx.target, row.star, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            motion::draw_hot_overlay(ctx.target, row.del, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            motion::draw_hot_overlay(ctx.target, row.gear, 6.0f, theme_hover_background(),
                                 theme_primary_container());
            const D2D1_RECT_F icon = rect_of(row.row.left + 8.0f, row.row.top + 7.0f,
                                             row.row.left + 40.0f, row.row.top + 39.0f);
            // D-085：按 loader 显示版本类型默认图标（原版/Fabric/Forge/NeoForge），
            // 取代原先的状态首字母占位块；颜色由 theme_version_*() 区分。
            draw_version_badge(ctx.target, icon, 5.0f, version_icon_kind(summary));
            draw_text(ctx, widen(summary.version.id),
                      rect_of(icon.right + 10.0f, row.row.top + 4.0f, row.star.left - 8.0f,
                              row.row.top + 25.0f),
                      ctx.f12, ctx.text.Get());
            draw_text(ctx, version_info_text(summary),
                      rect_of(icon.right + 10.0f, row.row.top + 25.0f, row.star.left - 8.0f,
                              row.row.top + 45.0f),
                      ctx.f10, ctx.secondary.Get());
            IconManager::instance().draw(
                ctx.target, "star",
                rect_of(row.star.left + 2.0f, row.star.top + 2.0f, row.star.right - 2.0f,
                        row.star.bottom - 2.0f),
                summary.starred ? theme_warning()
                                : (star_hovered ? theme_primary() : theme_text_muted()), 1.0f);
            IconManager::instance().draw(
                ctx.target, "delete",
                rect_of(row.del.left + 2.0f, row.del.top + 2.0f, row.del.right - 2.0f,
                        row.del.bottom - 2.0f),
                del_hovered ? theme_primary() : theme_text_secondary());
            IconManager::instance().draw(
                ctx.target, "settings",
                rect_of(row.gear.left + 2.0f, row.gear.top + 2.0f, row.gear.right - 2.0f,
                        row.gear.bottom - 2.0f),
                row_gear_hovered ? theme_primary() : theme_text_secondary());
        }
        target->PopAxisAlignedClip();
    }
    target->PopAxisAlignedClip();
}

void VersionSelectPage::draw_folder_menu(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    fill_round(ctx, m_folder_menu_rect, theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, m_folder_menu_rect, theme_radius(), ctx.border.Get());
    static const wchar_t* kItems[] = {L"重命名", L"打开", L"刷新", L"移出列表", L"删除"};
    for (int i = 0; i < 5; ++i) {
        const D2D1_RECT_F item = rect_of(m_folder_menu_rect.left + 4.0f,
                                         m_folder_menu_rect.top + 6.0f +
                                             static_cast<float>(i) * 30.0f,
                                         m_folder_menu_rect.right - 4.0f,
                                         m_folder_menu_rect.top + 6.0f +
                                             static_cast<float>(i + 1) * 30.0f);
        motion::draw_hot_overlay(ctx.target, item, 6.0f, theme_hover_background(),
                             theme_primary_container());
        const bool disabled = (i >= 3 && m_folder_menu_type != 2);
        draw_text(ctx, kItems[i], item, ctx.f12,
                  disabled ? ctx.muted.Get() : (i == 4 ? ctx.danger.Get() : ctx.text.Get()));
    }
}

void VersionSelectPage::draw_overlay(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    Ctx ctx;
    if (!make_ctx(ctx, target, dwrite)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dim;
    if (SUCCEEDED(target->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, motion::modal().dim()), dim.GetAddressOf()))) {
        target->FillRectangle(rect_of(0, 0, m_width_dip, m_height_dip), dim.Get());
    }
    const bool layered = motion::push_pop(target, motion::modal(), m_overlay_card);
    fill_round(ctx, m_overlay_card, theme_radius(), ctx.elevated.Get());
    stroke_round(ctx, m_overlay_card, theme_radius(), ctx.border.Get());
    draw_text(ctx, m_overlay_prompt,
              rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 20.0f,
                      m_overlay_card.right - 24.0f, m_overlay_card.top + 52.0f),
              ctx.f16b, ctx.text.Get());
    if (m_overlay == OverlayKind::kInput && m_input_box) {
        m_input_box->draw(target, dwrite);
    } else if (m_overlay == OverlayKind::kDelete) {
        draw_text(ctx, m_overlay_text,
                  rect_of(m_overlay_card.left + 24.0f, m_overlay_card.top + 58.0f,
                          m_overlay_card.right - 24.0f, m_overlay_card.bottom - 64.0f),
                  ctx.f12, ctx.secondary.Get(), DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    draw_button(ctx, m_overlay_cancel, L"取消", false, true, nullptr, m_overlay_hover == 1);
    draw_button(ctx, m_overlay_ok, m_overlay == OverlayKind::kDelete ? L"删除" : L"确定",
                m_overlay == OverlayKind::kDelete, true, nullptr, m_overlay_hover == 2);
    if (layered) {
        motion::pop_pop(target);
    }
}

bool VersionSelectPage::overlay_hit(float x_dip, float y_dip) const {
    return m_overlay != OverlayKind::kNone && contains(m_overlay_card, x_dip, y_dip);
}

void VersionSelectPage::layout_overlay() {
    const float width = m_overlay == OverlayKind::kInput ? 420.0f : 380.0f;
    const float height = m_overlay == OverlayKind::kDelete ? 220.0f : 180.0f;
    const float center_x = m_width_dip * 0.5f;
    const float center_y = m_height_dip * 0.5f;
    m_overlay_card = rect_of(center_x - width * 0.5f, center_y - height * 0.5f,
                             center_x + width * 0.5f, center_y + height * 0.5f);
    m_overlay_cancel = rect_of(m_overlay_card.right - 200.0f, m_overlay_card.bottom - 50.0f,
                               m_overlay_card.right - 116.0f, m_overlay_card.bottom - 18.0f);
    m_overlay_ok = rect_of(m_overlay_card.right - 104.0f, m_overlay_card.bottom - 50.0f,
                           m_overlay_card.right - 20.0f, m_overlay_card.bottom - 18.0f);
    if (m_overlay == OverlayKind::kInput && m_input_box) {
        m_input_box->bounds = rect_of(m_overlay_card.left + 20.0f, m_overlay_card.top + 70.0f,
                                      m_overlay_card.right - 20.0f, m_overlay_card.top + 106.0f);
    }
}

void VersionSelectPage::open_input(std::wstring prompt, std::wstring initial,
                                   std::function<void(const std::wstring&)> accept) {
    m_overlay = OverlayKind::kInput;
    m_overlay_prompt = std::move(prompt);
    m_overlay_text.clear();
    m_input_accept = std::move(accept);
    m_input_box = std::make_unique<TextBox>(std::move(initial), 12.0f);
    m_input_box->max_length = 64;
    m_input_box->set_focused(true);
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void VersionSelectPage::open_delete_version(const std::string& id, bool permanent) {
    m_overlay = OverlayKind::kDelete;
    m_overlay_permanent = permanent;
    m_delete_folder = false;
    m_delete_version_id = id;
    m_overlay_prompt = L"确定删除版本 " + widen(id) + L" 吗？";
    m_overlay_text = permanent
                         ? L"将永久删除（Shift）；若该版本隔离，存档/Mod 也一并删除。"
                         : L"将移动到回收站；若该版本隔离，存档/Mod 也一并删除。";
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void VersionSelectPage::open_delete_folder(const std::filesystem::path& path) {
    m_overlay = OverlayKind::kDelete;
    m_overlay_permanent = true;
    m_delete_folder = true;
    m_delete_folder_path = path;
    m_overlay_prompt = L"确定删除文件夹吗？";
    m_overlay_text = widen(sscl::util::wide_to_utf8(path.wstring())) +
                     L"\n这将永久删除磁盘上的整个文件夹，无法恢复。";
    motion::modal().open();  // 弹出：淡入 + 0.96→1 + 上移 8px（D-081）
    layout_overlay();
    if (request_repaint) {
        request_repaint();
    }
}

void VersionSelectPage::close_overlays() {
    m_overlay = OverlayKind::kNone;
    motion::modal().close();
    m_overlay_hover = 0;
    // 输入框/提示文本保留到淡出结束，下次 open 覆盖。
    m_overlay_permanent = false;
    m_delete_folder = false;
    m_delete_version_id.clear();
    m_delete_folder_path.clear();
    if (request_repaint) {
        request_repaint();
    }
}

VersionSelectPage::Hot VersionSelectPage::hit_hot(float x_dip, float y_dip,
                                                  std::string* row_id, int* folder_index,
                                                  int* card_index) const {
    if (row_id != nullptr) {
        row_id->clear();
    }
    if (folder_index != nullptr) {
        *folder_index = -1;
    }
    if (card_index != nullptr) {
        *card_index = -1;
    }
    if (m_overlay != OverlayKind::kNone || m_folder_menu_open) {
        return Hot::kNone;
    }
    if (contains(m_back_rect, x_dip, y_dip)) {
        return Hot::kBack;
    }
    if (contains(m_download_rect, x_dip, y_dip)) {
        return Hot::kDownload;
    }
    if (contains(m_add_folder_rect, x_dip, y_dip) ||
        contains(m_create_folder_rect, x_dip, y_dip)) {
        return Hot::kAddFolder;
    }
    if (contains(m_import_rect, x_dip, y_dip)) {
        return Hot::kImport;
    }
    for (std::size_t i = 0; i < m_folder_hits.size(); ++i) {
        if (contains(m_folder_hits[i].gear, x_dip, y_dip)) {
            if (folder_index != nullptr) {
                *folder_index = static_cast<int>(i);
            }
            return Hot::kFolderGear;
        }
        if (contains(m_folder_hits[i].row, x_dip, y_dip)) {
            if (folder_index != nullptr) {
                *folder_index = static_cast<int>(i);
            }
            return Hot::kFolder;
        }
    }
    for (std::size_t ci = 0; ci < m_card_layout.size(); ++ci) {
        const CardLayout& layout = m_card_layout[ci];
        if (contains(layout.header, x_dip, y_dip)) {
            if (card_index != nullptr) {
                *card_index = static_cast<int>(ci);
            }
            return Hot::kCardHeader;
        }
        // 展开/收起过渡中，内容区之外的行不可点（视觉上也已被裁剪）。
        if (y_dip > layout.body_bottom) {
            continue;
        }
        for (const RowHit& row : layout.rows) {
            if (contains(row.gear, x_dip, y_dip)) {
                if (row_id != nullptr) {
                    *row_id = row.id;
                }
                return Hot::kRowGear;
            }
            if (contains(row.star, x_dip, y_dip)) {
                if (row_id != nullptr) {
                    *row_id = row.id;
                }
                return Hot::kRowStar;
            }
            if (contains(row.del, x_dip, y_dip)) {
                if (row_id != nullptr) {
                    *row_id = row.id;
                }
                return Hot::kRowDelete;
            }
            if (contains(row.row, x_dip, y_dip)) {
                if (row_id != nullptr) {
                    *row_id = row.id;
                }
                return Hot::kRow;
            }
        }
    }
    return Hot::kNone;
}

bool VersionSelectPage::mouse_move(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_folder_menu_open) {
        int hover = -1;
        if (contains(m_folder_menu_rect, x_dip, y_dip)) {
            const float top = m_folder_menu_rect.top + 6.0f;
            const int index = static_cast<int>((y_dip - top) / 30.0f);
            if (index >= 0 && index < 5) {
                hover = index;
            }
        }
        if (hover != m_folder_menu_hover) {
            m_folder_menu_hover = hover;
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_overlay != OverlayKind::kNone) {
        bool changed = false;
        // 弹层是模态的：清掉底层页面的悬停/按下，避免视觉残留。
        if (m_hover != Hot::kNone || m_pressed != Hot::kNone) {
            m_hover = Hot::kNone;
            m_pressed = Hot::kNone;
            m_hover_row_id.clear();
            m_hover_folder = -1;
            m_hover_card = -1;
            changed = true;
        }
        int hover = 0;
        if (contains(m_overlay_cancel, x_dip, y_dip)) {
            hover = 1;
        } else if (contains(m_overlay_ok, x_dip, y_dip)) {
            hover = 2;
        }
        if (hover != m_overlay_hover) {
            m_overlay_hover = hover;
            changed = true;
        }
        if (changed && request_repaint) {
            request_repaint();
        }
        return changed;
    }
    bool changed = false;
    if (m_scroll.on_mouse_move(x_dip, y_dip)) {
        rebuild_hits();
        changed = true;
    }
    std::string row_id;
    int folder_index = -1;
    int card_index = -1;
    const Hot hot = hit_hot(x_dip, y_dip, &row_id, &folder_index, &card_index);
    if (hot != m_hover || row_id != m_hover_row_id || folder_index != m_hover_folder ||
        card_index != m_hover_card) {
        m_hover = hot;
        m_hover_row_id = std::move(row_id);
        m_hover_folder = folder_index;
        m_hover_card = card_index;
        changed = true;
    }
    if (changed && request_repaint) {
        request_repaint();
    }
    return changed;
}

bool VersionSelectPage::mouse_down(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_overlay != OverlayKind::kNone) {
        m_pressed = Hot::kNone;
        return true;
    }
    if (x_dip > kSidebarWidthDip && y_dip >= m_list_top && y_dip <= m_list_bottom &&
        m_scroll.on_mouse_down(x_dip, y_dip)) {
        m_pressed = Hot::kNone;
        return true;
    }
    std::string row_id;
    int folder_index = -1;
    int card_index = -1;
    const Hot hot = hit_hot(x_dip, y_dip, &row_id, &folder_index, &card_index);
    if (hot != Hot::kNone) {
        m_pressed = hot;
        if (request_repaint) {
            request_repaint();
        }
    }
    return true;
}

bool VersionSelectPage::mouse_up(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    if (m_pressed != Hot::kNone) {
        m_pressed = Hot::kNone;
        if (request_repaint) {
            request_repaint();
        }
    }

    if (m_overlay != OverlayKind::kNone) {
        if (!overlay_hit(x_dip, y_dip)) {
            return true;
        }
        if (contains(m_overlay_cancel, x_dip, y_dip)) {
            close_overlays();
            return true;
        }
        if (contains(m_overlay_ok, x_dip, y_dip)) {
            if (m_overlay == OverlayKind::kDelete) {
                if (m_delete_folder) {
                    if (on_delete_folder) {
                        on_delete_folder(m_delete_folder_path);
                    }
                } else if (m_service) {
                    m_service->delete_instance(m_delete_version_id, m_overlay_permanent);
                    refresh(m_selected_id);
                }
                close_overlays();
            } else if (m_overlay == OverlayKind::kInput && m_input_box) {
                const std::wstring value = m_input_box->text;
                auto accept = m_input_accept;
                close_overlays();
                if (!value.empty() && accept) {
                    accept(value);
                }
            }
        }
        return true;
    }

    if (m_folder_menu_open) {
        if (contains(m_folder_menu_rect, x_dip, y_dip)) {
            const int index = static_cast<int>(
                (y_dip - (m_folder_menu_rect.top + 6.0f)) / 30.0f);
            const std::filesystem::path path = m_folder_menu_path;
            m_folder_menu_open = false;
            if (index == 0) {
                std::wstring name = L"文件夹";
                for (const auto& folder : m_folders) {
                    if (folder.location == path) {
                        name = widen(folder.name);
                        break;
                    }
                }
                open_input(L"文件夹显示名", name, [this, path](const std::wstring& value) {
                    if (on_rename_folder) {
                        on_rename_folder(path, sscl::util::wide_to_utf8(value));
                    }
                });
            } else if (index == 1) {
                sscl::platform::open_folder(path);
            } else if (index == 2) {
                if (on_refresh_folder) {
                    on_refresh_folder(path);
                }
            } else if (index == 3) {
                if (m_folder_menu_type == 2 && on_remove_folder) {
                    on_remove_folder(path);
                }
            } else if (index == 4) {
                if (m_folder_menu_type == 2) {
                    open_delete_folder(path);
                }
            }
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        m_folder_menu_open = false;
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }

    if (m_scroll.on_mouse_up(x_dip, y_dip)) {
        rebuild_hits();
        return true;
    }

    if (contains(m_back_rect, x_dip, y_dip)) {
        if (on_back) {
            on_back();
        }
        return true;
    }
    if (contains(m_download_rect, x_dip, y_dip)) {
        if (m_show_hidden) {
            m_show_hidden = false;
            rebuild_cards();
            rebuild_hits();
        } else if (on_open_downloads) {
            on_open_downloads();
        }
        return true;
    }
    if (contains(m_add_folder_rect, x_dip, y_dip)) {
        std::wstring selected;
        if (m_window != nullptr &&
            sscl::platform::select_folder_dialog(m_window->hwnd(), L"选择 .minecraft 文件夹",
                                                 &selected) &&
            !selected.empty()) {
            const std::filesystem::path path(selected);
            std::wstring default_name = path.filename().wstring();
            if (default_name == L".minecraft") {
                default_name = path.parent_path().filename().wstring();
            }
            if (default_name.empty()) {
                default_name = L"文件夹";
            }
            open_input(L"文件夹显示名", default_name, [this, path](const std::wstring& value) {
                if (on_add_folder) {
                    on_add_folder(path, sscl::util::wide_to_utf8(value));
                }
            });
        }
        return true;
    }
    if (contains(m_import_rect, x_dip, y_dip)) {
        start_import_modpack();
        return true;
    }

    for (std::size_t i = 0; i < m_folder_hits.size() && i < m_folders.size(); ++i) {
        const auto& hit = m_folder_hits[i];
        if (contains(hit.gear, x_dip, y_dip)) {
            m_folder_menu_open = true;
            m_folder_menu_path = hit.path;
            m_folder_menu_type = static_cast<int>(m_folders[i].type);
            m_folder_menu_hover = -1;
            m_folder_menu_rect = rect_of(kSidebarWidthDip - 180.0f, hit.row.top,
                                         kSidebarWidthDip - 12.0f, hit.row.top + 162.0f);
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        if (contains(hit.row, x_dip, y_dip)) {
            if (!m_folders[i].selected && on_switch_folder) {
                motion::content().jump();  // D-108：页内切换瞬时；全局 content 已被整页转场占用
                on_switch_folder(hit.path);
            }
            return true;
        }
    }

    for (const auto& header : m_card_hits) {
        if (contains(header.header, x_dip, y_dip)) {
            m_expanded[header.category] = !m_expanded[header.category];
            rebuild_hits();
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
    }

    for (const auto& layout : m_card_layout) {
        for (const auto& row : layout.rows) {
            if (!contains(row.row, x_dip, y_dip)) {
                continue;
            }
            if (contains(row.star, x_dip, y_dip)) {
                if (m_service) {
                    bool starred = false;
                    for (const auto& card : m_cards) {
                        for (const auto& item : card.versions) {
                            if (item.version.id == row.id) {
                                starred = item.starred;
                                break;
                            }
                        }
                    }
                    m_service->set_instance_star(row.id, !starred);
                    refresh(m_selected_id);
                }
                return true;
            }
            if (contains(row.del, x_dip, y_dip)) {
                const bool permanent = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
                open_delete_version(row.id, permanent);
                return true;
            }
            if (contains(row.gear, x_dip, y_dip)) {
                if (on_open_instance) {
                    on_open_instance(row.id);
                }
                return true;
            }
            if (on_select) {
                on_select(row.id);
            }
            return true;
        }
    }
    return false;
}

bool VersionSelectPage::mouse_right_down(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (m_overlay != OverlayKind::kNone) {
        close_overlays();
        return true;
    }
    if (m_folder_menu_open) {
        m_folder_menu_open = false;
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    for (const auto& layout : m_card_layout) {
        for (const auto& row : layout.rows) {
            if (contains(row.row, x_dip, y_dip)) {
                if (on_open_instance) {
                    on_open_instance(row.id);
                }
                return true;
            }
        }
    }
    return false;
}

bool VersionSelectPage::mouse_leave() {
    bool changed = m_scroll.on_mouse_leave();
    if (m_hover != Hot::kNone || m_pressed != Hot::kNone) {
        m_hover = Hot::kNone;
        m_pressed = Hot::kNone;
        m_hover_row_id.clear();
        m_hover_folder = -1;
        m_hover_card = -1;
        changed = true;
    }
    if (m_folder_menu_open) {
        m_folder_menu_open = false;
        changed = true;
    }
    return changed;
}

bool VersionSelectPage::mouse_wheel(float x, float y, float delta) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;
    if (x_dip <= kSidebarWidthDip || y_dip < m_list_top || y_dip > m_list_bottom) {
        return false;
    }
    if (m_scroll.on_wheel(delta, 120.0f)) {
        rebuild_hits();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool VersionSelectPage::handle_char(wchar_t ch) {
    if (m_overlay != OverlayKind::kInput || !m_input_box) {
        return false;
    }
    if (m_input_box->handle_char(ch)) {
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    return false;
}

bool VersionSelectPage::handle_key_down(unsigned long key) {
    constexpr unsigned long kEscape = 0x1B;
    constexpr unsigned long kReturn = 0x0D;
    constexpr unsigned long kBack = 0x08;
    constexpr unsigned long kF11 = 0x7A;
    if (m_overlay == OverlayKind::kInput && m_input_box) {
        if (key == kEscape) {
            close_overlays();
            return true;
        }
        if (key == kReturn) {
            const std::wstring value = m_input_box->text;
            auto accept = m_input_accept;
            close_overlays();
            if (!value.empty() && accept) {
                accept(value);
            }
            return true;
        }
        if (key == kBack) {
            if (m_input_box->handle_key_down(key) && request_repaint) {
                request_repaint();
            }
            return true;
        }
        // D-120：退格之外的编辑键（Ctrl+V/C/X）也必须到输入框，
        // 否则弹层输入框无法粘贴（此前只转发退格，其余按键在这里被吞掉）。
        if (m_input_box->handle_key_down(key)) {
            if (request_repaint) {
                request_repaint();
            }
            return true;
        }
        return false;
    }
    if (m_overlay != OverlayKind::kNone) {
        if (key == kEscape) {
            close_overlays();
            return true;
        }
        return false;
    }
    if (key == kF11) {
        m_show_hidden = !m_show_hidden;
        rebuild_cards();
        rebuild_hits();
        if (request_repaint) {
            request_repaint();
        }
        return true;
    }
    if (key == kEscape) {
        if (m_folder_menu_open) {
            m_folder_menu_open = false;
        } else if (on_back) {
            on_back();
        }
        return true;
    }
    return false;
}

bool VersionSelectPage::is_busy() const {
    return m_importing;
}

std::wstring VersionSelectPage::busy_text() const {
    return L"正在导入整合包…";
}

bool VersionSelectPage::tick(float dt_ms) {
    bool active = false;
    if (m_scroll.tick(dt_ms)) {
        active = true;
    }
    // D-093：分类卡片展开/收起做高度过渡，避免整块内容瞬移（PCL 手风琴手感）。
    for (const auto& card : m_cards) {
        const int key = static_cast<int>(card.category);
        auto anim = m_expand_anim.find(key);
        if (anim == m_expand_anim.end()) {
            m_expand_anim[key] = m_expanded[key] ? 1.0f : 0.0f;
            continue;
        }
        const float target = m_expanded[key] ? 1.0f : 0.0f;
        if (motion::tween(anim->second, target, dt_ms, motion::Kind::kFade)) {
            active = true;
        }
    }
    if (active) {
        rebuild_hits();
        if (request_repaint) {
            request_repaint();
        }
    }
    return active;
}

}  // namespace sscl::ui
