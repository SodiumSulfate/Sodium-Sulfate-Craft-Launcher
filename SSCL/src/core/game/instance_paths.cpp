#include "core/game/instance_paths.hpp"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "core/game/pcl_version.hpp"
#include "platform/file_utils.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

std::string lower_ascii(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

const char* isolation_mode_name(IsolationMode mode) {
    switch (mode) {
        case IsolationMode::kOff:
            return "off";
        case IsolationMode::kModded:
            return "modded";
        case IsolationMode::kNonRelease:
            return "non_release";
        case IsolationMode::kNonReleaseOrModded:
            return "non_release_or_modded";
        case IsolationMode::kAll:
        default:
            return "all";
    }
}

IsolationMode isolation_mode_from_int(int value) {
    if (value < 0 || value > 4) {
        return IsolationMode::kAll;
    }
    return static_cast<IsolationMode>(value);
}

int isolation_mode_to_int(IsolationMode mode) {
    return static_cast<int>(mode);
}

bool isolation_mode_wants(IsolationMode mode, bool modded, bool release) {
    switch (mode) {
        case IsolationMode::kOff:
            return false;
        case IsolationMode::kModded:
            return modded;
        case IsolationMode::kNonRelease:
            return !release;
        case IsolationMode::kNonReleaseOrModded:
            return !release || modded;
        case IsolationMode::kAll:
        default:
            return true;
    }
}

util::Result<InstanceSettings> load_instance_settings(
    const std::filesystem::path& version_dir) {
    InstanceSettings settings;
    const std::filesystem::path path = version_dir / L"sscl-instance.json";
    util::Result<std::string> text = platform::read_file(path);
    if (text.is_error()) {
        return util::Result<InstanceSettings>::ok(settings);
    }
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        util::log_warn("[instance] ignore invalid sscl-instance.json: " +
                       util::wide_to_utf8(path.wstring()));
        return util::Result<InstanceSettings>::ok(settings);
    }
    auto read_bool = [&root](const char* key, bool* out) {
        if (root.contains(key) && root[key].is_boolean()) {
            *out = root[key].get<bool>();
        }
    };
    auto read_int = [&root](const char* key, int min_value, int max_value, int* out) {
        if (root.contains(key) && root[key].is_number_integer()) {
            const int value = root[key].get<int>();
            if (value >= min_value && value <= max_value) {
                *out = value;
            }
        }
    };
    auto read_string = [&root](const char* key, std::string* out) {
        if (root.contains(key) && root[key].is_string()) {
            *out = root[key].get<std::string>();
        }
    };

    read_bool("isolated", &settings.isolated);
    if (root.contains("decided") && root["decided"].is_boolean()) {
        settings.decided = root["decided"].get<bool>();
    } else {
        // 旧文件只有 isolated：视为已决策，避免被自动规则覆盖。
        settings.decided = root.contains("isolated");
    }
    read_bool("starred", &settings.starred);
    read_int("category", 0, 6, &settings.category);
    read_string("custom_info", &settings.custom_info);
    read_string("logo", &settings.logo);
    read_string("window_title", &settings.window_title);
    read_string("version_info", &settings.version_info);
    read_int("java_mode", 0, 3, &settings.java_mode);
    read_string("java_range", &settings.java_range);
    read_string("java_path", &settings.java_path);
    read_int("ram_type", 0, 2, &settings.ram_type);
    read_int("ram_custom_gb", 1, 49, &settings.ram_custom_gb);
    read_int("ram_optimize", 0, 2, &settings.ram_optimize);
    read_int("server_login", 0, 4, &settings.server_login);
    read_string("server_nide", &settings.server_nide);
    read_string("server_auth_url", &settings.server_auth_url);
    read_string("server_auth_register", &settings.server_auth_register);
    read_string("server_auth_name", &settings.server_auth_name);
    read_string("server_enter", &settings.server_enter);
    read_string("jvm_args", &settings.jvm_args);
    read_string("game_args", &settings.game_args);
    read_int("gc_mode", 0, 5, &settings.gc_mode);
    read_string("pre_launch", &settings.pre_launch);
    read_bool("pre_launch_wait", &settings.pre_launch_wait);
    read_bool("disable_mod_update", &settings.disable_mod_update);
    read_bool("disable_file_check", &settings.disable_file_check);
    read_bool("disable_jlw", &settings.disable_jlw);
    read_bool("disable_lua", &settings.disable_lua);
    return util::Result<InstanceSettings>::ok(settings);
}

util::Result<void> save_instance_settings(const std::filesystem::path& version_dir,
                                          const InstanceSettings& settings) {
    util::Result<void> created = ::sscl::platform::create_directories(version_dir);
    if (created.is_error()) {
        return created;
    }
    json root;
    root["isolated"] = settings.isolated;
    root["decided"] = settings.decided;
    root["starred"] = settings.starred;
    root["category"] = settings.category;
    root["custom_info"] = settings.custom_info;
    root["logo"] = settings.logo;
    root["window_title"] = settings.window_title;
    root["version_info"] = settings.version_info;
    root["java_mode"] = settings.java_mode;
    root["java_range"] = settings.java_range;
    root["java_path"] = settings.java_path;
    root["ram_type"] = settings.ram_type;
    root["ram_custom_gb"] = settings.ram_custom_gb;
    root["ram_optimize"] = settings.ram_optimize;
    root["server_login"] = settings.server_login;
    root["server_nide"] = settings.server_nide;
    root["server_auth_url"] = settings.server_auth_url;
    root["server_auth_register"] = settings.server_auth_register;
    root["server_auth_name"] = settings.server_auth_name;
    root["server_enter"] = settings.server_enter;
    root["jvm_args"] = settings.jvm_args;
    root["game_args"] = settings.game_args;
    root["gc_mode"] = settings.gc_mode;
    root["pre_launch"] = settings.pre_launch;
    root["pre_launch_wait"] = settings.pre_launch_wait;
    root["disable_mod_update"] = settings.disable_mod_update;
    root["disable_file_check"] = settings.disable_file_check;
    root["disable_jlw"] = settings.disable_jlw;
    root["disable_lua"] = settings.disable_lua;
    return platform::write_file_atomic(version_dir / L"sscl-instance.json", root.dump(2));
}

bool version_dir_has_mods(const std::filesystem::path& version_dir) {
    const std::filesystem::path mods = version_dir / L"mods";
    std::error_code ec;
    if (!std::filesystem::is_directory(mods, ec)) {
        return false;
    }
    // PCL ModFolder.EnumerateFiles.Any：任意普通文件即视为“有 Mod”（不限扩展名）。
    for (std::filesystem::directory_iterator it(mods, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            return true;
        }
    }
    return false;
}

bool version_dir_has_saves(const std::filesystem::path& version_dir) {
    const std::filesystem::path saves = version_dir / L"saves";
    std::error_code ec;
    if (!std::filesystem::is_directory(saves, ec)) {
        return false;
    }
    for (std::filesystem::directory_iterator it(saves, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec)) {
            return true;
        }
    }
    return false;
}

std::string detect_loader_key(const VersionJson& version) {
    if (!version.sscl.loader.empty()) {
        return lower_ascii(version.sscl.loader);
    }
    // 库名优先：NeoForge 必须先于 Forge（"neoforge" 含 "forge"）。
    for (const Library& library : version.libraries) {
        const std::string name = lower_ascii(library.name);
        if (contains(name, "neoforge")) {
            return "neoforge";
        }
        if (contains(name, "quilt-loader") || contains(name, "org.quiltmc")) {
            return "quilt";
        }
        if (contains(name, "fabric-loader") || contains(name, "net.fabricmc")) {
            return "fabric";
        }
        if (contains(name, "minecraftforge") || contains(name, "net.minecraftforge")) {
            return "forge";
        }
    }
    const std::string main_class = lower_ascii(version.main_class);
    if (contains(main_class, "neoforge")) {
        return "neoforge";
    }
    if (contains(main_class, "quilt")) {
        return "quilt";
    }
    if (contains(main_class, "fabric")) {
        return "fabric";
    }
    if (contains(main_class, "forge")) {
        return "forge";
    }
    return {};
}

bool loader_is_moddable(const std::string& loader_key) {
    return loader_key == "fabric" || loader_key == "forge" ||
           loader_key == "neoforge" || loader_key == "quilt";
}

std::string detect_minecraft_version(const VersionJson& version,
                                     const std::string& version_id) {
    if (!version.sscl.minecraft_version.empty()) {
        return version.sscl.minecraft_version;
    }
    // 加载器版本的父版本即原版 id。
    if (!version.inherits_from.empty()) {
        return version.inherits_from;
    }
    // 无元数据时按 id 规则兜底（仅用于显示/筛选，不保证覆盖所有命名）。
    const std::string id = version_id.empty() ? version.id : version_id;
    auto take_between = [&id](const char* prefix) -> std::string {
        const std::string p(prefix);
        if (id.rfind(p, 0) != 0) {
            return {};
        }
        const std::string rest = id.substr(p.size());
        const std::size_t dash = rest.find('-');
        if (dash == std::string::npos) {
            return {};
        }
        return rest.substr(0, dash);
    };
    if (id.rfind("forge-", 0) == 0 || id.rfind("neoforge-", 0) == 0) {
        const std::string mc = take_between(id.rfind("neoforge-", 0) == 0 ? "neoforge-"
                                                                           : "forge-");
        if (!mc.empty() && mc.find('.') != std::string::npos) {
            return mc;
        }
    }
    if (id.rfind("fabric-loader-", 0) == 0) {
        const std::string rest = id.substr(std::string("fabric-loader-").size());
        const std::size_t dash = rest.find('-');
        if (dash != std::string::npos) {
            return rest.substr(dash + 1);
        }
    }
    return id;
}

util::Result<InstanceTarget> resolve_instance_target(const InstallLayout& layout,
                                                     IsolationMode mode,
                                                     const VersionJson& merged,
                                                     bool persist_decision) {
    if (merged.id.empty()) {
        return util::Result<InstanceTarget>::fail(
            util::Error::make("instance.missing_id", "版本 id 为空，无法解析运行目录", {}));
    }
    const std::filesystem::path version_dir = layout.versions_dir / merged.id;

    InstanceSettings settings;
    util::Result<InstanceSettings> loaded = load_instance_settings(version_dir);
    if (loaded.is_ok()) {
        settings = loaded.value();
    }
    const PclVersionInfo pcl = detect_pcl_version(merged, merged.id);
    const std::string loader = detect_loader_key(merged);
    const bool moddable = loader_is_moddable(loader);
    if (!settings.decided) {
        // PCL PathIndie 语义：旧覆盖 → mods 有文件/saves 有子目录 → 全局策略；
        // 首次判定后持久化，后续全局改动不影响本版本。
        settings.isolated = version_dir_has_mods(version_dir) ||
                            version_dir_has_saves(version_dir) ||
                            isolation_mode_wants(mode, moddable, pcl.is_release);
        settings.decided = true;
        if (persist_decision) {
            util::Result<void> saved = save_instance_settings(version_dir, settings);
            if (saved.is_error()) {
                util::log_warn("[instance] 无法写入隔离设置（继续使用内存决策）：" +
                               saved.error().detail);
            }
        }
    }

    InstanceTarget target;
    target.version_id = merged.id;
    // D-116：pcl.vanilla_name 可能是加载器 id（neoforge-21.11.48），
    // 参与 Mod 平台 versions 筛选会直接筛出 0 条结果；不像原版号就退回识别链。
    target.minecraft_version =
        is_plausible_mc_version(pcl.vanilla_name)
            ? pcl.vanilla_name
            : detect_minecraft_version(merged, merged.id);
    target.loader = loader;
    target.loader_version = merged.sscl.loader_version;
    target.game_dir = layout.game_dir;
    target.version_dir = version_dir;
    target.isolated = settings.isolated;
    target.moddable = moddable;
    target.release = pcl.is_release;
    target.drop = pcl.drop;
    target.run_dir = settings.isolated ? version_dir : layout.game_dir;
    target.mods_dir = target.run_dir / L"mods";
    return util::Result<InstanceTarget>::ok(std::move(target));
}

}  // namespace sscl::core
