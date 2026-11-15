#include "core/game/instance_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

constexpr const char* kIllegalNameChars = "<>:\"/\\|?*";

std::string upper_ascii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

bool is_reserved_name(const std::string& name) {
    std::string base = name;
    const std::size_t dot = base.find('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    base = upper_ascii(base);
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    if (base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
        base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

int loader_priority(const std::string& loader) {
    if (loader == "fabric") {
        return 5;
    }
    if (loader == "quilt") {
        return 4;
    }
    if (loader == "neoforge") {
        return 3;
    }
    if (loader == "forge") {
        return 2;
    }
    return 0;
}

bool is_api_state(InstanceState state) {
    return state == InstanceState::kForge || state == InstanceState::kNeoForge ||
           state == InstanceState::kFabric || state == InstanceState::kQuilt;
}

bool release_time_greater(const std::string& left, const std::string& right) {
    // PCL ReleaseTime；ISO 字符串可直接字典序比较，空值最小。
    if (left.empty() != right.empty()) {
        return !left.empty();
    }
    return left > right;
}

std::string api_card_title(const std::vector<VersionSummary>& versions) {
    std::set<std::string> loaders;
    for (const VersionSummary& version : versions) {
        if (!version.loader.empty()) {
            loaders.insert(version.loader);
        }
    }
    if (loaders.size() > 1) {
        return "可安装 Mod";
    }
    if (loaders.count("forge") != 0) {
        return "Forge 版本";
    }
    if (loaders.count("neoforge") != 0) {
        return "NeoForge 版本";
    }
    if (loaders.count("fabric") != 0) {
        return "Fabric 版本";
    }
    if (loaders.count("quilt") != 0) {
        return "Quilt 版本";
    }
    return "可安装 Mod";
}

void sort_versions(std::vector<VersionSummary>& versions) {
    std::stable_sort(versions.begin(), versions.end(),
                     [](const VersionSummary& left, const VersionSummary& right) {
                         if (left.release_time != right.release_time) {
                             return release_time_greater(left.release_time, right.release_time);
                         }
                         const int left_loader = loader_priority(left.loader);
                         const int right_loader = loader_priority(right.loader);
                         if (left_loader != right_loader) {
                             return left_loader > right_loader;
                         }
                         if (left.loader_version != right.loader_version) {
                             return left.loader_version > right.loader_version;
                         }
                         return left.version.id > right.version.id;
                     });
}

std::wstring path_key(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        absolute = std::filesystem::absolute(path, ec);
    }
    if (ec) {
        absolute = path;
    }
    std::wstring key = absolute.lexically_normal().wstring();
    for (wchar_t& c : key) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) {
        key.pop_back();
    }
    return key;
}

bool has_versions_dir(const std::filesystem::path& folder) {
    std::error_code ec;
    return std::filesystem::is_directory(folder / L"versions", ec);
}

}  // namespace

const char* instance_state_key(InstanceState state) {
    switch (state) {
        case InstanceState::kOriginal:
            return "original";
        case InstanceState::kSnapshot:
            return "snapshot";
        case InstanceState::kOld:
            return "old";
        case InstanceState::kFool:
            return "fool";
        case InstanceState::kForge:
            return "forge";
        case InstanceState::kNeoForge:
            return "neoforge";
        case InstanceState::kFabric:
            return "fabric";
        case InstanceState::kQuilt:
            return "quilt";
        case InstanceState::kError:
        default:
            return "error";
    }
}

const char* instance_category_key(InstanceCategory category) {
    switch (category) {
        case InstanceCategory::kStar:
            return "star";
        case InstanceCategory::kAuto:
            return "auto";
        case InstanceCategory::kHidden:
            return "hidden";
        case InstanceCategory::kApi:
            return "api";
        case InstanceCategory::kOriginalLike:
            return "original";
        case InstanceCategory::kRubbish:
            return "rubbish";
        case InstanceCategory::kFool:
            return "fool";
        case InstanceCategory::kError:
        default:
            return "error";
    }
}

bool instance_name_is_valid(const std::string& name, std::string* reason) {
    auto fail = [reason](const char* text) {
        if (reason != nullptr) {
            *reason = text;
        }
        return false;
    };
    if (name.empty()) {
        return fail("名称不能为空");
    }
    if (name == "." || name == "..") {
        return fail("名称不能是 . 或 ..");
    }
    if (name.size() > 64) {
        return fail("名称不能超过 64 个字符");
    }
    if (name.back() == ' ' || name.back() == '.') {
        return fail("名称不能以空格或句点结尾");
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 32) {
            return fail("名称不能包含控制字符");
        }
        if (std::string(kIllegalNameChars).find(c) != std::string::npos) {
            return fail("名称不能包含 < > : \" / \\ | ? *");
        }
    }
    if (is_reserved_name(name)) {
        return fail("名称是 Windows 保留名");
    }
    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

std::vector<InstanceCard> classify_instance_summaries(std::vector<VersionSummary> versions,
                                                      bool show_hidden) {
    std::vector<VersionSummary> star;
    std::vector<VersionSummary> api;
    std::vector<VersionSummary> original;
    std::vector<VersionSummary> snapshot;
    std::vector<VersionSummary> rubbish;
    std::vector<VersionSummary> fool;
    std::vector<VersionSummary> error;
    std::vector<VersionSummary> hidden;

    for (VersionSummary& version : versions) {
        const bool is_hidden = version.category == InstanceCategory::kHidden;
        if (show_hidden) {
            if (is_hidden) {
                hidden.push_back(std::move(version));
            }
            continue;
        }
        if (is_hidden) {
            continue;
        }
        if (!version.error.empty() || version.state == InstanceState::kError) {
            error.push_back(std::move(version));
        } else if (version.starred) {
            star.push_back(std::move(version));
        } else if (version.state == InstanceState::kFool) {
            fool.push_back(std::move(version));
        } else if (is_api_state(version.state)) {
            api.push_back(std::move(version));
        } else if (version.state == InstanceState::kOld) {
            rubbish.push_back(std::move(version));
        } else if (version.state == InstanceState::kSnapshot) {
            snapshot.push_back(std::move(version));
        } else {
            original.push_back(std::move(version));
        }
    }

    if (show_hidden) {
        std::vector<InstanceCard> cards;
        if (!hidden.empty()) {
            sort_versions(hidden);
            cards.push_back(InstanceCard{InstanceCategory::kHidden, "隐藏的版本",
                                         std::move(hidden)});
        }
        return cards;
    }

    // 最新快照进入“常规版本”，其余快照进入“不常用”。
    std::size_t newest_snapshot = 0;
    bool has_newest_snapshot = false;
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (!has_newest_snapshot ||
            release_time_greater(snapshot[i].release_time, snapshot[newest_snapshot].release_time)) {
            newest_snapshot = i;
            has_newest_snapshot = true;
        }
    }
    if (has_newest_snapshot) {
        bool snapshot_is_newest = true;
        for (const VersionSummary& item : original) {
            if (release_time_greater(item.release_time, snapshot[newest_snapshot].release_time)) {
                snapshot_is_newest = false;
                break;
            }
        }
        if (snapshot_is_newest) {
            original.push_back(std::move(snapshot[newest_snapshot]));
        }
        for (std::size_t i = 0; i < snapshot.size(); ++i) {
            if (i == newest_snapshot) {
                continue;
            }
            rubbish.push_back(std::move(snapshot[i]));
        }
    }

    // 每个 Drop 只保留发布时间最新的原版；其余进入“不常用”。
    std::map<int, std::size_t> newest_by_drop;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const int drop = original[i].drop;
        const auto found = newest_by_drop.find(drop);
        if (found == newest_by_drop.end() ||
            release_time_greater(original[i].release_time, original[found->second].release_time)) {
            newest_by_drop[drop] = i;
        }
    }
    std::vector<VersionSummary> useful;
    useful.reserve(original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto found = newest_by_drop.find(original[i].drop);
        if (found != newest_by_drop.end() && found->second == i) {
            useful.push_back(std::move(original[i]));
        } else {
            rubbish.push_back(std::move(original[i]));
        }
    }

    // 用户强制分类（PCL DisplayType）：Star 已单独处理，Hidden 已排除。
    auto apply_override = [](std::vector<VersionSummary>& from,
                             std::vector<VersionSummary>& to, InstanceCategory category) {
        std::vector<VersionSummary> keep;
        keep.reserve(from.size());
        for (VersionSummary& version : from) {
            if (version.category == category) {
                to.push_back(std::move(version));
            } else {
                keep.push_back(std::move(version));
            }
        }
        from = std::move(keep);
    };
    apply_override(useful, api, InstanceCategory::kApi);
    apply_override(useful, rubbish, InstanceCategory::kRubbish);
    apply_override(useful, fool, InstanceCategory::kFool);
    apply_override(useful, error, InstanceCategory::kError);
    apply_override(api, useful, InstanceCategory::kOriginalLike);
    apply_override(api, rubbish, InstanceCategory::kRubbish);
    apply_override(api, fool, InstanceCategory::kFool);
    apply_override(api, error, InstanceCategory::kError);
    apply_override(rubbish, useful, InstanceCategory::kOriginalLike);
    apply_override(rubbish, api, InstanceCategory::kApi);
    apply_override(rubbish, fool, InstanceCategory::kFool);
    apply_override(rubbish, error, InstanceCategory::kError);

    std::vector<InstanceCard> cards;
    auto add_card = [&cards](InstanceCategory category, std::string title,
                             std::vector<VersionSummary> list) {
        if (list.empty()) {
            return;
        }
        sort_versions(list);
        cards.push_back(InstanceCard{category, std::move(title), std::move(list)});
    };
    add_card(InstanceCategory::kStar, "收藏夹", std::move(star));
    add_card(InstanceCategory::kApi, api_card_title(api), std::move(api));
    add_card(InstanceCategory::kOriginalLike, "常规版本", std::move(useful));
    add_card(InstanceCategory::kRubbish, "不常用版本", std::move(rubbish));
    add_card(InstanceCategory::kFool, "愚人节版本", std::move(fool));
    add_card(InstanceCategory::kError, "错误的版本", std::move(error));
    return cards;
}

std::vector<McFolderInfo> scan_mc_folders(const std::filesystem::path& base_game_dir,
                                          const std::filesystem::path& official_game_dir,
                                          const std::vector<McFolderConfig>& saved,
                                          const std::string& selected_path) {
    struct Candidate {
        McFolderInfo info;
        std::wstring key;
    };
    std::vector<Candidate> candidates;
    auto add = [&candidates](const std::filesystem::path& path, const std::string& name,
                             McFolderType type) {
        if (path.empty()) {
            return;
        }
        const std::wstring key = path_key(path);
        if (key.empty()) {
            return;
        }
        for (const Candidate& candidate : candidates) {
            if (candidate.key == key) {
                return;
            }
        }
        McFolderInfo info;
        info.name = name.empty() ? util::wide_to_utf8(path.filename().wstring()) : name;
        info.location = path;
        info.type = type;
        candidates.push_back(Candidate{std::move(info), key});
    };

    const std::wstring base_key = path_key(base_game_dir);
    if (has_versions_dir(base_game_dir)) {
        add(base_game_dir, "当前文件夹", McFolderType::kVanilla);
    }

    const std::filesystem::path parent = base_game_dir.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(parent, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_directory(ec)) {
                continue;
            }
            const std::filesystem::path child = it->path();
            if (path_key(child) == base_key) {
                continue;
            }
            if (has_versions_dir(child) || child.filename().wstring() == L".minecraft") {
                add(child, util::wide_to_utf8(child.filename().wstring()),
                    McFolderType::kVanilla);
            }
        }
    }

    if (!official_game_dir.empty() && has_versions_dir(official_game_dir)) {
        add(official_game_dir, "官方启动器文件夹", McFolderType::kVanilla);
    }

    for (const McFolderConfig& folder : saved) {
        if (folder.path.empty()) {
            continue;
        }
        const std::filesystem::path path =
            std::filesystem::path(util::utf8_to_wide(folder.path));
        const std::wstring key = path_key(path);
        bool merged = false;
        for (Candidate& candidate : candidates) {
            if (candidate.key == key) {
                candidate.info.name = folder.name;
                candidate.info.type = candidate.info.type == McFolderType::kVanilla
                                          ? McFolderType::kRenamedVanilla
                                          : McFolderType::kCustom;
                merged = true;
                break;
            }
        }
        if (!merged) {
            add(path, folder.name, McFolderType::kCustom);
        }
    }

    if (candidates.empty()) {
        add(base_game_dir, "当前文件夹", McFolderType::kVanilla);
    }

    const std::wstring selected_key =
        selected_path.empty()
            ? std::wstring()
            : path_key(std::filesystem::path(util::utf8_to_wide(selected_path)));
    std::vector<McFolderInfo> result;
    result.reserve(candidates.size());
    bool selected_set = false;
    for (Candidate& candidate : candidates) {
        candidate.info.selected = false;
        if (!selected_set && !selected_key.empty() && candidate.key == selected_key) {
            candidate.info.selected = true;
            selected_set = true;
        }
        result.push_back(std::move(candidate.info));
    }
    if (!selected_set && !result.empty()) {
        result.front().selected = true;
    }
    return result;
}

}  // namespace sscl::core
