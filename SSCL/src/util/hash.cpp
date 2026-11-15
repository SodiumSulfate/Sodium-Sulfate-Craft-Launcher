#include "util/hash.hpp"

#include "util/string_utils.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>

namespace sscl::util {

namespace {

const wchar_t* algorithm_name(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::kSha1:
            return BCRYPT_SHA1_ALGORITHM;
        case HashAlgorithm::kSha256:
            return BCRYPT_SHA256_ALGORITHM;
        case HashAlgorithm::kMd5:
            return BCRYPT_MD5_ALGORITHM;
    }
    return BCRYPT_SHA1_ALGORITHM;
}

std::size_t digest_length(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::kSha1:
            return 20;
        case HashAlgorithm::kSha256:
            return 32;
        case HashAlgorithm::kMd5:
            return 16;
    }
    return 20;
}

std::string to_hex(const unsigned char* data, std::size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out(length * 2, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0F];
    }
    return out;
}

// BCrypt 哈希句柄 RAII。NTSTATUS（=LONG）< 0 表示失败。
class Hasher {
public:
    explicit Hasher(HashAlgorithm algorithm) {
        if (::BCryptOpenAlgorithmProvider(&m_alg, algorithm_name(algorithm), nullptr, 0) < 0) {
            m_alg = nullptr;
            return;
        }
        if (::BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0) < 0) {
            m_hash = nullptr;
            ::BCryptCloseAlgorithmProvider(m_alg, 0);
            m_alg = nullptr;
            return;
        }
        m_valid = true;
    }

    ~Hasher() {
        if (m_hash != nullptr) {
            ::BCryptDestroyHash(m_hash);
        }
        if (m_alg != nullptr) {
            ::BCryptCloseAlgorithmProvider(m_alg, 0);
        }
    }

    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    bool valid() const { return m_valid; }

    bool update(const void* data, std::size_t size) {
        const unsigned char* cursor = static_cast<const unsigned char*>(data);
        while (size > 0) {
            const ULONG chunk = static_cast<ULONG>(
                (std::min)(static_cast<std::size_t>(64 * 1024 * 1024), size));
            if (::BCryptHashData(m_hash, const_cast<PUCHAR>(cursor), chunk, 0) < 0) {
                return false;
            }
            cursor += chunk;
            size -= chunk;
        }
        return true;
    }

    bool finish(unsigned char* digest, std::size_t length) {
        return ::BCryptFinishHash(m_hash, digest, static_cast<ULONG>(length), 0) >= 0;
    }

private:
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    bool m_valid = false;
};

}  // namespace

std::string hash_hex(HashAlgorithm algorithm, std::string_view data) {
    Hasher hasher(algorithm);
    if (!hasher.valid()) {
        return {};
    }
    if (!hasher.update(data.data(), data.size())) {
        return {};
    }
    std::vector<unsigned char> digest(digest_length(algorithm));
    if (!hasher.finish(digest.data(), digest.size())) {
        return {};
    }
    return to_hex(digest.data(), digest.size());
}

Result<std::string> hash_file_hex(HashAlgorithm algorithm, const std::filesystem::path& path) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<std::string>::fail(
            Error::make("hash.read_failed", "无法读取文件以计算哈希", std::to_string(::GetLastError())));
    }

    Hasher hasher(algorithm);
    if (!hasher.valid()) {
        ::CloseHandle(handle);
        return Result<std::string>::fail(
            Error::make("hash.init_failed", "哈希初始化失败", {}));
    }

    std::vector<char> buffer(1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            const unsigned long err = ::GetLastError();
            ::CloseHandle(handle);
            return Result<std::string>::fail(Error::make(
                "hash.read_failed", "读取文件失败", std::to_string(err)));
        }
        if (read == 0) {
            break;
        }
        if (!hasher.update(buffer.data(), read)) {
            ::CloseHandle(handle);
            return Result<std::string>::fail(Error::make("hash.update_failed", "哈希计算失败", {}));
        }
    }
    ::CloseHandle(handle);

    std::vector<unsigned char> digest(digest_length(algorithm));
    if (!hasher.finish(digest.data(), digest.size())) {
        return Result<std::string>::fail(Error::make("hash.update_failed", "哈希计算失败", {}));
    }
    return Result<std::string>::ok(to_hex(digest.data(), digest.size()));
}

bool hash_hex_equal(std::string_view a, std::string_view b) {
    return to_lower_ascii(a) == to_lower_ascii(b);
}

std::uint32_t murmurhash2_filtered(std::string_view data) {
    constexpr std::uint32_t m = 0x5BD1E995u;
    std::string filtered;
    filtered.reserve(data.size());
    for (const unsigned char c : data) {
        if (c == 0x09 || c == 0x0A || c == 0x0D || c == 0x20) {
            continue;
        }
        filtered.push_back(static_cast<char>(c));
    }
    const std::size_t length = filtered.size();
    std::uint32_t h = 1u ^ static_cast<std::uint32_t>(length);
    std::size_t i = 0;
    while (i + 4 <= length) {
        std::uint32_t k = static_cast<unsigned char>(filtered[i]) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 1])) << 8) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 2])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 3])) << 24);
        k *= m;
        k ^= k >> 24;
        k *= m;
        h *= m;
        h ^= k;
        i += 4;
    }
    switch (length - i) {
        case 3:
            h ^= (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i])) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 1])) << 8) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 2])) << 16));
            h *= m;
            break;
        case 2:
            h ^= (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i])) |
                  (static_cast<std::uint32_t>(static_cast<unsigned char>(filtered[i + 1])) << 8));
            h *= m;
            break;
        case 1:
            h ^= static_cast<unsigned char>(filtered[i]);
            h *= m;
            break;
        default:
            break;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

Result<std::uint32_t> curseforge_fingerprint(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Result<std::uint32_t>::fail(Error::make(
            "hash.read_failed", "无法读取文件以计算 CurseForge 指纹", path_to_utf8(path)));
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return Result<std::uint32_t>::fail(Error::make(
            "hash.read_failed", "读取文件失败", path_to_utf8(path)));
    }
    return Result<std::uint32_t>::ok(murmurhash2_filtered(bytes));
}

}  // namespace sscl::util
