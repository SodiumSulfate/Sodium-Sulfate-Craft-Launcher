#include "core/game/pcl_version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <vector>

namespace sscl::core {

namespace {

std::string lower_ascii(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

std::string regex_seek(const std::string& text, const char* pattern) {
    try {
        const std::regex re(pattern, std::regex::icase);
        std::smatch match;
        if (std::regex_search(text, match, re) && match.size() > 0) {
            return match.str(0);
        }
    } catch (const std::regex_error&) {
    }
    return {};
}

std::string regex_capture(const std::string& text, const char* pattern) {
    try {
        const std::regex re(pattern, std::regex::icase);
        std::smatch match;
        if (std::regex_search(text, match, re) && match.size() > 1) {
            return match.str(1);
        }
    } catch (const std::regex_error&) {
    }
    return {};
}

int parse_leading_int(const std::string& text) {
    int value = 0;
    bool any = false;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            break;
        }
        any = true;
        value = value * 10 + (c - '0');
    }
    return any ? value : 0;
}

struct Triplet {
    int major = 9999;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

Triplet parse_mc_triplet(const std::string& name) {
    Triplet result;
    if (name.rfind("1.", 0) == 0) {
        const std::string rest = name.substr(2);
        const std::size_t dot = rest.find('.');
        result.major = parse_leading_int(rest);
        result.minor = 0;
        result.patch = dot == std::string::npos ? 0 : parse_leading_int(rest.substr(dot + 1));
        result.valid = result.major != 0;
        return result;
    }
    static const std::regex modern(R"(^([2-9][0-9])\.([0-9]+)(?:\.([0-9]+))?)");
    std::smatch match;
    if (std::regex_search(name, match, modern)) {
        result.major = parse_leading_int(match.str(1));
        result.minor = parse_leading_int(match.str(2));
        result.patch = match.size() > 3 ? parse_leading_int(match.str(3)) : 0;
        result.valid = true;
    }
    return result;
}

std::string fool_base_version(const std::string& lower) {
    // PCL McVersion.NameToVersion 的愚人节映射（只用于 Drop/兼容判断）。
    if (lower.rfind("2.0", 0) == 0) return "1.5.1";
    if (lower == "15w14a") return "1.8.3";
    if (contains(lower, ".rv-pre")) return "1.9.2";
    if (contains(lower, "shareware")) return "1.13.2";
    if (lower.rfind("20w14", 0) == 0 && lower != "20w14a") return "1.15.2";
    if (contains(lower, "oneblockatatime")) return "1.18.2";
    if (lower.find("23w13a") == 0) return "1.19.4";
    if (lower == "24w14potato") return "1.20.4";
    if (lower == "25w14craftmine") return "1.21.4";
    if (lower == "26w14a") return "26.1.1";
    return {};
}

bool is_fool_name(const std::string& name) {
    const std::string lower = lower_ascii(name);
    return lower.rfind("2.0", 0) == 0 || lower == "15w14a" || lower == "1.rv-pre1" ||
           lower == "3d shareware v1.34" || lower == "20w14∞" ||
           lower == "22w13oneblockatatime" || lower == "23w13a_or_b" ||
           lower == "24w14potato" || lower == "25w14craftmine" || lower == "26w14a" ||
           lower.rfind("20w14inf", 0) == 0;
}

bool is_snapshot_name(const std::string& name) {
    const std::string lower = lower_ascii(name);
    return contains(lower, "w") || contains(lower, "snapshot") || contains(lower, "rc") ||
           contains(lower, "pre") || contains(lower, "experimental") || contains(lower, "-");
}

int release_year(const std::string& text) {
    if (text.size() < 4) {
        return 0;
    }
    return parse_leading_int(text);
}

bool release_is_april_first(const std::string& text, const std::string& type) {
    // PCL：releaseTime 以 UTC+2 修正后判断 4 月 1 日；这里用字符串近似。
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    return type == "snapshot" && text.substr(5, 2) == "04" && text.substr(8, 2) == "01";
}

// D-116：`parse_argument_entries` 把 arguments.game 的**每个数组元素**存成独立 entry，
// 因此 "--fml.mcVersion" 与它的取值 "1.21.11" 分属相邻两个 entry（各含 1 个字符串）。
// 旧实现只在单个 entry 内找 "标志 + 下一项"，永远找不到 → 版本识别退化到
// `--fml.mcVersion → jar` 的 jar 分支，把加载器 id（neoforge-21.11.48）当成原版号。
// 这里先按原顺序摊平，再取标志后的下一个元素。
std::string find_argument_value(const ArgumentList& list, const char* name) {
    std::vector<std::string> flat;
    for (const RuledValues& entry : list.entries) {
        flat.insert(flat.end(), entry.values.begin(), entry.values.end());
    }
    for (std::size_t i = 0; i + 1 < flat.size(); ++i) {
        if (flat[i] == name) {
            return flat[i + 1];
        }
    }
    return {};
}

std::string library_join(const VersionJson& version) {
    std::string text;
    for (const Library& library : version.libraries) {
        text += library.name;
        text += '\n';
    }
    return text;
}

std::string json_text_join(const VersionJson& version) {
    std::string text = library_join(version);
    text += version.id;
    text += '\n';
    text += version.main_class;
    text += '\n';
    text += version.type;
    text += '\n';
    text += version.assets;
    text += '\n';
    for (const RuledValues& entry : version.game_arguments.entries) {
        for (const std::string& value : entry.values) {
            text += value;
            text += '\n';
        }
    }
    return text;
}

std::string seek_mc_version(const std::string& text) {
    // PCL VersionPattern（去掉 lookbehind，保留捕获组）。
    return regex_seek(
        text,
        R"(((?:[1-9][0-9]w[0-9]{2}[a-g])|(?:(?:1|[2-9][0-9])\.[0-9]+(?:\.[0-9]+)?(?:-(?:pre|rc|snapshot-?)[1-9]*| Pre-Release(?: [1-9])?)?))(?:_unobfuscated)?)");
}

std::string seek_library_version(const VersionJson& version) {
    for (const Library& library : version.libraries) {
        const std::string name = library.name;
        std::string found = regex_capture(
            name, R"(net\.minecraftforge:forge:([0-9]{1,2}\.[0-9.+]+))");
        if (!found.empty()) {
            return found;
        }
        found = regex_capture(
            name, R"(net\.minecraftforge:fmlloader:([0-9]{1,2}\.[0-9.+]+))");
        if (!found.empty()) {
            return found;
        }
        found = regex_capture(name, R"(optifine:OptiFine:([0-9]{1,2}\.[0-9.+]+))");
        if (!found.empty()) {
            return found;
        }
        found = regex_capture(name, R"((?:fabricmc|quiltmc):intermediary:([^\s]+))");
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

}  // namespace

bool is_plausible_mc_version(const std::string& name) {
    if (name.empty() || name == "Unknown") {
        return false;
    }
    // PCL 的两个占位名：Old（2013 年前）/ pending（补丁待确认）。
    if (name == "Old" || name == "pending") {
        return true;
    }
    const std::string lower = lower_ascii(name);
    // D-116：加载器 id / jar 名（neoforge-21.11.48、fabric-loader-0.20.1-1.21.11、
    // forge-1.21.11-61.2.4）不是原版号，不能用作 Mod 平台的 versions 筛选值。
    static const char* const kLoaderKeys[] = {"forge", "fabric", "quilt", "loader", "optifine",
                                              "liteloader"};
    for (const char* key : kLoaderKeys) {
        if (lower.find(key) != std::string::npos) {
            return false;
        }
    }
    if (is_standard_mc_version(name)) {
        return true;
    }
    // 快照：16w01a / 26w14a。
    static const std::regex kSnapshot(R"(^[0-9]{2}w[0-9]{2}[a-g]$)", std::regex::icase);
    return std::regex_match(name, kSnapshot);
}

bool is_standard_mc_version(const std::string& name) {
    if (name.size() >= 3 && name[0] == '1' && name[1] == '.' &&
        std::isdigit(static_cast<unsigned char>(name[2]))) {
        return true;
    }
    static const std::regex modern(R"(^[2-9][0-9]\.[0-9]+)");
    if (!std::regex_search(name, modern)) {
        return false;
    }
    const Triplet triplet = parse_mc_triplet(name);
    return triplet.valid && triplet.major >= 26;
}

int mc_version_drop(const std::string& name) {
    std::string effective = name;
    const std::string base = fool_base_version(lower_ascii(name));
    if (!base.empty()) {
        effective = base;
    }
    const Triplet triplet = parse_mc_triplet(effective);
    if (!triplet.valid || triplet.major >= 1000) {
        return 209;
    }
    return triplet.major * 10 + triplet.minor;
}

PclVersionInfo detect_pcl_version(const VersionJson& version,
                                  const std::string& instance_name,
                                  const std::string& jar_version_name) {
    PclVersionInfo info;
    bool reliable = true;
    const int year = release_year(version.release_time);

    auto use = [&info](const std::string& value) {
        info.vanilla_name = value;
        info.valid = true;
    };

    // 1. 2001..2012 = Old；2. pending；3. clientVersion；4. SSCL 安装元数据。
    if (year > 2000 && year < 2013) {
        use("Old");
    } else if (version.type == "pending") {
        use("pending");
    } else if (!version.client_version.empty()) {
        use(version.client_version);
    } else if (!version.sscl.minecraft_version.empty()) {
        // D-116：SSCL 安装加载器时写入的原版号最可靠（子版本 id / jar 都是加载器名）。
        use(version.sscl.minecraft_version);
    } else {
        std::string candidate = find_argument_value(version.game_arguments, "--fml.mcVersion");
        // D-116：先认 inheritsFrom（加载器子版本的父版本就是原版），
        // 再认 jar —— 且两者都必须「像原版号」，否则会把 neoforge-21.11.48 当版本号。
        if (candidate.empty() && is_plausible_mc_version(version.inherits_from)) {
            candidate = version.inherits_from;
        }
        if (candidate.empty() && is_plausible_mc_version(version.jar)) {
            candidate = version.jar;
        }
        if (candidate.empty() && version.has_downloads && !version.downloads.client.url.empty()) {
            candidate = regex_seek(version.downloads.client.url,
                                   R"(launcher\.mojang\.com/mc/game/([^/]+))");
            if (candidate.find("launcher.mojang.com") != std::string::npos) {
                // regex_seek 返回整个匹配；用捕获组重新提取。
                candidate.clear();
                try {
                    const std::regex re(R"(launcher\.mojang\.com/mc/game/([^/]+))");
                    std::smatch match;
                    if (std::regex_search(version.downloads.client.url, match, re) &&
                        match.size() > 1) {
                        candidate = match.str(1);
                    }
                } catch (const std::regex_error&) {
                }
            }
        }
        if (candidate.empty()) {
            candidate = seek_library_version(version);
        }
        if (candidate.empty() && !version.jar.empty() && is_plausible_mc_version(version.jar)) {
            candidate = version.jar;
        }
        if (candidate.empty() && !jar_version_name.empty() && jar_version_name.size() < 32) {
            candidate = jar_version_name;
        }
        if (candidate.empty()) {
            candidate = seek_mc_version(version.id);
        }
        if (candidate.empty()) {
            reliable = false;
            candidate = seek_mc_version(instance_name);
        }
        if (candidate.empty()) {
            candidate = seek_mc_version(json_text_join(version));
        }
        if (candidate.empty() && is_fool_name(version.id)) {
            candidate = version.id;
        }
        if (candidate.empty() && is_fool_name(instance_name)) {
            candidate = instance_name;
        }
        if (candidate.empty()) {
            candidate = "Unknown";
        }
        use(candidate);
    }

    info.vanilla_name = info.vanilla_name.empty() ? "Unknown" : info.vanilla_name;
    info.reliable = reliable;
    info.release_time = version.release_time;
    {
        const Triplet triplet = parse_mc_triplet(info.vanilla_name);
        info.valid = triplet.valid && triplet.major < 1000;
    }
    info.drop = mc_version_drop(info.vanilla_name);
    info.is_fool = version.type == "fool" || is_fool_name(info.vanilla_name) ||
                   release_is_april_first(version.release_time, version.type) ||
                   contains(lower_ascii(info.vanilla_name), "fool");
    info.is_snapshot = is_snapshot_name(info.vanilla_name) ||
                       contains(lower_ascii(instance_name), "combat") ||
                       version.type == "snapshot" || version.type == "pending" ||
                       version.type == "fool";
    info.is_release = !info.is_fool && !info.is_snapshot &&
                      info.vanilla_name != "Old" && info.vanilla_name != "Unknown" &&
                      info.vanilla_name != "pending";
    return info;
}

}  // namespace sscl::core
