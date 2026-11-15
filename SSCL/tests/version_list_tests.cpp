#include <doctest/doctest.h>

#include <string>

#include "core/game/version_list.hpp"

namespace core = sscl::core;

TEST_CASE("versionlist: 清单解析（latest + entries）") {
    const std::string text = R"({
        "latest": {"release": "1.20.1", "snapshot": "23w31a"},
        "versions": [
            {"id": "1.20.1", "type": "release", "url": "https://piston-meta.mojang.com/v1/1.20.1.json",
             "time": "2023-06-12T00:00:00+00:00", "releaseTime": "2023-06-12T00:00:00+00:00"},
            {"id": "23w31a", "type": "snapshot", "url": "https://piston-meta.mojang.com/v1/23w31a.json"},
            {"id": "", "type": "release", "url": ""}
        ]
    })";
    auto parsed = core::parse_version_list(text);
    REQUIRE(parsed.is_ok());
    const core::VersionList& list = parsed.value();
    CHECK(list.latest_release == "1.20.1");
    CHECK(list.latest_snapshot == "23w31a");
    REQUIRE(list.entries.size() == 2);  // id/url 缺失的条目被丢弃
    CHECK(list.entries[0].id == "1.20.1");
    CHECK(list.entries[0].type == "release");
    CHECK(list.entries[0].url.find("1.20.1.json") != std::string::npos);
    CHECK(list.entries[1].id == "23w31a");
    CHECK(list.entries[1].release_time.empty());  // 缺字段容错
}

TEST_CASE("versionlist: 坏输入") {
    CHECK(core::parse_version_list("{not json").is_error());
    CHECK(core::parse_version_list("[]").is_error());
    auto empty = core::parse_version_list("{}");
    REQUIRE(empty.is_ok());
    CHECK(empty.value().entries.empty());
}
