#pragma once

// 外置登录（authlib-injector 协议；knowledge/launch-args.md §1 账号注入参数）。
// - 登录：POST {api_root}/authserver/authenticate（用户名 + 密码）；
// - 校验/续期：/authserver/validate、/authserver/refresh；
// - 启动注入：-javaagent:<authlib-injector.jar>=<api_root>（jar 缺失时按需下载）。
//
// 服务器地址语义与 PCL/HMCL 一致：用户填的是 API Root（如
// https://littleskin.cn/api/yggdrasil），认证端点为 <api_root>/authserver/*。

#include <filesystem>
#include <functional>
#include <string>

#include "core/account/account.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 归一化服务器地址：补 https://、去尾部 '/'、去掉误填的 /authserver 后缀。
// 空串返回空串（调用方据此报错）。
std::string normalize_authlib_server(const std::string& url);

// 用户名 + 密码登录；成功返回 kAuthlibInjector 账号（含 access_token / client_token /
// server_url / uuid）。user_type 统一为 "mojang"（authlib-injector 要求 legacy 语义）。
util::Result<AccountInfo> authlib_login(const std::string& server_url,
                                        const std::string& username,
                                        const std::string& password);

// accessToken 是否仍有效（用户改名/失效时返回 false，UI 提示重新登录）。
bool authlib_validate(const std::string& server_url, const AccountInfo& account);

// 用已存 accessToken 续期（PCL 启动前会刷新一次）；失败返回错误，调用方提示重新登录。
util::Result<AccountInfo> authlib_refresh(const std::string& server_url,
                                          const AccountInfo& account);

// 确保 <data_dir>/authlib-injector.jar 可用（缺失/损坏时从官方 + BMCLAPI 镜像下载，
// 有 sha256 则校验）。on_stage 用于 UI 文案（"正在下载外置登录组件…"）。
util::Result<std::filesystem::path> ensure_authlib_injector(
    const std::filesystem::path& data_dir,
    const std::function<void(const std::string&)>& on_stage = {});

// 账号启动注入参数（供 UI 层拼进 LaunchSettings::jvm_args）：
//   外置登录 → -javaagent:<jar路径>=<api_root>
// 其它类型返回空串。路径包含空格时整体加引号（launch_command 的 tokenize 支持引号）。
std::string authlib_javaagent_argument(const AccountInfo& account,
                                       const std::filesystem::path& injector_jar);

}  // namespace sscl::core
