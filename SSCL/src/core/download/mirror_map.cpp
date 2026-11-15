#include "core/download/mirror_map.hpp"

#include <utility>

namespace sscl::core {

namespace {

struct UrlParts {
    std::string scheme;  // https://
    std::string host;
    std::string path;  // 以 / 开头
};

bool split_url(const std::string& url, UrlParts* out) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const std::size_t host_begin = scheme_end + 3;
    const std::size_t path_begin = url.find('/', host_begin);
    out->scheme = url.substr(0, scheme_end + 3);
    if (path_begin == std::string::npos) {
        out->host = url.substr(host_begin);
        out->path.clear();
    } else {
        out->host = url.substr(host_begin, path_begin - host_begin);
        out->path = url.substr(path_begin);
    }
    return !out->host.empty();
}

bool host_in(const std::string& host, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (host == name) {
            return true;
        }
    }
    return false;
}

// D-107：全局策略的唯一存储（函数内 static 规避跨 TU 初始化顺序问题）。
SourcePolicy& mutable_source_policy() {
    static SourcePolicy policy;
    return policy;
}

}  // namespace

SourcePolicy make_source_policy(int files, int community) {
    SourcePolicy policy;
    policy.files = (files >= 0 && files <= 2)
                       ? static_cast<SourcePreference>(files)
                       : SourcePreference::kOfficialSlowSwitch;
    policy.community = (community >= 0 && community <= 2)
                           ? static_cast<SourcePreference>(community)
                           : SourcePreference::kPreferOfficial;
    return policy;
}

const SourcePolicy& global_source_policy() {
    return mutable_source_policy();
}

void set_global_source_policy(const SourcePolicy& policy) {
    mutable_source_policy() = policy;
}

std::string map_to_mirror(const std::string& url) {
    UrlParts parts;
    if (!split_url(url, &parts)) {
        return {};
    }

    const std::string& host = parts.host;
    const std::string& path = parts.path;

    // assets（resources.download.minecraft.net/<hash前2>/<hash>）
    if (host == "resources.download.minecraft.net") {
        return "https://bmclapi2.bangbang93.com/assets" + path;
    }

    if (host_in(host, {"piston-data.mojang.com", "piston-meta.mojang.com",
                       "launcher.mojang.com", "launchermeta.mojang.com"})) {
        // Java 运行时与版本元数据：原路径保留
        return "https://bmclapi2.bangbang93.com" + path;
    }

    if (host == "libraries.minecraft.net") {
        // 历史两种映射都保留为候选（maven 优先，见 build_source_candidates）
        return "https://bmclapi2.bangbang93.com/maven" + path;
    }

    // 加载器 maven 仓库（国内官方源常不可达；BMCLAPI 提供 maven 布局镜像）
    if (host == "meta.fabricmc.net") {
        return "https://bmclapi2.bangbang93.com/fabric-meta" + path;
    }
    if (host == "maven.fabricmc.net") {
        return "https://bmclapi2.bangbang93.com/maven" + path;
    }
    if (host == "maven.minecraftforge.net") {
        return "https://bmclapi2.bangbang93.com/maven" + path;
    }
    if (host == "maven.neoforged.net") {
        // 官方 releases 入口 /releases/net/neoforged/... → 镜像 /maven/net/neoforged/...
        std::string mirror_path = path;
        const std::string releases_prefix = "/releases";
        if (mirror_path.compare(0, releases_prefix.size(), releases_prefix) == 0) {
            mirror_path = mirror_path.substr(releases_prefix.size());
        }
        return "https://bmclapi2.bangbang93.com/maven" + mirror_path;
    }

    if (host == "authlib-injector.yushi.moe") {
        return "https://bmclapi2.bangbang93.com/mirrors/authlib-injector" + path;
    }

    if (host == "api.modrinth.com") {
        return "https://mod.mcimirror.top/modrinth" + path;
    }

    if (host == "api.curseforge.com") {
        return "https://mod.mcimirror.top/curseforge" + path;
    }

    // D-107：修复死分支。原写法 compare(host.size()-12, 12, ".forgecdn.net")
    // 中字面量长 13，与 12 字符后缀永不相等（标准/MSVC 两种语义皆然），
    // 导致 CurseForge 文件下载一直没有镜像候选。
    // 按 D-055 合规红线只白名单 edge（mediafilez.forgecdn.net 明确不替换）。
    if (host == "edge.forgecdn.net") {
        return "https://mod.mcimirror.top" + path;
    }
    // Modrinth 文件 CDN：与 mod_api.cpp::mcim_url 的映射保持一致。
    // 官方失败时由 DownloadScheduler 自动回退该候选（MCIM 未命中会返回错误并换源）。
    if (host == "cdn.modrinth.com") {
        return "https://mod.mcimirror.top" + path;
    }

    return {};
}

std::vector<std::string> build_source_candidates(const std::string& url) {
    return build_source_candidates(url, global_source_policy());
}

std::vector<std::string> build_source_candidates(const std::string& url,
                                                 const SourcePolicy& policy) {
    std::vector<std::string> candidates;
    candidates.push_back(url);

    const std::string mirror = map_to_mirror(url);
    if (!mirror.empty() && mirror != url) {
        candidates.push_back(mirror);
    }

    // libraries.minecraft.net 的第二种历史映射（/libraries/...）
    UrlParts parts;
    if (split_url(url, &parts) && parts.host == "libraries.minecraft.net") {
        const std::string alt = "https://bmclapi2.bangbang93.com/libraries" + parts.path;
        candidates.push_back(alt);
    }

    // D-107「尽量使用镜像源」：把镜像候选提到官方之前（官方仍保留作兜底）。
    if (policy.files_prefer_mirror() && !mirror.empty() && mirror != url &&
        candidates.size() >= 2) {
        std::swap(candidates[0], candidates[1]);
    }
    return candidates;
}

bool is_bmclapi_url(const std::string& url) {
    return url.find("bmclapi2.bangbang93.com") != std::string::npos;
}

}  // namespace sscl::core
