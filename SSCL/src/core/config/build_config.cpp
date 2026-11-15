#include "core/config/build_config.hpp"

// 构建期生成的密钥头：由 scripts\build.ps1 读取根目录 build-secrets.txt 生成。
// 明文 txt 与生成头都被 .gitignore 排除，源码/工程文件/日志中均无密钥。
// 文件不存在时（全新检出、VS IDE 直接生成）自动回退到构建期宏 / 运行时环境变量，
// 保证任何情况下都能编译（§11.5）。
#if __has_include("core/config/build_secrets.hpp")
#include "core/config/build_secrets.hpp"
#define SSCL_HAS_GENERATED_SECRETS 1
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace sscl::core {

namespace {

// D-090：不再读取构建期宏。
// 旧实现通过 `$(SSCL_MS_CLIENT_ID)` 注入裸 token 宏，Client ID 会出现在
// cl.exe 命令行与 CL.command.1.tlog（CI/构建缓存泄露面）。现在唯一来源是
// scripts\build.ps1 生成的 build_secrets.hpp 或运行时环境变量。

#if defined(SSCL_HAS_GENERATED_SECRETS)
constexpr const char* kFileClientId = sscl::core::detail::kGeneratedClientId;
constexpr const char* kFileCurseForgeKey = sscl::core::detail::kGeneratedCurseForgeKey;
// D-090：发布构建（scripts\build.ps1 -Publish）把该标志置 true 并清空生成值。
// 即使生成头被误带进发布副本，也绝不使用其中的默认凭据。
constexpr bool kPublishBuild = sscl::core::detail::kGeneratedPublishBuild;
#else
constexpr const char* kFileClientId = "";
constexpr const char* kFileCurseForgeKey = "";
constexpr bool kPublishBuild = false;
#endif

// 运行时环境变量回退（PCL 开源版同款机制）；Key 均为 ASCII。
std::string env_ascii(const wchar_t* name, DWORD capacity) {
    wchar_t buffer[256] = {};
    const DWORD cap = capacity < 256 ? capacity : 256;
    const DWORD length = ::GetEnvironmentVariableW(name, buffer, cap);
    if (length == 0 || length >= cap) {
        return {};
    }
    std::string out;
    out.reserve(length);
    for (DWORD i = 0; i < length; ++i) {
        out.push_back(static_cast<char>(buffer[i] & 0x7F));
    }
    return out;
}

std::string env_client_id() {
    return env_ascii(L"SSCL_MS_CLIENT_ID", 128);
}

std::string env_curseforge_key() {
    return env_ascii(L"SSCL_CF_API_KEY", 256);
}

const std::string& client_id_storage() {
    static const std::string value = [] {
        if (!kPublishBuild) {
            std::string from_file = kFileClientId;
            if (!from_file.empty()) {
                return from_file;
            }
        }
        // 发布构建只认运行时环境变量（可被用户自填/部署方注入），不含编译期默认值。
        return env_client_id();
    }();
    return value;
}

const std::string& curseforge_key_storage() {
    static const std::string value = [] {
        if (!kPublishBuild) {
            std::string from_file = kFileCurseForgeKey;
            if (!from_file.empty()) {
                return from_file;
            }
        }
        return env_curseforge_key();
    }();
    return value;
}

}  // namespace

std::string_view microsoft_client_id() {
    return client_id_storage();
}

bool microsoft_login_configured() {
    return !client_id_storage().empty();
}

std::string_view curseforge_api_key() {
    return curseforge_key_storage();
}

bool curseforge_configured() {
    return !curseforge_key_storage().empty();
}

}  // namespace sscl::core
