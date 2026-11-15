#pragma once

// 启动器配置（开发文档 §9.1：<data_dir>/config.json）。
// 当前承载 M1 所需最小项（玩家名、选中版本、窗口位置，D-015/D-017）；M5 设置页扩展。
// JSON + 原子写（write_file_atomic）；损坏文件回退默认值不抛异常。

#include <filesystem>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

// 窗口位置（物理像素；WINDOWPLACEMENT 往返语义，D-017 位置记忆）。
struct WindowPlacement {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

// 多 MC 文件夹（PCL LaunchFolders）：名称>路径；type 0=Vanilla 1=RenamedVanilla 2=Custom。
struct McFolderConfig {
    std::string name;
    std::string path;
    int type = 2;
};

struct LauncherConfig {
    std::string player_name = "Player";  // 离线玩家名（M1 默认）
    std::string selected_version_id;     // 启动页选中的版本；空 = 未选
    std::string selected_account_uuid;   // 选中账号 uuid（C-10；空 = 首项）
    WindowPlacement window;              // 上次关闭时的窗口位置

    // 外观（M5 设置页）：主题偏好 "system"（跟随 Windows）/ "light" / "dark"。
    // 非法值按 "system" 处理（见 theme::apply_theme_preference）。
    std::string theme_mode = "system";

    // 动效强度（M5 / D-081）："full"（默认）/ "soft" / "reduced" / "off"。
    // 非法值按 "full"；由 ui::motion::level_from_key 解析，作用于全程序动画。
    std::string motion_level = "full";

    // Java（M3）：手动指定 + 额外扫描目录。空 = 自动检测。
    // java_path 可为 java.exe / bin 目录 / Java 根目录（validate_java_path 语义）。
    std::string java_path;
    // 额外扫描根（如 "D:\\JDK"）；便携 JDK 不在 Program Files 时用。
    std::vector<std::string> java_scan_dirs;

    // D-115：Java 优先级列表（有序；越靠前越优先）。元素可为 java.exe / bin 目录 /
    // JDK 根目录，启动时按顺序挑第一个满足版本要求的运行时。空 = 走自动检测。
    std::vector<std::string> java_candidates;
    // D-115：java_scan_dirs 中由程序自动添加的项（首次启动 / 「自动扫描」按钮）。
    // 不在本列表中的项视为用户手动添加，用于设置页显示来源标签。
    std::vector<std::string> java_scan_dirs_auto;

    // 版本隔离（D-056）：0=关 1=仅加载器 2=仅非正式 3=非正式或加载器 4=全部（默认）。
    // 只影响之后新安装的版本；每版本覆盖见 versions/<id>/sscl-instance.json。
    int version_isolation = 4;

    // —— 启动选项（D-104，设置页「游戏 → 启动选项」）——
    // 游戏窗口标题：空 = 不更改；由启动后的游戏窗口 watcher 用 SetWindowTextW 应用。
    // 版本级 window_title 非空时优先于本全局值。
    std::string game_window_title;
    // 自定义信息：空 = 用版本自带 versionType；非空替换 ${version_type}
    // （显示在游戏主界面左下 / F3）。版本级 version_info 非空时优先。
    // PCL 约定禁止引号字符，输入时由设置页过滤。
    std::string custom_info;
    // 启动器可见性：0 保持不变（默认）/ 1 启动后立即关闭 /
    // 2 隐藏且游戏退出后自动关闭 / 3 隐藏且游戏退出后重新打开 / 4 最小化。
    int launcher_visibility = 0;
    // 进程优先级：0 低 / 1 中（平衡，默认）/ 2 高 → SetPriorityClass。
    int process_priority = 1;
    // 窗口大小：0 默认 / 1 全屏 / 2 与启动器尺寸一致 /
    // 3 自定义尺寸 / 4 最大化（watcher 用 SW_MAXIMIZE）。
    int window_size_mode = 0;
    int window_custom_width = 1280;   // 模式 3 使用
    int window_custom_height = 720;
    // —— 设置页「其他」（D-107）——
    // 下载及列表网络源：0 尽量镜像 / 1 官方优先+加载缓慢时换镜像（默认）/ 2 尽量官方。
    int download_source = 1;
    // 社区资源网络源：0 尽量镜像 / 1 仅官方加载缓慢时改镜像 / 2 尽量官方（默认）。
    int community_source = 2;
    // 内存分配：0 自动配置（默认，按可用物理内存推算）/ 1 自定义。
    int memory_mode = 0;
    // 自定义内存（MB），滑块范围 512..min(物理内存-1024, 16384)，256MB 步进。
    int memory_custom_mb = 4096;
    // 辅助功能：启动前把 options.txt 的 lang 设为 zh_cn（默认勾选）。
    bool auto_chinese_language = true;

    // —— 网络代理（D-109，设置页「其他 → 网络代理」）——
    // 0 使用 Windows 系统代理（默认，含 PAC）/ 1 手动 HTTP 代理 / 2 全部直连。
    int proxy_mode = 0;
    std::string proxy_host = "127.0.0.1";
    int proxy_port = 10800;

    // 是否已把“功能上线前就存在的版本”迁移为“不隔离”（一次性，已废弃）。
    bool instances_migrated = false;

    // PCL McFolderSelected：空 = <data>/minecraft；否则为绝对路径。
    std::string selected_mc_folder;
    // PCL McFolderList：用户命名/自定义的文件夹；Vanilla 当前/官启由扫描补足。
    std::vector<McFolderConfig> mc_folders;

    // Mod 下载（D-058）：依赖模式 0=询问 1=自动 2=从不；平台偏好 + 上次目标版本。
    int mod_dependency_mode = 0;
    std::string mod_platform = "modrinth";
    std::string mod_last_target_version;
};

// 读取配置；文件缺失或损坏返回默认配置（is_ok）。
util::Result<LauncherConfig> load_launcher_config(const std::filesystem::path& path);

// 原子写配置。
util::Result<void> save_launcher_config(const std::filesystem::path& path,
                                        const LauncherConfig& config);

}  // namespace sscl::core

