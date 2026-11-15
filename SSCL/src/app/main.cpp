// SSCL 程序入口
// 流程：Per-Monitor V2 DPI → 数据目录（§4.3）→ 日志 → 窗口/渲染器/GameService/AppShell
// → 消息循环。GUI 对标 PCL2（D-013）：AppShell 负责顶部导航与页面管理。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "core/account/account_manager.hpp"
#include "core/game/game_service.hpp"
#include "core/game/launcher_config.hpp"
#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"
#include "ui/framework/motion.hpp"
#include "ui/framework/renderer.hpp"
#include "ui/framework/remote_image.hpp"
#include "ui/framework/splash.hpp"
#include "ui/framework/window.hpp"
#include "ui/setup/setup_wizard.hpp"
#include "ui/shell/app_shell.hpp"
#include "ui/theme/theme.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace {

int show_error_and_exit(const std::string& message, const std::string& detail) {
    std::wstring text = sscl::util::utf8_to_wide(message);
    if (!detail.empty()) {
        text += L"\n";
        text += sscl::util::utf8_to_wide(detail);
    }
    ::MessageBoxW(nullptr, text.c_str(), L"SSCL", MB_OK | MB_ICONERROR);
    return 1;
}

// 真实 Windows 版本（GetVersionEx 受 manifest 影响；RtlGetVersion 不谎报）。
std::string get_windows_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        auto rtl_get_version =
            reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr && rtl_get_version(&info) == 0) {
            return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
                   "." + std::to_string(info.dwBuildNumber);
        }
    }
    return "10.0";
}

sscl::core::InstallLayout make_layout(const std::filesystem::path& data_dir) {
    sscl::core::InstallLayout layout;
    layout.game_dir = data_dir / L"minecraft";
    layout.versions_dir = layout.game_dir / L"versions";
    layout.libraries_dir = layout.game_dir / L"libraries";
    layout.assets_dir = layout.game_dir / L"assets";
    return layout;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int cmd_show) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    sscl::util::Result<std::filesystem::path> data = sscl::platform::ensure_data_dir();
    if (data.is_error()) {
        return show_error_and_exit(data.error().message, data.error().detail);
    }

    const std::filesystem::path log_dir = data.value() / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    sscl::util::Logger::instance().init(sscl::util::wide_to_utf8(log_dir.wstring()),
                                        sscl::util::LogLevel::kInfo);
    sscl::util::log_info("SSCL starting (frameless+glass v2, D-016)");

    // 启动画面（D-084）：对象在 try 之外，任何异常/失败路径都能经析构自动收尾。
    sscl::ui::SplashScreen splash;

    // 顶层异常防线：任何未捕获异常不得静默以 0xC0000409 终止（会表现为“无窗口闪退”）。
    try {
    // 主题（D-025）：启动即按 Windows 应用模式选玫瑰粉 浅/深 调色板。
    sscl::ui::apply_system_theme();

    // 双击后立即出现（首帧提交完成才返回），早于配置/账号/渲染器/D2D 首帧的串行耗时。
    splash.show(instance);

    sscl::ui::Renderer renderer;
    sscl::ui::AppWindow window;

    // 远程图标（M4 附加页）：缓存目录 + UI 线程派发，后台线程只负责取字节/解码。
    sscl::ui::RemoteImage::instance().configure(
        data.value() / L"cache" / L"icons",
        [&window](std::function<void()> fn) { window.post_to_ui(std::move(fn)); });

    sscl::core::OsInfo os;
    os.name = "windows";
    os.version = get_windows_version();
    os.arch = "x86_64";
    std::shared_ptr<sscl::core::GameService> game_service =
        std::make_shared<sscl::core::GameService>(make_layout(data.value()), os);

    // 启动器配置（§9.1：<data_dir>/config.json）
    const std::filesystem::path config_path = data.value() / L"config.json";
    // D-124：首次启动判定 = 一次 GetFileAttributes（不读文件、不解析 JSON）。
    // 必须放在 AppShell 构造**之前**：它会在 Java 扫描目录为空时自动填充并立即写出
    // config.json；放到之后判断就会永远得到 false。出错时保守视为非首次，避免反复弹窗。
    std::error_code probe_ec;
    const bool config_exists = std::filesystem::exists(config_path, probe_ec);
    const bool first_run = !probe_ec && !config_exists;
    sscl::util::Result<sscl::core::LauncherConfig> config =
        sscl::core::load_launcher_config(config_path);
    sscl::core::LauncherConfig runtime_config =
        config.is_ok() ? config.value() : sscl::core::LauncherConfig{};
    // 外观（M5）：config.json 的主题偏好覆盖启动时的系统检测（system = 跟随 Windows）。
    sscl::ui::apply_theme_preference(runtime_config.theme_mode);
    splash.update_theme();  // 启动画面同步为用户实际选择的浅/深配色（D-084）
    // 动效强度（D-081）：默认完整；不跟随系统「减少动态效果」，完全由用户选择决定。
    sscl::ui::motion::set_level(
        sscl::ui::motion::level_from_key(runtime_config.motion_level));
    if (!runtime_config.selected_mc_folder.empty()) {
        game_service->set_game_folder(std::filesystem::path(
            sscl::util::utf8_to_wide(runtime_config.selected_mc_folder)));
    }
    if (first_run) {
        // 立刻落盘默认配置：即便向导被强杀/异常退出，下次启动也不会重复弹出。
        if (sscl::util::Result<void> marked =
                sscl::core::save_launcher_config(config_path, runtime_config);
            marked.is_error()) {
            sscl::util::log_warn("[setup] first-run marker save failed: " +
                                 marked.error().detail);
        }
    }

    // 账号（M2/C-10：<data_dir>/accounts.dat，DPAPI）
    std::shared_ptr<sscl::core::AccountManager> accounts =
        std::make_shared<sscl::core::AccountManager>(data.value() / L"accounts.dat");
    if (sscl::util::Result<void> loaded = accounts->load(); loaded.is_error()) {
        sscl::util::log_warn("[accounts] load failed: " + loaded.error().detail);
    }
    accounts->select_uuid(runtime_config.selected_account_uuid);

    const auto persist_config =
        [&config_path, &runtime_config](const sscl::core::LauncherConfig& updated) {
            runtime_config = updated;
            sscl::util::Result<void> saved = sscl::core::save_launcher_config(config_path,
                                                                              runtime_config);
            if (saved.is_error()) {
                sscl::util::log_warn("[config] save failed: " + saved.error().detail);
            }
        };

    sscl::ui::AppShell shell(&window, game_service, accounts, runtime_config, persist_config);

    // D-124：只有首次启动才构造向导。非首次启动这里是一个空 shared_ptr，
    // 后续回调仅多一次指针判空，不创建窗口/设备、不做任何扫描。
    std::shared_ptr<sscl::ui::SetupWizard> wizard;
    if (first_run) {
        wizard = std::make_shared<sscl::ui::SetupWizard>(
            &window, runtime_config, data.value() / L"secrets.dat",
            [&shell](const sscl::core::LauncherConfig& updated) {
                shell.apply_settings(updated);  // 同步到所有页面 + 立即落盘
            });
    }
    const auto wizard_active = [&wizard]() { return wizard && wizard->active(); };

    // 窗口位置记忆（D-017）：销毁前捕获位置/大小/最大化，与配置同盘。
    window.on_destroy = [&window, &config_path, &runtime_config]() {
        sscl::core::WindowPlacement placement{};
        if (window.capture_placement(&placement.x, &placement.y, &placement.width,
                                     &placement.height, &placement.maximized)) {
            placement.valid = true;
            runtime_config.window = placement;
            sscl::util::Result<void> saved =
                sscl::core::save_launcher_config(config_path, runtime_config);
            if (saved.is_error()) {
                sscl::util::log_warn("[config] save window placement failed: " +
                                     saved.error().detail);
            }
        }
    };

    // 窗口 → 外壳 接线（§5.1：回调均在 UI 线程串行执行）
    // D-124：向导激活时作为模态覆盖层，吞掉所有输入并绘制在最上层。
    // 非首次启动时 wizard 为空指针，这里只多一次判空，不引入任何启动开销。
    window.on_resize = [&renderer, &shell, &window, &wizard](UINT width, UINT height) {
        renderer.resize(width, height);
        shell.layout(width, height, renderer.dpi());
        if (wizard && wizard->active()) {
            const float dpi = static_cast<float>(renderer.dpi());
            const float to_dip = dpi > 0.0f ? 96.0f / dpi : 1.0f;
            wizard->layout(static_cast<float>(width) * to_dip,
                           static_cast<float>(height) * to_dip, dpi);
        }
        window.invalidate();
    };
    window.on_paint = [&renderer, &shell, &wizard]() {
        renderer.render_frame([&shell, &wizard](ID2D1RenderTarget* target,
                                               IDWriteFactory* dwrite) {
            shell.draw(target, dwrite);
            if (wizard && wizard->active()) {
                wizard->draw(target, dwrite);
            }
        });
    };
    window.on_mouse_move = [&window, &shell, &wizard](float x, float y) {
        if (wizard && wizard->active()) {
            if (wizard->mouse_move(x, y)) {
                window.invalidate();
            }
            return;
        }
        if (shell.mouse_move(x, y)) {
            window.invalidate();
        }
    };
    window.on_mouse_down = [&window, &shell, &wizard](float x, float y) {
        if (wizard && wizard->active()) {
            if (wizard->mouse_down(x, y)) {
                window.invalidate();
            }
            return;
        }
        if (shell.mouse_down(x, y)) {
            window.invalidate();
        }
    };
    window.on_mouse_up = [&window, &shell, &wizard](float x, float y) {
        if (wizard && wizard->active()) {
            if (wizard->mouse_up(x, y)) {
                window.invalidate();
            }
            return;
        }
        if (shell.mouse_up(x, y)) {
            window.invalidate();
        }
    };
    window.on_mouse_right_down = [&window, &shell, &wizard](float x, float y) {
        if (wizard && wizard->active()) {
            return;  // 模态：右键也不穿透
        }
        if (shell.mouse_right_down(x, y)) {
            window.invalidate();
        }
    };
    window.on_mouse_leave = [&window, &shell, &wizard]() {
        if (wizard && wizard->active()) {
            if (wizard->mouse_leave()) {
                window.invalidate();
            }
            return;
        }
        if (shell.mouse_leave()) {
            window.invalidate();
        }
    };
    window.on_mouse_wheel = [&window, &shell, &wizard](float x, float y, float delta) {
        if (wizard && wizard->active()) {
            if (wizard->mouse_wheel(x, y, delta)) {
                window.invalidate();
            }
            return;
        }
        if (shell.mouse_wheel(x, y, delta)) {
            window.invalidate();
        }
    };
    window.on_char = [&window, &shell, &wizard](wchar_t ch) {
        if (wizard && wizard->active()) {
            if (wizard->handle_char(ch)) {
                window.invalidate();
            }
            return;
        }
        if (shell.handle_char(ch)) {
            window.invalidate();
        }
    };
    window.on_key_down = [&window, &shell, &wizard](unsigned long key) {
        if (wizard && wizard->active()) {
            if (wizard->handle_key_down(key)) {
                window.invalidate();
            }
            return;
        }
        if (shell.handle_key_down(key)) {
            window.invalidate();
        }
    };
    // 动画帧（D-021）：vsync 自驱循环；tick 返回 false 时窗口侧停止渲染。
    window.on_frame = [&shell, &wizard](UINT dt_ms) {
        bool active = shell.tick(dt_ms);
        if (wizard && wizard->active() && wizard->tick(static_cast<float>(dt_ms))) {
            active = true;
        }
        return active;
    };
    // 固定 0.1s 系统状态刷新（批次 43）：CPU/RAM 显示，与动画帧循环解耦。
    window.on_status_timer = [&shell]() { shell.on_status_timer(); };

    sscl::util::Result<void> created = window.create(L"SSCL", 1024, 640);
    if (created.is_error()) {
        sscl::util::log_error("create window failed: " + created.error().detail);
        splash.dismiss();
        return show_error_and_exit(created.error().message, created.error().detail);
    }

    // 恢复上次关闭时的窗口位置（D-017）；无效则保持创建时的居中默认。
    if (runtime_config.window.valid) {
        window.apply_placement(runtime_config.window.x, runtime_config.window.y,
                               runtime_config.window.width, runtime_config.window.height,
                               runtime_config.window.maximized);
    }

    sscl::util::Result<void> graphics = renderer.initialize(window.hwnd(), window.dpi());
    if (graphics.is_error()) {
        sscl::util::log_error("initialize renderer failed: " + graphics.error().detail);
        splash.dismiss();
        return show_error_and_exit(graphics.error().message, graphics.error().detail);
    }

    RECT client{};
    ::GetClientRect(window.hwnd(), &client);
    const UINT client_width = static_cast<UINT>(client.right - client.left);
    const UINT client_height = static_cast<UINT>(client.bottom - client.top);
    shell.layout(client_width, client_height, renderer.dpi());
    if (wizard_active()) {
        const float dpi = static_cast<float>(renderer.dpi());
        const float to_dip = dpi > 0.0f ? 96.0f / dpi : 1.0f;
        // 必须在 show() 之前布局：show() 会先提交首帧，向导要出现在第一帧里。
        wizard->layout(static_cast<float>(client_width) * to_dip,
                       static_cast<float>(client_height) * to_dip, dpi);
    }
    window.invalidate();

    window.show(cmd_show);
    // 主窗口首帧已提交并显示，启动画面固定 150ms 淡出（不阻塞主线程）。
    splash.dismiss();
    window.start_status_timer();  // 0.1s CPU/RAM 刷新（首次采样已由 AppShell 构造完成）
    const int exit_code = window.run_message_loop();
    sscl::util::log_info("SSCL exit, code=" + std::to_string(exit_code));
    sscl::util::Logger::instance().shutdown();
    return exit_code;
    } catch (const std::exception& ex) {
        sscl::util::log_error(std::string("uncaught exception: ") + ex.what());
        splash.dismiss();  // 错误弹窗出现前先让启动画面淡出，避免遮挡
        sscl::util::Logger::instance().shutdown();
        return show_error_and_exit("程序发生未处理异常，已终止", ex.what());
    } catch (...) {
        sscl::util::log_error("uncaught exception: unknown");
        splash.dismiss();
        sscl::util::Logger::instance().shutdown();
        return show_error_and_exit("程序发生未处理异常，已终止", "未知异常");
    }
}
