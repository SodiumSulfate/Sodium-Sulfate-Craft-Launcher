#pragma once

// 文件下载任务（下载器雏形，开发文档 §7.4 文件级任务）：
// 下载到同目录临时文件 → 可选哈希校验（优先 SHA1）→ 原子改名到目标。
// 分块 / 断点续传 / 镜像回退属于下载调度器，后续批次实现。

#include <filesystem>
#include <string>

#include "core/task/task.hpp"
#include "util/hash.hpp"

namespace sscl::core {

class DownloadTask : public Task {
public:
    struct Params {
        std::string url;
        std::filesystem::path destination;
        std::string expected_hash_hex;  // 空 = 不校验
        util::HashAlgorithm hash_algorithm = util::HashAlgorithm::kSha1;
        std::string display_name;  // 进度显示名；空 = 目标文件名
    };

    explicit DownloadTask(Params params);

protected:
    util::Result<void> do_run() override;

private:
    std::string display_name() const;

    Params m_params;
};

}  // namespace sscl::core
