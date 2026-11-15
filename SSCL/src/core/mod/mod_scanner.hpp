#pragma once

// 本地 mods 扫描与 SHA1 缓存（D-058）：
// - 安装前判断“同名/同哈希”；
// - 缓存键 = 路径 + 大小 + 修改时间，文件变动自动失效；
// - 只读扫描，不修改文件。

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

struct LocalModEntry {
    std::string file_name;
    std::filesystem::path full_path;
    std::string enabled_name;  // 去掉 .disabled/.old 后的名称
    std::uint64_t size = 0;
    bool disabled = false;
    bool old = false;
    bool enabled = false;  // PCL IsEnabled：扩展名为 jar/zip/litemod
};

// 列出 Mod 文件；include_old=true 时含 .old。PCL 支持 jar/zip/litemod。
std::vector<LocalModEntry> list_local_mods(const std::filesystem::path& mods_dir,
                                           bool include_old = false);

// PCL 扫描：按 enabled_name 去重，同名启用优先；默认含 .old，排序文件名。
std::vector<LocalModEntry> scan_pcl_mods(const std::filesystem::path& mods_dir);

// PCL 文件名语义：去掉 .disabled/.old（大小写不敏感），返回启用时的文件名。
std::string mod_enabled_name(const std::string& file_name);
bool mod_file_is_enabled(const std::string& file_name);
bool mod_file_is_old(const std::string& file_name);

// PCL 本地导入：校验扩展名、去状态后缀、无扩展补 .jar、同名直接覆盖、跳过自身。
// 返回成功导入数量；单个文件失败会跳过并保留已完成项。
util::Result<int> import_mod_files(const std::filesystem::path& mods_dir,
                                   const std::vector<std::filesystem::path>& files);

// 启用/禁用。禁用优先 .old（若已存在），否则 .disabled；目标已存在且内容不同返回
// mod.conflict，相同则删除源文件视为已完成。失败返回文件相关错误。
util::Result<void> set_mod_enabled(const std::filesystem::path& file, bool enable);

class ModHashCache {
public:
    explicit ModHashCache(std::filesystem::path cache_file = {});

    bool enabled() const { return !m_file.empty(); }

    // 计算/读取缓存；失败返回 mod.file_unreadable。
    util::Result<std::string> sha1(const std::filesystem::path& file);

    // CurseForge MurmurHash2 指纹；失败返回 mod.file_unreadable。
    util::Result<std::uint32_t> curseforge_fingerprint(const std::filesystem::path& file);

    // 新计算的条目落盘；无变化时直接成功。
    util::Result<void> save();

private:
    std::string key_for(const std::filesystem::path& file, std::uint64_t size,
                        std::int64_t mtime) const;
    util::Result<void> load();

    std::filesystem::path m_file;
    std::map<std::string, std::string> m_entries;
    bool m_dirty = false;
};

}  // namespace sscl::core
