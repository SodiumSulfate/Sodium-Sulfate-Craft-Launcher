#pragma once

// 构建期注入的配置（开发文档 §9.3；D-106 凭据硬约束）。
//
// 源码中**只允许出现下面两个独一无二的占位符标识符**，绝不出现它们的值：
//   sscl::core::detail::kGeneratedClientId        （微软应用 Client ID）
//   sscl::core::detail::kGeneratedCurseForgeKey   （CurseForge API Key）
// 值只存在于私有文件 <root>\build-secrets.txt 与由 scripts\build.ps1 生成的、
// git-ignored 的 build_secrets.hpp 中。build.ps1 在编译前会扫描源码树，
// 一旦发现明文值立即中止构建；.vcxproj / 命令行 / 日志同样不得携带明文。
//
// 微软应用 Client ID 不得硬编码进源码：
//   1) 构建期由 MSBuild 宏 SSCL_MS_CLIENT_ID 注入（环境变量 SSCL_MS_CLIENT_ID → $(SSCL_MS_CLIENT_ID)）；
//   2) 缺省时回退读取运行时环境变量 SSCL_MS_CLIENT_ID（同 PCL 开源版做法）。
// 两者皆空 = 微软登录不可用（UI 显示提示，不崩溃）。

#include <string_view>

namespace sscl::core {

// 当前生效的微软应用 Client ID（GUID 文本）；未配置时为空串。
std::string_view microsoft_client_id();

// 是否已配置（非空）。
bool microsoft_login_configured();

// CurseForge API Key：构建期生成的 build_secrets.hpp（由根目录 build-secrets.txt
//   经 scripts\build.ps1 派生，二者均 git-ignored）→ 运行时同名环境变量；空 = 未配置。
//   明文不得出现在源码、.vcxproj、日志或文档中；Key 含 '$' / '.'，不能作为裸 token 宏。
//   用户可在“附加”页填写，保存到 DPAPI（secret_store），优先级高于此处。
std::string_view curseforge_api_key();
bool curseforge_configured();

}  // namespace sscl::core
