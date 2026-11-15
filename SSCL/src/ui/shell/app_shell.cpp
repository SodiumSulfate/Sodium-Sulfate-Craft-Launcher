#include "ui/shell/app_shell.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <d2d1helper.h>

#include "resource.h"
#include "core/download/mirror_map.hpp"
#include "core/java/java_manager.hpp"
#include "core/task/task_center.hpp"
#include "platform/http_client.hpp"
#include "platform/shell_utils.hpp"
#include "ui/framework/anim.hpp"
#include "ui/framework/icon_manager.hpp"
#include "ui/framework/image_loader.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/window.hpp"
#include "ui/shell/shell_layout.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::ui {

namespace {

constexpr int kNavCount = 4;
const wchar_t* kNavLabels[kNavCount] = {L"启动", L"下载", L"附加", L"设置"};

// 右下下载悬浮窗（PCL MyExtraButton/BtnExtraDownload 对照，D-019/D-023 重设计）
constexpr float kFloatMarginDip = 15.0f;
constexpr float kFloatWidthDip = 240.0f;
constexpr float kFloatHeightDip = 52.0f;
constexpr float kFloatCircleDip = 34.0f;
constexpr float kFloatInsetDip = 9.0f;

// 超长任务名截断（胶囊固定宽，两行布局）
std::wstring truncate_label(const std::wstring& text, std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars - 1) + L"…";
}

D2D1_RECT_F rect_of(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

// 标题按钮几何（Win11：右上 3×46 DIP，满栏高）。
D2D1_RECT_F title_button_rect(float width_dip, int index) {
    const float left =
        width_dip - kWindowButtonsWidthDip + static_cast<float>(index) * kWindowButtonWidthDip;
    return rect_of(left, 0.0f, left + kWindowButtonWidthDip, kNavHeightDip);
}

// 动画推进统一用 ui/framework/anim.hpp（tween/ease_out，D-020/D-033）。

}  // namespace

int AppShell::title_button_hit(float x_dip, float y_dip, float width_dip) {
    if (y_dip < 0.0f || y_dip >= kNavHeightDip) {
        return -1;
    }
    const float zone_left = width_dip - kWindowButtonsWidthDip;
    if (x_dip < zone_left || x_dip >= width_dip) {
        return -1;
    }
    const int index =
        static_cast<int>((x_dip - zone_left) / kWindowButtonWidthDip);
    return index < 0 ? 0 : (index > 2 ? 2 : index);
}

AppShell::AppShell(AppWindow* window, std::shared_ptr<sscl::core::GameService> service,
                   std::shared_ptr<sscl::core::AccountManager> accounts,
                   sscl::core::LauncherConfig initial_config,
                   std::function<void(const sscl::core::LauncherConfig&)> on_config_changed)
    : m_window(window),
      m_service(std::move(service)),
      m_config(std::move(initial_config)),
      m_on_config_changed(std::move(on_config_changed)) {
    // 版本隔离（D-060，PCL 语义）：只设置全局策略；已有版本的隔离决策
    // 由 resolve_target 首次按 mods/saves + 全局策略判定，不再做“强制不隔离”迁移。
    m_service->set_isolation_mode(
        sscl::core::isolation_mode_from_int(m_config.version_isolation));

    // D-115：首次启动（扫描位置为空）按常见安装位置自动填充，标记为「程序添加」。
    // 只在空列表时执行，用户主动删空后不会再次注入。
    if (m_config.java_scan_dirs.empty()) {
        for (const std::filesystem::path& dir : sscl::core::default_java_scan_dirs()) {
            const std::string utf8 = sscl::util::wide_to_utf8(dir.wstring());
            if (utf8.empty()) {
                continue;
            }
            m_config.java_scan_dirs.push_back(utf8);
            m_config.java_scan_dirs_auto.push_back(utf8);
        }
        if (!m_config.java_scan_dirs.empty() && m_on_config_changed) {
            m_on_config_changed(m_config);  // 立即落盘，下次启动不再重复探测
        }
    }

    m_accounts = std::move(accounts);
    m_home = std::make_shared<HomePage>(m_window, m_service, m_accounts);
    m_version_select = std::make_shared<VersionSelectPage>(m_window, m_service);
    m_instance_detail = std::make_shared<InstanceDetailPage>(m_window, m_service);
    m_downloads = std::make_shared<VersionPage>(m_window, m_service);
    m_tasks = std::make_shared<TaskPage>(m_window, m_service);
    m_addons = std::make_shared<AddonsPage>(m_window, m_service, m_config);
    m_settings = std::make_shared<SettingsPage>(
        m_window, m_service, m_config,
        [this](const sscl::core::LauncherConfig& updated) { apply_settings(updated); });

    // 导航接线（PCL PageChange 行为对照）
    m_home->on_open_downloads = [this]() { switch_page(ShellPage::kDownloads); };
    m_home->on_open_version_select = [this]() {
        refresh_version_folders();
        m_version_select->refresh(m_config.selected_version_id);
        switch_page(ShellPage::kVersionSelect);
    };
    m_home->on_account_selected = [this](const std::string& uuid) {
        if (uuid != m_config.selected_account_uuid) {
            m_config.selected_account_uuid = uuid;
            notify_config_changed();
        }
    };
    // D-118：账号管理统一走独立子窗口；子窗口切换/新增后同步配置与主页卡片。
    m_home->on_open_account_manager = [this]() { open_account_manager(); };
    m_account_window = std::make_unique<AccountWindow>(m_accounts);
    m_account_window->on_selected = [this](const std::string& uuid) {
        if (uuid != m_config.selected_account_uuid) {
            m_config.selected_account_uuid = uuid;
            notify_config_changed();
        }
        if (m_home) {
            m_home->refresh_accounts();
        }
    };

    m_version_select->on_select = [this](const std::string& version_id) {
        m_config.selected_version_id = version_id;
        notify_config_changed();
        refresh_home_state();
        switch_page(ShellPage::kHome);
    };
    m_version_select->on_open_instance = [this](const std::string& version_id) {
        m_instance_detail->set_instance(version_id);
        switch_page(ShellPage::kInstanceDetail);
    };
    m_instance_detail->on_back = [this]() {
        refresh_version_folders();
        m_version_select->refresh(m_config.selected_version_id);
        switch_page(ShellPage::kVersionSelect);
    };
    m_instance_detail->on_renamed = [this](const std::string& old_id,
                                           const std::string& new_id) {
        if (m_config.selected_version_id == old_id) {
            m_config.selected_version_id = new_id;
        }
        notify_config_changed();
        refresh_home_state();
    };
    m_instance_detail->on_deleted = [this](const std::string& version_id) {
        if (m_config.selected_version_id == version_id) {
            m_config.selected_version_id.clear();
        }
        notify_config_changed();
        refresh_home_state();
    };
    m_instance_detail->on_changed = [this]() {
        notify_config_changed();
        refresh_home_state();
    };
    m_instance_detail->on_open_downloads = [this]() {
        switch_page(ShellPage::kDownloads);
    };
    m_version_select->on_open_downloads = [this]() { switch_page(ShellPage::kDownloads); };
    m_version_select->on_back = [this]() { switch_page(ShellPage::kHome); };
    m_version_select->on_switch_folder = [this](const std::filesystem::path& folder) {
        m_service->set_game_folder(folder);
        m_config.selected_mc_folder = sscl::util::wide_to_utf8(folder.wstring());
        m_config.selected_version_id.clear();
        notify_config_changed();
        refresh_home_state();
        refresh_version_folders();
        m_version_select->refresh(m_config.selected_version_id);
    };
    m_version_select->on_add_folder = [this](const std::filesystem::path& folder,
                                             const std::string& name) {
        const std::string path = sscl::util::wide_to_utf8(folder.wstring());
        bool updated = false;
        for (auto& item : m_config.mc_folders) {
            if (item.path == path) {
                item.name = name;
                updated = true;
                break;
            }
        }
        if (!updated) {
            sscl::core::McFolderConfig item;
            item.name = name;
            item.path = path;
            item.type = 2;
            m_config.mc_folders.push_back(std::move(item));
        }
        m_service->set_game_folder(folder);
        m_config.selected_mc_folder = path;
        m_config.selected_version_id.clear();
        notify_config_changed();
        refresh_home_state();
        refresh_version_folders();
        m_version_select->refresh(m_config.selected_version_id);
    };
    m_version_select->on_rename_folder = [this](const std::filesystem::path& folder,
                                                const std::string& name) {
        const std::string path = sscl::util::wide_to_utf8(folder.wstring());
        bool found = false;
        for (auto& item : m_config.mc_folders) {
            if (item.path == path) {
                item.name = name;
                found = true;
                break;
            }
        }
        if (!found) {
            sscl::core::McFolderConfig item;
            item.name = name;
            item.path = path;
            item.type = 1;
            m_config.mc_folders.push_back(std::move(item));
        }
        notify_config_changed();
        refresh_version_folders();
    };
    m_version_select->on_remove_folder = [this](const std::filesystem::path& folder) {
        const std::string path = sscl::util::wide_to_utf8(folder.wstring());
        const std::string current =
            sscl::util::wide_to_utf8(m_service->layout().game_dir.wstring());
        if (path == current || path == m_config.selected_mc_folder) {
            m_version_select->set_message(L"当前文件夹不能移出，请先切换到其他文件夹");
            return;
        }
        m_config.mc_folders.erase(
            std::remove_if(m_config.mc_folders.begin(), m_config.mc_folders.end(),
                           [&path](const sscl::core::McFolderConfig& item) {
                               return item.path == path;
                           }),
            m_config.mc_folders.end());
        notify_config_changed();
        refresh_version_folders();
    };
    m_version_select->on_delete_folder = [this](const std::filesystem::path& folder) {
        const std::string path = sscl::util::wide_to_utf8(folder.wstring());
        const std::string current =
            sscl::util::wide_to_utf8(m_service->layout().game_dir.wstring());
        if (path == current || path == m_config.selected_mc_folder) {
            m_version_select->set_message(L"当前文件夹不能删除，请先切换到其他文件夹");
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        if (ec) {
            m_version_select->set_message(L"删除文件夹失败：" +
                                          sscl::util::utf8_to_wide(ec.message()));
            return;
        }
        m_config.mc_folders.erase(
            std::remove_if(m_config.mc_folders.begin(), m_config.mc_folders.end(),
                           [&path](const sscl::core::McFolderConfig& item) {
                               return item.path == path;
                           }),
            m_config.mc_folders.end());
        notify_config_changed();
        refresh_version_folders();
    };
    m_version_select->on_refresh_folder = [this](const std::filesystem::path&) {
        refresh_version_folders();
        m_version_select->refresh(m_config.selected_version_id);
    };

    // 下载 → 组件选择（D-040）→ 任务页（D-019）。原版走既有 install_full；
    // 含加载器时走 install_with_loader（D-044）。同一时间仅一个安装任务（D-020）。
    m_downloads->on_start_install = [this](const sscl::core::VersionListEntry& entry) {
        if (!m_tasks->is_running()) {
            sscl::core::InstallJob job;
            job.minecraft = entry;
            job.loader.kind = sscl::core::LoaderKind::kNone;
            m_tasks->start_install(job);
        }
        switch_page(ShellPage::kTasks);
    };
    m_downloads->on_start_loader_job = [this](const sscl::core::InstallJob& job) {
        if (!m_tasks->is_running()) {
            m_tasks->start_install(job);
        }
        switch_page(ShellPage::kTasks);
    };
    m_tasks->on_back = [this]() { switch_page(ShellPage::kDownloads); };
    // 任务状态变化（开始/结束）→ 悬浮窗弹出/收起动画帧
    m_tasks->on_state_changed = [this]() {
        if (m_window != nullptr) {
            m_window->start_frame_loop();
        }
    };
    // D-023：取消确认后返回上一页面（仅当仍在任务页时）
    m_tasks->on_task_cancelled = [this]() {
        if (m_current == ShellPage::kTasks && m_prev_page != ShellPage::kTasks) {
            switch_page(m_prev_page);
        }
    };
    // D-047：安装完成后自动退回上一页面（下载页/首页；首页会刷新已安装列表）
    m_tasks->on_task_done = [this]() {
        if (m_current == ShellPage::kTasks && m_prev_page != ShellPage::kTasks) {
            switch_page(m_prev_page);
        }
    };
    // 附加页平台切换 → 持久化到 config。
    m_addons->on_platform_changed = [this](sscl::core::ModPlatform platform) {
        const char* key = sscl::core::mod_platform_key(platform);
        if (m_config.mod_platform != key) {
            m_config.mod_platform = key;
            notify_config_changed();
        }
    };

    // 启动与下载互斥（D-020）
    m_home->download_busy = [this]() { return m_tasks != nullptr && m_tasks->is_running(); };

    // Java 选择（M3）：config.json 的 java_path / java_scan_dirs 透传给启动流程。
    m_home->set_java_settings(m_config.java_path, m_config.java_scan_dirs);
    m_home->set_launch_config(m_config);  // D-104：启动选项同步到首页
    // D-107：网络源策略写入进程级全局，供下载/清单/Mod 平台即时读取。
    sscl::core::set_global_source_policy(
        sscl::core::make_source_policy(m_config.download_source, m_config.community_source));
    // D-109：代理模式写入 platform 层全局，下一次 HTTP 请求即采用。
    sscl::platform::set_proxy_config(sscl::platform::make_proxy_config(
        m_config.proxy_mode, m_config.proxy_host, m_config.proxy_port));

    // 页面重绘请求接线（D-033）：此前 request_repaint 未接线，异步状态/动画不刷新。
    // 统一 = 启动动画帧循环 + 失效重绘；帧循环在 tick 收敛后自动停止（空闲不绘制）。
    const auto repaint = [this]() {
        if (m_window != nullptr) {
            m_window->start_frame_loop();
            m_window->invalidate();
        }
    };
    m_home->request_repaint = repaint;
    m_version_select->request_repaint = repaint;
    m_instance_detail->request_repaint = repaint;
    m_downloads->request_repaint = repaint;
    m_tasks->request_repaint = repaint;
    m_addons->request_repaint = repaint;
    m_settings->request_repaint = repaint;

    // D-098：后台任务中心的进度通知 → UI 线程重绘。这样即使当前不在任务页，
    // 右下角悬浮窗也能跟随后台 Mod 下载的进度。
    if (m_window != nullptr) {
        AppWindow* window = m_window;
        sscl::core::TaskCenter::instance().set_listener([window]() {
            window->post_to_ui([window]() {
                window->start_frame_loop();
                window->invalidate();
            });
        });
    }

    refresh_home_state();
    refresh_version_folders();
    refresh_system_stats();  // 建立 CPU 基线并填充左侧系统信息
}

void AppShell::refresh_version_folders() {
    if (m_version_select == nullptr || m_service == nullptr) {
        return;
    }
    const std::filesystem::path official =
        sscl::platform::roaming_appdata_dir() / L".minecraft";
    const auto folders = sscl::core::scan_mc_folders(
        m_service->layout().game_dir, official, m_config.mc_folders,
        m_config.selected_mc_folder);
    m_version_select->set_folders(folders);
}

void AppShell::refresh_system_stats() {
    m_last_system_sample_tick = ::GetTickCount64();
    m_system_load = m_system_monitor.sample();
    if (m_tasks != nullptr) {
        m_tasks->set_system_stats(m_system_load);
    }
}

void AppShell::on_status_timer() {
    // D-114：帧循环活跃时 tick 已按 100ms 采过，这里按时间戳去重，避免重复采样
    // 导致 CPU 增量窗口过短而抖动；帧循环停下后本定时器仍每 0.1s 兜底。
    const unsigned long long now = ::GetTickCount64();
    if (now - m_last_system_sample_tick >= 90ULL) {
        refresh_system_stats();
    }
    // D-099：附加页的临时状态（「已加入后台任务…」）到期自动恢复默认文案。
    if (m_addons != nullptr && m_addons->tick_status(100.0f) && m_window != nullptr) {
        m_window->start_frame_loop();
        m_window->invalidate();
    }
    // D-102：页面进入等待时可能只调 invalidate（未启动帧循环）；0.1s 定时器兜底启动。
    // 等待刚结束时也强制重绘一帧，清掉残留指示器（部分回调只改状态、不主动重绘）。
    const bool busy = current() != nullptr && current()->is_busy();
    if (m_window != nullptr && (busy || m_busy_prev != busy)) {
        m_window->start_frame_loop();  // 幂等：已在循环中则直接返回
        if (m_busy_prev != busy) {
            m_window->invalidate();
        }
    }
    m_busy_prev = busy;
}

void AppShell::notify_config_changed() {
    if (m_on_config_changed) {
        m_on_config_changed(m_config);
    }
}

// D-118：打开独立账号管理子窗口（首次调用时创建，关闭只是隐藏）。
void AppShell::open_account_manager() {
    if (!m_account_window) {
        return;
    }
    m_account_window->open(m_window != nullptr ? m_window->hwnd() : nullptr);
}

// 设置页写回的配置（M5）：先落盘，再把即时生效的项应用到运行中的服务/页面。
void AppShell::apply_settings(const sscl::core::LauncherConfig& updated) {
    const bool isolation_changed = updated.version_isolation != m_config.version_isolation;
    const bool java_changed = updated.java_path != m_config.java_path ||
                              updated.java_scan_dirs != m_config.java_scan_dirs ||
                              updated.java_candidates != m_config.java_candidates;
    m_config = updated;
    // D-107：设置页改动后下一次网络请求立即采用新策略。
    sscl::core::set_global_source_policy(
        sscl::core::make_source_policy(m_config.download_source, m_config.community_source));
    // D-109：代理设置同样即时生效（含后台正在排队的请求）。
    sscl::platform::set_proxy_config(sscl::platform::make_proxy_config(
        m_config.proxy_mode, m_config.proxy_host, m_config.proxy_port));
    if (isolation_changed && m_service != nullptr) {
        m_service->set_isolation_mode(
            sscl::core::isolation_mode_from_int(m_config.version_isolation));
    }
    if (m_home != nullptr) {
        // D-104：启动选项改完立即对下一次启动生效。
        m_home->set_launch_config(m_config);
        if (java_changed) {
            m_home->set_java_settings(m_config.java_path, m_config.java_scan_dirs);
        }
    }
    if (m_settings != nullptr) {
        m_settings->set_config(m_config);
    }
    notify_config_changed();
}

void AppShell::refresh_home_state() {
    if (m_service == nullptr) {
        return;
    }
    const std::vector<sscl::core::InstalledVersion> installed = m_service->installed_versions();
    std::string selected = m_config.selected_version_id;
    bool found = false;
    for (const auto& item : installed) {
        if (item.id == selected) {
            found = true;
            break;
        }
    }
    if (!found) {
        selected = installed.empty() ? std::string{} : installed.front().id;
        if (selected != m_config.selected_version_id) {
            m_config.selected_version_id = selected;
            notify_config_changed();
        }
    }
    m_home->set_installed_versions(installed, selected);
}

void AppShell::switch_page(ShellPage page) {
    if (m_current == ShellPage::kTasks && page != ShellPage::kTasks) {
        m_tasks->set_visible(false);  // 批次 42：离开任务页 → 收缩日志/释放活动文件表
    }
    if (page == ShellPage::kHome) {
        refresh_home_state();  // 返回首页时刷新已安装列表（下载安装后可能变化）
    }
    if (page == ShellPage::kDownloads && !m_downloads->loaded()) {
        m_downloads->refresh();  // 首次进入自动拉取清单（PCL 行为）
    }
    if (page != m_current) {
        // 页面切换：新页以透明度覆盖旧页（D-020/D-093）。
        m_prev_page = m_current;
        m_page_anim = 0.0f;
        m_page_anim_active = true;
        m_page_anim_fresh = true;
        // D-083：切换页面时清空上一页的悬停态。
        // 内容淡入**不**重启：页面过渡本身已负责渐显，叠加会造成双重变暗（闪黑）。
        motion::hot().clear();
        motion::content().jump();
        motion::modal().jump_closed();
        motion::menu().jump_closed();
    }
    if (page == ShellPage::kSettings && m_settings != nullptr) {
        m_settings->set_config(m_config);  // 其它页面可能改过配置（选中版本/账号等）
    }
    if (page == ShellPage::kAddons && m_addons != nullptr) {
        m_addons->on_enter();  // PCL：首次进入自动加载下载排行
    }
    m_current = page;
    if (page == ShellPage::kTasks) {
        m_tasks->set_visible(true);
        m_tasks->set_system_stats(m_system_load);  // 立即用最近一次采样刷新左侧信息
    }
    if (m_window != nullptr) {
        m_window->start_frame_loop();
        m_window->invalidate();
    }
}

Page* AppShell::current() {
    switch (m_current) {
        case ShellPage::kHome:
            return m_home.get();
        case ShellPage::kVersionSelect:
            return m_version_select.get();
        case ShellPage::kInstanceDetail:
            return m_instance_detail.get();
        case ShellPage::kDownloads:
            return m_downloads.get();
        case ShellPage::kTasks:
            return m_tasks.get();
        case ShellPage::kAddons:
            return m_addons.get();
        case ShellPage::kSettings:
            return m_settings.get();
    }
    return m_home.get();
}

Page* AppShell::page_for(ShellPage page) const {
    switch (page) {
        case ShellPage::kHome:
            return m_home.get();
        case ShellPage::kVersionSelect:
            return m_version_select.get();
        case ShellPage::kInstanceDetail:
            return m_instance_detail.get();
        case ShellPage::kDownloads:
            return m_downloads.get();
        case ShellPage::kTasks:
            return m_tasks.get();
        case ShellPage::kAddons:
            return m_addons.get();
        case ShellPage::kSettings:
            return m_settings.get();
    }
    return m_home.get();
}

bool AppShell::tick(UINT dt_ms) {
    bool active = false;
    const float dt = static_cast<float>(dt_ms);

    // D-114：系统负载改由帧循环采样。WM_TIMER 是低优先级消息，在 60fps 自驱渲染
    // （如加载器处理器阶段的扫光动画）下会被持续产生的 WM_PAINT 饿死，表现为
    // CPU/内存数字冻结。这里每 100ms 采一次；帧循环停下后由 on_status_timer 兜底。
    m_system_accum_ms += dt;
    if (m_system_accum_ms >= 100.0f) {
        m_system_accum_ms = 0.0f;
        refresh_system_stats();
    }

    // D-105：页面常在 draw() 里才同步 menu/modal 状态，而 WM_PAINT 是「先 tick 再 draw」。
    // 输入后的第一帧 update_all 看不到刚启动的浮层动画，会把帧循环收敛掉，
    // 表现为「点击后必须移动鼠标，下拉/弹层才出现」。输入事件置 2 帧保活，
    // 第二帧的 update_all 即可接管动画推进，之后由动画自身 active 续帧。
    if (m_keep_frames > 0) {
        --m_keep_frames;
        active = true;
    }

    // D-083：全局动画状态层单点驱动（hover/pressed 交叉渐变、内容淡入、
    // 模态弹层与菜单开合）。所有页面共用，保证逐页行为完全一致。
    if (motion::update_all(dt)) {
        active = true;
    }
    // D-102：加载指示器相位全局单点推进（各页只负责上报忙碌，避免重复加速）。
    motion::tick_spinner(dt);

    if (m_page_anim_active) {
        // 页面切换时长随「动效强度」缩放；关闭档直接落到终点（不产生中间帧）。
        const float page_ms = motion::duration(motion::Kind::kPage);
        if (page_ms <= 0.0f) {
            m_page_anim = 1.0f;
            m_page_anim_active = false;
            m_page_anim_fresh = false;
        } else if (m_page_anim_fresh) {
            // 切换当帧只呈现过渡起点（旧页整屏），下一帧再开始推进。
            m_page_anim_fresh = false;
            active = true;
        } else {
            m_page_anim += dt / page_ms;
            if (m_page_anim >= 1.0f) {
                m_page_anim = 1.0f;
                m_page_anim_active = false;
            } else {
                active = true;
            }
        }
    }

    for (int i = 0; i < kNavCount; ++i) {
        if (motion::tween(m_nav_hover_anim[i], m_hover_nav == i ? 1.0f : 0.0f, dt,
                          motion::Kind::kHover)) {
            active = true;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (motion::tween(m_title_hover_anim[i], m_hover_title_button == i ? 1.0f : 0.0f, dt,
                          motion::Kind::kHover)) {
            active = true;
        }
    }
    if (motion::tween(m_float_anim, float_widget_visible() ? 1.0f : 0.0f, dt,
                      motion::Kind::kPop)) {
        active = true;
    }
    if (motion::tween(m_float_hover_anim, m_hover_float ? 1.0f : 0.0f, dt,
                      motion::Kind::kHover)) {
        active = true;
    }

    if (current()->tick(dt)) {
        active = true;
    }
    // D-102：任一页面处于前台等待时保持帧循环，否则转圈会静止。
    if (current() != nullptr && current()->is_busy()) {
        active = true;
    }
    return active;
}

AppShell::~AppShell() {
    // D-098：后台任务可能比外壳活得更久，销毁前摘掉监听。
    sscl::core::TaskCenter::instance().clear_listener();
}

void AppShell::layout(UINT width, UINT height, UINT dpi) {
    m_to_dip = 96.0f / static_cast<float>(dpi == 0 ? 96 : dpi);
    m_width_dip = static_cast<float>(width) * m_to_dip;
    m_height_dip = static_cast<float>(height) * m_to_dip;

    m_home->layout(width, height, dpi);
    m_version_select->layout(width, height, dpi);
    m_instance_detail->layout(width, height, dpi);
    m_downloads->layout(width, height, dpi);
    m_tasks->layout(width, height, dpi);
    m_addons->layout(width, height, dpi);
    m_settings->layout(width, height, dpi);
}

void AppShell::draw_busy_indicator(ID2D1RenderTarget* target, IDWriteFactory* dwrite,
                                   const std::wstring& text) const {
    if (target == nullptr || m_width_dip <= 0.0f || m_height_dip <= 0.0f) {
        return;
    }
    // 胶囊宽度按字符估算：ASCII 7px、全角 12px；夹在 [240, 440]，避免长文案出界。
    float text_width = 0.0f;
    for (const wchar_t ch : text) {
        text_width += ch < 128 ? 7.0f : 12.0f;
    }
    float width = text_width + 96.0f;
    if (width < 240.0f) width = 240.0f;
    if (width > 440.0f) width = 440.0f;
    constexpr float kHeight = 56.0f;
    const float cx = m_width_dip * 0.5f;
    const float cy = (kNavHeightDip + m_height_dip) * 0.5f;
    const D2D1_RECT_F box = rect_of(cx - width * 0.5f, cy - kHeight * 0.5f,
                                    cx + width * 0.5f, cy + kHeight * 0.5f);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surface;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border;
    if (FAILED(target->CreateSolidColorBrush(theme_elevated_surface(), surface.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(theme_border(), border.GetAddressOf()))) {
        return;
    }
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(box, theme_radius(), theme_radius());
    target->FillRoundedRectangle(rounded, surface.Get());
    target->DrawRoundedRectangle(rounded, border.Get(), 1.0f);

    // 16 段渐隐圆环：相位由 AppShell::tick 单点推进，转速随「动效强度」缩放。
    motion::draw_spinner(target, D2D1::Point2F(box.left + 30.0f, cy), 11.0f,
                         theme_primary(), 0.95f, 2.4f);

    if (dwrite == nullptr || text.empty()) {
        return;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        12.0f, L"zh-cn", format.GetAddressOf()))) {
        return;
    }
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    if (FAILED(target->CreateSolidColorBrush(theme_text_primary(), text_brush.GetAddressOf()))) {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                      rect_of(box.left + 54.0f, box.top, box.right - 16.0f, box.bottom),
                      text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);
}

void AppShell::draw_nav_bar(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bar;
    if (FAILED(target->CreateSolidColorBrush(theme_nav_background(), bar.GetAddressOf()))) {
        return;
    }
    target->FillRectangle(rect_of(0.0f, 0.0f, m_width_dip, kNavHeightDip), bar.Get());

    // Logo 图标（resources/sscl.rc 的 IDR_SSCL_APP_ICON；位图缓存于 m_logo_bitmap）
    if (m_logo_bitmap == nullptr || m_logo_target != target) {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        HRSRC resource = ::FindResourceW(instance, MAKEINTRESOURCEW(IDR_SSCL_APP_ICON), RT_RCDATA);
        if (resource != nullptr) {
            HGLOBAL loaded = ::LoadResource(instance, resource);
            const void* data = loaded != nullptr ? ::LockResource(loaded) : nullptr;
            const DWORD size = data != nullptr ? ::SizeofResource(instance, resource) : 0;
            if (data != nullptr && size > 0) {
                std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(data),
                                                static_cast<const std::uint8_t*>(data) + size);
                m_logo_bitmap = load_bitmap_from_ico(target, bytes, 32);
                m_logo_target = target;
            }
        }
    }
    if (m_logo_bitmap != nullptr) {
        target->DrawBitmap(m_logo_bitmap.Get(), rect_of(12.0f, 11.0f, 37.0f, 36.0f), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Logo
    Microsoft::WRL::ComPtr<IDWriteTextFormat> logo_format;
    if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           17.0f, L"zh-cn", logo_format.GetAddressOf()))) {
        logo_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> logo_brush;
        target->CreateSolidColorBrush(theme_nav_text(), logo_brush.GetAddressOf());
        const std::wstring logo = L"SSCL";
        target->DrawTextW(logo.c_str(), static_cast<UINT32>(logo.size()), logo_format.Get(),
                          rect_of(42.0f, 0.0f, kNavLogoWidthDip, kNavHeightDip), logo_brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    // 导航项（PCL 标题栏 Tab：白字 + 当前项半透明白胶囊）
    int active = 0;
    switch (m_current) {
        case ShellPage::kHome:
        case ShellPage::kVersionSelect:
        case ShellPage::kInstanceDetail:
            active = 0;
            break;
        case ShellPage::kDownloads:
            active = 1;
            break;
        case ShellPage::kTasks:
            active = -1;  // 任务页为下载子页（PCL 任务管理非主 Tab），导航不高亮
            break;
        case ShellPage::kAddons:
            active = 2;
            break;
        case ShellPage::kSettings:
            active = 3;
            break;
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> item_format;
    if (FAILED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        13.0f, L"zh-cn", item_format.GetAddressOf()))) {
        return;
    }
    item_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    item_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    item_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> active_text_brush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> pill_brush;
    target->CreateSolidColorBrush(theme_nav_text(), text_brush.GetAddressOf());
    // 选中项文字用主文字色（PrimaryContainer 底上保证对比度，§八）
    target->CreateSolidColorBrush(theme_text_primary(), active_text_brush.GetAddressOf());
    target->CreateSolidColorBrush(theme_nav_item_active(), pill_brush.GetAddressOf());

    for (int i = 0; i < kNavCount; ++i) {
        const float left = kNavLogoWidthDip + 8.0f +
                           static_cast<float>(i) * (kNavItemWidthDip + 8.0f);
        const float top = (kNavHeightDip - kNavItemHeightDip) * 0.5f;
        const D2D1_RECT_F item =
            rect_of(left, top, left + kNavItemWidthDip, top + kNavItemHeightDip);
        if (i == active) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(item, 8.0f, 8.0f), pill_brush.Get());
        } else if (m_nav_hover_anim[i] > 0.01f) {
            // hover 渐变（D-020）
            pill_brush->SetOpacity(0.22f * m_nav_hover_anim[i]);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(item, 8.0f, 8.0f), pill_brush.Get());
            pill_brush->SetOpacity(1.0f);
        }
        const wchar_t* icons[] = {L"home", L"download", L"extension", L"settings"};
        IconManager::instance().draw(target, sscl::util::wide_to_utf8(icons[i]),
                                     rect_of(item.left + 20.0f, item.top + 6.0f,
                                             item.left + 38.0f, item.bottom - 6.0f),
                                     i == active ? theme_text_primary() : theme_nav_text());
        target->DrawTextW(kNavLabels[i], 2, item_format.Get(),
                          rect_of(item.left + 40.0f, item.top, item.right - 8.0f,
                                  item.bottom),
                          (i == active ? active_text_brush : text_brush).Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    draw_title_buttons(target);
}

// 自绘标题按钮（D-016）：几何字形（最小化—/最大化□/还原❐/关闭✕），Win11 悬停底色。
void AppShell::draw_title_buttons(ID2D1RenderTarget* target) const {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glyph;
    if (FAILED(target->CreateSolidColorBrush(theme_nav_text(), glyph.GetAddressOf()))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    if (FAILED(target->CreateSolidColorBrush(theme_titlebar_hover(), hover.GetAddressOf()))) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> close_hover;
    if (FAILED(target->CreateSolidColorBrush(theme_titlebar_close_hover(),
                                              close_hover.GetAddressOf()))) {
        return;
    }

    const bool maximized = (m_window != nullptr) && m_window->is_maximized();
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F rect = title_button_rect(m_width_dip, i);
        if (m_title_hover_anim[i] > 0.01f) {
            // hover 渐变（D-020）：SetOpacity 与画刷自身 alpha 相乘
            ID2D1SolidColorBrush* hover_brush = (i == 2) ? close_hover.Get() : hover.Get();
            hover_brush->SetOpacity(m_title_hover_anim[i]);
            target->FillRectangle(rect, hover_brush);
            hover_brush->SetOpacity(1.0f);
        }
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;

        if (i == 0) {
            // 最小化：水平线
            target->DrawLine(D2D1::Point2F(cx - 6.0f, cy), D2D1::Point2F(cx + 6.0f, cy),
                             glyph.Get(), 1.2f);
        } else if (i == 1) {
            if (maximized) {
                // 还原：两个偏移方框
                const D2D1_RECT_F back = rect_of(cx - 3.5f, cy - 6.5f, cx + 4.5f, cy + 1.5f);
                const D2D1_RECT_F front = rect_of(cx - 4.5f, cy - 1.5f, cx + 3.5f, cy + 6.5f);
                target->DrawRectangle(back, glyph.Get(), 1.1f);
                target->DrawRectangle(front, glyph.Get(), 1.1f);
            } else {
                // 最大化：方框
                const D2D1_RECT_F box = rect_of(cx - 5.5f, cy - 5.0f, cx + 5.5f, cy + 5.0f);
                target->DrawRectangle(box, glyph.Get(), 1.2f);
            }
        } else {
            // 关闭：✕
            target->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 5.5f), D2D1::Point2F(cx + 5.5f, cy + 5.5f),
                             glyph.Get(), 1.2f);
            target->DrawLine(D2D1::Point2F(cx + 5.5f, cy - 5.5f), D2D1::Point2F(cx - 5.5f, cy + 5.5f),
                             glyph.Get(), 1.2f);
        }
    }
}

// —— 右下下载悬浮窗（PCL BtnExtraDownload 行为对照）——————————————
// 显示条件 = 有下载任务运行中 且 当前不在任务页（PCL ShowCheck）；
// 点击 → 任务管理页（PCL Click → PageChange(TaskManager)）。

bool AppShell::float_widget_visible() const {
    if (m_tasks == nullptr || m_current == ShellPage::kTasks) {
        return false;
    }
    // D-098：后台任务（附加页 Mod 下载/安装）同样弹悬浮窗，用户切到任何页面都能看到。
    return m_tasks->is_running() || m_tasks->has_background_job();
}

D2D1_RECT_F AppShell::float_widget_rect() const {
    return rect_of(m_width_dip - kFloatMarginDip - kFloatWidthDip,
                   m_height_dip - kFloatMarginDip - kFloatHeightDip,
                   m_width_dip - kFloatMarginDip, m_height_dip - kFloatMarginDip);
}

bool AppShell::float_widget_hit(float x_dip, float y_dip) const {
    if (!float_widget_visible()) {
        return false;
    }
    const D2D1_RECT_F rect = float_widget_rect();
    return x_dip >= rect.left && x_dip <= rect.right && y_dip >= rect.top &&
           y_dip <= rect.bottom;
}

void AppShell::draw_float_widget(ID2D1RenderTarget* target, IDWriteFactory* dwrite) const {
    // 弹出动画（D-020）：下载开始时缩放弹入，结束后弹出隐藏
    if ((!float_widget_visible() && m_float_anim <= 0.01f) || target == nullptr ||
        dwrite == nullptr) {
        return;
    }
    const D2D1_RECT_F rect = float_widget_rect();
    const float mid_y = (rect.top + rect.bottom) * 0.5f;

    // 弹出缩放（0.92→1，围绕控件中心）
    const float scale = 0.92f + 0.08f * m_float_anim;
    const D2D1_POINT_2F widget_center = D2D1::Point2F((rect.left + rect.right) * 0.5f, mid_y);
    target->SetTransform(
        D2D1::Matrix3x2F::Scale(scale, scale, widget_center));

    // 柔和阴影 + 玻璃圆角矩形底（D-024：不再用胶囊形，固定圆角 12）
    constexpr float kFloatRadiusDip = 12.0f;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadow;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_card_shadow(), shadow.GetAddressOf()))) {
        for (int layer = 2; layer >= 1; --layer) {
            const float spread = static_cast<float>(layer) * 1.5f;
            const D2D1_RECT_F s = rect_of(rect.left, rect.top + spread, rect.right,
                                          rect.bottom + spread);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(s, kFloatRadiusDip + spread * 0.5f,
                                  kFloatRadiusDip + spread * 0.5f),
                shadow.Get());
        }
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> pill;
    // 浮层表面（D-026：Elevated Surface；hover 向 Hover Background 过渡）
    D2D1_COLOR_F pill_color = theme_elevated_surface();
    const D2D1_COLOR_F hover_color = theme_hover_background();
    const float ht = m_float_hover_anim;
    pill_color.r += (hover_color.r - pill_color.r) * ht;
    pill_color.g += (hover_color.g - pill_color.g) * ht;
    pill_color.b += (hover_color.b - pill_color.b) * ht;
    if (FAILED(target->CreateSolidColorBrush(pill_color, pill.GetAddressOf()))) {
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    target->FillRoundedRectangle(
        D2D1::RoundedRect(rect, kFloatRadiusDip, kFloatRadiusDip), pill.Get());

    // 圆形进度钮（PCL：内圆 + 白 18% 进度填充 + 图标；D-023 比例重调）
    const float circle_radius = kFloatCircleDip * 0.5f;
    const D2D1_POINT_2F center =
        D2D1::Point2F(rect.left + kFloatInsetDip + circle_radius, mid_y);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent;
    if (FAILED(target->CreateSolidColorBrush(theme_primary(), accent.GetAddressOf()))) {
        return;
    }
    target->FillEllipse(D2D1::Ellipse(center, circle_radius, circle_radius), accent.Get());

    // 进度填充（自底向上，圆形裁剪 —— PCL PanProgress 对照）
    const float fraction = m_tasks->progress_fraction();
    if (fraction > 0.001f) {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if (factory != nullptr) {
            Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> circle;
            if (SUCCEEDED(factory->CreateEllipseGeometry(
                    D2D1::Ellipse(center, circle_radius, circle_radius),
                    circle.GetAddressOf()))) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
                if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f),
                                                            fill.GetAddressOf()))) {
                    // 圆形裁剪用几何蒙版 Layer（PushAxisAlignedClip 仅支持轴对齐矩形）
                    D2D1_LAYER_PARAMETERS layer =
                        D2D1::LayerParameters(D2D1::InfiniteRect(), circle.Get());
                    target->PushLayer(&layer, nullptr);
                    const float fill_height = kFloatCircleDip * fraction;
                    target->FillRectangle(
                        rect_of(center.x - circle_radius, center.y + circle_radius - fill_height,
                                center.x + circle_radius, center.y + circle_radius),
                        fill.Get());
                    target->PopLayer();
                }
            }
        }
    }

    // 下载箭头（白色几何线）
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glyph;
    if (SUCCEEDED(target->CreateSolidColorBrush(theme_nav_text(), glyph.GetAddressOf()))) {
        const float gx = center.x;
        const float gy = center.y;
        target->DrawLine(D2D1::Point2F(gx, gy - 7.0f), D2D1::Point2F(gx, gy + 5.0f),
                         glyph.Get(), 1.6f);
        target->DrawLine(D2D1::Point2F(gx - 5.0f, gy + 0.5f), D2D1::Point2F(gx, gy + 5.5f),
                         glyph.Get(), 1.6f);
        target->DrawLine(D2D1::Point2F(gx + 5.0f, gy + 0.5f), D2D1::Point2F(gx, gy + 5.5f),
                         glyph.Get(), 1.6f);
    }

    // 任务 + 进度文字（D-023：两行布局——标题行 + 进度行，超长截断）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (SUCCEEDED(dwrite->CreateTextFormat(theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           12.5f, L"zh-cn", format.GetAddressOf()))) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> detail_brush;
        if (SUCCEEDED(target->CreateSolidColorBrush(theme_text_primary(),
                                                    text_brush.GetAddressOf())) &&
            SUCCEEDED(target->CreateSolidColorBrush(theme_text_secondary(),
                                                    detail_brush.GetAddressOf()))) {
            const float text_left = rect.left + kFloatInsetDip + kFloatCircleDip + 12.0f;
            const std::wstring title = truncate_label(m_tasks->activity_title(), 20);
            target->DrawTextW(title.c_str(), static_cast<UINT32>(title.size()), format.Get(),
                              rect_of(text_left, rect.top + 7.0f, rect.right - 14.0f,
                                      rect.top + 28.0f),
                              text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                              DWRITE_MEASURING_MODE_NATURAL);

            Microsoft::WRL::ComPtr<IDWriteTextFormat> detail_format;
            if (SUCCEEDED(dwrite->CreateTextFormat(
                    theme_font_family(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn",
                    detail_format.GetAddressOf()))) {
                detail_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                detail_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                const std::wstring detail = m_tasks->activity_detail();
                target->DrawTextW(detail.c_str(), static_cast<UINT32>(detail.size()),
                                  detail_format.Get(),
                                  rect_of(text_left, rect.top + 27.0f, rect.right - 14.0f,
                                          rect.top + 46.0f),
                                  detail_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                                  DWRITE_MEASURING_MODE_NATURAL);
            }
        }
    }

    target->SetTransform(D2D1::Matrix3x2F::Identity());  // 复位弹出缩放
}

void AppShell::draw(ID2D1RenderTarget* target, IDWriteFactory* dwrite) {
    if (target == nullptr) {
        return;
    }
    // D-083：每帧重新登记可交互矩形，页面在绘制按钮/行/tab 时调用 observe()。
    motion::hot().begin_frame();
    // D-108：不再用全局 content() 作为整页图层透明度。
    // content() 是全站单例，设置页/附加页/版本选择页在「页内」切换时会 restart 把它
    // 归零；但没有任何页面用 content().opacity() 做局部淡入，所以那个 restart 的唯一
    // 效果就是让整页（含页面自己铺的底色）以 opacity 0 绘制 → 露出窗口底色 = 闪黑。
    // 跨页转场由独立的 m_page_anim 负责（D-093 覆盖式），这里直接绘制。
    const auto draw_content = [&]() { current()->draw(target, dwrite); };
    if (m_page_anim_active && m_page_anim < 1.0f && page_for(m_prev_page) != current()) {
        // 覆盖式页面过渡（D-093）：旧页**整屏不透明**铺底，新页以图层透明度覆盖其上。
        // 之前是「旧页淡出 + 新页淡入」，两层 alpha 同时 <1，中间态会透出页面底色
        // （深色主题接近纯黑）→ 观感就是「瞬间闪黑再渐显」。现在任一帧的合成结果
        // 都不会比旧页更暗，且首帧固定呈现旧页（见 tick 的 m_page_anim_fresh）。
        Page* previous = page_for(m_prev_page);
        if (previous != nullptr) {
            previous->draw(target, dwrite);
            // 旧页只是背景板：不参与本帧命中登记，避免悬停命中旧页控件。
            motion::hot().begin_frame();
        }
        const float t = ease_out(m_page_anim);
        if (t >= 0.999f) {
            current()->draw(target, dwrite);
        } else {
            // 透明度直接用图层 opacity（与 motion::push_pop 同规），避免遮罩画刷在
            // PopLayer 前被释放、以及 mask 与变换叠加带来的实现差异。
            const float dy = motion::offset(motion::Kind::kPage) * (1.0f - t);
            const D2D1_MATRIX_3X2_F move = D2D1::Matrix3x2F::Translation(0.0f, dy);
            const D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, move, t,
                nullptr);
            target->PushLayer(&layer, nullptr);
            current()->draw(target, dwrite);
            target->PopLayer();
        }
    } else {
        draw_content();
    }
    // D-102：所有「前台等待」统一转圈。画在页面内容之后、导航条之前，
    // 背后始终是完整页面（不使用 early-return，避免 D-099 的弹层淡出黑屏问题）。
    if (current() != nullptr && current()->is_busy()) {
        draw_busy_indicator(target, dwrite, current()->busy_text());
    }
    draw_nav_bar(target, dwrite);
    draw_float_widget(target, dwrite);  // 右下下载悬浮窗（覆盖于页面之上）
}

bool AppShell::nav_hit(float x_dip, float y_dip, int* index) const {
    if (y_dip < 0.0f || y_dip >= kNavHeightDip) {
        return false;
    }
    const float top = (kNavHeightDip - kNavItemHeightDip) * 0.5f;
    if (y_dip < top || y_dip > top + kNavItemHeightDip) {
        return false;
    }
    for (int i = 0; i < kNavCount; ++i) {
        const float left = kNavLogoWidthDip + 8.0f +
                           static_cast<float>(i) * (kNavItemWidthDip + 8.0f);
        if (x_dip >= left && x_dip <= left + kNavItemWidthDip) {
            *index = i;
            return true;
        }
    }
    return false;
}

bool AppShell::mouse_move(float x, float y) {
    bool changed = false;
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    int index = -1;
    int hover = -1;
    if (nav_hit(x_dip, y_dip, &index)) {
        hover = index;
    }
    if (hover != m_hover_nav) {
        m_hover_nav = hover;
        changed = true;
    }

    const int button = title_button_hit(x_dip, y_dip, m_width_dip);
    if (button != m_hover_title_button) {
        m_hover_title_button = button;
        changed = true;
    }

    const bool float_hover = float_widget_hit(x_dip, y_dip);
    if (float_hover != m_hover_float) {
        m_hover_float = float_hover;
        changed = true;
    }

    if (y_dip >= kNavHeightDip) {
        // D-083：统一命中（DIP 坐标），页面无需各自维护 hover 渐变。
        motion::hot().pick(x_dip, y_dip);
        if (current()->mouse_move(x, y)) {
            changed = true;
        }
    } else {
        motion::hot().set_hover(D2D1_RECT_F{});
    }
    if (m_window != nullptr) {
        m_window->start_frame_loop();  // hover 渐变动画帧（D-020）
    }
    return changed;
}

bool AppShell::mouse_down(float x, float y) {
    const float y_dip = y * m_to_dip;
    if (y_dip < kNavHeightDip) {
        return false;
    }
    const bool handled = current()->mouse_down(x, y);
    // D-083：按下态以当前悬停矩形为准，统一 60ms 渐变。
    motion::hot().set_pressed(motion::hot().hover_rect());
    // 点击可能开启弹层/切换状态：启动帧循环，保证弹出/按下动画有帧可走（D-081）。
    if (handled && m_window != nullptr) {
        m_window->start_frame_loop();
    }
    if (handled) {
        m_keep_frames = 2;  // D-105
    }
    return handled;
}

bool AppShell::mouse_right_down(float x, float y) {
    const float y_dip = y * m_to_dip;
    if (y_dip < kNavHeightDip) {
        return false;
    }
    return current()->mouse_right_down(x, y);
}

bool AppShell::mouse_up(float x, float y) {
    const float x_dip = x * m_to_dip;
    const float y_dip = y * m_to_dip;

    // 标题按钮（无边框自绘，D-016）
    const int button = title_button_hit(x_dip, y_dip, m_width_dip);
    if (button >= 0 && m_window != nullptr) {
        switch (button) {
            case 0:
                m_window->minimize();
                break;
            case 1:
                m_window->toggle_maximize();
                break;
            case 2:
                m_window->close_window();
                break;
        }
        return true;
    }

    // 右下下载悬浮窗 → 任务管理页（PCL BtnExtraDownload 对照）
    if (float_widget_hit(x_dip, y_dip)) {
        switch_page(ShellPage::kTasks);
        return true;
    }

    int index = -1;
    if (nav_hit(x_dip, y_dip, &index)) {
        switch (index) {
            case 0:
                switch_page(ShellPage::kHome);
                break;
            case 1:
                switch_page(ShellPage::kDownloads);
                break;
            case 2:
                switch_page(ShellPage::kAddons);
                break;
            case 3:
                switch_page(ShellPage::kSettings);
                break;
        }
        return true;
    }
    if (y_dip < kNavHeightDip) {
        return false;
    }
    const bool handled = current()->mouse_up(x, y);
    motion::hot().set_pressed(D2D1_RECT_F{});  // 抬起即回弹（D-083）
    // 同上：抬起可能触发弹层打开或页面切换，必须保证后续动画帧。
    if (handled && m_window != nullptr) {
        m_window->start_frame_loop();
    }
    if (handled) {
        m_keep_frames = 2;  // D-105
    }
    return handled;
}

bool AppShell::mouse_leave() {
    bool changed = false;
    if (m_hover_nav != -1) {
        m_hover_nav = -1;
        changed = true;
    }
    if (m_hover_title_button != -1) {
        m_hover_title_button = -1;
        changed = true;
    }
    if (m_hover_float) {
        m_hover_float = false;
        changed = true;
    }
    // 离开窗口：悬停与按下都平滑回退（D-083）。
    motion::hot().set_hover(D2D1_RECT_F{});
    motion::hot().set_pressed(D2D1_RECT_F{});
    if (current()->mouse_leave()) {
        changed = true;
    }
    // 同类断路：hover 回退也是渐变动画，离开时同样需要帧循环才能收敛。
    if (changed && m_window != nullptr) {
        m_window->start_frame_loop();
    }
    return changed;
}

bool AppShell::mouse_wheel(float x, float y, float delta) {
    const bool handled = current()->mouse_wheel(x, y, delta);
    if (handled && m_window != nullptr) {
        // 惯性滚动：on_wheel 只写目标值，实际偏移由 SmoothScroll::tick 在每帧趋近。
        // 必须启动帧循环，否则画面停在原地，直到下一次鼠标移动（mouse_move 会启动循环）
        // 才画出一帧 —— 表现为「滚轮不滚、动一下光标才滚」（D-022）。
        m_window->start_frame_loop();
    }
    if (handled) {
        m_keep_frames = 2;  // D-105
    }
    return handled;
}

bool AppShell::handle_char(wchar_t ch) {
    const bool handled = current()->handle_char(ch);
    if (handled && m_window != nullptr) {
        m_window->start_frame_loop();
        m_keep_frames = 2;  // D-105
    }
    return handled;
}

bool AppShell::handle_key_down(unsigned long key) {
    const bool handled = current()->handle_key_down(key);
    if (handled && m_window != nullptr) {
        m_window->start_frame_loop();
        m_keep_frames = 2;  // D-105
    }
    return handled;
}

}  // namespace sscl::ui
