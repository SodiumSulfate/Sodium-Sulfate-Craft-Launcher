#include "core/launch/game_launcher.hpp"

#include <set>
#include <string>
#include <thread>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "platform/win_error.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

// D-104：游戏窗口标题 / 最大化无法写进命令行，只能在进程起来后找到主窗口再设置。
// 轮询最多 30 秒（Java 冷启动 + LWJGL 建窗），按 PID 过滤，跳过不可见/过小窗口。
struct GameWindowSearch {
    unsigned long pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK find_game_window(HWND hwnd, LPARAM param) {
    GameWindowSearch* search = reinterpret_cast<GameWindowSearch*>(param);
    DWORD owner_pid = 0;
    ::GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != static_cast<DWORD>(search->pid)) {
        return TRUE;
    }
    if (!::IsWindowVisible(hwnd) || ::GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT client{};
    if (!::GetClientRect(hwnd, &client) || client.right - client.left < 320 ||
        client.bottom - client.top < 240) {
        return TRUE;
    }
    search->found = hwnd;
    return FALSE;
}

void apply_game_window_options(unsigned long pid, std::wstring title, bool maximize) {
    if (title.empty() && !maximize) {
        return;
    }
    for (int attempt = 0; attempt < 120; ++attempt) {
        GameWindowSearch search{pid, nullptr};
        ::EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
        if (search.found != nullptr) {
            if (!title.empty()) {
                ::SetWindowTextW(search.found, title.c_str());
            }
            if (maximize) {
                ::ShowWindow(search.found, SW_MAXIMIZE);
            }
            return;
        }
        ::Sleep(250);
    }
}

util::Result<VersionJson> resolve_recursive(const VersionTextLoader& loader,
                                            const std::string& id,
                                            std::set<std::string>* resolved_so_far) {
    if (resolved_so_far->count(id) != 0) {
        // Version.java:318-320：环状 inheritsFrom 检测
        return util::Result<VersionJson>::fail(
            util::Error::make("version.inherit_cycle", "版本继承链存在环：" + id, id));
    }
    resolved_so_far->insert(id);

    util::Result<std::string> text = loader(id);
    if (text.is_error()) {
        return util::Result<VersionJson>::fail(std::move(text.error()));
    }
    util::Result<VersionJson> parsed = parse_version_json(text.value());
    if (parsed.is_error()) {
        return parsed;
    }
    VersionJson self = std::move(parsed.value());
    if (self.inherits_from.empty()) {
        return util::Result<VersionJson>::ok(std::move(self));
    }

    util::Result<VersionJson> parent =
        resolve_recursive(loader, self.inherits_from, resolved_so_far);
    if (parent.is_error()) {
        return parent;
    }
    return util::Result<VersionJson>::ok(merge_version(self, parent.value()));
}

}  // namespace

util::Result<VersionJson> resolve_version_chain(const VersionTextLoader& loader,
                                                const std::string& root_id) {
    if (!loader) {
        return util::Result<VersionJson>::fail(
            util::Error::make("version.bad_request", "版本加载器未设置", root_id));
    }
    std::set<std::string> resolved;
    return resolve_recursive(loader, root_id, &resolved);
}

util::Result<GameProcess> launch_game(const LaunchPlan& plan,
                                      const std::filesystem::path& working_dir) {
    // CreateProcessW 要求命令行缓冲区可写
    std::wstring command_line = sscl::util::utf8_to_wide(plan.command_line);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    const wchar_t* work_dir = working_dir.empty() ? nullptr : working_dir.c_str();
    // CREATE_NO_WINDOW：java.exe 是控制台程序，不带此标志会弹出黑色命令行窗口；
    // 仅抑制控制台窗口，游戏自己的图形窗口不受影响（D-020，负责人 2026-10-24）。
    const DWORD creation_flags = CREATE_NO_WINDOW;
    const BOOL created = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                          creation_flags, nullptr, work_dir, &startup, &process);
    if (!created) {
        const unsigned long err = ::GetLastError();
        return util::Result<GameProcess>::fail(
            util::Error::make("launch.create_failed", "无法启动游戏进程",
                              platform::format_win_error(err)));
    }

    // D-104：进程优先级（0 低 / 1 中 / 2 高）。
    const DWORD priority = plan.process_priority == 2
                               ? HIGH_PRIORITY_CLASS
                               : (plan.process_priority == 0 ? BELOW_NORMAL_PRIORITY_CLASS
                                                             : NORMAL_PRIORITY_CLASS);
    ::SetPriorityClass(process.hProcess, priority);
    // 窗口标题 / 最大化：交由后台 watcher 等窗口出现后应用。
    if (!plan.window_title.empty() || plan.maximize_window) {
        std::thread(apply_game_window_options, process.dwProcessId,
                    sscl::util::utf8_to_wide(plan.window_title),
                    plan.maximize_window).detach();
    }

    GameProcess game;
    game.pid = process.dwProcessId;
    game.process_handle = process.hProcess;
    game.thread_handle = process.hThread;
    return util::Result<GameProcess>::ok(game);
}

void close_game_process(GameProcess& process) {
    if (process.thread_handle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process.thread_handle));
        process.thread_handle = nullptr;
    }
    if (process.process_handle != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process.process_handle));
        process.process_handle = nullptr;
    }
}

}  // namespace sscl::core
