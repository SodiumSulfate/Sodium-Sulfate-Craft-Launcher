#include <cstdint>
#include <filesystem>
#include <string>

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include "core/account/account_skin.hpp"

namespace {

using nlohmann::json;

// 测试用编码器（与解析器里的解码器互为逆运算；也覆盖带填充的情况）。
std::string base64_encode(const std::string& input) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(
                                     static_cast<unsigned char>(input[i])) << 16) |
                                (i + 1 < input.size()
                                     ? static_cast<std::uint32_t>(
                                           static_cast<unsigned char>(input[i + 1])) << 8
                                     : 0u) |
                                (i + 2 < input.size()
                                     ? static_cast<std::uint32_t>(
                                           static_cast<unsigned char>(input[i + 2]))
                                     : 0u);
        out.push_back(kAlphabet[(v >> 18) & 63u]);
        out.push_back(kAlphabet[(v >> 12) & 63u]);
        out.push_back(i + 1 < input.size() ? kAlphabet[(v >> 6) & 63u] : '=');
        out.push_back(i + 2 < input.size() ? kAlphabet[v & 63u] : '=');
    }
    return out;
}

std::string profile_response(const std::string& textures_json) {
    json property;
    property["name"] = "textures";
    property["value"] = base64_encode(textures_json);
    json root;
    root["id"] = "0123456789abcdef0123456789abcdef";
    root["name"] = "Player";
    root["properties"] = json::array({property});
    return root.dump();
}

std::string skin_url(const std::string& body) {
    auto result = sscl::core::parse_authlib_skin_url(body);
    REQUIRE(result.is_ok());
    return result.value();
}

}  // namespace

TEST_CASE("account.skin: 微软 profile_json 取皮肤 URL") {
    CHECK(sscl::core::skin_url_from_ms_profile(
              R"({"skins":[{"url":"https://textures.minecraft.net/texture/abcd"}]})") ==
          "https://textures.minecraft.net/texture/abcd");
    CHECK(sscl::core::skin_url_from_ms_profile(R"({"skins":[]})").empty());
    CHECK(sscl::core::skin_url_from_ms_profile("not json").empty());
    // 非 HTTP(S) 必须拒绝（Axolotl 同款安全约束）。
    CHECK(sscl::core::skin_url_from_ms_profile(
              R"({"skins":[{"url":"file:///C:/secret.png"}]})")
              .empty());
}

TEST_CASE("account.skin: Yggdrasil textures 解析（D-121 / PCL McSkinGetAddress）") {
    // 标准大写 SKIN。
    CHECK(skin_url(profile_response(
              R"({"textures":{"SKIN":{"url":"https://littleskin.cn/textures/skin.png"}}})")) ==
          "https://littleskin.cn/textures/skin.png");
    // 部分皮肤站返回小写 / 首字母大写，同样要兼容。
    CHECK(skin_url(profile_response(
              R"({"textures":{"skin":{"url":"https://skin.example/a.png"}}})")) ==
          "https://skin.example/a.png");
    CHECK(skin_url(profile_response(
              R"({"textures":{"Skin":{"url":"https://skin.example/b.png"}}})")) ==
          "https://skin.example/b.png");
    // PCL：minecraft.net 的 http 链接升为 https。
    CHECK(skin_url(profile_response(
              R"({"textures":{"SKIN":{"url":"http://textures.minecraft.net/a.png"}}})")) ==
          "https://textures.minecraft.net/a.png");
    // 无填充 Base64 与内嵌空白都要能解析（不同皮肤站实现不一）。
    json property;
    property["name"] = "textures";
    std::string encoded = base64_encode(
        R"({"textures":{"SKIN":{"url":"https://skin.example/no-pad.png"}}})");
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    property["value"] = encoded + "\n";
    json root;
    root["properties"] = json::array({property});
    CHECK(skin_url(root.dump()) == "https://skin.example/no-pad.png");
}

TEST_CASE("account.skin: 缺皮肤 / 非法数据一律拒绝（不回退占位以外的加载）") {
    auto empty_body = sscl::core::parse_authlib_skin_url("");
    CHECK(empty_body.is_error());

    auto no_properties = sscl::core::parse_authlib_skin_url(R"({"id":"a","name":"b"})");
    CHECK(no_properties.is_error());

    auto no_skin = sscl::core::parse_authlib_skin_url(profile_response(R"({"textures":{}})"));
    CHECK(no_skin.is_error());

    auto bad_base64 = sscl::core::parse_authlib_skin_url(
        R"({"properties":[{"name":"textures","value":"not-base64!!!"}]})");
    CHECK(bad_base64.is_error());

    auto bad_json = sscl::core::parse_authlib_skin_url("{{{");
    CHECK(bad_json.is_error());

    // 恶意认证服务器不得让我们去读本地文件。
    auto file_scheme = sscl::core::parse_authlib_skin_url(profile_response(
        R"({"textures":{"SKIN":{"url":"file:///C:/Windows/win.ini"}}})"));
    CHECK(file_scheme.is_error());
}

TEST_CASE("account.skin: 缓存目录与离线账号") {
    const std::filesystem::path dir = sscl::core::account_skin_cache_dir();
    CHECK_FALSE(dir.empty());
    CHECK(dir.filename() == "skins");

    sscl::core::AccountInfo offline;
    offline.type = sscl::core::AccountType::kOffline;
    offline.username = "Steve";
    // 离线账号不发起网络请求，直接返回不可用。
    CHECK(sscl::core::fetch_account_skin_png(offline).is_error());
}
