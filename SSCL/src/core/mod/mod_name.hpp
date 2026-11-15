#pragma once

// Mod 文件名安全化（D-058）：
// - 远端/平台返回的文件名不可信：必须取 basename、拒绝路径穿越；
// - 清洗 Windows 非法字符、保留名、结尾点/空格；
// - `~` 替换为 `-`（PCL 实测：`~` 会导致 Mixin 加载失败）；
// - 只允许 .jar；超长按 UTF-8 码点截断（保留扩展名）。
//
// 该模块只做纯字符串处理，便于单元测试；不访问磁盘。

#include <cstddef>
#include <string>

#include "util/result.hpp"

namespace sscl::core {

inline constexpr std::size_t kMaxModFileNameBytes = 120;

// 清洗文件名；失败返回 mod.file_name_invalid。
util::Result<std::string> sanitize_mod_file_name(const std::string& raw);

// 是否为可安装的 Mod 文件（当前只接受 .jar，忽略大小写）。
bool is_installable_mod_file(const std::string& file_name);

// 是否已被禁用（.disabled）或备份（.old）。
bool is_disabled_mod_file(const std::string& file_name);
bool is_old_mod_file(const std::string& file_name);

// 去掉 .disabled / .old 后缀（用于显示与“同 Mod”判断）。
std::string strip_mod_state_suffix(const std::string& file_name);

// 备份/禁用文件名：x.jar -> x.jar.old / x.jar.disabled。
std::string make_old_mod_file_name(const std::string& file_name);
std::string make_disabled_mod_file_name(const std::string& file_name);

}  // namespace sscl::core
