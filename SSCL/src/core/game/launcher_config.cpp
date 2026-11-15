#include "core/game/launcher_config.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"

namespace sscl::core {

util::Result<LauncherConfig> load_launcher_config(const std::filesystem::path& path) {
    LauncherConfig config;
    util::Result<std::string> text = platform::read_file(path);
    if (text.is_error()) {
        return util::Result<LauncherConfig>::ok(config);  // 缺失 = 默认
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text.value(), nullptr, false);
        if (!parsed.is_object()) {
            return util::Result<LauncherConfig>::ok(config);
        }
        if (parsed.contains("player_name") && parsed["player_name"].is_string()) {
            const std::string name = parsed["player_name"].get<std::string>();
            if (!name.empty()) {
                config.player_name = name;
            }
        }
        if (parsed.contains("selected_version_id") && parsed["selected_version_id"].is_string()) {
            config.selected_version_id = parsed["selected_version_id"].get<std::string>();
        }
        if (parsed.contains("selected_account_uuid") &&
            parsed["selected_account_uuid"].is_string()) {
            config.selected_account_uuid = parsed["selected_account_uuid"].get<std::string>();
        }
        // 外观（M5）：主题偏好三键；其它值保持默认 "system"
        if (parsed.contains("theme_mode") && parsed["theme_mode"].is_string()) {
            const std::string mode = parsed["theme_mode"].get<std::string>();
            if (mode == "system" || mode == "light" || mode == "dark") {
                config.theme_mode = mode;
            }
        }
        // 动效强度（D-081）：四档；非法值保持默认 "full"。
        if (parsed.contains("motion_level") && parsed["motion_level"].is_string()) {
            const std::string level = parsed["motion_level"].get<std::string>();
            if (level == "full" || level == "soft" || level == "reduced" || level == "off") {
                config.motion_level = level;
            }
        }
        // Java（M3）：手动路径 + 额外扫描目录
        if (parsed.contains("java_path") && parsed["java_path"].is_string()) {
            config.java_path = parsed["java_path"].get<std::string>();
        }
        if (parsed.contains("java_scan_dirs") && parsed["java_scan_dirs"].is_array()) {
            for (const nlohmann::json& item : parsed["java_scan_dirs"]) {
                if (item.is_string()) {
                    const std::string dir = item.get<std::string>();
                    if (!dir.empty()) {
                        config.java_scan_dirs.push_back(dir);
                    }
                }
            }
        }
        // Java 优先级与自动扫描目录（D-115）：去重、过滤空值与孤立项。
        if (parsed.contains("java_candidates") && parsed["java_candidates"].is_array()) {
            for (const nlohmann::json& item : parsed["java_candidates"]) {
                if (!item.is_string()) {
                    continue;
                }
                const std::string candidate = item.get<std::string>();
                if (candidate.empty() ||
                    std::find(config.java_candidates.begin(), config.java_candidates.end(),
                              candidate) != config.java_candidates.end()) {
                    continue;
                }
                config.java_candidates.push_back(candidate);
            }
        }
        if (parsed.contains("java_scan_dirs_auto") &&
            parsed["java_scan_dirs_auto"].is_array()) {
            for (const nlohmann::json& item : parsed["java_scan_dirs_auto"]) {
                if (!item.is_string()) {
                    continue;
                }
                const std::string directory = item.get<std::string>();
                // 自动项必须是扫描目录的子集，且自身不重复；旧配置可能缺失 java_scan_dirs。
                if (directory.empty() ||
                    std::find(config.java_scan_dirs.begin(), config.java_scan_dirs.end(),
                              directory) == config.java_scan_dirs.end() ||
                    std::find(config.java_scan_dirs_auto.begin(), config.java_scan_dirs_auto.end(),
                              directory) != config.java_scan_dirs_auto.end()) {
                    continue;
                }
                config.java_scan_dirs_auto.push_back(directory);
            }
        }
        // 版本隔离（D-056）：越界回退默认 4（全隔离）
        if (parsed.contains("version_isolation") &&
            parsed["version_isolation"].is_number_integer()) {
            const int mode = parsed["version_isolation"].get<int>();
            config.version_isolation = (mode >= 0 && mode <= 4) ? mode : 4;
        }
        // 启动选项（D-104）：字符串直接读；整数越界回退默认。
        if (parsed.contains("game_window_title") &&
            parsed["game_window_title"].is_string()) {
            config.game_window_title = parsed["game_window_title"].get<std::string>();
        }
        if (parsed.contains("custom_info") && parsed["custom_info"].is_string()) {
            config.custom_info = parsed["custom_info"].get<std::string>();
        }
        if (parsed.contains("launcher_visibility") &&
            parsed["launcher_visibility"].is_number_integer()) {
            const int v = parsed["launcher_visibility"].get<int>();
            config.launcher_visibility = (v >= 0 && v <= 4) ? v : 0;
        }
        if (parsed.contains("process_priority") &&
            parsed["process_priority"].is_number_integer()) {
            const int v = parsed["process_priority"].get<int>();
            config.process_priority = (v >= 0 && v <= 2) ? v : 1;
        }
        if (parsed.contains("window_size_mode") &&
            parsed["window_size_mode"].is_number_integer()) {
            const int v = parsed["window_size_mode"].get<int>();
            config.window_size_mode = (v >= 0 && v <= 4) ? v : 0;
        }
        if (parsed.contains("window_custom_width") &&
            parsed["window_custom_width"].is_number_integer()) {
            const int v = parsed["window_custom_width"].get<int>();
            config.window_custom_width = (v >= 320 && v <= 7680) ? v : 1280;
        }
        if (parsed.contains("window_custom_height") &&
            parsed["window_custom_height"].is_number_integer()) {
            const int v = parsed["window_custom_height"].get<int>();
            config.window_custom_height = (v >= 240 && v <= 4320) ? v : 720;
        }
        // 设置页「其他」（D-107）：整数越界回退默认，bool 缺失保持 true。
        if (parsed.contains("download_source") &&
            parsed["download_source"].is_number_integer()) {
            const int v = parsed["download_source"].get<int>();
            config.download_source = (v >= 0 && v <= 2) ? v : 1;
        }
        if (parsed.contains("community_source") &&
            parsed["community_source"].is_number_integer()) {
            const int v = parsed["community_source"].get<int>();
            config.community_source = (v >= 0 && v <= 2) ? v : 2;
        }
        if (parsed.contains("memory_mode") && parsed["memory_mode"].is_number_integer()) {
            const int v = parsed["memory_mode"].get<int>();
            config.memory_mode = (v == 1) ? 1 : 0;
        }
        if (parsed.contains("memory_custom_mb") &&
            parsed["memory_custom_mb"].is_number_integer()) {
            const int v = parsed["memory_custom_mb"].get<int>();
            config.memory_custom_mb = (v >= 512 && v <= 16384) ? v : 4096;
        }
        // 网络代理（D-109）：模式 0..2、端口 1..65535，非法回退默认。
        if (parsed.contains("proxy_mode") && parsed["proxy_mode"].is_number_integer()) {
            const int v = parsed["proxy_mode"].get<int>();
            config.proxy_mode = (v >= 0 && v <= 2) ? v : 0;
        }
        if (parsed.contains("proxy_host") && parsed["proxy_host"].is_string()) {
            const std::string v = parsed["proxy_host"].get<std::string>();
            config.proxy_host = v.empty() ? "127.0.0.1" : v;
        }
        if (parsed.contains("proxy_port") && parsed["proxy_port"].is_number_integer()) {
            const int v = parsed["proxy_port"].get<int>();
            config.proxy_port = (v >= 1 && v <= 65535) ? v : 10800;
        }
        if (parsed.contains("auto_chinese_language") &&
            parsed["auto_chinese_language"].is_boolean()) {
            config.auto_chinese_language = parsed["auto_chinese_language"].get<bool>();
        }
        if (parsed.contains("instances_migrated") &&
            parsed["instances_migrated"].is_boolean()) {
            config.instances_migrated = parsed["instances_migrated"].get<bool>();
        }
        if (parsed.contains("selected_mc_folder") &&
            parsed["selected_mc_folder"].is_string()) {
            config.selected_mc_folder =
                parsed["selected_mc_folder"].get<std::string>();
        }
        if (parsed.contains("mc_folders") && parsed["mc_folders"].is_array()) {
            for (const nlohmann::json& item : parsed["mc_folders"]) {
                if (!item.is_object()) {
                    continue;
                }
                McFolderConfig folder;
                if (item.contains("name") && item["name"].is_string()) {
                    folder.name = item["name"].get<std::string>();
                }
                if (item.contains("path") && item["path"].is_string()) {
                    folder.path = item["path"].get<std::string>();
                }
                if (item.contains("type") && item["type"].is_number_integer()) {
                    const int type = item["type"].get<int>();
                    folder.type = (type >= 0 && type <= 2) ? type : 2;
                }
                if (!folder.path.empty()) {
                    config.mc_folders.push_back(std::move(folder));
                }
            }
        }
        // Mod 下载（D-058）
        if (parsed.contains("mod_dependency_mode") &&
            parsed["mod_dependency_mode"].is_number_integer()) {
            const int mode = parsed["mod_dependency_mode"].get<int>();
            config.mod_dependency_mode = (mode >= 0 && mode <= 2) ? mode : 0;
        }
        if (parsed.contains("mod_platform") && parsed["mod_platform"].is_string()) {
            const std::string platform = parsed["mod_platform"].get<std::string>();
            if (platform == "curseforge" || platform == "modrinth") {
                config.mod_platform = platform;
            }
        }
        if (parsed.contains("mod_last_target_version") &&
            parsed["mod_last_target_version"].is_string()) {
            config.mod_last_target_version =
                parsed["mod_last_target_version"].get<std::string>();
        }
        // 窗口位置（D-017）：字段齐全才视为有效
        if (parsed.contains("window") && parsed["window"].is_object()) {
            const nlohmann::json& window = parsed["window"];
            if (window.contains("x") && window["x"].is_number_integer() &&
                window.contains("y") && window["y"].is_number_integer() &&
                window.contains("width") && window["width"].is_number_integer() &&
                window.contains("height") && window["height"].is_number_integer()) {
                config.window.valid = true;
                config.window.x = window["x"].get<int>();
                config.window.y = window["y"].get<int>();
                config.window.width = window["width"].get<int>();
                config.window.height = window["height"].get<int>();
                config.window.maximized =
                    window.contains("maximized") && window["maximized"].is_boolean() &&
                    window["maximized"].get<bool>();
            }
        }
    } catch (...) {
        // 损坏 = 默认配置
    }
    return util::Result<LauncherConfig>::ok(config);
}

util::Result<void> save_launcher_config(const std::filesystem::path& path,
                                        const LauncherConfig& config) {
    nlohmann::json out;
    out["player_name"] = config.player_name.empty() ? "Player" : config.player_name;
    out["selected_version_id"] = config.selected_version_id;
    out["selected_account_uuid"] = config.selected_account_uuid;
    out["theme_mode"] = config.theme_mode.empty() ? "system" : config.theme_mode;
    out["motion_level"] = config.motion_level.empty() ? "full" : config.motion_level;
    if (!config.java_path.empty()) {
        out["java_path"] = config.java_path;
    }
    if (!config.java_scan_dirs.empty()) {
        out["java_scan_dirs"] = config.java_scan_dirs;
    }
    // Java 优先级与自动扫描来源（D-115）。
    if (!config.java_candidates.empty()) {
        out["java_candidates"] = config.java_candidates;
    }
    if (!config.java_scan_dirs_auto.empty()) {
        out["java_scan_dirs_auto"] = config.java_scan_dirs_auto;
    }
    out["version_isolation"] = config.version_isolation;
    // 设置页「其他」（D-107）。
    out["download_source"] = config.download_source;
    out["community_source"] = config.community_source;
    out["memory_mode"] = config.memory_mode;
    out["memory_custom_mb"] = config.memory_custom_mb;
    out["auto_chinese_language"] = config.auto_chinese_language;
    // 网络代理（D-109）。
    out["proxy_mode"] = config.proxy_mode;
    out["proxy_host"] = config.proxy_host;
    out["proxy_port"] = config.proxy_port;
    // 启动选项（D-104）：空字符串不写，保持 config.json 精简。
    if (!config.game_window_title.empty()) {
        out["game_window_title"] = config.game_window_title;
    }
    if (!config.custom_info.empty()) {
        out["custom_info"] = config.custom_info;
    }
    out["launcher_visibility"] = config.launcher_visibility;
    out["process_priority"] = config.process_priority;
    out["window_size_mode"] = config.window_size_mode;
    out["window_custom_width"] = config.window_custom_width;
    out["window_custom_height"] = config.window_custom_height;
    if (config.instances_migrated) {
        out["instances_migrated"] = true;
    }
    if (!config.selected_mc_folder.empty()) {
        out["selected_mc_folder"] = config.selected_mc_folder;
    }
    if (!config.mc_folders.empty()) {
        nlohmann::json folders = nlohmann::json::array();
        for (const McFolderConfig& folder : config.mc_folders) {
            if (folder.path.empty()) {
                continue;
            }
            nlohmann::json item;
            item["name"] = folder.name;
            item["path"] = folder.path;
            item["type"] = folder.type;
            folders.push_back(std::move(item));
        }
        out["mc_folders"] = std::move(folders);
    }
    out["mod_dependency_mode"] = config.mod_dependency_mode;
    if (!config.mod_platform.empty()) {
        out["mod_platform"] = config.mod_platform;
    }
    if (!config.mod_last_target_version.empty()) {
        out["mod_last_target_version"] = config.mod_last_target_version;
    }
    if (config.window.valid && config.window.width > 0 && config.window.height > 0) {
        out["window"]["x"] = config.window.x;
        out["window"]["y"] = config.window.y;
        out["window"]["width"] = config.window.width;
        out["window"]["height"] = config.window.height;
        out["window"]["maximized"] = config.window.maximized;
    }
    // D-119：切换账号/改设置会在 UI 线程立即保存，不强制刷盘（仍原子替换）。
    return platform::write_file_atomic(path, out.dump(2), /*flush_to_disk=*/false);
}

}  // namespace sscl::core
