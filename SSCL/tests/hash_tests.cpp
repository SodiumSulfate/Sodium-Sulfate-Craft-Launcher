// 哈希单元测试（M0 批次 4）：NIST/RFC 已知测试向量。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "platform/path_utils.hpp"
#include "util/hash.hpp"

using sscl::util::HashAlgorithm;

TEST_CASE("hash: 内存摘要已知向量") {
    CHECK(sscl::util::hash_hex(HashAlgorithm::kSha1, "abc") ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sscl::util::hash_hex(HashAlgorithm::kSha256, "abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sscl::util::hash_hex(HashAlgorithm::kMd5, "abc") == "900150983cd24fb0d6963f7d28e17f72");

    // 空输入
    CHECK(sscl::util::hash_hex(HashAlgorithm::kSha1, "") ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sscl::util::hash_hex(HashAlgorithm::kMd5, "") == "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_CASE("hash: 十六进制比较不区分大小写") {
    CHECK(sscl::util::hash_hex_equal("ABC", "abc"));
    CHECK(!sscl::util::hash_hex_equal("abc", "abd"));
}

TEST_CASE("hash: 文件摘要与内存摘要一致") {
    const std::filesystem::path dir = sscl::platform::exe_dir() / L"unittest_tmp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / L"hash_input.bin";

    // 写入 "abc"
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write("abc", 3);
    }

    auto digest = sscl::util::hash_file_hex(HashAlgorithm::kSha1, file);
    REQUIRE(digest.is_ok());
    CHECK(digest.value() == "a9993e364706816aba3e25717850c26c9cd0d89d");

    auto missing = sscl::util::hash_file_hex(HashAlgorithm::kSha1, dir / L"no-such-hash.bin");
    CHECK(missing.is_error());
    CHECK(missing.error().code == "hash.read_failed");
}
