#include "core/account/account_manager.hpp"

#include <algorithm>
#include <ctime>
#include <utility>

#include "util/log.hpp"

namespace sscl::core {

AccountManager::AccountManager(std::filesystem::path store_file)
    : m_file(std::move(store_file)), m_store(m_file) {}

util::Result<void> AccountManager::load() {
    util::Result<std::vector<AccountInfo>> loaded = m_store.load();
    if (loaded.is_error()) {
        return util::Result<void>::fail(std::move(loaded.error()));
    }
    m_accounts = std::move(loaded.value());
    if (m_selected >= m_accounts.size()) {
        m_selected = 0;
    }
    return util::Result<void>::ok();
}

std::optional<AccountInfo> AccountManager::selected() const {
    if (m_accounts.empty()) {
        return std::nullopt;
    }
    return m_accounts[m_selected];
}

void AccountManager::select_uuid(const std::string& uuid_compact) {
    for (std::size_t i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].uuid_compact == uuid_compact) {
            m_selected = i;
            return;
        }
    }
}

util::Result<void> AccountManager::add_and_select(const AccountInfo& account) {
    // 同 uuid 覆盖（改名/续登场景），否则追加。
    for (std::size_t i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].uuid_compact == account.uuid_compact) {
            m_accounts[i] = account;
            m_selected = i;
            return persist();
        }
    }
    m_accounts.push_back(account);
    m_selected = m_accounts.size() - 1;
    return persist();
}

util::Result<void> AccountManager::remove(const std::string& uuid_compact) {
    const auto it = std::remove_if(m_accounts.begin(), m_accounts.end(),
                                   [&](const AccountInfo& item) {
                                       return item.uuid_compact == uuid_compact;
                                   });
    if (it == m_accounts.end()) {
        return util::Result<void>::ok();
    }
    m_accounts.erase(it, m_accounts.end());
    if (m_selected >= m_accounts.size()) {
        m_selected = 0;
    }
    return persist();
}

util::Result<void> AccountManager::ensure_valid(AccountInfo* account) {
    if (account == nullptr || account->type != AccountType::kMicrosoft) {
        return util::Result<void>::ok();
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (account->expires_at_utc > now + 300 && !account->access_token.empty()) {
        return util::Result<void>::ok();  // 未过期（留 5 分钟余量，Axolotl 策略）
    }
    MicrosoftAccount provider;
    util::Result<AccountInfo> refreshed = provider.login(account->refresh_token);
    if (refreshed.is_error()) {
        return util::Result<void>::fail(std::move(refreshed.error()));
    }
    *account = refreshed.value();
    return add_and_select(*account);
}

util::Result<void> AccountManager::persist() {
    return m_store.save(m_accounts);
}

}  // namespace sscl::core
