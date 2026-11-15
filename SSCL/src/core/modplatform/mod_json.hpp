#pragma once

// 平台 JSON 解析的小工具（Modrinth/CurseForge 共用）。
// 只做类型安全的取值，不抛异常（nlohmann 的 `at` 会抛，故全部 contains 检查）。

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/result.hpp"

namespace sscl::core::modjson {

inline std::string str(const nlohmann::json& node, const char* key) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_string()) {
        return {};
    }
    return node[key].get<std::string>();
}

inline std::int64_t integer(const nlohmann::json& node, const char* key,
                            std::int64_t fallback = 0) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_number_integer()) {
        return fallback;
    }
    return node[key].get<std::int64_t>();
}

inline bool boolean(const nlohmann::json& node, const char* key, bool fallback = false) {
    if (!node.is_object() || !node.contains(key) || !node[key].is_boolean()) {
        return fallback;
    }
    return node[key].get<bool>();
}

inline bool has(const nlohmann::json& node, const char* key) {
    return node.is_object() && node.contains(key);
}

inline std::vector<std::string> str_array(const nlohmann::json& node, const char* key) {
    std::vector<std::string> out;
    if (!node.is_object() || !node.contains(key) || !node[key].is_array()) {
        return out;
    }
    for (const nlohmann::json& item : node[key]) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

// 解析失败返回 mod.api_invalid。
inline util::Result<nlohmann::json> parse_object(const std::string& text,
                                                 const char* what) {
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        return util::Result<nlohmann::json>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据",
            std::string(what) + "：JSON 解析失败"));
    }
    if (!root.is_object()) {
        return util::Result<nlohmann::json>::fail(util::Error::make(
            "mod.api_invalid", "平台返回了无法识别的数据",
            std::string(what) + "：根节点不是对象"));
    }
    return util::Result<nlohmann::json>::ok(std::move(root));
}

}  // namespace sscl::core::modjson
