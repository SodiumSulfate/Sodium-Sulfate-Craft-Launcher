#pragma once

// 账号存储（§5.1/§9.1：accounts.dat，DPAPI 用户级加密；§9.2 日志脱敏由 Logger 覆盖）。
// 文件格式：CryptProtectData 密文；明文为 JSON 数组。

#include <filesystem>
#include <vector>

#include "core/account/account.hpp"
#include "util/result.hpp"

namespace sscl::core {

class AccountStore {
public:
    explicit AccountStore(std::filesystem::path file);

    // 读取账号列表；文件不存在返回空列表（不视为错误）。
    util::Result<std::vector<AccountInfo>> load() const;

    // 覆盖保存（DPAPI 加密 + 原子写）。
    util::Result<void> save(const std::vector<AccountInfo>& accounts) const;

private:
    std::filesystem::path m_file;
};

}  // namespace sscl::core
