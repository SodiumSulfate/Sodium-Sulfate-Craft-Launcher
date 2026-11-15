#include "core/modplatform/mod_cache.hpp"

#include <chrono>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ModCache::ModCache(std::filesystem::path root) : m_root(std::move(root)) {}

std::filesystem::path ModCache::path_for(ModPlatform platform,
                                         const std::string& key) const {
    const std::string name = util::hash_hex(util::HashAlgorithm::kSha1, key);
    return m_root / mod_platform_key(platform) / (name + ".json");
}

util::Result<CachedJson> ModCache::read(ModPlatform platform,
                                        const std::string& key) const {
    CachedJson result;
    if (!enabled()) {
        return util::Result<CachedJson>::ok(std::move(result));
    }
    const std::filesystem::path path = path_for(platform, key);
    util::Result<std::string> text = platform::read_file(path);
    if (text.is_error()) {
        return util::Result<CachedJson>::ok(std::move(result));
    }
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        util::log_warn("[modcache] ignore invalid cache: " + util::path_to_utf8(path));
        return util::Result<CachedJson>::ok(std::move(result));
    }
    if (root.contains("saved_at") && root["saved_at"].is_number_integer()) {
        result.saved_at = root["saved_at"].get<std::int64_t>();
    }
    if (root.contains("body") && root["body"].is_string()) {
        result.body = root["body"].get<std::string>();
        result.found = true;
    }
    return util::Result<CachedJson>::ok(std::move(result));
}

util::Result<void> ModCache::write(ModPlatform platform, const std::string& key,
                                   const std::string& body,
                                   std::int64_t saved_at) const {
    if (!enabled()) {
        return util::Result<void>::ok();
    }
    const std::filesystem::path path = path_for(platform, key);
    util::Result<void> created = ::sscl::platform::create_directories(path.parent_path());
    if (created.is_error()) {
        return created;
    }
    json root;
    root["saved_at"] = saved_at > 0 ? saved_at : now_seconds();
    root["body"] = body;
    return platform::write_file_atomic(path, root.dump());
}

}  // namespace sscl::core
