#pragma once

// 官方 ↔ 镜像 URL 映射（knowledge/download-mirrors.md §2，PCL ModDownload.vb 行为数据）。
// §7.4：官方 → BMCLAPI 自动回退；映射集中于此便于日后增删镜像。

#include <string>
#include <vector>

namespace sscl::core {

// D-107 网络源偏好（设置页「其他」两组下拉共用，0..2）。
// 0 尽量使用镜像源 / 1 官方优先且加载缓慢时换镜像 / 2 尽量使用官方源。
enum class SourcePreference {
    kPreferMirror = 0,
    kOfficialSlowSwitch = 1,
    kPreferOfficial = 2,
};

struct SourcePolicy {
    SourcePreference files = SourcePreference::kOfficialSlowSwitch;  // 下载及列表（默认）
    SourcePreference community = SourcePreference::kPreferOfficial;  // 社区资源（默认）

    bool files_prefer_mirror() const { return files == SourcePreference::kPreferMirror; }
    bool files_slow_switch() const { return files == SourcePreference::kOfficialSlowSwitch; }
    bool community_prefer_mirror() const {
        return community == SourcePreference::kPreferMirror;
    }
    bool community_slow_switch() const {
        return community == SourcePreference::kOfficialSlowSwitch;
    }
};

// 从 config.json 的整数构造，非法值回退默认；进程级全局策略由 AppShell 同步，
// 避免把策略参数逐层穿透到 loader_catalog / mod_api / download_scheduler。
SourcePolicy make_source_policy(int files, int community);
void set_global_source_policy(const SourcePolicy& policy);
const SourcePolicy& global_source_policy();

// 把官方 URL 换算为 BMCLAPI/MCIM 镜像 URL；无法映射时返回空串。
std::string map_to_mirror(const std::string& url);

// 源候选列表（保序去重）：官方优先 → 镜像（§7.4 固定优先级；动态调序留 M5）。
std::vector<std::string> build_source_candidates(const std::string& url);

// 显式策略版本（供单测与特殊调用方使用）；无参版本读全局策略。
std::vector<std::string> build_source_candidates(const std::string& url,
                                                 const SourcePolicy& policy);

// 是否为 BMCLAPI 源（403/429 为限频，不计入禁用，knowledge §3）。
bool is_bmclapi_url(const std::string& url);

}  // namespace sscl::core
