#include <string>

#include <doctest/doctest.h>

#include "core/mod/mod_name.hpp"

using sscl::core::is_disabled_mod_file;
using sscl::core::is_installable_mod_file;
using sscl::core::is_old_mod_file;
using sscl::core::make_disabled_mod_file_name;
using sscl::core::make_old_mod_file_name;
using sscl::core::sanitize_mod_file_name;
using sscl::core::strip_mod_state_suffix;

TEST_CASE("mod.name: 取 basename 并阻止路径穿越") {
    const auto a = sanitize_mod_file_name("../evil.jar");
    REQUIRE(a.is_ok());
    REQUIRE(a.value() == "evil.jar");

    const auto b = sanitize_mod_file_name("dir\\sub\\mod.jar");
    REQUIRE(b.is_ok());
    REQUIRE(b.value() == "mod.jar");

    REQUIRE(sanitize_mod_file_name("..").is_error());
    REQUIRE(sanitize_mod_file_name("dir/").is_error());
    REQUIRE(sanitize_mod_file_name("").is_error());
}

TEST_CASE("mod.name: 清洗非法字符、~ 与保留名") {
    const auto a = sanitize_mod_file_name("a:b?c*.jar");
    REQUIRE(a.is_ok());
    REQUIRE(a.value() == "a_b_c_.jar");

    const auto b = sanitize_mod_file_name("mod~1.jar");
    REQUIRE(b.is_ok());
    REQUIRE(b.value() == "mod-1.jar");

    const auto c = sanitize_mod_file_name("CON.jar");
    REQUIRE(c.is_ok());
    REQUIRE(c.value() == "_CON.jar");

    const auto d = sanitize_mod_file_name("aux");
    REQUIRE(d.is_error());  // 非 jar

    const auto e = sanitize_mod_file_name("name .jar");
    REQUIRE(e.is_ok());
    REQUIRE(e.value() == "name.jar");  // 扩展名前空格被清理
}

TEST_CASE("mod.name: 只接受 .jar（大小写不敏感）") {
    REQUIRE(is_installable_mod_file("x.jar"));
    REQUIRE(is_installable_mod_file("X.JAR"));
    REQUIRE_FALSE(is_installable_mod_file("x.zip"));
    REQUIRE_FALSE(is_installable_mod_file("x.jar.disabled"));
    REQUIRE(sanitize_mod_file_name("x.zip").is_error());
    REQUIRE(sanitize_mod_file_name("x.JAR").is_ok());
    REQUIRE(sanitize_mod_file_name("x.JAR").value() == "x.jar");
}

TEST_CASE("mod.name: 长度截断保留扩展名且不切断 UTF-8") {
    std::string long_name(300, 'a');
    long_name += ".jar";
    const auto a = sanitize_mod_file_name(long_name);
    REQUIRE(a.is_ok());
    REQUIRE(a.value().size() <= sscl::core::kMaxModFileNameBytes);
    REQUIRE(a.value().size() >= 4);
    REQUIRE(a.value().substr(a.value().size() - 4) == ".jar");

    // 中文：按字节截断后必须是合法 UTF-8（不能出现半个字符）。
    std::string chinese;
    for (int i = 0; i < 60; ++i) {
        chinese += "测试";
    }
    chinese += ".jar";
    const auto b = sanitize_mod_file_name(chinese);
    REQUIRE(b.is_ok());
    REQUIRE(b.value().size() <= sscl::core::kMaxModFileNameBytes);
    REQUIRE(b.value().substr(b.value().size() - 4) == ".jar");
    // 简单校验：首字节不是 UTF-8 续字节（0x80..0xBF）。
    const unsigned char first_byte = static_cast<unsigned char>(b.value()[0]);
    const bool valid_utf8_start = first_byte < 0x80 || first_byte >= 0xC0;
    REQUIRE(valid_utf8_start);
}

TEST_CASE("mod.name: 禁用/备份后缀") {
    REQUIRE(is_disabled_mod_file("x.jar.disabled"));
    REQUIRE(is_old_mod_file("x.jar.old"));
    REQUIRE_FALSE(is_disabled_mod_file("x.jar"));
    REQUIRE(make_disabled_mod_file_name("x.jar") == "x.jar.disabled");
    REQUIRE(make_old_mod_file_name("x.jar") == "x.jar.old");
    REQUIRE(strip_mod_state_suffix("x.jar.disabled") == "x.jar");
    REQUIRE(strip_mod_state_suffix("x.jar.old") == "x.jar");
    REQUIRE(strip_mod_state_suffix("x.jar") == "x.jar");
}
