#include "ui/pages/home_page.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <random>
#include <thread>
#include <utility>

#include "core/account/account.hpp"
#include "core/account/account_skin.hpp"
#include "core/account/authlib_auth.hpp"
#include "core/account/microsoft_auth.hpp"
#include "core/config/build_config.hpp"
#include "platform/http_client.hpp"
#include "platform/path_utils.hpp"
#include "platform/system_monitor.hpp"
#include "platform/oauth_loopback_server.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/anim.hpp"
#include "ui/framework/motion.hpp"
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

std::string narrow(const std::wstring& text) {
    return sscl::util::wide_to_utf8(text);
}

// Label 现已在绘制层用 DirectWrite trimming 按宽度省略（D-054）。
// 这里仅保留兜底上限，避免异常长的远端文本进入布局（正常文案不会被它截断）。
std::wstring clip_status(std::wstring text, std::size_t max_units = 160) {
    if (text.size() <= max_units) {
        return text;
    }
    std::wstring out = text.substr(0, max_units);
    if (!out.empty()) {
        const wchar_t last = out.back();
        if (last >= 0xD800 && last <= 0xDBFF) {
            out.pop_back();  // 不切断代理对
        }
    }
    out += L"…";
    return out;
}

std::wstring account_type_display(sscl::core::AccountType type) {
    switch (type) {
        case sscl::core::AccountType::kMicrosoft:
            return L"正版";
        case sscl::core::AccountType::kAuthlibInjector:
            return L"外置登录";
        case sscl::core::AccountType::kNide8:
            return L"统一通行证";
        default:
            return L"离线";
    }
}

// 皮肤 URL 解析已统一收敛到 core/account/account_skin（D-121），
// 微软 profile_json 与外置登录 sessionserver 两种来源共用同一套下载/缓存逻辑。

}  // namespace

HomePage::HomePage(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
                   std::shared_ptr<sscl::core::AccountManager> accounts)
    : m_window(window), m_service(std::move(service)), m_accounts(std::move(accounts)) {
    // 左面板：账号卡片（C-10，PCL 式）
    auto avatar = std::make_unique<SkinAvatar>();
    auto account_name = std::make_unique<Label>(L"Player", 15.0f, theme_text_primary(), true);
    auto account_type = std::make_unique<Label>(L"离线账号", 11.0f, theme_text_secondary());
    auto switch_accounts = std::make_unique<Button>(L"切换账号", 11.0f, false);
    auto manage = std::make_unique<Button>(L"账号管理", 11.0f, false);
    auto version_select = std::make_unique<Button>(L"版本选择", 13.0f, false);
    auto caption = std::make_unique<Label>(L"未选择版本", 11.0f, theme_text_secondary());
    auto launch = std::make_unique<Button>(L"下载游戏", 18.0f, true);
    auto cancel_launch = std::make_unique<Button>(L"取消启动", 13.0f, false);
    auto status = std::make_unique<Label>(L"就绪", 11.0f, theme_text_secondary());

    cancel_launch->visible = false;  // D-028：启动中才显示

    m_avatar = avatar.get();
    m_account_name = account_name.get();
    m_account_type = account_type.get();
    m_switch_accounts = switch_accounts.get();
    m_manage_accounts = manage.get();
    m_version_select = version_select.get();
    m_version_caption = caption.get();
    m_launch = launch.get();
    m_cancel_launch = cancel_launch.get();
    m_status = status.get();

    // D-118：账号管理走独立子窗口；主页只保留「切换账号」的轻量弹层。
    manage->on_click = [this]() {
        if (on_open_account_manager) {
            on_open_account_manager();
        }
    };
    switch_accounts->on_click = [this]() {
        refresh_account_rows();
        set_overlay(OverlayMode::kList);
    };
    version_select->on_click = [this]() {
        if (on_open_version_select) {
            on_open_version_select();
        }
    };
    launch->on_click = [this]() { start_launch(); };
    cancel_launch->on_click = [this]() {
        if (m_launching) {
            m_launch_stop.request_stop();  // D-028
            set_status(L"正在取消启动…");
            if (request_repaint) {
                request_repaint();
            }
        }
    };

    m_root.add(std::move(avatar));
    m_root.add(std::move(account_name));
    m_root.add(std::move(account_type));
    m_root.add(std::move(switch_accounts));
    m_root.add(std::move(manage));
    m_root.add(std::move(version_select));
    m_root.add(std::move(caption));
    m_root.add(std::move(launch));
    m_root.add(std::move(cancel_launch));
    m_root.add(std::move(status));

    // 右侧卡片
    auto quick_card = std::make_unique<Card>(L"快速开始");
    auto quick_text = std::make_unique<Label>(
        L"还没有安装版本？前往下载页获取最新的 Minecraft。", 12.5f, theme_text_secondary());
    auto quick_download = std::make_unique<Button>(L"下载游戏", 13.0f, true);
    quick_download->on_click = [this]() {
        if (on_open_downloads) {
            on_open_downloads();
        }
    };
    m_quick_card = quick_card.get();
    m_quick_text = quick_text.get();
    m_quick_download = quick_download.get();
    quick_card->add(std::move(quick_text));
    quick_card->add(std::move(quick_download));
    m_root.add(std::move(quick_card));

    auto info_card = std::make_unique<Card>(L"版本信息");
    auto info_version = std::make_unique<Label>(L"版本：未选择", 12.5f, theme_text_primary());
    auto info_type = std::make_unique<Label>(L"类型：-", 12.5f, theme_text_secondary());
    auto info_java = std::make_unique<Label>(L"Java：启动时自动选择", 12.5f, theme_text_secondary());
    m_info_card = info_card.get();
    m_info_version = info_version.get();
    m_info_type = info_type.get();
    m_info_java = info_java.get();
    info_card->add(std::move(info_version));
    info_card->add(std::move(info_type));
    info_card->add(std::move(info_java));
    m_root.add(std::move(info_card));

    // 账号切换弹层（D-118：只做切换；增删/登录全部在独立「账号管理」子窗口）
    auto overlay = std::make_unique<Card>(L"");
    overlay->elevated = true;  // D-032：弹窗表面 + 描边，与主背景拉开层次
    auto title = std::make_unique<Label>(L"切换账号", 15.0f, theme_text_primary(), true);
    m_overlay_card = overlay.get();
    m_overlay_title = title.get();
    overlay->add(std::move(title));
    for (int i = 0; i < kMaxAccountRows; ++i) {
        auto name = std::make_unique<Label>(L"", 12.5f, theme_text_primary());
        auto type = std::make_unique<Label>(L"", 11.0f, theme_text_secondary());
        auto select = std::make_unique<Button>(L"使用", 11.0f, false);
        auto remove = std::make_unique<Button>(L"删除", 11.0f, false);
        name->visible = false;
        type->visible = false;
        select->visible = false;
        remove->visible = false;
        m_row_names[i] = name.get();
        m_row_types[i] = type.get();
        m_row_select[i] = select.get();
        m_row_delete[i] = remove.get();
        overlay->add(std::move(name));
        overlay->add(std::move(type));
        overlay->add(std::move(select));
        overlay->add(std::move(remove));
    }
    auto offline_box = std::make_unique<TextBox>(L"", 12.5f);
    offline_box->placeholder = L"新离线玩家名";
    offline_box->max_length = 16;
    auto add_offline = std::make_unique<Button>(L"添加离线账号", 11.0f, true);
    auto add_ms_device = std::make_unique<Button>(L"微软登录（设备码）", 11.0f, true);
    auto add_ms_browser = std::make_unique<Button>(L"微软登录（浏览器）", 11.0f, false);
    auto overlay_close = std::make_unique<Button>(L"关闭", 12.0f, false);
    auto overlay_manage = std::make_unique<Button>(L"管理账号…", 12.0f, true);
    auto ms_code_label = std::make_unique<Label>(L"", 26.0f, theme_primary(), true);
    auto ms_status = std::make_unique<Label>(L"", 12.0f, theme_text_secondary());
    auto ms_copy = std::make_unique<Button>(L"复制代码", 12.0f, true);
    auto ms_open = std::make_unique<Button>(L"打开登录页", 12.0f, false);
    auto ms_cancel = std::make_unique<Button>(L"取消", 12.0f, false);
    m_offline_box = offline_box.get();
    m_add_offline = add_offline.get();
    m_add_ms_device = add_ms_device.get();
    m_add_ms_browser = add_ms_browser.get();
    m_overlay_close = overlay_close.get();
    m_overlay_manage = overlay_manage.get();
    m_ms_code_label = ms_code_label.get();
    m_ms_status = ms_status.get();
    m_ms_copy = ms_copy.get();
    m_ms_open = ms_open.get();
    m_ms_cancel = ms_cancel.get();

    m_switch_accounts->icon = "switch_account";
    m_manage_accounts->icon = "manage_accounts";
    m_overlay_manage->icon = "manage_accounts";
    m_version_select->icon = "list";
    m_launch->icon = "download";
    m_cancel_launch->icon = "close";
    m_quick_download->icon = "download";
    m_add_offline->icon = "person_add";
    m_add_ms_device->icon = "login";
    m_add_ms_browser->icon = "login";
    m_overlay_close->icon = "close";
    m_ms_copy->icon = "content_copy";
    m_ms_open->icon = "open_in_new";
    m_ms_cancel->icon = "close";
    for (int i = 0; i < kMaxAccountRows; ++i) {
        m_row_select[i]->icon = "check";
        m_row_delete[i]->icon = "delete";
    }

    add_offline->on_click = [this]() {
        const std::string name = narrow(m_offline_box->text);
        if (name.empty()) {
            set_status(L"请输入玩家名");
            return;
        }
        sscl::core::OfflineAccount provider;
        sscl::util::Result<sscl::core::AccountInfo> account = provider.login(name);
        if (account.is_ok()) {
            (void)m_accounts->add_and_select(account.value());
            refresh_account_card();
            refresh_account_rows();
            if (on_account_selected) {
                on_account_selected(account.value().uuid_compact);
            }
            m_offline_box->text.clear();
            set_overlay(OverlayMode::kList);
        } else {
            set_status(widen(account.error().message));
        }
    };
    add_ms_device->on_click = [this]() {
        if (!sscl::core::microsoft_login_configured()) {
            set_status(L"微软登录未配置应用 ID（SSCL_MS_CLIENT_ID）");
            return;
        }
        start_ms_device_login();
    };
    add_ms_browser->on_click = [this]() {
        if (!sscl::core::microsoft_login_configured()) {
            set_status(L"微软登录未配置应用 ID（SSCL_MS_CLIENT_ID）");
            return;
        }
        start_ms_browser_login();
    };
    overlay_close->on_click = [this]() { set_overlay(OverlayMode::kHidden); };
    overlay_manage->on_click = [this]() {
        set_overlay(OverlayMode::kHidden);
        if (on_open_account_manager) {
            on_open_account_manager();
        }
    };
    ms_cancel->on_click = [this]() { set_overlay(OverlayMode::kList); };
    ms_copy->on_click = [this]() {
        if (!m_device_user_code.empty()) {
            sscl::platform::set_clipboard_text(m_device_user_code);
            set_status(L"登录代码已复制");
        }
    };
    ms_open->on_click = [this]() {
        const std::string& url =
            m_overlay == OverlayMode::kBrowserLogin ? m_browser_auth_url : m_device_verify_url;
        if (!url.empty()) {
            sscl::platform::open_url(url);
        }
    };

    overlay->add(std::move(offline_box));
    overlay->add(std::move(add_offline));
    overlay->add(std::move(add_ms_device));
    overlay->add(std::move(add_ms_browser));
    overlay->add(std::move(overlay_close));
    overlay->add(std::move(overlay_manage));
    overlay->add(std::move(ms_code_label));
    overlay->add(std::move(ms_status));
    overlay->add(std::move(ms_copy));
    overlay->add(std::move(ms_open));
    overlay->add(std::move(ms_cancel));
    m_root.add(std::move(overlay));

    set_overlay(OverlayMode::kHidden);
    refresh_account_card();
}

void HomePage::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;
    const float w = m_width_dip;
    const float h = m_height_dip;
    const float margin = kSidebarMarginDip;
    const float sidebar_right = kSidebarWidthDip;

    // 左面板：账号卡（头像 44x44 + 名称/类型 + 管理按钮）
    const float card_top = kNavHeightDip + 20.0f;
    m_avatar->bounds = rect_of(margin, card_top, margin + 44.0f, card_top + 44.0f);
    m_account_name->bounds =
        rect_of(margin + 52.0f, card_top + 2.0f, sidebar_right - margin, card_top + 26.0f);
    m_account_type->bounds =
        rect_of(margin + 52.0f, card_top + 26.0f, sidebar_right - margin, card_top + 44.0f);
    m_switch_accounts->bounds =
        rect_of(margin, card_top + 52.0f, sidebar_right - margin, card_top + 84.0f);
    m_manage_accounts->bounds =
        rect_of(margin, card_top + 90.0f, sidebar_right - margin, card_top + 122.0f);

    // “取消启动”与“版本选择”同槽位（D-028）
    m_version_select->bounds = rect_of(margin, h - 186.0f, sidebar_right - margin, h - 151.0f);
    m_cancel_launch->bounds = rect_of(margin, h - 186.0f, sidebar_right - margin, h - 151.0f);
    m_version_caption->bounds = rect_of(margin, h - 148.0f, sidebar_right - margin, h - 124.0f);
    m_launch->bounds = rect_of(margin, h - 114.0f, sidebar_right - margin, h - 60.0f);
    m_status->bounds = rect_of(margin, h - 52.0f, sidebar_right - margin, h - 26.0f);

    // 右侧卡片
    const float card_left = sidebar_right + 24.0f;
    const float card_right = w - 24.0f;
    m_quick_card->bounds = rect_of(card_left, kNavHeightDip + 24.0f, card_right,
                                   kNavHeightDip + 188.0f);
    m_quick_text->bounds = rect_of(card_left + 16.0f, kNavHeightDip + 68.0f, card_right - 16.0f,
                                   kNavHeightDip + 112.0f);
    m_quick_download->bounds =
        rect_of(card_left + 16.0f, kNavHeightDip + 124.0f, card_left + 176.0f,
                kNavHeightDip + 164.0f);

    m_info_card->bounds = rect_of(card_left, kNavHeightDip + 212.0f, card_right,
                                  kNavHeightDip + 372.0f);
    m_info_version->bounds = rect_of(card_left + 16.0f, kNavHeightDip + 260.0f, card_right - 16.0f,
                                     kNavHeightDip + 284.0f);
    m_info_type->bounds = rect_of(card_left + 16.0f, kNavHeightDip + 288.0f, card_right - 16.0f,
                                  kNavHeightDip + 312.0f);
    m_info_java->bounds = rect_of(card_left + 16.0f, kNavHeightDip + 316.0f, card_right - 16.0f,
                                  kNavHeightDip + 340.0f);

    layout_overlay();
}

void HomePage::layout_overlay() {
    if (m_overlay == OverlayMode::kHidden) {
        return;
    }
    const float w = m_width_dip;
    const float h = m_height_dip;
    // D-118：切换账号弹层只列账号 + 一个「管理账号…」入口。
    const float panel_w = 420.0f;
    const float panel_h = 400.0f;
    const float left = (w - panel_w) / 2.0f;
    const float top = kNavHeightDip + (h - kNavHeightDip - panel_h) / 2.0f;
    m_overlay_card->bounds = rect_of(left, top, left + panel_w, top + panel_h);
    m_overlay_title->bounds = rect_of(left + 24.0f, top + 20.0f, left + panel_w - 24.0f, top + 48.0f);

    float row_y = top + 62.0f;
    for (int i = 0; i < kMaxAccountRows; ++i) {
        m_row_names[i]->bounds = rect_of(left + 24.0f, row_y, left + 268.0f, row_y + 22.0f);
        m_row_types[i]->bounds = rect_of(left + 24.0f, row_y + 22.0f, left + 268.0f, row_y + 42.0f);
        m_row_select[i]->bounds = rect_of(left + 286.0f, row_y + 8.0f, left + 354.0f, row_y + 38.0f);
        m_row_delete[i]->bounds = rect_of(left + 360.0f, row_y + 8.0f, left + 396.0f, row_y + 38.0f);
        row_y += 46.0f;
    }

    // 添加控件的布局保留（D-118 起在主页统一隐藏，登录/新增走子窗口）。
    const float action_y = top + panel_h - 132.0f;
    m_offline_box->bounds = rect_of(left + 24.0f, action_y, left + 220.0f, action_y + 32.0f);
    m_add_offline->bounds = rect_of(left + 228.0f, action_y, left + 352.0f, action_y + 32.0f);
    m_add_ms_device->bounds = rect_of(left + 24.0f, action_y + 42.0f, left + 210.0f, action_y + 74.0f);
    m_add_ms_browser->bounds = rect_of(left + 218.0f, action_y + 42.0f, left + 420.0f, action_y + 74.0f);

    m_overlay_manage->bounds =
        rect_of(left + 24.0f, top + panel_h - 48.0f, left + 140.0f, top + panel_h - 16.0f);
    m_overlay_close->bounds =
        rect_of(left + panel_w - 112.0f, top + panel_h - 48.0f, left + panel_w - 24.0f,
                top + panel_h - 16.0f);

    // 微软登录视图（D-118 起不在主页使用，保留几何避免空 bounds）。
    const float code_x = left + 24.0f;
    m_ms_code_label->bounds = rect_of(code_x, top + 120.0f, left + panel_w - 24.0f, top + 170.0f);
    m_ms_status->bounds = rect_of(code_x, top + 180.0f, left + panel_w - 24.0f, top + 240.0f);
    m_ms_copy->bounds = rect_of(code_x, top + 260.0f, code_x + 140.0f, top + 296.0f);
    m_ms_open->bounds = rect_of(code_x + 150.0f, top + 260.0f, code_x + 310.0f, top + 296.0f);
    m_ms_cancel->bounds = rect_of(code_x + 320.0f, top + 260.0f, code_x + 420.0f, top + 296.0f);
}

void HomePage::refresh_account_card() {
    const std::optional<sscl::core::AccountInfo> account = m_accounts->selected();
    if (!account.has_value()) {
        m_account_name->text = L"未登录";
        m_account_type->text = L"点击「账号管理」添加账号";
        // D-032：删除最后一个账号后清理头像与待拉取皮肤缓存，杜绝残留旧头像。
        ++m_skin_seq;  // 作废在途皮肤回调
        m_pending_skin_account.reset();
        m_avatar->set_png({});
        return;
    }
    m_account_name->text = widen(account->username);
    m_account_type->text = account_type_display(account->type);
    // D-121：微软与外置登录都有皮肤；离线账号的 UUID 是本地派生，服务器上没有皮肤。
    if (account->type == sscl::core::AccountType::kOffline) {
        ++m_skin_seq;  // D-032：切到离线账号，作废在途皮肤回调
        m_avatar->set_png({});
    } else {
        fetch_skin(*account);
    }
}

void HomePage::refresh_accounts() {
    refresh_account_card();
    if (m_overlay == OverlayMode::kList) {
        refresh_account_rows();
    }
}

void HomePage::refresh_account_rows() {
    const auto& list = m_accounts->accounts();
    for (int i = 0; i < kMaxAccountRows; ++i) {
        const bool shown = static_cast<std::size_t>(i) < list.size();
        m_row_names[i]->visible = shown;
        m_row_types[i]->visible = shown;
        m_row_select[i]->visible = shown;
        m_row_delete[i]->visible = false;  // D-118：删除只在账号管理子窗口
        if (!shown) {
            continue;
        }
        const sscl::core::AccountInfo& item = list[static_cast<std::size_t>(i)];
        const std::optional<sscl::core::AccountInfo> current = m_accounts->selected();
        const bool is_current =
            current.has_value() && current->uuid_compact == item.uuid_compact;
        m_row_names[i]->text = widen(item.username);
        m_row_types[i]->text = (is_current ? L"当前 · " : L"") +
                               account_type_display(item.type);
        const std::string uuid = item.uuid_compact;
        m_row_select[i]->text = is_current ? L"当前" : L"切换";
        m_row_select[i]->enabled = !is_current;
        m_row_select[i]->on_click = [this, uuid]() {
            m_accounts->select_uuid(uuid);
            refresh_account_card();
            if (on_account_selected) {
                on_account_selected(uuid);
            }
            set_overlay(OverlayMode::kHidden);  // D-118：切换后立即收起弹层
        };
    }
    if (m_overlay == OverlayMode::kList) {
        layout_overlay();
    }
}

void HomePage::set_overlay(OverlayMode mode) {
    // D-102：离开设备码视图即结束等待（用户可能在请求返回前就点了取消）。
    if (mode != OverlayMode::kDeviceLogin) {
        m_ms_code_loading = false;
    }
    const OverlayMode prev = m_overlay;
    m_overlay = mode;
    const bool list = mode == OverlayMode::kList;
    const bool login = mode == OverlayMode::kDeviceLogin || mode == OverlayMode::kBrowserLogin;

    if (mode == OverlayMode::kHidden) {
        // 关闭：卡片立即退出命中，子元素保留最后视图随淡出绘制（D-033）。
        m_overlay_card->visible = false;
        if (prev == OverlayMode::kHidden || m_overlay_anim <= 0.001f) {
            // 初次/无淡出：立即隐藏全部子元素。
            m_overlay_title->visible = false;
            for (int i = 0; i < kMaxAccountRows; ++i) {
                m_row_names[i]->visible = false;
                m_row_types[i]->visible = false;
                m_row_select[i]->visible = false;
                m_row_delete[i]->visible = false;
            }
            m_offline_box->visible = false;
            m_add_offline->visible = false;
            m_add_ms_device->visible = false;
            m_add_ms_browser->visible = false;
            m_overlay_close->visible = false;
            m_overlay_manage->visible = false;
            m_ms_code_label->visible = false;
            m_ms_status->visible = false;
            m_ms_copy->visible = false;
            m_ms_open->visible = false;
            m_ms_cancel->visible = false;
        }
    } else {
        m_overlay_card->visible = true;
        m_overlay_title->visible = true;
        for (int i = 0; i < kMaxAccountRows; ++i) {
            const bool shown = list && static_cast<std::size_t>(i) < m_accounts->accounts().size();
            m_row_names[i]->visible = shown;
            m_row_types[i]->visible = shown;
            m_row_select[i]->visible = shown;
            m_row_delete[i]->visible = false;  // D-118：删除只在账号管理子窗口
        }
        // D-118：主页只保留「切换账号」；新增/登录统一在独立子窗口。
        m_offline_box->visible = false;
        m_add_offline->visible = false;
        m_add_ms_device->visible = false;
        m_add_ms_browser->visible = false;
        m_overlay_close->visible = list;
        m_overlay_manage->visible = list;

        m_ms_code_label->visible = login;
        m_ms_status->visible = login;
        m_ms_copy->visible = login && mode == OverlayMode::kDeviceLogin;
        m_ms_open->visible = login;
        m_ms_cancel->visible = login;

        if (prev != mode) {
            m_overlay_view_anim = 0.0f;  // 打开/视图切换：内容淡入（D-033）
        }
        if (list) {
            refresh_account_rows();
        }
        layout_overlay();
    }
    if (request_repaint) {
        request_repaint();  // 驱动开合/切换动画帧（D-033）
    }
}

void HomePage::start_ms_device_login() {
    set_overlay(OverlayMode::kDeviceLogin);
    m_ms_code_loading = true;  // D-102：拿到 user_code 后由回调清零
    m_ms_code_label->text = L"正在获取登录代码…";
    m_ms_status->text = L"";
    if (request_repaint) {
        request_repaint();
    }
    const std::string client_id(sscl::core::microsoft_client_id());
    std::shared_ptr<HomePage> self = shared_from_this();

    std::thread([self, client_id]() {
        using namespace sscl::core;
        util::Result<DeviceCodeInfo> device = msa_request_device_code(client_id);
        if (device.is_error()) {
            self->m_window->post_to_ui([self, device]() {
                self->m_ms_code_loading = false;
                self->m_ms_code_label->text = L"";
                self->m_ms_status->text = widen(device.error().message);
                if (self->request_repaint) {
                    self->request_repaint();
                }
            });
            return;
        }
        const DeviceCodeInfo info = device.value();
        self->m_window->post_to_ui([self, info]() {
            self->m_ms_code_loading = false;  // D-102：验证码要显示，居中指示器必须撤掉
            self->m_device_user_code = info.user_code;
            self->m_device_verify_url = info.verification_uri;
            self->m_ms_code_label->text = widen(info.user_code);
            self->m_ms_status->text = L"请在打开的网页中输入上述代码（已尝试自动打开）。\n"
                                      L"若网页未打开，请点「打开登录页」。\n"
                                      L"网页提示完成后回到本窗口即可，通常 5~30 秒自动继续。";
            if (self->request_repaint) {
                self->request_repaint();
            }
            sscl::platform::open_url(info.verification_uri);
            sscl::platform::set_clipboard_text(info.user_code);
        });

        // 轮询节奏（对齐 PCL）：首次按 interval-1 等待；pending 后 2s 快轮询，
        // 网页“大功告成”后最多 2 秒即进入后续链路，而不是再等一个完整 interval。
        int interval_ms = (info.interval_seconds > 1 ? info.interval_seconds - 1 : 1) * 1000;
        const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(info.expires_in_seconds) * 1000;
        while (::GetTickCount64() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            util::Result<MsOAuthTokens> tokens = msa_poll_device_token(client_id, info.device_code);
            if (tokens.is_ok()) {
                // 完整链路：XBL → XSTS → MC token → entitlements → profile（阶段实时回显）
                util::Result<MinecraftSession> session = msa_minecraft_login(tokens.value(),
                    [self](const std::string& stage) {
                        self->m_window->post_to_ui([self, stage]() {
                            self->m_ms_status->text = widen(stage) + L"\n请稍候，勿关闭窗口。";
                            if (self->request_repaint) {
                                self->request_repaint();
                            }
                        });
                    });
                if (session.is_ok()) {
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
                    self->m_window->post_to_ui([self, out]() { self->finish_ms_login(
                        sscl::util::Result<AccountInfo>::ok(out)); });
                    return;
                }
                self->m_window->post_to_ui([self, session]() {
                    self->finish_ms_login(
                        sscl::util::Result<AccountInfo>::fail(session.error()));
                });
                return;
            }
            const std::string& code = tokens.error().code;
            if (code == "msa.pending") {
                interval_ms = 2000;  // PCL 策略：pending 后 2s 快轮询，缩短网页完成后的感知等待
                continue;
            }
            if (code == "msa.slow_down") {
                interval_ms += 5000;
                continue;
            }
            self->m_window->post_to_ui([self, tokens]() {
                self->finish_ms_login(sscl::util::Result<AccountInfo>::fail(tokens.error()));
            });
            return;
        }
        self->m_window->post_to_ui([self]() {
            self->finish_ms_login(sscl::util::Result<AccountInfo>::fail(
                sscl::util::Error::make("msa.expired", "登录用时太长，请重新登录", {})));
        });
    }).detach();
}

void HomePage::start_ms_browser_login() {
    set_overlay(OverlayMode::kBrowserLogin);
    m_ms_code_label->text = L"等待浏览器登录…";
    m_ms_status->text = L"浏览器已打开登录页；完成后本窗口会自动继续。\n"
                        L"网页提示完成后回到本窗口即可，通常 5~30 秒自动继续。";
    if (request_repaint) {
        request_repaint();
    }
    const std::string client_id(sscl::core::microsoft_client_id());
    std::shared_ptr<HomePage> self = shared_from_this();

    std::thread([self, client_id]() {
        using namespace sscl::core;
        // PKCE + state（HMCL 方案）
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
            self->m_window->post_to_ui([self, started]() {
                self->finish_ms_login(
                    util::Result<AccountInfo>::fail(started.error()));
            });
            return;
        }
        const std::string redirect = server.redirect_uri();
        const std::string auth_url = msa_authorize_url(client_id, pkce.challenge, redirect, state);
        self->m_window->post_to_ui([self, auth_url]() {
            self->m_browser_auth_url = auth_url;
            sscl::platform::open_url(auth_url);
        });

        util::Result<std::string> code = server.wait_for_code(300 * 1000);
        if (code.is_error()) {
            self->m_window->post_to_ui([self, code]() {
                self->finish_ms_login(util::Result<AccountInfo>::fail(code.error()));
            });
            return;
        }
        util::Result<MsOAuthTokens> tokens =
            msa_exchange_code(client_id, code.value(), pkce.verifier, redirect);
        if (tokens.is_error()) {
            self->m_window->post_to_ui([self, tokens]() {
                self->finish_ms_login(util::Result<AccountInfo>::fail(tokens.error()));
            });
            return;
        }
        util::Result<MinecraftSession> session = msa_minecraft_login(tokens.value(),
            [self](const std::string& stage) {
                self->m_window->post_to_ui([self, stage]() {
                    self->m_ms_status->text = widen(stage) + L"\n请稍候，勿关闭窗口。";
                    if (self->request_repaint) {
                        self->request_repaint();
                    }
                });
            });
        if (session.is_error()) {
            self->m_window->post_to_ui([self, session]() {
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
        self->m_window->post_to_ui(
            [self, out]() { self->finish_ms_login(util::Result<AccountInfo>::ok(out)); });
    }).detach();
}

void HomePage::finish_ms_login(sscl::util::Result<sscl::core::AccountInfo> result) {
    if (result.is_ok()) {
        (void)m_accounts->add_and_select(result.value());
        refresh_account_card();
        if (on_account_selected) {
            on_account_selected(result.value().uuid_compact);
        }
        set_overlay(OverlayMode::kList);
        set_status(L"微软账号登录成功：" + widen(result.value().username));
    } else {
        set_overlay(OverlayMode::kList);
        set_status(L"登录失败：" + widen(result.error().message));
    }
    if (request_repaint) {
        request_repaint();
    }
}

void HomePage::fetch_skin(const sscl::core::AccountInfo& account) {
    // 构造期 weak_this 尚未建立：shared_from_this() 会抛 bad_weak_ptr → terminate。
    // 此处推迟到构造完成后的 set_installed_versions 再拉取。
    std::shared_ptr<HomePage> self = weak_from_this().lock();
    if (self == nullptr) {
        m_pending_skin_account = account;
        return;
    }
    const std::uint64_t seq = ++m_skin_seq;  // 新代际：旧的在途回调作废
    std::thread([self, account, seq]() {
        // D-121：统一入口内部按账号类型解析 URL、读缓存、下载 PNG（同步、不上 UI 线程）。
        sscl::util::Result<std::vector<std::uint8_t>> png =
            sscl::core::fetch_account_skin_png(account);
        if (png.is_error() || png.value().empty()) {
            return;
        }
        self->m_window->post_to_ui([self, bytes = std::move(png.value()), seq]() mutable {
            // 账号已删除/切换（D-032）：丢弃过期皮肤，不覆盖当前头像。
            if (seq != self->m_skin_seq) {
                return;
            }
            self->m_avatar->set_png(std::move(bytes));
            if (self->request_repaint) {
                self->request_repaint();
            }
        });
    }).detach();
}

void HomePage::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
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
    // 覆盖层半透明遮罩（弹窗感；随开合淡入淡出，D-033）
    if (m_overlay_anim > 0.001f) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mask;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_background(), mask.GetAddressOf()))) {
            mask->SetOpacity(0.72f * m_overlay_anim);  // D-032 深遮罩 × D-033 淡入
            target->FillRectangle(rect_of(0.0f, kNavHeightDip, m_width_dip, m_height_dip),
                                  mask.Get());
        }
    }
    // 基础子元素（覆盖层卡片单独带动画绘制，见下）
    for (auto& child : m_root.children) {
        if (child.get() == static_cast<UiElement*>(m_overlay_card)) {
            continue;
        }
        if (child->visible) {
            child->draw(target, dwrite);
        }
    }
    // 覆盖层弹出动画（D-033）：缩放 0.94→1（ease-out）+ 整体透明度（开合 160ms / 视图切换 120ms）
    if (m_overlay_anim > 0.001f) {
        const D2D1_RECT_F card_rect = m_overlay_card->bounds;
        const D2D1_POINT_2F center = D2D1::Point2F((card_rect.left + card_rect.right) * 0.5f,
                                                  (card_rect.top + card_rect.bottom) * 0.5f);
        const float scale = 0.94f + 0.06f * ease_out(m_overlay_anim);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center));

        Microsoft::WRL::ComPtr<ID2D1Layer> layer;
        const float opacity = clamp01(m_overlay_anim * m_overlay_view_anim);
        if (opacity < 0.999f && SUCCEEDED(target->CreateLayer(layer.GetAddressOf()))) {
            D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
            params.opacity = opacity;
            target->PushLayer(&params, layer.Get());
        }
        m_overlay_card->draw(target, dwrite);
        if (layer != nullptr) {
            target->PopLayer();
        }
        target->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

bool HomePage::is_busy() const {
    return m_ms_code_loading;
}

std::wstring HomePage::busy_text() const {
    return L"正在获取登录代码…";
}

bool HomePage::tick(float dt_ms) {
    bool active = m_root.tick(dt_ms);  // 控件悬停渐变（D-020）
    // 覆盖层开合 + 视图切换淡入（时长随动效强度，D-081）
    if (motion::advance(m_overlay_anim, m_overlay == OverlayMode::kHidden ? 0.0f : 1.0f, dt_ms,
                        motion::Kind::kPop)) {
        active = true;
    }
    if (m_overlay != OverlayMode::kHidden &&
        motion::advance(m_overlay_view_anim, 1.0f, dt_ms, motion::Kind::kFade)) {
        active = true;
    }
    return active;
}

bool HomePage::mouse_move(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_move(x * m_to_dip, y * m_to_dip);
    return m_root.state_changed;
}

bool HomePage::mouse_down(float x, float y) {
    m_root.state_changed = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    // 输入框聚焦仅在列表视图生效（关闭淡出期不接受输入，D-033）
    const bool focus =
        m_overlay == OverlayMode::kList && m_offline_box->hit_test(x_dip, y_dip);
    if (m_offline_box->is_focused() != focus) {
        m_offline_box->set_focused(focus);
        m_root.state_changed = true;
    }
    m_root.on_mouse_down(x_dip, y_dip);
    return m_root.state_changed;
}

bool HomePage::mouse_up(float x, float y) {
    m_root.state_changed = false;
    m_root.on_mouse_up(x * m_to_dip, y * m_to_dip);
    return m_root.state_changed;
}

bool HomePage::mouse_leave() {
    m_root.state_changed = false;
    m_root.on_mouse_leave();
    return m_root.state_changed;
}

bool HomePage::handle_char(wchar_t ch) {
    if (!m_offline_box->is_focused()) {
        return false;
    }
    return m_offline_box->handle_char(ch);
}

bool HomePage::handle_key_down(unsigned long key) {
    if (!m_offline_box->is_focused()) {
        return false;
    }
    return m_offline_box->handle_key_down(key);
}

void HomePage::set_installed_versions(std::vector<sscl::core::InstalledVersion> versions,
                                      const std::string& selected_id) {
    m_installed = std::move(versions);
    m_selected_id = selected_id;
    update_version_caption();
    // 构造期推迟的皮肤拉取在此续跑（此时 weak_this 已建立）。
    if (m_pending_skin_account.has_value()) {
        const sscl::core::AccountInfo pending = std::move(*m_pending_skin_account);
        m_pending_skin_account.reset();
        fetch_skin(pending);
    }
}

void HomePage::set_status(const std::wstring& text) {
    m_status->text = clip_status(text);
}

void HomePage::set_java_settings(std::string java_path,
                                 std::vector<std::string> java_scan_dirs) {
    m_java_path = std::move(java_path);
    m_java_scan_dirs = std::move(java_scan_dirs);
}

void HomePage::update_version_caption() {
    const sscl::core::InstalledVersion* selected = nullptr;
    for (const auto& item : m_installed) {
        if (item.id == m_selected_id) {
            selected = &item;
            break;
        }
    }
    if (selected == nullptr) {
        m_version_caption->text = L"未选择版本";
        m_info_version->text = L"版本：未选择";
        m_info_type->text = L"类型：-";
        if (!m_launching) {
            m_launch->text = L"下载游戏";
            m_launch->icon = "download";
        }
        return;
    }
    m_version_caption->text = L"当前版本：" + widen(selected->id);
    m_info_version->text = L"版本：" + widen(selected->id);
    m_info_type->text = L"类型：" + widen(selected->type);
    if (!m_launching) {
        m_launch->text = L"启动游戏";
        m_launch->icon = "play_arrow";
    }
}

void HomePage::set_launch_config(sscl::core::LauncherConfig config) {
    m_launch_config = std::move(config);
    // 保持 M3 的 Java 字段与配置同源（AppShell 两处注入顺序不影响结果）。
    m_java_path = m_launch_config.java_path;
    m_java_scan_dirs = m_launch_config.java_scan_dirs;
}

void HomePage::apply_launcher_visibility(const sscl::core::GameProcess& process) {
    const int mode = m_launch_config.launcher_visibility;
    if (m_window == nullptr || mode == 0) {
        return;  // 默认：游戏启动后仍保持不变
    }
    HWND hwnd = m_window->hwnd();
    if (hwnd == nullptr) {
        return;
    }
    if (mode == 4) {
        m_window->minimize();
        return;
    }
    if (mode == 1) {
        m_window->close_window();
        return;
    }
    if (mode != 2 && mode != 3) {
        return;
    }
    ::ShowWindow(hwnd, SW_HIDE);
    // 隐藏后等游戏进程退出：mode 2 关闭启动器，mode 3 重新显示。
    // 用 weak_ptr 避免后台线程把页面对象延长到程序结束。
    const std::weak_ptr<HomePage> weak = weak_from_this();
    HANDLE handle = static_cast<HANDLE>(process.process_handle);
    std::thread([weak, handle, hwnd, mode]() {
        if (handle != nullptr) {
            ::WaitForSingleObject(handle, INFINITE);
        }
        std::shared_ptr<HomePage> page = weak.lock();
        if (!page || page->m_window == nullptr) {
            return;
        }
        page->m_window->post_to_ui([weak, hwnd, mode]() {
            std::shared_ptr<HomePage> alive = weak.lock();
            if (!alive || alive->m_window == nullptr) {
                return;
            }
            if (mode == 3) {
                ::ShowWindow(hwnd, SW_SHOW);
                ::SetForegroundWindow(hwnd);
                alive->m_window->invalidate();
            } else {
                alive->m_window->close_window();
            }
        });
    }).detach();
}

void HomePage::start_launch() {
    if (m_launching) {
        return;
    }
    if (download_busy && download_busy()) {
        set_status(L"有下载任务进行中，请先完成或取消");
        if (request_repaint) {
            request_repaint();
        }
        return;
    }
    if (m_selected_id.empty()) {
        if (on_open_downloads) {
            on_open_downloads();
        }
        return;
    }

    m_launching = true;
    m_launch->text = L"启动中…";
    m_launch->enabled = false;
    m_version_select->visible = false;
    m_cancel_launch->visible = true;  // D-028
    set_status(L"正在准备启动 " + widen(m_selected_id) + L" …");
    if (request_repaint) {
        request_repaint();
    }

    const std::string version_id = m_selected_id;
    std::shared_ptr<sscl::core::AccountManager> accounts = m_accounts;
    m_launch_stop = std::stop_source();
    std::stop_token token = m_launch_stop.get_token();
    std::shared_ptr<HomePage> self = shared_from_this();

    auto finish = [self](const std::wstring& status_text, bool launch_ok) {
        self->m_launching = false;
        self->m_launch->enabled = true;
        self->m_launch->text = L"启动游戏";
        self->m_launch->icon = "play_arrow";
        self->m_version_select->visible = true;
        self->m_cancel_launch->visible = false;
        self->set_status(status_text);
        if (launch_ok) {
            self->update_version_caption();
        }
        if (self->request_repaint) {
            self->request_repaint();
        }
    };

    // D-104：「与启动器尺寸一致」在 UI 线程取客户区，避免后台线程读 HWND。
    int launcher_width = 0;
    int launcher_height = 0;
    if (m_window != nullptr && m_window->hwnd() != nullptr) {
        RECT client{};
        if (::GetClientRect(m_window->hwnd(), &client)) {
            launcher_width = client.right - client.left;
            launcher_height = client.bottom - client.top;
        }
    }
    const sscl::core::LauncherConfig launch_config = m_launch_config;
    std::thread([self, accounts, version_id, token, finish, launcher_width, launcher_height,
                 launch_config]() {
        // 账号：选中项（微软账号过期则静默续登）；无账号回退离线 Player。
        sscl::core::AccountInfo account;
        std::optional<sscl::core::AccountInfo> selected = accounts->selected();
        if (selected.has_value()) {
            account = selected.value();
            sscl::util::Result<void> valid = accounts->ensure_valid(&account);
            if (valid.is_error()) {
                self->m_window->post_to_ui([self, valid, finish]() {
                    finish(L"登录已过期：" + widen(valid.error().message) + L"（请重新登录）", true);
                });
                return;
            }
        } else {
            sscl::core::OfflineAccount provider;
            sscl::util::Result<sscl::core::AccountInfo> offline = provider.login("Player");
            if (offline.is_error()) {
                self->m_window->post_to_ui([self, offline, finish]() {
                    finish(L"启动失败：" + widen(offline.error().message), true);
                });
                return;
            }
            account = offline.value();
        }
        if (token.stop_requested()) {
            self->m_window->post_to_ui([finish]() { finish(L"已取消启动", true); });
            return;
        }

        // D-118：外置登录账号需要在 JVM 前注入 authlib-injector（首次自动下载组件）。
        std::string authlib_jvm_args;
        if ((account.type == sscl::core::AccountType::kAuthlibInjector ||
             account.type == sscl::core::AccountType::kNide8) &&
            !account.server_url.empty()) {
            const auto stage = [self](const std::string& text) {
                if (text.empty()) {
                    return;
                }
                self->m_window->post_to_ui(
                    [self, text]() { self->set_status(widen(text)); });
            };
            stage("正在准备外置登录组件…");
            sscl::util::Result<std::filesystem::path> injector =
                sscl::core::ensure_authlib_injector(sscl::platform::data_dir(), stage);
            if (injector.is_error()) {
                self->m_window->post_to_ui([self, injector, finish]() {
                    finish(L"外置登录组件准备失败：" + widen(injector.error().message), true);
                });
                return;
            }
            authlib_jvm_args =
                sscl::core::authlib_javaagent_argument(account, injector.value());
            if (token.stop_requested()) {
                self->m_window->post_to_ui([finish]() { finish(L"已取消启动", true); });
                return;
            }
        }

        sscl::core::LaunchRequest request;
        request.version_id = version_id;
        request.account = account;
        request.settings.jvm_args = authlib_jvm_args;
        // D-107：自动配置按可用物理内存推算；自定义用设置页滑块值。
        request.settings.memory_mb =
            launch_config.memory_mode == 1
                ? (std::max)(512, (std::min)(16384, launch_config.memory_custom_mb))
                : sscl::platform::recommended_game_memory_mb();
        request.settings.auto_chinese_language = launch_config.auto_chinese_language;
        request.settings.java_path = launch_config.java_path;             // M3：手动 Java
        request.settings.java_scan_dirs = launch_config.java_scan_dirs;   // M3：额外扫描目录
        request.settings.java_candidates = launch_config.java_candidates;  // D-115：优先级列表
        // D-104：设置页「启动选项」→ 启动请求（全局值；版本级留空时才会用到）。
        request.settings.window_title = launch_config.game_window_title;
        request.settings.version_info = launch_config.custom_info;
        request.settings.process_priority = launch_config.process_priority;
        switch (launch_config.window_size_mode) {
            case 1:
                request.settings.fullscreen = true;
                break;
            case 2:
                if (launcher_width >= 320 && launcher_height >= 240) {
                    request.settings.width = launcher_width;
                    request.settings.height = launcher_height;
                }
                break;
            case 3:
                request.settings.width = launch_config.window_custom_width;
                request.settings.height = launch_config.window_custom_height;
                break;
            case 4:
                request.settings.maximize_window = true;
                break;
            default:
                break;
        }

        // D-052：启动前补全/Java 检测阶段实时反馈，避免用户长时间看到"启动中…"干等。
        // 文案只保留"阶段：子状态"，当前文件名路径过长不再拼入（改由 set_status 截断，D-053）。
        const auto progress = [self](const sscl::core::InstallProgress& report) {
            std::wstring text;
            if (!report.phase.empty()) {
                text += widen(report.phase);
            }
            if (!report.detail.empty()) {
                if (!text.empty()) {
                    text += L"：";
                }
                text += widen(report.detail);
            }
            if (text.empty()) {
                return;
            }
            self->m_window->post_to_ui([self, text]() { self->set_status(text); });
        };
        sscl::util::Result<sscl::core::LaunchOutcome> result =
            self->m_service->launch_installed(request, token, progress);

        self->m_window->post_to_ui([self, result, finish]() {
            if (result.is_ok()) {
                finish(L"已启动游戏（PID " + std::to_wstring(result.value().process.pid) + L"）",
                       false);
                sscl::util::log_info("[ui] game launched pid=" +
                                     std::to_string(result.value().process.pid));
                self->apply_launcher_visibility(result.value().process);
                return;
            }
            const std::string& code = result.error().code;
            if (code == "download.cancelled" || code == "launch.cancelled") {
                finish(L"已取消启动", true);  // D-028
                return;
            }
            finish(L"启动失败：" + widen(result.error().message), true);
            sscl::util::log_error("[ui] launch failed: " + code + " / " +
                                  result.error().detail);
        });
    }).detach();
}

}  // namespace sscl::ui
