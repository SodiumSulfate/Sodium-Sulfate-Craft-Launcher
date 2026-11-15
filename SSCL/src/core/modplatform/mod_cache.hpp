#pragma once

// Mod API 磁盘缓存（D-055）：
// - 每个平台一个子目录，按 key 的 SHA1 命名；
// - 缓存体是平台原始 JSON（不再二次序列化，保留原始字段）；
// - 文件外层记录 saved_at，供 TTL 判断与“过期回退”；
// - 原子写，失败只记录不影响主流程。

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/modplatform/mod_types.hpp"
#include "util/result.hpp"

namespace sscl::core {

struct CachedJson {
    std::string body;
    std::int64_t saved_at = 0;  // Unix 秒
    bool found = false;
};

class ModCache {
public:
    explicit ModCache(std::filesystem::path root = {});

    bool enabled() const { return !m_root.empty(); }

    // 缺失/损坏都返回 found=false（不视为错误）。
    util::Result<CachedJson> read(ModPlatform platform, const std::string& key) const;

    // saved_at=0 表示当前时间。
    util::Result<void> write(ModPlatform platform, const std::string& key,
                             const std::string& body, std::int64_t saved_at = 0) const;

    std::filesystem::path path_for(ModPlatform platform, const std::string& key) const;

private:
    std::filesystem::path m_root;
};

}  // namespace sscl::core
