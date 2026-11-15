#pragma once

// ZIP 读取工具（miniz；供加载器安装器解包：install_profile/version.json、内嵌 maven/**、data/*）。
// 设计取舍：每次调用独立打开归档（安装器体积小、调用次数少），避免 RAII 归档对象的移动语义复杂度。
// 安全：解包一律做路径校验（拒绝绝对路径、盘符、`.`/`..`、空段），绝不写到目标目录之外。

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.hpp"

namespace sscl::platform {

// 归档内文件条目名（不含目录条目；保留原始 "/" 分隔）。
util::Result<std::vector<std::string>> zip_entry_names(const std::filesystem::path& archive);
util::Result<std::vector<std::string>> zip_entry_names_from_memory(std::string_view bytes);

// 按条目名读取全部字节；条目不存在或不是文件 ⇒ zip.entry_missing。
util::Result<std::string> zip_read_entry(const std::filesystem::path& archive,
                                        const std::string& entry);
util::Result<std::string> zip_read_entry_from_memory(std::string_view bytes,
                                                     const std::string& entry);

// 解包子树到 dest_dir（prefix 为空 = 全部；prefix 按目录前缀匹配，并**从目标相对路径中剥除**，
// 例如 zip_extract_subtree(installer, "maven/", libraries) → libraries/net/minecraftforge/...）。
// 返回写出的文件数；已存在且大小一致的文件跳过（幂等）。
util::Result<std::size_t> zip_extract_subtree(const std::filesystem::path& archive,
                                             const std::string& prefix,
                                             const std::filesystem::path& dest_dir);
util::Result<std::size_t> zip_extract_subtree_from_memory(std::string_view bytes,
                                                          const std::string& prefix,
                                                          const std::filesystem::path& dest_dir);

struct ZipWriteEntry {
    std::string archive_path;       // 归档内相对路径（"/" 分隔）
    std::filesystem::path file;     // 本地文件
};

// 写 ZIP（V6 整合包导出）：覆盖创建；任一文件失败则删除半成品并返回错误。
util::Result<void> zip_write_files(const std::filesystem::path& archive,
                                   const std::vector<ZipWriteEntry>& entries);

}  // namespace sscl::platform
