#pragma once

// 版本隔离与实例运行目录（D-056/D-060，PCL 2.99 PathIndie 语义）：
// - 每个版本可独立决定是否使用 `versions/<id>/` 作为 --gameDir（隔离）；
// - 隔离后 mods/saves/config 等相对目录全部落在版本目录；
// - 非隔离则使用全局 <game>/minecraft（SSCL 现有行为）；
// - 决策持久化到 `versions/<id>/sscl-instance.json`，避免目录漂移。
//
// 默认策略 kAll（隔离所有新安装版本）；首次判定顺序：
// 旧覆盖（decided）→ 版本目录 mods 有文件/saves 有子目录 → 全局策略。
// D-060 删除了“升级时把已有版本强制写为不隔离”的旧迁移。

#include <filesystem>
#include <string>

#include "core/game/file_plan.hpp"
#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

enum class IsolationMode {
    kOff = 0,               // 全部不隔离（全局 mods/saves）
    kModded = 1,            // 仅带加载器的版本隔离
    kNonRelease = 2,        // 仅非正式版隔离
    kNonReleaseOrModded = 3,
    kAll = 4,               // 全部隔离（默认，D-056）
};

struct InstanceSettings {
    // 隔离（PCL VersionArgumentIndieV2）
    bool isolated = false;
    bool decided = false;  // 是否已做过决策（首次判定后持久化）

    // 版本列表展示（PCL Setup.ini）
    bool starred = false;                 // IsStar
    int category = 0;                     // DisplayType：0=自动 1=隐藏 2=可安装Mod 3=常规 4=不常用 5=愚人节 6=错误
    std::string custom_info;              // 版本列表自定义描述
    std::string logo;                     // 自定义图标路径（空 = 自动）

    // 启动选项（PCL PageInstanceSetup）
    std::string window_title;             // VersionArgumentTitle
    std::string version_info;             // VersionArgumentInfo
    int java_mode = 0;                    // 0 自动 / 1 指定范围 / 2 版本文件夹 / 3 指定 Java
    std::string java_range;               // 例如 [17.0.1, 25.0)
    std::string java_path;                // 指定 Java 的 java.exe/bin/根目录
    int ram_type = 2;                     // 0 自动 / 1 自定义 / 2 跟随全局
    int ram_custom_gb = 4;                // 自定义内存（GB）
    int ram_optimize = 0;                 // 0 跟随全局 / 1 开启 / 2 关闭
    int server_login = 0;                 // 0 正版或离线 / 1 仅正版 / 2 仅离线 / 3 Nide8 / 4 Authlib
    std::string server_nide;              // VersionServerNide
    std::string server_auth_url;          // VersionServerAuthServer
    std::string server_auth_register;     // VersionServerAuthRegister
    std::string server_auth_name;         // VersionServerAuthName
    std::string server_enter;             // VersionServerEnter
    std::string jvm_args;                 // VersionAdvanceJvm
    std::string game_args;                // VersionAdvanceGame
    int gc_mode = 0;                      // 0 跟随全局 / 1 ZGC / 2 分代 ZGC / 3 G1 / 4 调优 G1 / 5 不指定
    std::string pre_launch;               // VersionAdvanceRun
    bool pre_launch_wait = false;         // VersionAdvanceRunWait
    bool disable_mod_update = false;      // VersionAdvanceDisableModUpdate
    bool disable_file_check = false;      // VersionAdvanceAssetsV2
    bool disable_jlw = false;             // VersionAdvanceDisableJLW
    bool disable_lua = false;             // VersionAdvanceDisableLUA
};

struct InstanceTarget {
    std::string version_id;
    std::string minecraft_version;
    std::string loader;          // fabric / forge / neoforge / quilt / 空
    std::string loader_version;
    std::filesystem::path game_dir;     // MC 根（PCL McFolderSelected）
    std::filesystem::path version_dir;  // versions/<id>（PCL PathVersion）
    std::filesystem::path run_dir;      // --gameDir（PCL PathIndie）
    std::filesystem::path mods_dir;     // run_dir/mods
    bool isolated = false;
    bool moddable = false;           // 非原版
    bool release = true;             // PCL IsRelease（非 Fool/Old/Snapshot/Unknown）
    int drop = 209;                  // PCL Drop
};

const char* isolation_mode_name(IsolationMode mode);
IsolationMode isolation_mode_from_int(int value);
int isolation_mode_to_int(IsolationMode mode);

// 给定“是否带加载器 / 是否正式版”，返回该全局策略下是否应隔离。
bool isolation_mode_wants(IsolationMode mode, bool modded, bool release);

// 读取/写入每版本覆盖配置；缺失或损坏返回 decided=false（不视为错误）。
util::Result<InstanceSettings> load_instance_settings(const std::filesystem::path& version_dir);
util::Result<void> save_instance_settings(const std::filesystem::path& version_dir,
                                          const InstanceSettings& settings);

bool version_dir_has_mods(const std::filesystem::path& version_dir);
bool version_dir_has_saves(const std::filesystem::path& version_dir);

// 加载器识别：优先 sscl 元数据，其次库名/主类（兼容手工导入的版本）。
std::string detect_loader_key(const VersionJson& version);
bool loader_is_moddable(const std::string& loader_key);

// MC 版本：sscl > inheritsFrom（加载器版本的父版本）> id 规则解析。
std::string detect_minecraft_version(const VersionJson& version, const std::string& version_id);

// 解析目标实例：读覆盖 → 首次按“版本目录 mods/saves + 全局策略”判定并（可选）持久化。
// merged 必须是已合并 inheritsFrom 的版本，version.id 用子版本 id。
util::Result<InstanceTarget> resolve_instance_target(const InstallLayout& layout,
                                                     IsolationMode mode,
                                                     const VersionJson& merged,
                                                     bool persist_decision);

}  // namespace sscl::core
