#pragma once

// 账号管理（M2；C-10 对标 PCL 左面板账号卡片）：
// 账号列表增删/切换/持久化（AccountStore，DPAPI accounts.dat），
// 微软账号静默续登（MicrosoftAccount::login(refresh_token)）。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/account/account.hpp"
#include "core/account/account_store.hpp"
#include "util/result.hpp"

namespace sscl::core {

class AccountManager {
public:
    explicit AccountManager(std::filesystem::path store_file);

    // 读取 accounts.dat（缺失 = 空列表）。
    util::Result<void> load();

    // 当前选中账号；无则 nullopt。
    std::optional<AccountInfo> selected() const;

    // 选中项下标（持久化于 config.json 的 selected_account_uuid 由 UI 层负责）。
    void select_uuid(const std::string& uuid_compact);

    // 追加并保存（选中它）。
    util::Result<void> add_and_select(const AccountInfo& account);

    // 删除（按 uuid）；删除选中项则回退到第一项。
    util::Result<void> remove(const std::string& uuid_compact);

    // 微软账号过期（提前 1h 已折算进 expires_at_utc）→ 用 refresh_token 静默续登并保存。
    // 非微软/未过期 → Ok 不动作；续登失败返回错误（UI 提示重新登录）。
    util::Result<void> ensure_valid(AccountInfo* account);

    const std::vector<AccountInfo>& accounts() const { return m_accounts; }

private:
    util::Result<void> persist();

    std::filesystem::path m_file;
    AccountStore m_store;
    std::vector<AccountInfo> m_accounts;
    std::size_t m_selected = 0;
};

}  // namespace sscl::core
