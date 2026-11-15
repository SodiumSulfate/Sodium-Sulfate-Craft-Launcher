#pragma once

// natives 解压（knowledge/launch-args.md §3；开发文档 §7.1）。
// 目录 versions\<id>\<id>-natives（由调用方给定）；解压前清空；
// 跳过 extract.exclude 前缀、.sha1/.git 后缀、已存在且大小一致的文件。

#include <filesystem>
#include <string>
#include <vector>

#include "core/launch/version_json.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 解压所有 natives 库（select_native_classifier 命中的）到 natives_dir。
util::Result<void> extract_natives(const std::vector<Library>& libraries,
                                   const std::filesystem::path& libraries_dir,
                                   const std::string& os_name, bool is_64bit,
                                   const std::filesystem::path& natives_dir);

}  // namespace sscl::core
