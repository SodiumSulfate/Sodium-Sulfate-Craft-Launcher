#include "core/loader/installer_profile.hpp"

#include <nlohmann/json.hpp>

namespace sscl::core {

namespace {

using nlohmann::json;

std::string get_string(const json& node, const char* key) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_string()) {
        return std::string();
    }
    return node[key].get<std::string>();
}

int get_int(const json& node, const char* key, int fallback = 0) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_number_integer()) {
        return fallback;
    }
    return node[key].get<int>();
}

// 库条目：name 必需；url 基址；downloads.artifact{path,url,sha1,size}。
Library parse_library(const json& node) {
    Library library;
    if (!node.is_object()) {
        return library;
    }
    library.name = get_string(node, "name");
    library.url = get_string(node, "url");
    if (node.contains("downloads") && node["downloads"].is_object()) {
        const json& downloads = node["downloads"];
        if (downloads.contains("artifact") && downloads["artifact"].is_object()) {
            const json& artifact = downloads["artifact"];
            library.has_artifact = true;
            library.artifact.path = get_string(artifact, "path");
            library.artifact.url = get_string(artifact, "url");
            library.artifact.sha1 = get_string(artifact, "sha1");
            const auto it = artifact.find("size");
            if (it != artifact.end() && it->is_number_integer()) {
                library.artifact.size = it->get<std::int64_t>();
            }
            // 与 VersionJson 一致的语义：url 为空 ⇒ 由处理器生成，禁止下载
            library.local_only = library.artifact.url.empty();
        }
    }
    return library;
}

InstallerProcessor parse_processor(const json& node) {
    InstallerProcessor processor;
    if (!node.is_object()) {
        return processor;
    }
    if (node.contains("sides") && node["sides"].is_array()) {
        for (const json& side : node["sides"]) {
            if (side.is_string()) {
                processor.sides.push_back(side.get<std::string>());
            }
        }
    }
    processor.jar = get_string(node, "jar");
    if (node.contains("classpath") && node["classpath"].is_array()) {
        for (const json& entry : node["classpath"]) {
            if (entry.is_string()) {
                processor.classpath.push_back(entry.get<std::string>());
            }
        }
    }
    if (node.contains("args") && node["args"].is_array()) {
        for (const json& entry : node["args"]) {
            if (entry.is_string()) {
                processor.args.push_back(entry.get<std::string>());
            }
        }
    }
    if (node.contains("outputs") && node["outputs"].is_object()) {
        for (auto it = node["outputs"].begin(); it != node["outputs"].end(); ++it) {
            if (it.value().is_string()) {
                processor.outputs[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return processor;
}

}  // namespace

util::Result<InstallerProfile> parse_installer_profile(const std::string& json_text) {
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<InstallerProfile>::fail(util::Error::make(
            "loader.installer_invalid", "安装器配置不是有效 JSON", {}));
    }

    InstallerProfile profile;
    profile.spec = get_int(root, "spec", 0);
    profile.profile = get_string(root, "profile");
    profile.version = get_string(root, "version");
    profile.minecraft = get_string(root, "minecraft");
    const std::string json_field = get_string(root, "json");
    if (!json_field.empty()) {
        profile.json_entry = json_field.front() == '/' ? json_field.substr(1) : json_field;
    }

    // 判定顺序（§5.4）：spec → install+versionInfo → 只有 json
    if (profile.spec > 0) {
        profile.flavor = InstallerFlavor::kModern;
    } else if (root.contains("install") && root.contains("versionInfo")) {
        profile.flavor = InstallerFlavor::kLegacy;
        const json& install = root["install"];
        profile.legacy_install_path = get_string(install, "path");
        profile.legacy_file_path = get_string(install, "filePath");
        if (profile.minecraft.empty()) {
            profile.minecraft = get_string(install, "minecraft");
        }
        profile.embedded_version_json = root["versionInfo"].dump();
    } else if (!json_field.empty() || root.contains("json")) {
        profile.flavor = InstallerFlavor::kMavenOnly;
    } else {
        return util::Result<InstallerProfile>::fail(util::Error::make(
            "loader.installer_unsupported", "无法识别的安装器结构（缺少 spec/install/json）", {}));
    }

    // 现代/中古的 version 可能来自 install_profile.version；legacy 用 versionInfo.id 兜底
    if (root.contains("path")) {
        // path 既可能是字符串，也可能是 {"path": "..."}（旧写法）
        const json& path_node = root["path"];
        if (path_node.is_object() && profile.legacy_install_path.empty()) {
            profile.legacy_install_path = get_string(path_node, "path");
        } else if (path_node.is_string() && profile.legacy_install_path.empty()) {
            profile.legacy_install_path = path_node.get<std::string>();
        }
    }

    if (root.contains("data") && root["data"].is_object()) {
        for (auto it = root["data"].begin(); it != root["data"].end(); ++it) {
            SidedData value;
            if (it.value().is_string()) {
                value.client = it.value().get<std::string>();
                value.server = value.client;
            } else if (it.value().is_object()) {
                value.client = get_string(it.value(), "client");
                value.server = get_string(it.value(), "server");
            }
            profile.data[it.key()] = std::move(value);
        }
    }

    if (root.contains("processors") && root["processors"].is_array()) {
        for (const json& node : root["processors"]) {
            profile.processors.push_back(parse_processor(node));
        }
    }
    if (root.contains("libraries") && root["libraries"].is_array()) {
        for (const json& node : root["libraries"]) {
            profile.libraries.push_back(parse_library(node));
        }
    }

    if (profile.minecraft.empty()) {
        return util::Result<InstallerProfile>::fail(util::Error::make(
            "loader.installer_invalid", "安装器配置缺少 minecraft 字段", {}));
    }
    return util::Result<InstallerProfile>::ok(std::move(profile));
}

}  // namespace sscl::core
