#include "core/modplatform/mod_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace sscl::core {

namespace {

// 归一化：只保留 [a-z0-9]，用于项目去重与相似度（PCL IsLike 的简化实现）。
std::string fold_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string project_key(const ModProject& project) {
    return fold_text(project.slug.empty() ? project.title : project.slug);
}

// 二元字组覆盖率（0..1）：中文/英文都可用，避免引入分词依赖。
double bigram_ratio(const std::string& needle, const std::string& haystack) {
    if (needle.size() < 2 || haystack.size() < 2) {
        return needle == haystack ? 1.0 : 0.0;
    }
    std::size_t matched = 0;
    std::size_t total = 0;
    for (std::size_t i = 0; i + 2 <= needle.size(); ++i) {
        ++total;
        if (haystack.find(needle.substr(i, 2)) != std::string::npos) {
            ++matched;
        }
    }
    return total == 0 ? 0.0 : static_cast<double>(matched) / static_cast<double>(total);
}

double similarity(const std::string& raw_query, const ModProject& project) {
    const std::string query = fold_text(raw_query);
    if (query.empty()) {
        return 0.0;
    }
    const std::string title = fold_text(project.title);
    const std::string summary = fold_text(project.summary);
    if (title.find(query) != std::string::npos) {
        return 1.0;
    }
    const double title_score = bigram_ratio(query, title);
    const double summary_score = bigram_ratio(query, summary) * 0.3;
    return (std::max)(title_score, summary_score);
}

// PCL ResourceSearcher：Mod/ModPack 的下载量平台系数（CF×1、Modrinth×5）。
double download_weight(const ModProject& project) {
    return project.platform == ModPlatform::kCurseForge ? 1.0 : 5.0;
}

}  // namespace

ModCatalogOptions make_mod_catalog_options(const std::filesystem::path& data_dir,
                                           const std::string& curseforge_key,
                                           bool use_mcim) {
    ModCatalogOptions options;
    options.modrinth.platform = ModPlatform::kModrinth;
    options.modrinth.user_agent = kModUserAgent;
    options.modrinth.use_mcim = use_mcim;
    options.modrinth.cache_dir = data_dir / L"cache" / L"modplatform";

    options.curseforge = options.modrinth;
    options.curseforge.platform = ModPlatform::kCurseForge;
    options.curseforge.curseforge_api_key = curseforge_key;
    return options;
}

ModCatalog::ModCatalog(ModCatalogOptions options)
    : m_modrinth_http(std::move(options.modrinth)),
      m_curseforge_http(std::move(options.curseforge)),
      m_modrinth(m_modrinth_http),
      m_curseforge(m_curseforge_http) {}

bool ModCatalog::available(ModPlatform platform, std::string* reason) const {
    if (platform == ModPlatform::kModrinth) {
        return true;
    }
    return m_curseforge_http.platform_available(reason);
}

util::Result<ModSearchPage> ModCatalog::search(const ModSearchQuery& query,
                                               const std::stop_token& stop) {
    if (query.platform == ModPlatform::kCurseForge) {
        return m_curseforge.search(query, stop);
    }
    return m_modrinth.search(query, stop);
}

util::Result<ModSearchPage> ModCatalog::search_merged(const ModSearchQuery& base,
                                                       const std::stop_token& stop) {
    // PCL Tag 语义：Tag="CF分类ID/MR分类slug" 两平台都查；
    // "/slug" 只查 Modrinth，"ID/" 只查 CurseForge（PageResource.xaml 的分类下拉）。
    const bool allow_curseforge = !base.category_cf.empty() || base.category.empty();
    const bool allow_modrinth = !base.category.empty() || base.category_cf.empty();

    ModSearchQuery modrinth_query = base;
    modrinth_query.platform = ModPlatform::kModrinth;
    modrinth_query.category = base.category;
    modrinth_query.category_cf.clear();
    ModSearchQuery curseforge_query = base;
    curseforge_query.platform = ModPlatform::kCurseForge;
    curseforge_query.category.clear();
    curseforge_query.category_cf = base.category_cf;

    // 两平台各用独立客户端，互不共享可变状态 → 可并行（PCL 也是双线程）。
    std::optional<util::Result<ModSearchPage>> modrinth_result;
    std::optional<util::Result<ModSearchPage>> curseforge_result;
    std::vector<std::thread> threads;
    if (allow_modrinth) {
        threads.emplace_back(
            [&]() { modrinth_result = m_modrinth.search(modrinth_query, stop); });
    }
    if (allow_curseforge) {
        threads.emplace_back(
            [&]() { curseforge_result = m_curseforge.search(curseforge_query, stop); });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const bool modrinth_failed =
        allow_modrinth && (!modrinth_result || modrinth_result->is_error());
    const bool curseforge_failed =
        allow_curseforge && (!curseforge_result || curseforge_result->is_error());
    if ((!allow_modrinth || modrinth_failed) &&
        (!allow_curseforge || curseforge_failed)) {
        if (modrinth_failed) {
            return util::Result<ModSearchPage>::fail(modrinth_result->error());
        }
        if (curseforge_failed) {
            return util::Result<ModSearchPage>::fail(curseforge_result->error());
        }
        return util::Result<ModSearchPage>::fail(
            util::Error::make("mod.platform_unavailable", "所选类型不被任何平台支持", {}));
    }

    std::string warning;
    std::vector<ModProject> merged;
    // PCL：CF 结果先入列，避免线程完成顺序影响同名项目的保留优先级。
    if (allow_curseforge && !curseforge_failed) {
        auto& items = curseforge_result->value().items;
        merged.insert(merged.end(), std::make_move_iterator(items.begin()),
                      std::make_move_iterator(items.end()));
        if (!curseforge_result->value().warning.empty()) {
            warning = curseforge_result->value().warning;
        }
    } else if (curseforge_failed) {
        warning = "CurseForge 不可用，搜索结果可能不全";
    } else if (!base.category.empty()) {
        warning = "CurseForge 不支持所选的类型";
    }
    if (allow_modrinth && !modrinth_failed) {
        auto& items = modrinth_result->value().items;
        merged.insert(merged.end(), std::make_move_iterator(items.begin()),
                      std::make_move_iterator(items.end()));
        if (warning.empty() && !modrinth_result->value().warning.empty()) {
            warning = modrinth_result->value().warning;
        }
    } else if (modrinth_failed) {
        if (warning.empty()) {
            warning = "Modrinth 不可用，搜索结果可能不全";
        } else {
            warning += "；Modrinth 也不可用";
        }
    } else if (!base.category_cf.empty() && warning.empty()) {
        warning = "Modrinth 不支持所选的类型";
    }

    std::unordered_set<std::string> seen;
    std::vector<ModProject> unique;
    unique.reserve(merged.size());
    for (auto& project : merged) {
        const std::string key = project_key(project);
        if (key.empty() || seen.insert(key).second) {
            unique.push_back(std::move(project));
        }
    }

    std::vector<double> scores;
    scores.reserve(unique.size());
    const bool no_query = base.text.empty();
    for (const auto& project : unique) {
        const double weighted = static_cast<double>((std::max)(project.downloads,
                                                                std::uint64_t{1})) *
                                download_weight(project);
        double score = 0.0;
        if (no_query) {
            score = weighted;
        } else {
            score = std::log10(weighted) / 9.0 * 8.0 + similarity(base.text, project);
        }
        scores.push_back(score);
    }
    std::vector<std::size_t> order(unique.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&scores, &unique, &base](std::size_t a,
                                                                            std::size_t b) {
        // 搜索页显式选「最近更新」时按发布日期倒序；其余按 PCL 权重分。
        if (base.sort == ModSort::kUpdated || base.sort == ModSort::kNewest) {
            return unique[a].updated_at > unique[b].updated_at;
        }
        return scores[a] > scores[b];
    });

    ModSearchPage page;
    page.limit = base.limit;
    page.offset = base.offset;
    page.total = -1;
    page.warning = std::move(warning);
    const std::size_t take =
        (std::min)(order.size(), static_cast<std::size_t>((std::max)(1, base.limit)));
    page.items.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        page.items.push_back(std::move(unique[order[i]]));
    }
    page.has_more = order.size() > take;
    return util::Result<ModSearchPage>::ok(std::move(page));
}

util::Result<ModProject> ModCatalog::project(ModPlatform platform, const std::string& id,
                                             const std::stop_token& stop) {
    if (platform == ModPlatform::kCurseForge) {
        return m_curseforge.project(id, stop);
    }
    return m_modrinth.project(id, stop);
}

util::Result<std::vector<ModVersion>> ModCatalog::versions(
    ModPlatform platform, const std::string& project_id, const std::string& game_version,
    ModLoader loader, bool all_versions, const std::stop_token& stop) {
    if (platform == ModPlatform::kCurseForge) {
        return m_curseforge.versions(project_id, game_version, loader, all_versions, stop);
    }
    return m_modrinth.versions(project_id, game_version, loader, all_versions, stop);
}

util::Result<ModVersion> ModCatalog::version(ModPlatform platform,
                                             const std::string& project_id,
                                             const std::string& version_id,
                                             const std::stop_token& stop) {
    if (platform == ModPlatform::kCurseForge) {
        return m_curseforge.version(project_id, version_id, stop);
    }
    return m_modrinth.version(project_id, version_id, stop);
}

util::Result<std::map<std::string, ModVersion>> ModCatalog::version_files(
    ModPlatform platform, const std::vector<std::string>& hashes,
    const std::stop_token& stop) {
    if (platform == ModPlatform::kCurseForge) {
        return util::Result<std::map<std::string, ModVersion>>::fail(util::Error::make(
            "mod.unsupported", "CurseForge 批量 hash 查询暂未接入", {}));
    }
    return m_modrinth.version_files(hashes, stop);
}

util::Result<std::map<std::string, ModVersion>> ModCatalog::update_individual(
    ModPlatform platform, const std::vector<ModUpdateQuery>& queries,
    const std::stop_token& stop) {
    if (platform == ModPlatform::kCurseForge) {
        return util::Result<std::map<std::string, ModVersion>>::fail(util::Error::make(
            "mod.unsupported", "CurseForge 更新查询暂未接入", {}));
    }
    return m_modrinth.update_individual(queries, stop);
}

util::Result<std::vector<CurseForgeFingerprintMatch>> ModCatalog::curseforge_fingerprints(
    const std::vector<std::uint32_t>& fingerprints, const std::stop_token& stop) {
    return m_curseforge.fingerprints(fingerprints, stop);
}

util::Result<std::vector<ModProject>> ModCatalog::curseforge_projects(
    const std::vector<std::string>& ids, const std::stop_token& stop) {
    return m_curseforge.projects(ids, stop);
}

util::Result<std::vector<ModVersion>> ModCatalog::curseforge_files(
    const std::vector<std::string>& ids, const std::stop_token& stop) {
    return m_curseforge.files(ids, stop);
}

}  // namespace sscl::core
