#pragma once

// 小密钥存储（D-055）：DPAPI 当前用户加密，文件放在 <data>/secrets.dat。
// 目前只存 CurseForge API Key；后续可扩展其他 Key。
// 明文只在内存中出现；文件内容不可移植（换用户/机器需重新填写）。

#include <filesystem>
#include <string>

#include "util/result.hpp"

namespace sscl::core {

// 缺失文件返回空串（is_ok）；解密失败返回 config.secret_corrupt。
util::Result<std::string> load_secret_value(const std::filesystem::path& file,
                                            const std::string& key);

// 空 value 表示删除该 key；文件不存在时创建。
util::Result<void> store_secret_value(const std::filesystem::path& file,
                                      const std::string& key, const std::string& value);

}  // namespace sscl::core
