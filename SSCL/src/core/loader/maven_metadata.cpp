#include "core/loader/maven_metadata.hpp"

#include <cctype>

namespace sscl::core {

namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 取第一个 <tag>...</tag> 的内容（无则空）。仅用于 maven-metadata 这种扁平结构。
std::string tag_text(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::size_t begin = xml.find(open);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t content_begin = begin + open.size();
    const std::size_t end = xml.find(close, content_begin);
    if (end == std::string::npos) {
        return {};
    }
    return trim(xml.substr(content_begin, end - content_begin));
}

}  // namespace

util::Result<MavenMetadata> parse_maven_metadata(const std::string& xml_text) {
    MavenMetadata metadata;
    if (xml_text.find("<metadata") == std::string::npos) {
        return util::Result<MavenMetadata>::fail(util::Error::make(
            "loader.maven_metadata_invalid", "Maven 元数据格式不正确", "缺少 <metadata>"));
    }

    metadata.latest = tag_text(xml_text, "latest");
    metadata.release = tag_text(xml_text, "release");

    const std::string open = "<version>";
    const std::string close = "</version>";
    std::size_t cursor = 0;
    while (true) {
        const std::size_t begin = xml_text.find(open, cursor);
        if (begin == std::string::npos) {
            break;
        }
        const std::size_t content_begin = begin + open.size();
        const std::size_t end = xml_text.find(close, content_begin);
        if (end == std::string::npos) {
            break;
        }
        std::string value = trim(xml_text.substr(content_begin, end - content_begin));
        if (!value.empty()) {
            metadata.versions.push_back(std::move(value));
        }
        cursor = end + close.size();
    }

    if (metadata.versions.empty()) {
        return util::Result<MavenMetadata>::fail(util::Error::make(
            "loader.maven_metadata_invalid", "Maven 元数据中没有版本条目", {}));
    }
    if (metadata.latest.empty()) {
        metadata.latest = metadata.versions.front();
    }
    if (metadata.release.empty()) {
        metadata.release = metadata.latest;
    }
    return util::Result<MavenMetadata>::ok(std::move(metadata));
}

}  // namespace sscl::core
