#include "core/modplatform/mod_types.hpp"

#include <algorithm>
#include <cctype>

namespace sscl::core {

namespace {

std::string lower_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool is_attachment_type(const std::string& type) {
    const std::string lower = lower_ascii(type);
    return lower == "sources-jar" || lower == "dev-jar" || lower == "javadoc-jar" ||
           lower == "signature";
}

}  // namespace

const char* mod_platform_key(ModPlatform platform) {
    return platform == ModPlatform::kCurseForge ? "curseforge" : "modrinth";
}

const char* mod_loader_key(ModLoader loader) {
    switch (loader) {
        case ModLoader::kVanilla:
            return "vanilla";
        case ModLoader::kFabric:
            return "fabric";
        case ModLoader::kForge:
            return "forge";
        case ModLoader::kNeoForge:
            return "neoforge";
        case ModLoader::kQuilt:
            return "quilt";
        case ModLoader::kUnknown:
        default:
            return "";
    }
}

ModLoader mod_loader_from_key(std::string_view key) {
    const std::string lower = lower_ascii(key);
    if (lower.empty()) {
        return ModLoader::kUnknown;
    }
    if (lower == "vanilla" || lower == "minecraft") {
        return ModLoader::kVanilla;
    }
    if (lower == "fabric") {
        return ModLoader::kFabric;
    }
    if (lower == "forge") {
        return ModLoader::kForge;
    }
    if (lower == "neoforge") {
        return ModLoader::kNeoForge;
    }
    if (lower == "quilt") {
        return ModLoader::kQuilt;
    }
    return ModLoader::kUnknown;
}

ModLoader mod_loader_from_curseforge_type(int type) {
    switch (type) {
        case 1:
            return ModLoader::kForge;
        case 3:
            return ModLoader::kUnknown;  // LiteLoader：当前无独立枚举，不参与更新匹配
        case 4:
            return ModLoader::kFabric;
        case 5:
            return ModLoader::kQuilt;
        case 6:
            return ModLoader::kNeoForge;
        default:
            return ModLoader::kUnknown;
    }
}

int mod_loader_to_curseforge_type(ModLoader loader) {
    switch (loader) {
        case ModLoader::kForge:
            return 1;
        case ModLoader::kFabric:
            return 4;
        case ModLoader::kQuilt:
            return 5;
        case ModLoader::kNeoForge:
            return 6;
        default:
            return 0;
    }
}

bool mod_loader_is_moddable(ModLoader loader) {
    return loader == ModLoader::kFabric || loader == ModLoader::kForge ||
           loader == ModLoader::kNeoForge || loader == ModLoader::kQuilt;
}

const char* mod_channel_key(ModChannel channel) {
    switch (channel) {
        case ModChannel::kRelease:
            return "release";
        case ModChannel::kBeta:
            return "beta";
        case ModChannel::kAlpha:
            return "alpha";
        case ModChannel::kUnknown:
        default:
            return "unknown";
    }
}

ModChannel mod_channel_from_key(std::string_view key) {
    const std::string lower = lower_ascii(key);
    if (lower == "release") {
        return ModChannel::kRelease;
    }
    if (lower == "beta") {
        return ModChannel::kBeta;
    }
    if (lower == "alpha") {
        return ModChannel::kAlpha;
    }
    return ModChannel::kUnknown;
}

const ModFile* select_primary_file(const ModVersion& version) {
    for (const ModFile& file : version.files) {
        if (file.primary && !is_attachment_type(file.file_type)) {
            return &file;
        }
    }
    for (const ModFile& file : version.files) {
        if (!is_attachment_type(file.file_type)) {
            return &file;
        }
    }
    return version.files.empty() ? nullptr : &version.files.front();
}

}  // namespace sscl::core
