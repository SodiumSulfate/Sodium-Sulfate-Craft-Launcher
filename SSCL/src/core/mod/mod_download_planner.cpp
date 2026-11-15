#include "core/mod/mod_download_planner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

std::string lower_ascii(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool loader_matches(const ModVersion& version, const InstanceTarget& target) {
    if (version.loaders.empty()) {
        return true;  // PCL：无加载器信息 = 不限制
    }
    if (target.loader.empty()) {
        return false;
    }
    const ModLoader loader = mod_loader_from_key(target.loader);
    if (loader == ModLoader::kUnknown) {
        return true;
    }
    return std::find(version.loaders.begin(), version.loaders.end(), loader) !=
           version.loaders.end();
}

std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(sscl::util::utf8_to_wide(text));
}

std::string base_name(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    // 远端文件名不可信：只取最后一段，兼容 / 与 \。
    std::size_t cut = raw.find_last_of("/\\");
    return cut == std::string::npos ? raw : raw.substr(cut + 1);
}

std::string sanitize_component(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // 保留 UTF-8 多字节序列。
        if (c >= 0x80) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        switch (c) {
            case '\\':
                out += "＼";
                break;
            case '/':
                out += "／";
                break;
            case '|':
                out += "｜";
                break;
            case ':':
                out += "：";
                break;
            case '<':
                out += "＜";
                break;
            case '>':
                out += "＞";
                break;
            case '*':
                out += "＊";
                break;
            case '?':
                out += "？";
                break;
            case '"':
                break;  // PCL 直接删除引号
            default:
                out.push_back(static_cast<char>(c));
                break;
        }
        ++i;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out;
}

std::string truncate_utf8(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut);
}

std::string apply_translate(const std::string& raw, const std::string& translated,
                            int mode) {
    if (translated.empty() || translated == raw) {
        return raw;
    }
    const std::size_t dot = raw.find_last_of('.');
    const std::string stem = dot == std::string::npos ? raw : raw.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : raw.substr(dot);
    switch (mode) {
        case 0:
            return "【" + translated + "】" + stem + ext;
        case 1:
            return "[" + translated + "] " + stem + ext;
        case 2:
            return translated + "-" + stem + ext;
        case 3:
            return stem + "-" + translated + ext;
        default:
            return raw;
    }
}

}  // namespace

bool mod_version_suitable_for(const ModVersion& version, const InstanceTarget& target) {
    if (!target.minecraft_version.empty() && !version.game_versions.empty() &&
        !contains(version.game_versions, target.minecraft_version)) {
        return false;
    }
    return loader_matches(version, target);
}

std::string pcl_download_file_name(const std::string& raw_file_name,
                                   const std::string& translated_name, int translate_mode) {
    const std::string raw = base_name(raw_file_name);
    if (raw.empty()) {
        return "mod.jar";
    }
    std::string translated = translated_name;
    const std::size_t space = translated.find(" (");
    if (space != std::string::npos) {
        translated.resize(space);
    }
    const std::size_t dash = translated.find(" - ");
    if (dash != std::string::npos) {
        translated.resize(dash);
    }
    translated = sanitize_component(translated);
    std::string name = sanitize_component(apply_translate(raw, translated, translate_mode));
    // PCL：~ 会导致 Mixin 加载失败。
    std::replace(name.begin(), name.end(), '~', '-');
    if (name.empty()) {
        name = "mod.jar";
    }
    // 保留扩展名，限制整体 120 字节。
    if (name.size() > 120) {
        const std::size_t dot = name.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
        const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
        const std::size_t keep = ext.size() < 120 ? 120 - ext.size() : 0;
        name = truncate_utf8(stem, keep) + ext;
    }
    const std::string lower = lower_ascii(name);
    static const char* kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3",
                                      "com4", "com5", "com6", "com7", "com8", "com9",
                                      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                      "lpt7", "lpt8", "lpt9"};
    for (const char* reserved : kReserved) {
        if (lower == reserved || lower.rfind(std::string(reserved) + ".", 0) == 0) {
            name = "_" + name;
            break;
        }
    }
    return name;
}

ModDownloadPlan plan_pcl_mod_download(const std::filesystem::path& game_dir,
                                      const InstanceTarget* selected,
                                      const std::vector<InstanceTarget>& instances,
                                      const std::string& cached_folder,
                                      const ModVersion& version, const std::string& raw_file_name,
                                      const std::string& translated_name, int translate_mode) {
    ModDownloadPlan plan;
    plan.file_name = pcl_download_file_name(raw_file_name, translated_name, translate_mode);

    // 1. 上次同类目录（会话内），PCL CachedFolder。
    if (!cached_folder.empty()) {
        plan.default_folder = from_utf8(cached_folder);
        plan.selected_instance_compatible =
            selected != nullptr && mod_version_suitable_for(version, *selected);
        return plan;
    }
    // 2. 当前选中且兼容。
    if (selected != nullptr && mod_version_suitable_for(version, *selected)) {
        plan.default_folder = selected->run_dir / L"mods";
        plan.selected_instance_compatible = true;
        return plan;
    }
    // 3. 扫描所有兼容实例：目录修改时间降序，再按文件数降序。
    struct Candidate {
        std::filesystem::path folder;
        std::filesystem::file_time_type time{};
        std::size_t files = 0;
    };
    std::vector<Candidate> candidates;
    for (const InstanceTarget& target : instances) {
        if (!mod_version_suitable_for(version, target)) {
            continue;
        }
        Candidate candidate;
        candidate.folder = target.run_dir / L"mods";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate.folder, ec)) {
            candidate.time = std::filesystem::last_write_time(candidate.folder, ec);
            if (ec) {
                candidate.time = std::filesystem::file_time_type::min();
            }
            for (std::filesystem::directory_iterator it(candidate.folder, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    ++candidate.files;
                }
            }
        } else {
            candidate.time = std::filesystem::file_time_type::min();
        }
        candidates.push_back(std::move(candidate));
    }
    if (!candidates.empty()) {
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const Candidate& a, const Candidate& b) {
                             if (a.time != b.time) {
                                 return a.time > b.time;
                             }
                             return a.files > b.files;
                         });
        plan.default_folder = candidates.front().folder;
        return plan;
    }
    // 4. MC 根。
    plan.default_folder = game_dir;
    return plan;
}

}  // namespace sscl::core
