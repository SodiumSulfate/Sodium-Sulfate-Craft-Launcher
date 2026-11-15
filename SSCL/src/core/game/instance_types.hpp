#pragma once

// 版本管理共享类型（批次 V1）：
// - InstalledVersion：磁盘上存在 <versions>/<id>/<id>.json 的版本；
// - VersionSummary：供版本选择页/详情页使用的识别结果与用户设置；
// - InstanceState / InstanceCategory：与 PCL McInstanceState / McInstanceCardType 对齐。
// 只放数据，不访问磁盘；扫描与分类见 instance_registry。

#include <string>

namespace sscl::core {

// 已安装版本（versions/<id>/<id>.json 存在即算）。
struct InstalledVersion {
    std::string id;
    std::string type;  // release / snapshot / old_beta / old_alpha（JSON 缺省 release）
};

enum class InstanceState {
    kOriginal,
    kSnapshot,
    kOld,
    kFool,
    kForge,
    kNeoForge,
    kFabric,
    kQuilt,
    kError,
};

// 与 PCL McInstanceCardType 对齐；kStar 只用于分类结果，不写入 DisplayType。
enum class InstanceCategory {
    kStar = -1,
    kAuto = 0,
    kHidden = 1,
    kApi = 2,
    kOriginalLike = 3,
    kRubbish = 4,
    kFool = 5,
    kError = 6,
};

struct VersionSummary {
    InstalledVersion version;
    std::string minecraft_version;
    std::string loader;          // fabric / forge / neoforge / quilt / 空
    std::string loader_version;
    std::string release_time;    // PCL releaseTime（ISO 字符串；未知为空）
    bool isolated = false;
    bool moddable = false;
    bool release = true;
    int drop = 209;
    InstanceState state = InstanceState::kOriginal;
    bool starred = false;
    InstanceCategory category = InstanceCategory::kAuto;
    std::string custom_info;     // 版本列表的“自定义描述”
    std::string logo;            // 自定义图标路径（空 = 自动）
    std::string error;           // 非空 = 错误版本原因
};

}  // namespace sscl::core
