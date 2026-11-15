#pragma once

// 账号皮肤（D-121）：把「账号 → 皮肤 PNG 字节」收敛到一处，供主页账号卡与
// 账号管理子窗口列表共用。
//
// 各账号类型的取法：
//   - 微软正版：微软 profile JSON 的 skins[0].url（原有 C-6 实现）。
//   - 外置登录 / 统一通行证：**Yggdrasil sessionserver 的资料端点**（PCL
//     McSkinGetAddress("Auth")、HMCL AuthlibInjectorProvider、Axolotl
//     fetch_yggdrasil_profile 三家一致）：
//       GET <api_root>/sessionserver/session/minecraft/profile/<uuid>
//       → properties[name=textures].value（Base64）
//       → {"textures":{"SKIN":{"url":"https://..."}}}
//     204 表示该角色没有皮肤。此前只对 kMicrosoft 拉皮肤，所以外置登录账号
//     永远显示占位头像。
//   - 离线：UUID 是本地派生，服务器上没有正版皮肤（PCL 同样跳过）。
//
// 皮肤 PNG 按 **URL 的 SHA-256** 缓存到 <data_dir>/cache/skins/：换皮肤后 URL
// 变化会自然重新下载；缓存命中不再发网络请求（第二次打开子窗口秒显）。
// 所有函数都是同步的，UI 必须放到后台线程调用（HomePage / AccountDialog 已如此）。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/account/account.hpp"
#include "util/result.hpp"

namespace sscl::core {

// 微软 profile JSON → 第一件皮肤 URL；无 skins / 解析失败返回空串。
std::string skin_url_from_ms_profile(const std::string& profile_json);

// 纯解析（可单测）：Yggdrasil profile 响应 → 皮肤 URL。
// 校验 Base64、textures.SKIN.url（兼容大小写）与 URL 协议（只接受 http/https，
// 拒绝 file:// 等，避免恶意认证服务器指向本地文件）。
util::Result<std::string> parse_authlib_skin_url(const std::string& profile_body);

// 网络版：请求 <server_url>/sessionserver/session/minecraft/profile/<uuid>。
// server_url 会先经 normalize_authlib_server 归一化。
util::Result<std::string> authlib_skin_url(const std::string& server_url,
                                          const std::string& uuid_compact);

// 统一入口：解析 URL → 读/写磁盘缓存 → 下载 PNG（校验魔数与大小上限）。
// 离线账号返回错误（UI 显示占位）。
util::Result<std::vector<std::uint8_t>> fetch_account_skin_png(const AccountInfo& account);

// 皮肤缓存目录（暴露给诊断/测试）。
std::filesystem::path account_skin_cache_dir();

}  // namespace sscl::core
