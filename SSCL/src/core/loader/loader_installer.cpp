#include "core/loader/loader_installer.hpp"

#include <cctype>

#include <nlohmann/json.hpp>

namespace sscl::core {

namespace {

using nlohmann::json;

// 版本目录名安全化：Windows 非法字符与空白替换为 '-'，避免产生不可用路径。
std::string sanitize_id(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_dash = false;
    for (const unsigned char c : text) {
        const bool legal = std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-' || c == '+';
        if (legal && c != ' ') {
            out.push_back(static_cast<char>(c));
            last_dash = false;
        } else if (!last_dash) {
            out.push_back('-');
            last_dash = true;
        }
    }
    if (out.empty()) {
        return "loader";
    }
    return out;
}

bool is_legacy_neoforge(const LoaderEntry& entry) {
    // 1.20.1 legacy：full_version 形如 "1.20.1-47.1.106"
    return entry.full_version.size() > 2 && entry.full_version[0] == '1' &&
           entry.full_version[1] == '.' && entry.full_version.find('-') != std::string::npos;
}

}  // namespace

std::string make_loader_version_id(LoaderKind kind, const std::string& minecraft_version,
                                   const LoaderEntry& entry) {
    switch (kind) {
        case LoaderKind::kFabric:
            return sanitize_id("fabric-loader-" + entry.version + "-" + minecraft_version);
        case LoaderKind::kForge:
            return sanitize_id("forge-" + minecraft_version + "-" + entry.version);
        case LoaderKind::kNeoForge:
            if (is_legacy_neoforge(entry)) {
                return sanitize_id("neoforge-" + entry.full_version);
            }
            return sanitize_id("neoforge-" + entry.version);
        case LoaderKind::kNone:
        default:
            return sanitize_id(minecraft_version);
    }
}

util::Result<std::string> patch_loader_profile(const std::string& profile_json, LoaderKind kind,
                                               const std::string& version_id,
                                               const std::string& minecraft_version,
                                               const LoaderEntry& entry) {
    json root = json::parse(profile_json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.installer_invalid", "加载器版本配置格式不正确", "profile JSON 不是对象"));
    }
    if (!root.contains("mainClass") || !root["mainClass"].is_string() ||
        root["mainClass"].get<std::string>().empty()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.installer_invalid", "加载器版本配置缺少 mainClass", version_id));
    }
    if (!root.contains("libraries") || !root["libraries"].is_array()) {
        return util::Result<std::string>::fail(util::Error::make(
            "loader.installer_invalid", "加载器版本配置缺少 libraries", version_id));
    }

    root["id"] = version_id;
    const std::string existing_inherits =
        root.contains("inheritsFrom") && root["inheritsFrom"].is_string()
            ? root["inheritsFrom"].get<std::string>()
            : std::string();
    if (existing_inherits.empty()) {
        root["inheritsFrom"] = minecraft_version;
    }

    // SSCL 元数据（启动期忽略；用于诊断与将来修复）
    json metadata;
    metadata["loader"] = loader_kind_key(kind);
    metadata["loaderVersion"] = entry.version;
    metadata["minecraftVersion"] = minecraft_version;
    if (!entry.download_url.empty()) {
        metadata["installerUrl"] = entry.download_url;
    }
    root["sscl"] = std::move(metadata);

    return util::Result<std::string>::ok(root.dump(2));
}

}  // namespace sscl::core
