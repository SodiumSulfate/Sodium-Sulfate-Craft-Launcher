#include "core/config/secret_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dpapi.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"
#include "util/log.hpp"

#pragma comment(lib, "crypt32.lib")

namespace sscl::core {

namespace {

using nlohmann::json;

// DPAPI 输出由 LocalAlloc 分配，必须 LocalFree；输入借用 string 缓冲区，不能释放。
struct Blob {
    DATA_BLOB data{};
    ~Blob() {
        if (data.pbData != nullptr) {
            ::LocalFree(data.pbData);
        }
    }
    Blob() = default;
    Blob(const Blob&) = delete;
    Blob& operator=(const Blob&) = delete;
};

util::Result<std::string> decrypt(const std::string& cipher) {
    if (cipher.empty()) {
        return util::Result<std::string>::ok(std::string());
    }
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(cipher.data()));
    input.cbData = static_cast<DWORD>(cipher.size());
    Blob output;
    if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output.data)) {
        return util::Result<std::string>::fail(util::Error::make(
            "config.secret_corrupt", "无法解密密钥文件", std::to_string(::GetLastError())));
    }
    return util::Result<std::string>::ok(
        std::string(reinterpret_cast<const char*>(output.data.pbData), output.data.cbData));
}

util::Result<std::string> encrypt(const std::string& plain) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    Blob output;
    if (!::CryptProtectData(&input, L"SSCL secrets", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output.data)) {
        return util::Result<std::string>::fail(util::Error::make(
            "config.secret_encrypt_failed", "无法加密密钥文件",
            std::to_string(::GetLastError())));
    }
    return util::Result<std::string>::ok(
        std::string(reinterpret_cast<const char*>(output.data.pbData), output.data.cbData));
}

util::Result<json> load_json(const std::filesystem::path& file) {
    json root = json::object();
    util::Result<std::string> raw = platform::read_file(file);
    if (raw.is_error()) {
        return util::Result<json>::ok(std::move(root));
    }
    util::Result<std::string> plain = decrypt(raw.value());
    if (plain.is_error()) {
        return util::Result<json>::fail(std::move(plain.error()));
    }
    if (plain.value().empty()) {
        return util::Result<json>::ok(std::move(root));
    }
    json parsed = json::parse(plain.value(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return util::Result<json>::fail(util::Error::make(
            "config.secret_corrupt", "密钥文件格式不正确", {}));
    }
    return util::Result<json>::ok(std::move(parsed));
}

}  // namespace

util::Result<std::string> load_secret_value(const std::filesystem::path& file,
                                            const std::string& key) {
    util::Result<json> root = load_json(file);
    if (root.is_error()) {
        return util::Result<std::string>::fail(std::move(root.error()));
    }
    if (!root.value().contains(key) || !root.value()[key].is_string()) {
        return util::Result<std::string>::ok(std::string());
    }
    return util::Result<std::string>::ok(root.value()[key].get<std::string>());
}

util::Result<void> store_secret_value(const std::filesystem::path& file,
                                      const std::string& key, const std::string& value) {
    util::Result<json> root = load_json(file);
    if (root.is_error()) {
        return util::Result<void>::fail(std::move(root.error()));
    }
    if (value.empty()) {
        root.value().erase(key);
    } else {
        root.value()[key] = value;
    }
    util::Result<std::string> cipher = encrypt(root.value().dump());
    if (cipher.is_error()) {
        return util::Result<void>::fail(std::move(cipher.error()));
    }
    util::Result<void> created = ::sscl::platform::create_directories(file.parent_path());
    if (created.is_error()) {
        return created;
    }
    return platform::write_file_atomic(file, cipher.value());
}

}  // namespace sscl::core
