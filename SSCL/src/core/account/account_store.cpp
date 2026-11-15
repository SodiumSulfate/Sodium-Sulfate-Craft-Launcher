#include "core/account/account_store.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dpapi.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"

#pragma comment(lib, "crypt32.lib")

namespace sscl::core {

namespace {

using nlohmann::json;

struct Blob {
    DATA_BLOB data{};
    ~Blob() {
        if (data.pbData != nullptr) {
            ::LocalFree(data.pbData);  // 仅用于 DPAPI 输出（LocalAlloc 分配）
        }
    }
    Blob() = default;
    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;
};

json account_to_json(const AccountInfo& account) {
    json node;
    node["type"] = account_type_name(account.type);
    node["username"] = account.username;
    node["uuid"] = account.uuid_compact;
    node["token"] = account.access_token;
    node["user_type"] = account.user_type;
    node["user_properties"] = account.user_properties;
    node["server_url"] = account.server_url;
    node["refresh_token"] = account.refresh_token;
    node["expires_at_utc"] = account.expires_at_utc;
    node["profile_json"] = account.profile_json;
    node["client_token"] = account.client_token;
    return node;
}

AccountType account_type_from_name(const std::string& name) {
    if (name == "authlib_injector") return AccountType::kAuthlibInjector;
    if (name == "nide8") return AccountType::kNide8;
    if (name == "microsoft") return AccountType::kMicrosoft;
    return AccountType::kOffline;
}

AccountInfo account_from_json(const json& node) {
    AccountInfo account;
    account.type = account_type_from_name(node.value("type", "offline"));
    account.username = node.value("username", "");
    account.uuid_compact = node.value("uuid", "");
    account.access_token = node.value("token", "");
    account.user_type = node.value("user_type", "msa");
    account.user_properties = node.value("user_properties", "{}");
    account.server_url = node.value("server_url", "");
    account.refresh_token = node.value("refresh_token", "");
    account.expires_at_utc = node.value("expires_at_utc", static_cast<std::int64_t>(0));
    account.profile_json = node.value("profile_json", "");
    account.client_token = node.value("client_token", "");
    return account;
}

}  // namespace

AccountStore::AccountStore(std::filesystem::path file) : m_file(std::move(file)) {}

util::Result<std::vector<AccountInfo>> AccountStore::load() const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(m_file, ec)) {
        return util::Result<std::vector<AccountInfo>>::ok({});
    }
    util::Result<std::string> encrypted = platform::read_file(m_file);
    if (encrypted.is_error()) {
        return util::Result<std::vector<AccountInfo>>::fail(std::move(encrypted.error()));
    }

    // input 借用 string 缓冲区，绝不能 LocalFree —— 用裸 DATA_BLOB（无析构）
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.value().data()));
    input.cbData = static_cast<DWORD>(encrypted.value().size());

    Blob output;
    if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output.data)) {
        return util::Result<std::vector<AccountInfo>>::fail(util::Error::make(
            "account.decrypt_failed", "无法解密账号数据", std::to_string(::GetLastError())));
    }

    const std::string plain(reinterpret_cast<const char*>(output.data.pbData),
                            output.data.cbData);
    json root = json::parse(plain, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return util::Result<std::vector<AccountInfo>>::fail(
            util::Error::make("account.parse_failed", "账号数据格式错误", {}));
    }

    std::vector<AccountInfo> accounts;
    for (const json& node : root) {
        if (node.is_object()) {
            accounts.push_back(account_from_json(node));
        }
    }
    return util::Result<std::vector<AccountInfo>>::ok(std::move(accounts));
}

util::Result<void> AccountStore::save(const std::vector<AccountInfo>& accounts) const {
    json root = json::array();
    for (const AccountInfo& account : accounts) {
        root.push_back(account_to_json(account));
    }
    const std::string plain = root.dump();

    // input 借用 string 缓冲区，绝不能 LocalFree —— 用裸 DATA_BLOB（无析构）
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());

    Blob output;
    if (!::CryptProtectData(&input, L"SSCL accounts", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output.data)) {
        return util::Result<void>::fail(util::Error::make(
            "account.encrypt_failed", "无法加密账号数据", std::to_string(::GetLastError())));
    }

    const std::string encrypted(reinterpret_cast<const char*>(output.data.pbData),
                                output.data.cbData);
    // D-119：账号新增/删除发生在 UI 线程按钮回调里，不等强制刷盘（仍原子替换），
    // 避免 DPAPI 之后又卡一次磁盘 flush。
    return platform::write_file_atomic(m_file, encrypted, /*flush_to_disk=*/false);
}

}  // namespace sscl::core
