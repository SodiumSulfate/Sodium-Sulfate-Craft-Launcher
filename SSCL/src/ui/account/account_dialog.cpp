#include "ui/account/account_dialog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <random>
#include <thread>
#include <utility>

#include <d2d1helper.h>
#include <wrl/client.h>

#include "core/account/account.hpp"
#include "core/account/account_skin.hpp"
#include "core/account/authlib_auth.hpp"
#include "core/account/microsoft_auth.hpp"
#include "core/config/build_config.hpp"
#include "platform/oauth_loopback_server.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/window.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

using sscl::core::AccountInfo;
using sscl::core::AccountType;

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

bool rect_valid(const D2D1_RECT_F& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

std::wstring widen(const std::string& text) {
    return sscl::util::utf8_to_wide(text);
}

std::string narrow(const std::wstring& text) {
    return sscl::util::wide_to_utf8(text);
}

std::wstring account_type_display(AccountType type) {
    switch (type) {
        case AccountType::kMicrosoft:
            return L"正版账号";
        case AccountType::kAuthlibInjector:
            return L"外置登录";
        case AccountType::kNide8:
            return L"统一通行证";
        default:
            return L"离线账号";
    }
}

std::wstring initial_of(const std::wstring& name) {
    for (const wchar_t ch : name) {
        if (ch != L' ' && ch != L'\t') {
            return std::wstring(1, ch);
        }
    }
    return L"?";
}

void draw_text(ID2D1RenderTarget* target, IDWriteFactory* dwrite, const std::wstring& text,
               const D2D1_RECT_F& rect, float size, DWRITE_FONT_WEIGHT weight,
               const D2D1_COLOR_F& color,
               DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
               bool wrap = false) {
    if (target == nullptr || dwrite == nullptr || text.empty() || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), rect,
                      brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void fill_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                const D2D1_COLOR_F& color) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
    }
}

void stroke_round(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, float radius,
                  const D2D1_COLOR_F& color, float width = 1.0f) {
    if (target == nullptr || !rect_valid(rect)) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(target->CreateSolidColorBrush(color, brush.GetAddressOf()))) {
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
    }
}

constexpr float kRowHeight = 56.0f;

}  // namespace

AccountDialog::AccountDialog(AppWindow* host, std::shared_ptr<sscl::core::AccountManager> accounts)
    : m_host(host), m_accounts(std::move(accounts)) {
    build_controls();
    refresh_from_store();
}

void AccountDialog::build_controls() {
    auto title = std::make_unique<Label>(L"账号管理", 18.0f, theme_text_primary(), true);
    auto subtitle = std::make_unique<Label>(L"", 11.5f, theme_text_secondary());
    auto close = std::make_unique<Button>(L"", 13.0f, false);

    auto list_title = std::make_unique<Label>(L"", 12.0f, theme_text_secondary(), true);
    auto add_offline = std::make_unique<Button>(L"离线", 12.0f, false);
    auto add_ms = std::make_unique<Button>(L"正版", 12.0f, false);
    auto add_ext = std::make_unique<Button>(L"第三方", 12.0f, false);
    auto list_hint = std::make_unique<Label>(L"", 10.5f, theme_text_muted());

    auto pane = std::make_unique<Card>(L"");
    pane->elevated = true;

    auto detail_name = std::make_unique<Label>(L"", 20.0f, theme_text_primary(), true);
    auto detail_type = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto detail_uuid = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    auto detail_server = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    auto detail_hint = std::make_unique<Label>(L"", 11.5f, theme_text_muted());
    auto detail_use = std::make_unique<Button>(L"设为启动账号", 13.0f, true);
    auto detail_relogin = std::make_unique<Button>(L"重新登录", 12.5f, false);
    auto detail_delete = std::make_unique<Button>(L"删除账号", 12.5f, false);

    auto offline_title = std::make_unique<Label>(L"添加离线账号", 17.0f, theme_text_primary(), true);
    auto offline_hint = std::make_unique<Label>(
        L"离线账号只在本机生成 UUID，不联网验证。适合单机 / 局域网联机。", 11.5f,
        theme_text_muted());
    auto offline_name = std::make_unique<TextBox>(L"", 13.0f);
    auto offline_create = std::make_unique<Button>(L"创建并使用", 13.0f, true);
    auto offline_back = std::make_unique<Button>(L"返回", 12.5f, false);

    auto ms_title = std::make_unique<Label>(L"添加正版账号", 17.0f, theme_text_primary(), true);
    auto ms_hint = std::make_unique<Label>(
        L"使用 Microsoft 账号登录；令牌保存在本机（DPAPI 加密），可随时切换。", 11.5f,
        theme_text_muted());
    auto ms_device = std::make_unique<Button>(L"设备码登录", 13.0f, true);
    auto ms_browser = std::make_unique<Button>(L"浏览器登录", 13.0f, false);
    auto ms_code = std::make_unique<Label>(L"", 26.0f, theme_primary(), true);
    auto ms_status = std::make_unique<Label>(L"", 11.5f, theme_text_secondary());
    auto ms_hint2 = std::make_unique<Label>(L"", 11.0f, theme_text_muted());
    auto ms_copy = std::make_unique<Button>(L"复制代码", 12.0f, true);
    auto ms_open = std::make_unique<Button>(L"打开登录页", 12.0f, false);
    auto ms_cancel = std::make_unique<Button>(L"取消", 12.0f, false);

    auto ext_title =
        std::make_unique<Label>(L"添加第三方账号", 17.0f, theme_text_primary(), true);
    auto ext_hint = std::make_unique<Label>(
        L"外置登录（authlib-injector）· 填写服务器的 API 根地址，例如 "
        L"https://littleskin.cn/api/yggdrasil",
        11.5f, theme_text_muted());
    auto ext_server = std::make_unique<TextBox>(L"", 13.0f);
    auto ext_user = std::make_unique<TextBox>(L"", 13.0f);
    auto ext_password = std::make_unique<TextBox>(L"", 13.0f);
    auto ext_login = std::make_unique<Button>(L"登录并使用", 13.0f, true);
    auto ext_back = std::make_unique<Button>(L"返回", 12.5f, false);

    m_title = title.get();
    m_subtitle = subtitle.get();
    m_close = close.get();
    m_list_title = list_title.get();
    m_add_offline = add_offline.get();
    m_add_microsoft = add_ms.get();
    m_add_external = add_ext.get();
    m_list_hint = list_hint.get();
    m_pane_card = pane.get();
    m_detail_name = detail_name.get();
    m_detail_type = detail_type.get();
    m_detail_uuid = detail_uuid.get();
    m_detail_server = detail_server.get();
    m_detail_hint = detail_hint.get();
    m_detail_use = detail_use.get();
    m_detail_relogin = detail_relogin.get();
    m_detail_delete = detail_delete.get();
    m_offline_title = offline_title.get();
    m_offline_hint = offline_hint.get();
    m_offline_name = offline_name.get();
    m_offline_create = offline_create.get();
    m_offline_back = offline_back.get();
    m_ms_title = ms_title.get();
    m_ms_hint = ms_hint.get();
    m_ms_device = ms_device.get();
    m_ms_browser = ms_browser.get();
    m_ms_code = ms_code.get();
    m_ms_status = ms_status.get();
    m_ms_hint2 = ms_hint2.get();
    m_ms_copy = ms_copy.get();
    m_ms_open = ms_open.get();
    m_ms_cancel = ms_cancel.get();
    m_ext_title = ext_title.get();
    m_ext_hint = ext_hint.get();
    m_ext_server = ext_server.get();
    m_ext_user = ext_user.get();
    m_ext_password = ext_password.get();
    m_ext_login = ext_login.get();
    m_ext_back = ext_back.get();

    m_close->icon = "close";
    m_add_offline->icon = "person_add";
    m_add_microsoft->icon = "login";
    m_add_external->icon = "public";
    m_detail_use->icon = "check";
    m_detail_relogin->icon = "refresh";
    m_detail_delete->icon = "delete";
    m_offline_create->icon = "person_add";
    m_offline_back->icon = "arrow_back";
    m_ms_device->icon = "login";
    m_ms_browser->icon = "open_in_new";
    m_ms_copy->icon = "content_copy";
    m_ms_open->icon = "open_in_new";
    m_ms_cancel->icon = "close";
    m_ext_login->icon = "login";
    m_ext_back->icon = "arrow_back";

    m_offline_name->placeholder = L"玩家名（3~16 位，可用中文）";
    m_offline_name->max_length = 16;
    m_ext_server->placeholder = L"认证服务器 API 根地址";
    m_ext_server->max_length = 200;
    m_ext_user->placeholder = L"邮箱 / 账号";
    m_ext_user->max_length = 64;
    m_ext_password->placeholder = L"密码";
    m_ext_password->max_length = 64;
    m_ext_password->password = true;

    m_add_offline->on_click = [this]() { set_pane(Pane::kOffline); };
    m_add_microsoft->on_click = [this]() { set_pane(Pane::kMicrosoft); };
    m_add_external->on_click = [this]() { set_pane(Pane::kExternal); };
    m_close->on_click = [this]() {
        if (on_close) {
            on_close();
        }
    };

    m_detail_use->on_click = [this]() {
        const auto& list = m_accounts->accounts();
        if (m_selected_row < 0 || static_cast<std::size_t>(m_selected_row) >= list.size()) {
            return;
        }
        const std::string uuid = list[static_cast<std::size_t>(m_selected_row)].uuid_compact;
        m_accounts->select_uuid(uuid);
        refresh_list();
        refresh_detail();
        notify_selected();
        set_status(L"已切换启动账号：" + widen(list[static_cast<std::size_t>(m_selected_row)].username));
    };
    m_detail_delete->on_click = [this]() {
        const auto& list = m_accounts->accounts();
        if (m_selected_row < 0 || static_cast<std::size_t>(m_selected_row) >= list.size()) {
            return;
        }
        const std::string uuid = list[static_cast<std::size_t>(m_selected_row)].uuid_compact;
        const std::wstring name = widen(list[static_cast<std::size_t>(m_selected_row)].username);
        (void)m_accounts->remove(uuid);
        refresh_from_store();
        notify_selected();
        set_status(L"已删除账号：" + name);
    };
    m_detail_relogin->on_click = [this]() {
        const auto& list = m_accounts->accounts();
        if (m_selected_row < 0 || static_cast<std::size_t>(m_selected_row) >= list.size()) {
            return;
        }
        const AccountInfo& account = list[static_cast<std::size_t>(m_selected_row)];
        if (account.type == AccountType::kMicrosoft) {
            set_pane(Pane::kMicrosoft);
            set_status(L"请重新完成微软登录；成功后会用新令牌覆盖该账号。");
        } else if (account.type == AccountType::kAuthlibInjector ||
                   account.type == AccountType::kNide8) {
            m_ext_server->text = widen(account.server_url);
            m_ext_user->text = widen(account.username);
            m_ext_password->text.clear();
            set_pane(Pane::kExternal);
            set_status(L"请重新输入密码；成功后会用新令牌覆盖该账号。");
        } else {
            set_status(L"离线账号无需重新登录。");
        }
    };

    m_offline_create->on_click = [this]() { submit_offline(); };
    m_offline_back->on_click = [this]() { set_pane(Pane::kDetail); };

    m_ms_device->on_click = [this]() { start_ms_device_login(); };
    m_ms_browser->on_click = [this]() { start_ms_browser_login(); };
    m_ms_copy->on_click = [this]() {
        if (!m_device_user_code.empty()) {
            sscl::platform::set_clipboard_text(m_device_user_code);
            set_status(L"登录代码已复制");
        }
    };
    m_ms_open->on_click = [this]() {
        const std::string url = m_browser_auth_url.empty() ? m_device_verify_url : m_browser_auth_url;
        if (!url.empty()) {
            sscl::platform::open_url(url);
        }
    };
    m_ms_cancel->on_click = [this]() {
        ++m_login_seq;  // 作废在途回调
        m_ms_busy = false;
        m_ms_code->text.clear();
        m_ms_status->text.clear();
        m_ms_hint2->text.clear();
        set_pane(Pane::kDetail);
    };

    m_ext_login->on_click = [this]() { submit_external(); };
    m_ext_back->on_click = [this]() { set_pane(Pane::kDetail); };

    m_root.add(std::move(title));
    m_root.add(std::move(subtitle));
    m_root.add(std::move(close));
    m_root.add(std::move(list_title));
    m_root.add(std::move(add_offline));
    m_root.add(std::move(add_ms));
    m_root.add(std::move(add_ext));
    m_root.add(std::move(list_hint));
    m_root.add(std::move(pane));
    // 右侧内容加入 Card（Card 是 Panel，坐标同为绝对 DIP）。
    m_pane_card->add(std::move(detail_name));
    m_pane_card->add(std::move(detail_type));
    m_pane_card->add(std::move(detail_uuid));
    m_pane_card->add(std::move(detail_server));
    m_pane_card->add(std::move(detail_hint));
    m_pane_card->add(std::move(detail_use));
    m_pane_card->add(std::move(detail_relogin));
    m_pane_card->add(std::move(detail_delete));
    m_pane_card->add(std::move(offline_title));
    m_pane_card->add(std::move(offline_hint));
    m_pane_card->add(std::move(offline_name));
    m_pane_card->add(std::move(offline_create));
    m_pane_card->add(std::move(offline_back));
    m_pane_card->add(std::move(ms_title));
    m_pane_card->add(std::move(ms_hint));
    m_pane_card->add(std::move(ms_device));
    m_pane_card->add(std::move(ms_browser));
    m_pane_card->add(std::move(ms_code));
    m_pane_card->add(std::move(ms_status));
    m_pane_card->add(std::move(ms_hint2));
    m_pane_card->add(std::move(ms_copy));
    m_pane_card->add(std::move(ms_open));
    m_pane_card->add(std::move(ms_cancel));
    m_pane_card->add(std::move(ext_title));
    m_pane_card->add(std::move(ext_hint));
    m_pane_card->add(std::move(ext_server));
    m_pane_card->add(std::move(ext_user));
    m_pane_card->add(std::move(ext_password));
    m_pane_card->add(std::move(ext_login));
    m_pane_card->add(std::move(ext_back));

    apply_visibility();
}

void AccountDialog::layout(float width_dip, float height_dip, float dpi) {
    m_to_dip = dpi <= 0.0f ? 1.0f : 96.0f / dpi;
    m_width_dip = width_dip;
    m_height_dip = height_dip;
    layout_controls();
}

void AccountDialog::layout_controls() {
    const float w = m_width_dip;
    const float h = m_height_dip;
    const float margin = 20.0f;

    m_title->bounds = rect_of(margin + 4.0f, 14.0f, margin + 260.0f, 40.0f);
    m_subtitle->bounds = rect_of(margin + 4.0f, 40.0f, margin + 420.0f, 58.0f);
    m_close->bounds = rect_of(w - 52.0f, 16.0f, w - 20.0f, 48.0f);

    const float panel_left = margin;
    const float panel_right = margin + 310.0f;
    const float panel_top = 76.0f;
    const float panel_bottom = h - 72.0f;

    m_list_title->bounds =
        rect_of(panel_left + 14.0f, panel_top + 10.0f, panel_right - 14.0f, panel_top + 34.0f);
    m_list_rect = rect_of(panel_left + 8.0f, panel_top + 38.0f, panel_right - 8.0f,
                          panel_bottom - 62.0f);

    const float add_top = panel_bottom - 52.0f;
    const float gap = 6.0f;
    const float button_w = (panel_right - panel_left - 16.0f - gap * 2.0f) / 3.0f;
    float x = panel_left + 8.0f;
    m_add_offline->bounds = rect_of(x, add_top, x + button_w, add_top + 40.0f);
    x += button_w + gap;
    m_add_microsoft->bounds = rect_of(x, add_top, x + button_w, add_top + 40.0f);
    x += button_w + gap;
    m_add_external->bounds = rect_of(x, add_top, x + button_w, add_top + 40.0f);
    m_list_hint->bounds = rect_of(panel_left + 14.0f, panel_bottom + 2.0f, panel_right - 14.0f,
                                  panel_bottom + 20.0f);

    const float card_left = panel_right + 16.0f;
    const float card_right = w - margin;
    const float card_top = panel_top;
    const float card_bottom = panel_bottom;
    m_pane_card->bounds = rect_of(card_left, card_top, card_right, card_bottom);

    const float left = card_left + 26.0f;
    const float top = card_top + 24.0f;
    const float right = card_right - 26.0f;
    const float bottom = card_bottom - 24.0f;

    layout_pane_detail(left, top, right, bottom);
    layout_pane_offline(left, top, right, bottom);
    layout_pane_microsoft(left, top, right, bottom);
    layout_pane_external(left, top, right, bottom);
    clamp_scroll();
}

void AccountDialog::layout_pane_detail(float left, float top, float right, float bottom) {
    m_detail_name->bounds = rect_of(left, top, right, top + 34.0f);
    m_detail_type->bounds = rect_of(left, top + 36.0f, right, top + 58.0f);
    m_detail_uuid->bounds = rect_of(left, top + 84.0f, right, top + 106.0f);
    m_detail_server->bounds = rect_of(left, top + 106.0f, right, top + 128.0f);
    m_detail_hint->bounds = rect_of(left, bottom - 108.0f, right, bottom - 62.0f);
    const float button_y = bottom - 46.0f;
    m_detail_use->bounds = rect_of(left, button_y, left + 150.0f, button_y + 40.0f);
    m_detail_relogin->bounds =
        rect_of(left + 160.0f, button_y, left + 160.0f + 118.0f, button_y + 40.0f);
    m_detail_delete->bounds =
        rect_of(right - 108.0f, button_y, right, button_y + 40.0f);
    (void)top;
}

void AccountDialog::layout_pane_offline(float left, float top, float right, float bottom) {
    m_offline_title->bounds = rect_of(left, top, right, top + 30.0f);
    m_offline_hint->bounds = rect_of(left, top + 38.0f, right, top + 62.0f);
    m_offline_name->bounds = rect_of(left, top + 84.0f, right - 140.0f, top + 124.0f);
    m_offline_create->bounds = rect_of(left, top + 140.0f, left + 150.0f, top + 180.0f);
    m_offline_back->bounds = rect_of(left + 160.0f, top + 140.0f, left + 160.0f + 96.0f,
                                     top + 180.0f);
    (void)bottom;
}

void AccountDialog::layout_pane_microsoft(float left, float top, float right, float bottom) {
    m_ms_title->bounds = rect_of(left, top, right, top + 30.0f);
    m_ms_hint->bounds = rect_of(left, top + 38.0f, right, top + 62.0f);
    m_ms_device->bounds = rect_of(left, top + 76.0f, left + 132.0f, top + 114.0f);
    m_ms_browser->bounds = rect_of(left + 142.0f, top + 76.0f, left + 142.0f + 132.0f,
                                   top + 114.0f);
    m_ms_code->bounds = rect_of(left, top + 130.0f, right, top + 176.0f);
    m_ms_status->bounds = rect_of(left, top + 182.0f, right, top + 204.0f);
    m_ms_hint2->bounds = rect_of(left, top + 206.0f, right, top + 246.0f);
    const float button_y = bottom - 46.0f;
    m_ms_copy->bounds = rect_of(left, button_y, left + 104.0f, button_y + 38.0f);
    m_ms_open->bounds = rect_of(left + 112.0f, button_y, left + 112.0f + 118.0f,
                                button_y + 38.0f);
    m_ms_cancel->bounds = rect_of(right - 96.0f, button_y, right, button_y + 38.0f);
}

void AccountDialog::layout_pane_external(float left, float top, float right, float bottom) {
    m_ext_title->bounds = rect_of(left, top, right, top + 30.0f);
    m_ext_hint->bounds = rect_of(left, top + 38.0f, right, top + 74.0f);
    m_ext_server->bounds = rect_of(left, top + 86.0f, right, top + 126.0f);
    m_ext_user->bounds = rect_of(left, top + 136.0f, right, top + 176.0f);
    m_ext_password->bounds = rect_of(left, top + 186.0f, right, top + 226.0f);
    m_ext_login->bounds = rect_of(left, top + 242.0f, left + 150.0f, top + 282.0f);
    m_ext_back->bounds = rect_of(left + 160.0f, top + 242.0f, left + 160.0f + 96.0f,
                                 top + 282.0f);
    (void)bottom;
}

void AccountDialog::apply_visibility() {
    const bool detail = m_pane == Pane::kDetail;
    const bool offline = m_pane == Pane::kOffline;
    const bool ms = m_pane == Pane::kMicrosoft;
    const bool external = m_pane == Pane::kExternal;

    m_detail_name->visible = detail;
    m_detail_type->visible = detail;
    m_detail_uuid->visible = detail;
    m_detail_server->visible = detail;
    m_detail_hint->visible = detail;
    m_detail_use->visible = detail;
    m_detail_relogin->visible = detail && m_detail_relogin_available;
    m_detail_delete->visible = detail;

    m_offline_title->visible = offline;
    m_offline_hint->visible = offline;
    m_offline_name->visible = offline;
    m_offline_create->visible = offline;
    m_offline_back->visible = offline;

    m_ms_title->visible = ms;
    m_ms_hint->visible = ms;
    m_ms_device->visible = ms && !m_ms_busy;
    m_ms_browser->visible = ms && !m_ms_busy;
    m_ms_code->visible = ms && !m_ms_code->text.empty();
    m_ms_status->visible = ms && !m_ms_status->text.empty();
    m_ms_hint2->visible = ms && !m_ms_hint2->text.empty();
    m_ms_copy->visible = ms && !m_device_user_code.empty();
    m_ms_open->visible = ms && (!m_device_verify_url.empty() || !m_browser_auth_url.empty());
    m_ms_cancel->visible = ms;
    m_ms_cancel->text = m_ms_busy ? L"取消登录" : L"返回";

    m_ext_title->visible = external;
    m_ext_hint->visible = external;
    m_ext_server->visible = external;
    m_ext_user->visible = external;
    m_ext_password->visible = external;
    m_ext_login->visible = external;
    m_ext_back->visible = external;
    m_ext_login->enabled = !m_ext_busy;
    m_ext_back->enabled = !m_ext_busy;

    set_field(m_field);
}

void AccountDialog::set_pane(Pane pane) {
    if (m_pane == pane) {
        return;
    }
    if (m_pane == Pane::kMicrosoft && pane != Pane::kMicrosoft) {
        ++m_login_seq;
        m_ms_busy = false;
    }
    m_pane = pane;
    if (pane != Pane::kMicrosoft) {
        m_ms_code->text.clear();
        m_ms_status->text.clear();
        m_ms_hint2->text.clear();
        m_device_user_code.clear();
        m_device_verify_url.clear();
        m_browser_auth_url.clear();
    }
    if (pane == Pane::kOffline) {
        set_field(Field::kOfflineName);
    } else if (pane == Pane::kExternal) {
        set_field(Field::kExtServer);
    } else {
        set_field(Field::kNone);
    }
    apply_visibility();
    if (m_host != nullptr) {
        m_host->invalidate();
    }
}

void AccountDialog::set_field(Field field) {
    m_field = field;
    m_offline_name->set_focused(field == Field::kOfflineName);
    m_ext_server->set_focused(field == Field::kExtServer);
    m_ext_user->set_focused(field == Field::kExtUser);
    m_ext_password->set_focused(field == Field::kExtPassword);
}

void AccountDialog::refresh_from_store() {
    // 每次打开都回到详情页，避免上次停在半截表单。
    m_pane = Pane::kDetail;
    m_field = Field::kNone;
    m_ms_busy = false;
    m_ext_busy = false;
    ++m_login_seq;
    m_ms_code->text.clear();
    m_ms_status->text.clear();
    m_ms_hint2->text.clear();
    m_device_user_code.clear();
    m_device_verify_url.clear();
    m_browser_auth_url.clear();
    refresh_list();
    const auto& list = m_accounts->accounts();
    m_selected_row = list.empty() ? -1 : 0;
    // 默认定位到「当前启动账号」，用户打开窗口即可看到正在用的那个。
    const std::optional<AccountInfo> current = m_accounts->selected();
    if (current.has_value()) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].uuid_compact == current->uuid_compact) {
                m_selected_row = static_cast<int>(i);
                break;
            }
        }
    }
    refresh_detail();
    load_account_skins();  // D-121：刷新列表头像（缓存命中不发请求）
}

// D-121：外置登录账号的皮肤来自 Yggdrasil sessionserver，微软账号来自 profile_json。
// 全部在后台线程串行拉取，逐个 post 回 UI；构造期 weak_this 尚不可用时直接跳过
// （AccountWindow::open 每次都会再调一次 refresh_from_store，那时已可用）。
void AccountDialog::load_account_skins() {
    std::shared_ptr<AccountDialog> self = weak_from_this().lock();
    if (self == nullptr || !m_accounts) {
        return;
    }
    const std::vector<AccountInfo> snapshot = m_accounts->accounts();
    const std::uint64_t seq = ++m_skin_seq;

    // 清理已删除账号留下的头像，避免 uuid 复用时显示旧皮肤。
    for (auto it = m_row_avatars.begin(); it != m_row_avatars.end();) {
        const bool still_exists =
            std::any_of(snapshot.begin(), snapshot.end(), [&it](const AccountInfo& item) {
                return item.uuid_compact == it->first;
            });
        it = still_exists ? std::next(it) : m_row_avatars.erase(it);
    }

    std::thread([self, snapshot = std::move(snapshot), seq]() {
        for (const AccountInfo& account : snapshot) {
            if (account.uuid_compact.empty()) {
                continue;
            }
            sscl::util::Result<std::vector<std::uint8_t>> png =
                sscl::core::fetch_account_skin_png(account);
            if (png.is_error() || png.value().empty()) {
                continue;
            }
            self->post([self, uuid = account.uuid_compact,
                        bytes = std::move(png.value()), seq]() mutable {
                if (seq != self->m_skin_seq) {
                    return;
                }
                auto avatar = std::make_unique<SkinAvatar>();
                avatar->set_png(std::move(bytes));
                self->m_row_avatars[uuid] = std::move(avatar);
                if (self->m_host != nullptr) {
                    // SkinAvatar 有淡入动画，需要帧循环推进（否则停在 fade=0 不显示）。
                    self->m_host->start_frame_loop();
                    self->m_host->invalidate();
                }
            });
        }
    }).detach();
}

void AccountDialog::refresh_list() {
    const auto& list = m_accounts->accounts();
    m_list_title->text = L"本机账号（" + std::to_wstring(list.size()) + L"）";
    m_subtitle->text = L"共 " + std::to_wstring(list.size()) +
                       L" 个账号 · 启动时使用「当前」账号；账号仅保存在这台电脑上";
    m_list_hint->text = list.empty() ? L"还没有账号，先用下方按钮添加一个。" : L"";
    if (m_selected_row >= static_cast<int>(list.size())) {
        m_selected_row = list.empty() ? -1 : static_cast<int>(list.size()) - 1;
    }
    clamp_scroll();
}

void AccountDialog::refresh_detail() {
    const auto& list = m_accounts->accounts();
    const bool has = m_selected_row >= 0 && static_cast<std::size_t>(m_selected_row) < list.size();
    m_detail_use->enabled = has;
    m_detail_delete->enabled = has;
    m_detail_relogin_available = false;
    if (!has) {
        m_detail_name->text = L"未选择账号";
        m_detail_type->text = L"从左侧列表选择一个账号，或添加新账号";
        m_detail_uuid->text.clear();
        m_detail_server->text.clear();
        m_detail_hint->text.clear();
        apply_visibility();
        return;
    }
    const AccountInfo& account = list[static_cast<std::size_t>(m_selected_row)];
    const std::optional<AccountInfo> current = m_accounts->selected();
    const bool is_current = current.has_value() &&
                            current->uuid_compact == account.uuid_compact;
    m_detail_name->text = widen(account.username);
    m_detail_type->text = account_type_display(account.type);
    m_detail_uuid->text = L"UUID：" + widen(account.uuid_compact);
    m_detail_server->text = account.server_url.empty()
                                ? std::wstring()
                                : L"服务器：" + widen(account.server_url);
    m_detail_hint->text =
        is_current ? L"这是当前启动账号。" : L"点「设为启动账号」后，启动游戏将使用该账号。";
    m_detail_relogin_available = account.type == AccountType::kMicrosoft ||
                                 account.type == AccountType::kAuthlibInjector ||
                                 account.type == AccountType::kNide8;
    apply_visibility();
}

void AccountDialog::clamp_scroll() {
    const float list_height = m_list_rect.bottom - m_list_rect.top;
    const float content = static_cast<float>(m_accounts->accounts().size()) * m_row_height;
    const float max_scroll = (std::max)(0.0f, content - list_height);
    m_scroll_target = (std::max)(0.0f, (std::min)(m_scroll_target, max_scroll));
    m_scroll = (std::max)(0.0f, (std::min)(m_scroll, max_scroll));
}

int AccountDialog::row_hit(float x, float y) const {
    if (x < m_list_rect.left || x >= m_list_rect.right || y < m_list_rect.top ||
        y >= m_list_rect.bottom) {
        return -1;
    }
    const float local = y - m_list_rect.top + m_scroll;
    if (local < 0.0f) {
        return -1;
    }
    const int index = static_cast<int>(local / m_row_height);
    if (index < 0 || index >= static_cast<int>(m_accounts->accounts().size())) {
        return -1;
    }
    return index;
}

void AccountDialog::select_index(int index) {
    if (index < 0 || index >= static_cast<int>(m_accounts->accounts().size())) {
        return;
    }
    m_selected_row = index;
    refresh_detail();
}

void AccountDialog::activate_row(int index) {
    select_index(index);
    if (m_host != nullptr) {
        m_host->invalidate();
    }
}

void AccountDialog::submit_offline() {
    const std::string name = narrow(m_offline_name->text);
    if (name.empty()) {
        set_status(L"请输入玩家名", true);
        return;
    }
    sscl::core::OfflineAccount provider;
    sscl::util::Result<AccountInfo> account = provider.login(name);
    if (account.is_error()) {
        set_status(L"创建失败：" + widen(account.error().message), true);
        return;
    }
    sscl::util::Result<void> added = m_accounts->add_and_select(account.value());
    if (added.is_error()) {
        set_status(L"保存失败：" + widen(added.error().message), true);
        return;
    }
    m_offline_name->text.clear();
    refresh_from_store();  // 内部会把选中行定位到当前（刚新增）账号
    set_pane(Pane::kDetail);
    notify_selected();
    set_status(L"已添加离线账号：" + widen(account.value().username));
}

void AccountDialog::submit_external() {
    if (m_ext_busy) {
        return;
    }
    const std::string server = narrow(m_ext_server->text);
    const std::string user = narrow(m_ext_user->text);
    const std::string password = narrow(m_ext_password->text);
    if (server.empty() || user.empty() || password.empty()) {
        set_status(L"请填写服务器地址、账号与密码", true);
        return;
    }
    m_ext_busy = true;
    apply_visibility();
    set_status(L"正在登录外置账号…");
    if (m_host != nullptr) {
        m_host->invalidate();
    }
    const std::uint64_t seq = ++m_login_seq;
    std::shared_ptr<AccountDialog> self = shared_from_this();
    std::thread([self, server, user, password, seq]() {
        sscl::util::Result<AccountInfo> result =
            sscl::core::authlib_login(server, user, password);
        self->post([self, result = std::move(result), seq]() mutable {
            if (seq != self->m_login_seq) {
                return;
            }
            self->m_ext_busy = false;
            self->finish_external_login(std::move(result));
        });
    }).detach();
}

void AccountDialog::finish_external_login(sscl::util::Result<AccountInfo> result) {
    if (result.is_error()) {
        apply_visibility();
        set_status(L"登录失败：" + widen(result.error().message), true);
        return;
    }
    sscl::util::Result<void> added = m_accounts->add_and_select(result.value());
    if (added.is_error()) {
        apply_visibility();
        set_status(L"保存失败：" + widen(added.error().message), true);
        return;
    }
    m_ext_password->text.clear();
    refresh_from_store();  // 内部会把选中行定位到当前（刚新增）账号
    set_pane(Pane::kDetail);
    notify_selected();
    set_status(L"第三方账号登录成功：" + widen(result.value().username) +
               L"（启动时会自动注入登录组件）");
}

void AccountDialog::start_ms_device_login() {
    if (!sscl::core::microsoft_login_configured()) {
        set_status(L"微软登录未配置应用 ID（SSCL_MS_CLIENT_ID）", true);
        return;
    }
    if (m_ms_busy) {
        return;
    }
    m_ms_busy = true;
    m_device_user_code.clear();
    m_browser_auth_url.clear();
    m_ms_code->text = L"正在获取登录代码…";
    m_ms_status->text.clear();
    m_ms_hint2->text.clear();
    apply_visibility();
    if (m_host != nullptr) {
        m_host->invalidate();
    }
    const std::string client_id(sscl::core::microsoft_client_id());
    const std::uint64_t seq = ++m_login_seq;
    std::shared_ptr<AccountDialog> self = shared_from_this();

    std::thread([self, client_id, seq]() {
        using namespace sscl::core;
        util::Result<DeviceCodeInfo> device = msa_request_device_code(client_id);
        if (device.is_error()) {
            self->post([self, device, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->m_ms_busy = false;
                self->m_ms_code->text.clear();
                self->m_ms_status->text = widen(device.error().message);
                self->apply_visibility();
                self->m_host->invalidate();
            });
            return;
        }
        const DeviceCodeInfo info = device.value();
        self->post([self, info, seq]() {
            if (seq != self->m_login_seq) {
                return;
            }
            self->m_device_user_code = info.user_code;
            self->m_device_verify_url = info.verification_uri;
            self->m_ms_code->text = widen(info.user_code);
            self->m_ms_status->text = L"在打开的网页中输入上面的代码";
            self->m_ms_hint2->text = L"网页提示完成后回到本窗口，通常 5~30 秒自动继续。";
            self->apply_visibility();
            self->m_host->invalidate();
            sscl::platform::open_url(info.verification_uri);
            sscl::platform::set_clipboard_text(info.user_code);
        });

        int interval_ms = (info.interval_seconds > 1 ? info.interval_seconds - 1 : 1) * 1000;
        const ULONGLONG deadline =
            ::GetTickCount64() + static_cast<ULONGLONG>(info.expires_in_seconds) * 1000;
        while (::GetTickCount64() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            util::Result<MsOAuthTokens> tokens = msa_poll_device_token(client_id, info.device_code);
            if (tokens.is_ok()) {
                util::Result<MinecraftSession> session = msa_minecraft_login(
                    tokens.value(), [self](const std::string& stage) {
                        self->post([self, stage]() {
                            self->m_ms_status->text = widen(stage);
                            self->m_host->invalidate();
                        });
                    });
                if (session.is_error()) {
                    self->post([self, session, seq]() {
                        if (seq != self->m_login_seq) {
                            return;
                        }
                        self->finish_ms_login(util::Result<AccountInfo>::fail(session.error()));
                    });
                    return;
                }
                AccountInfo out;
                out.type = AccountType::kMicrosoft;
                out.username = session.value().username;
                out.uuid_compact = session.value().uuid_compact;
                out.access_token = session.value().access_token;
                out.user_type = "msa";
                out.user_properties = "{}";
                out.refresh_token = session.value().refresh_token;
                out.expires_at_utc = session.value().expires_at_utc;
                out.profile_json = session.value().profile_json;
                self->post([self, out, seq]() {
                    if (seq != self->m_login_seq) {
                        return;
                    }
                    self->finish_ms_login(util::Result<AccountInfo>::ok(out));
                });
                return;
            }
            const std::string& code = tokens.error().code;
            if (code == "msa.pending") {
                interval_ms = 2000;
                continue;
            }
            if (code == "msa.slow_down") {
                interval_ms += 5000;
                continue;
            }
            self->post([self, tokens, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->finish_ms_login(util::Result<AccountInfo>::fail(tokens.error()));
            });
            return;
        }
        self->post([self, seq]() {
            if (seq != self->m_login_seq) {
                return;
            }
            self->finish_ms_login(util::Result<AccountInfo>::fail(
                sscl::util::Error::make("msa.expired", "登录用时太长，请重新登录", {})));
        });
    }).detach();
}

void AccountDialog::start_ms_browser_login() {
    if (!sscl::core::microsoft_login_configured()) {
        set_status(L"微软登录未配置应用 ID（SSCL_MS_CLIENT_ID）", true);
        return;
    }
    if (m_ms_busy) {
        return;
    }
    m_ms_busy = true;
    m_device_user_code.clear();
    m_ms_code->text = L"等待浏览器登录…";
    m_ms_status->text = L"浏览器已打开登录页，完成后本窗口会自动继续";
    m_ms_hint2->text.clear();
    apply_visibility();
    if (m_host != nullptr) {
        m_host->invalidate();
    }
    const std::string client_id(sscl::core::microsoft_client_id());
    const std::uint64_t seq = ++m_login_seq;
    std::shared_ptr<AccountDialog> self = shared_from_this();

    std::thread([self, client_id, seq]() {
        using namespace sscl::core;
        const PkcePair pkce = msa_generate_pkce();
        std::array<unsigned char, 16> raw{};
        std::random_device rd;
        for (unsigned char& ch : raw) {
            ch = static_cast<unsigned char>(rd());
        }
        std::string state;
        static constexpr char kHex[] = "0123456789abcdef";
        for (unsigned char ch : raw) {
            state.push_back(kHex[ch >> 4]);
            state.push_back(kHex[ch & 0x0F]);
        }

        platform::OAuthLoopbackServer server;
        util::Result<void> started = server.start(state);
        if (started.is_error()) {
            self->post([self, started, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->finish_ms_login(util::Result<AccountInfo>::fail(started.error()));
            });
            return;
        }
        const std::string redirect = server.redirect_uri();
        const std::string auth_url = msa_authorize_url(client_id, pkce.challenge, redirect, state);
        self->post([self, auth_url]() {
            self->m_browser_auth_url = auth_url;
            sscl::platform::open_url(auth_url);
            self->m_host->invalidate();
        });

        util::Result<std::string> code = server.wait_for_code(300 * 1000);
        if (code.is_error()) {
            self->post([self, code, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->finish_ms_login(util::Result<AccountInfo>::fail(code.error()));
            });
            return;
        }
        util::Result<MsOAuthTokens> tokens =
            msa_exchange_code(client_id, code.value(), pkce.verifier, redirect);
        if (tokens.is_error()) {
            self->post([self, tokens, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->finish_ms_login(util::Result<AccountInfo>::fail(tokens.error()));
            });
            return;
        }
        util::Result<MinecraftSession> session = msa_minecraft_login(
            tokens.value(), [self](const std::string& stage) {
                self->post([self, stage]() {
                    self->m_ms_status->text = widen(stage);
                    self->m_host->invalidate();
                });
            });
        if (session.is_error()) {
            self->post([self, session, seq]() {
                if (seq != self->m_login_seq) {
                    return;
                }
                self->finish_ms_login(util::Result<AccountInfo>::fail(session.error()));
            });
            return;
        }
        AccountInfo out;
        out.type = AccountType::kMicrosoft;
        out.username = session.value().username;
        out.uuid_compact = session.value().uuid_compact;
        out.access_token = session.value().access_token;
        out.user_type = "msa";
        out.user_properties = "{}";
        out.refresh_token = session.value().refresh_token;
        out.expires_at_utc = session.value().expires_at_utc;
        out.profile_json = session.value().profile_json;
        self->post([self, out, seq]() {
            if (seq != self->m_login_seq) {
                return;
            }
            self->finish_ms_login(util::Result<AccountInfo>::ok(out));
        });
    }).detach();
}

void AccountDialog::finish_ms_login(sscl::util::Result<AccountInfo> result) {
    m_ms_busy = false;
    if (result.is_error()) {
        m_ms_status->text = L"登录失败：" + widen(result.error().message);
        m_ms_hint2->text.clear();
        apply_visibility();
        if (m_host != nullptr) {
            m_host->invalidate();
        }
        return;
    }
    sscl::util::Result<void> added = m_accounts->add_and_select(result.value());
    if (added.is_error()) {
        apply_visibility();
        set_status(L"保存失败：" + widen(added.error().message), true);
        return;
    }
    refresh_from_store();  // 内部会把选中行定位到当前（刚新增）账号
    set_pane(Pane::kDetail);
    notify_selected();
    set_status(L"正版账号登录成功：" + widen(result.value().username));
}

void AccountDialog::notify_selected() {
    if (!on_selected) {
        return;
    }
    const std::optional<AccountInfo> current = m_accounts->selected();
    if (current.has_value()) {
        on_selected(current->uuid_compact);
    }
}

void AccountDialog::set_status(const std::wstring& text, bool error) {
    m_status = text;
    m_status_error = error;
    if (m_host != nullptr) {
        m_host->invalidate();
    }
}

void AccountDialog::post(std::function<void()> fn) {
    if (m_host != nullptr) {
        m_host->post_to_ui(std::move(fn));
    }
}

void AccountDialog::detach_host() {
    m_host = nullptr;
    // 递增序号：已排队的异步回调（微软/外置登录、皮肤拉取）在 UI 线程执行时直接失效，
    // 不会再解引用已销毁的宿主窗口。
    ++m_login_seq;
    ++m_skin_seq;
}

void AccountDialog::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr || dwrite == nullptr) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), background.GetAddressOf()))) {
        target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip + 64.0f, m_height_dip + 64.0f),
                              background.Get());
    }

    draw_header(target, dwrite);

    // 左侧列表底板
    fill_round(target, rect_of(20.0f, 76.0f, 330.0f, m_height_dip - 72.0f), 10.0f,
               theme_container());
    draw_list(target, dwrite);

    m_root.draw(target, dwrite);
    draw_status(target, dwrite);
}

void AccountDialog::draw_header(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    // 标题/副标题由 Label 绘制（已在 m_root 中）；这里只画一条分隔线。
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_border(), border.GetAddressOf()))) {
        border->SetOpacity(0.6f);
        target->DrawLine(D2D1::Point2F(20.0f, 66.0f), D2D1::Point2F(m_width_dip - 20.0f, 66.0f),
                         border.Get(), 1.0f);
    }
    (void)dwrite;
}

void AccountDialog::draw_list(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    const auto& list = m_accounts->accounts();
    const std::optional<AccountInfo> current = m_accounts->selected();

    target->PushAxisAlignedClip(m_list_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float top = m_list_rect.top + static_cast<float>(i) * m_row_height - m_scroll;
        if (top + m_row_height < m_list_rect.top || top > m_list_rect.bottom) {
            continue;
        }
        const D2D1_RECT_F row =
            rect_of(m_list_rect.left, top + 2.0f, m_list_rect.right, top + m_row_height - 2.0f);
        const int index = static_cast<int>(i);
        const bool is_selected = index == m_selected_row;
        const bool is_current = current.has_value() &&
                                current->uuid_compact == list[i].uuid_compact;
        if (is_selected) {
            fill_round(target, row, 8.0f, theme_primary_container());
        } else if (index == m_hover_row) {
            fill_round(target, row, 8.0f, theme_hover_background());
        }

        // 头像（D-121/D-122）：有皮肤画「脸层 + 帽子层」并用圆角正方形遮罩裁剪；
        // 没有皮肤（离线账号 / 尚未加载 / 服务器无皮肤）退回首字母圆角方块。
        // 两者形状、半径、描边完全一致，不会再出现圆形描边里露出方形皮肤四角。
        const float avatar_size = 34.0f;
        const D2D1_RECT_F avatar = rect_of(row.left + 10.0f, top + (m_row_height - avatar_size) * 0.5f,
                                          row.left + 10.0f + avatar_size,
                                          top + (m_row_height + avatar_size) * 0.5f);
        auto skin = m_row_avatars.find(list[i].uuid_compact);
        if (skin != m_row_avatars.end() && skin->second != nullptr) {
            // SkinAvatar::draw 自己负责圆角遮罩与描边，这里只给矩形。
            skin->second->bounds = avatar;
            skin->second->draw(target, dwrite);
        } else {
            fill_round(target, avatar, SkinAvatar::kCornerRadius, theme_container());
            stroke_round(target, avatar, SkinAvatar::kCornerRadius, theme_border(), 1.0f);
            draw_text(target, dwrite, initial_of(widen(list[i].username)), avatar, 14.0f,
                      DWRITE_FONT_WEIGHT_BOLD,
                      is_selected ? theme_primary() : theme_text_secondary(),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float text_left = avatar.right + 12.0f;
        const float text_right = row.right - 10.0f;
        draw_text(target, dwrite, widen(list[i].username),
                  rect_of(text_left, top + 8.0f, text_right - 44.0f, top + 30.0f), 13.5f,
                  DWRITE_FONT_WEIGHT_SEMI_BOLD, theme_text_primary());
        draw_text(target, dwrite, account_type_display(list[i].type),
                  rect_of(text_left, top + 30.0f, text_right - 44.0f, top + 48.0f), 10.5f,
                  DWRITE_FONT_WEIGHT_NORMAL, theme_text_muted());
        if (is_current) {
            draw_text(target, dwrite, L"当前", rect_of(text_right - 42.0f, top + 18.0f, text_right,
                                                       top + 38.0f),
                      10.5f, DWRITE_FONT_WEIGHT_BOLD, theme_primary(),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
    target->PopAxisAlignedClip();
}

void AccountDialog::draw_status(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (m_status.empty()) {
        return;
    }
    draw_text(target, dwrite, m_status,
              rect_of(22.0f, m_height_dip - 40.0f, m_width_dip - 22.0f, m_height_dip - 14.0f),
              11.5f, DWRITE_FONT_WEIGHT_NORMAL,
              m_status_error ? theme_danger() : theme_text_secondary());
}

bool AccountDialog::mouse_move(float x, float y) {
    bool changed = false;
    const int row = row_hit(x, y);
    if (row != m_hover_row) {
        m_hover_row = row;
        changed = true;
    }
    m_root.state_changed = false;
    m_root.on_mouse_move(x, y);
    return changed || m_root.state_changed;
}

bool AccountDialog::mouse_down(float x, float y) {
    const int row = row_hit(x, y);
    if (row >= 0) {
        activate_row(row);
        return true;
    }
    // 焦点：点击到哪个输入框就聚焦哪个
    if (m_pane == Pane::kOffline && m_offline_name->hit_test(x, y)) {
        set_field(Field::kOfflineName);
    } else if (m_pane == Pane::kExternal) {
        if (m_ext_server->hit_test(x, y)) {
            set_field(Field::kExtServer);
        } else if (m_ext_user->hit_test(x, y)) {
            set_field(Field::kExtUser);
        } else if (m_ext_password->hit_test(x, y)) {
            set_field(Field::kExtPassword);
        }
    }
    m_root.state_changed = false;
    m_root.on_mouse_down(x, y);
    return true;
}

bool AccountDialog::mouse_up(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_up(x, y);
    return true;
}

bool AccountDialog::mouse_leave() {
    m_hover_row = -1;
    m_root.on_mouse_leave();
    return true;
}

bool AccountDialog::mouse_wheel(float x, float y, float delta) {
    if (x < m_list_rect.left || x >= m_list_rect.right || y < m_list_rect.top ||
        y >= m_list_rect.bottom) {
        return false;
    }
    m_scroll_target -= delta * m_row_height * 0.5f;
    clamp_scroll();
    return true;
}

bool AccountDialog::handle_char(wchar_t ch) {
    TextBox* box = nullptr;
    if (m_field == Field::kOfflineName) {
        box = m_offline_name;
    } else if (m_field == Field::kExtServer) {
        box = m_ext_server;
    } else if (m_field == Field::kExtUser) {
        box = m_ext_user;
    } else if (m_field == Field::kExtPassword) {
        box = m_ext_password;
    }
    if (box == nullptr) {
        return false;
    }
    if (!box->handle_char(ch)) {
        return false;
    }
    if (m_host != nullptr) {
        m_host->invalidate();
    }
    return true;
}

bool AccountDialog::handle_key_down(unsigned long key) {
    if (key == VK_ESCAPE) {
        if (m_pane != Pane::kDetail) {
            set_pane(Pane::kDetail);
        } else if (on_close) {
            on_close();
        }
        return true;
    }
    TextBox* box = nullptr;
    if (m_field == Field::kOfflineName) {
        box = m_offline_name;
    } else if (m_field == Field::kExtServer) {
        box = m_ext_server;
    } else if (m_field == Field::kExtUser) {
        box = m_ext_user;
    } else if (m_field == Field::kExtPassword) {
        box = m_ext_password;
    }
    if (key == VK_RETURN) {
        if (m_pane == Pane::kOffline) {
            submit_offline();
            return true;
        }
        if (m_pane == Pane::kExternal) {
            submit_external();
            return true;
        }
        return false;
    }
    if (box != nullptr && box->handle_key_down(key)) {
        if (m_host != nullptr) {
            m_host->invalidate();
        }
        return true;
    }
    return false;
}

bool AccountDialog::tick(float dt_ms) {
    bool active = m_root.tick(dt_ms);
    // D-121：SkinAvatar 的淡入需要逐帧推进，否则新头像会停在 opacity=0。
    for (auto& entry : m_row_avatars) {
        if (entry.second != nullptr && entry.second->tick(dt_ms)) {
            active = true;
        }
    }
    if (std::fabs(m_scroll_target - m_scroll) > 0.5f) {
        const float step = (std::min)(1.0f, dt_ms / 90.0f);
        m_scroll += (m_scroll_target - m_scroll) * step;
        active = true;
    } else if (m_scroll != m_scroll_target) {
        m_scroll = m_scroll_target;
        active = true;
    }
    return active;
}

}  // namespace sscl::ui
