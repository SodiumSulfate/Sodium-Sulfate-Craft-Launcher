#pragma once

// Maven maven-metadata.xml 轻量解析（**不引入 XML 库**，只提取 <latest>/<release>/<version>）。
// 用途：Forge 版本清单（https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml）。

#include <string>
#include <vector>

#include "util/result.hpp"

namespace sscl::core {

struct MavenMetadata {
    std::string latest;
    std::string release;
    std::vector<std::string> versions;  // 文档原序（通常新→旧）
};

// 解析失败（不是 metadata / 无 <version>）返回 loader.maven_metadata_invalid。
util::Result<MavenMetadata> parse_maven_metadata(const std::string& xml_text);

}  // namespace sscl::core
