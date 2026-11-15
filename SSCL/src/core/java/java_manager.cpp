#include "core/java/java_manager.hpp"

#include "platform/file_utils.hpp"
#include "platform/path_utils.hpp"
#include "platform/process.hpp"
#include "util/string_utils.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#pragma comment(lib, "advapi32.lib")

#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace sscl::core {

namespace {

constexpr int kMaxDirectoryScanDepth = 5;

bool is_ascii_alpha_numeric(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool parse_decimal_component(std::string_view text, size_t* position, int* value) {
    const size_t begin = *position;
    int parsed = 0;
    while (*position < text.size() && text[*position] >= '0' && text[*position] <= '9') {
        const int digit = text[*position] - '0';
        if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
        ++*position;
    }
    if (*position == begin) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_version_token(std::string_view token, int* major_version) {
    size_t position = 0;
    int first = 0;
    if (!parse_decimal_component(token, &position, &first)) {
        return false;
    }

    int major = first;
    if (first == 1 && position < token.size() && token[position] == '.') {
        ++position;
        if (!parse_decimal_component(token, &position, &major)) {
            return false;
        }
    }

    if (position < token.size() && std::string_view("._-+").find(token[position]) ==
                                       std::string_view::npos) {
        return false;
    }
    *major_version = major;
    return major > 0;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : normalized;
}

bool paths_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring left_text = left.wstring();
    const std::wstring right_text = right.wstring();
    return ::CompareStringOrdinal(left_text.c_str(), -1, right_text.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

bool is_java_executable_name(const std::filesystem::path& path) {
    const std::wstring name = path.filename().wstring();
    return ::CompareStringOrdinal(name.c_str(), -1, L"java.exe", -1, TRUE) == CSTR_EQUAL ||
           ::CompareStringOrdinal(name.c_str(), -1, L"javaw.exe", -1, TRUE) == CSTR_EQUAL;
}

void append_candidate(std::vector<JavaCandidate>* candidates,
                      const std::filesystem::path& executable, JavaSource source) {
    if (!platform::file_exists(executable) ||
        ::CompareStringOrdinal(executable.filename().c_str(), -1, L"java.exe", -1, TRUE) !=
            CSTR_EQUAL) {
        return;
    }

    const std::filesystem::path normalized = normalized_path(executable);
    for (const JavaCandidate& existing : *candidates) {
        if (paths_equal(existing.executable, normalized)) {
            return;
        }
    }
    candidates->push_back({normalized, source});
}

void append_java_home(std::vector<JavaCandidate>* candidates,
                      const std::filesystem::path& java_home, JavaSource source) {
    if (!java_home.empty()) {
        append_candidate(candidates, java_home / L"bin" / L"java.exe", source);
    }
}

std::wstring read_environment(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring trim_path_entry(std::wstring value) {
    const size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) {
        return {};
    }
    const size_t end = value.find_last_not_of(L" \t\r\n");
    value = value.substr(begin, end - begin + 1);
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

void scan_java_tree(const std::filesystem::path& root, JavaSource source,
                    std::vector<JavaCandidate>* candidates) {
    if (!platform::directory_exists(root)) {
        return;
    }

    std::error_code ec;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && iterator != end) {
        std::error_code type_error;
        if (iterator.depth() >= kMaxDirectoryScanDepth &&
            iterator->is_directory(type_error)) {
            iterator.disable_recursion_pending();
        }
        if (paths_equal(iterator->path().filename(), std::filesystem::path(L"java.exe"))) {
            append_candidate(candidates, iterator->path(), source);
        }
        iterator.increment(ec);
    }
}

void scan_root_once(const std::filesystem::path& root, JavaSource source,
                    std::vector<std::filesystem::path>* scanned_roots,
                    std::vector<JavaCandidate>* candidates) {
    if (root.empty()) {
        return;
    }
    const std::filesystem::path normalized = normalized_path(root);
    for (const std::filesystem::path& scanned : *scanned_roots) {
        if (paths_equal(scanned, normalized)) {
            return;
        }
    }
    scanned_roots->push_back(normalized);
    scan_java_tree(normalized, source, candidates);
}

std::wstring read_registry_string(HKEY key, const wchar_t* value_name) {
    DWORD type = 0;
    DWORD byte_count = 0;
    if (::RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &byte_count) !=
            ERROR_SUCCESS ||
        type != REG_SZ || byte_count < sizeof(wchar_t)) {
        return {};
    }

    std::vector<wchar_t> buffer(byte_count / sizeof(wchar_t) + 1, L'\0');
    DWORD available = byte_count;
    if (::RegQueryValueExW(key, value_name, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer.data()), &available) != ERROR_SUCCESS ||
        type != REG_SZ) {
        return {};
    }
    buffer.back() = L'\0';
    return std::wstring(buffer.data());
}

// 读取一个"厂商版本键"可能存在的若干布局（Oracle/Adoptium/Microsoft/BellSoft 各不相同）：
//   <ver>\JavaHome | <ver>\Path | <ver>\Home | <ver>\InstallationPath
//   <ver>\hotspot\MSI\Path（Adoptium/Microsoft MSI）
//   <ver>\MSI\Path
void append_registry_vendor_key(const std::wstring& version_path,
                                std::vector<JavaCandidate>* candidates) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, version_path.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    for (const wchar_t* value_name :
         {L"JavaHome", L"Path", L"Home", L"InstallationPath"}) {
        const std::wstring value = read_registry_string(key, value_name);
        if (!value.empty()) {
            append_java_home(candidates, std::filesystem::path(value), JavaSource::kRegistry);
        }
    }
    ::RegCloseKey(key);

    const std::wstring candidates_paths[] = {
        version_path + L"\\hotspot\\MSI",
        version_path + L"\\MSI",
    };
    for (const std::wstring& msi_path : candidates_paths) {
        HKEY msi_key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, msi_path.c_str(), 0, KEY_READ, &msi_key) !=
            ERROR_SUCCESS) {
            continue;
        }
        const std::wstring value = read_registry_string(msi_key, L"Path");
        if (!value.empty()) {
            append_java_home(candidates, std::filesystem::path(value), JavaSource::kRegistry);
        }
        ::RegCloseKey(msi_key);
    }
}

void append_registry_root(const std::wstring& root_path,
                          std::vector<JavaCandidate>* candidates) {
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path.c_str(), 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return;
    }

    DWORD sub_key_count = 0;
    DWORD max_sub_key_length = 0;
    const LSTATUS info_result =
        ::RegQueryInfoKeyW(root, nullptr, nullptr, nullptr, &sub_key_count,
                           &max_sub_key_length, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr);
    if (info_result != ERROR_SUCCESS) {
        ::RegCloseKey(root);
        return;
    }

    std::vector<wchar_t> name(static_cast<size_t>(max_sub_key_length) + 1, L'\0');
    for (DWORD index = 0; index < sub_key_count; ++index) {
        DWORD name_length = static_cast<DWORD>(name.size());
        const LSTATUS enum_result =
            ::RegEnumKeyExW(root, index, name.data(), &name_length, nullptr, nullptr, nullptr,
                            nullptr);
        if (enum_result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enum_result != ERROR_SUCCESS) {
            continue;
        }
        append_registry_vendor_key(
            root_path + L"\\" + std::wstring(name.data(), name_length), candidates);
    }
    ::RegCloseKey(root);
}

std::string path_utf8(const std::filesystem::path& path) {
    return util::wide_to_utf8(path.wstring());
}

}  // namespace

util::Result<JavaVersion> parse_java_version(std::string_view version_output) {
    const std::string lower_output = util::to_lower_ascii(version_output);
    size_t search_position = 0;
    while (search_position < lower_output.size()) {
        const size_t keyword = lower_output.find("version", search_position);
        if (keyword == std::string::npos) {
            break;
        }
        search_position = keyword + 7;

        const bool valid_left = keyword == 0 || !is_ascii_alpha_numeric(lower_output[keyword - 1]);
        const bool valid_right = search_position >= lower_output.size() ||
                                 !is_ascii_alpha_numeric(lower_output[search_position]);
        if (!valid_left || !valid_right) {
            continue;
        }

        size_t quote = search_position;
        while (quote < version_output.size() &&
               (version_output[quote] == ' ' || version_output[quote] == '\t')) {
            ++quote;
        }
        if (quote >= version_output.size() || version_output[quote] != '"') {
            continue;
        }
        const size_t end_quote = version_output.find('"', quote + 1);
        if (end_quote == std::string_view::npos) {
            continue;
        }

        const std::string_view token = version_output.substr(quote + 1, end_quote - quote - 1);
        int major = 0;
        if (parse_version_token(token, &major)) {
            return util::Result<JavaVersion>::ok(JavaVersion{major, std::string(token)});
        }
        search_position = end_quote + 1;
    }

    return util::Result<JavaVersion>::fail(util::Error::make(
        "java.version_parse_failed", "无法识别 Java 版本输出", std::string(version_output)));
}

util::Result<JavaSelection> select_java_runtime(const std::vector<JavaRuntime>& runtimes,
                                                const JavaVersionInfo& required) {
    if (required.major_version <= 0) {
        return util::Result<JavaSelection>::fail(util::Error::make(
            "java.invalid_requirement", "Minecraft 要求的 Java 主版本无效",
            std::to_string(required.major_version)));
    }

    const size_t not_found = runtimes.size();
    size_t best = not_found;
    for (size_t index = 0; index < runtimes.size(); ++index) {
        if (runtimes[index].version.major_version == required.major_version) {
            best = index;
            break;
        }
    }
    if (best != not_found) {
        return util::Result<JavaSelection>::ok(
            JavaSelection{runtimes[best], JavaMatchKind::kExact});
    }

    for (size_t index = 0; index < runtimes.size(); ++index) {
        const int major = runtimes[index].version.major_version;
        if (major > required.major_version &&
            (best == not_found || major < runtimes[best].version.major_version)) {
            best = index;
        }
    }
    if (best != not_found) {
        return util::Result<JavaSelection>::ok(
            JavaSelection{runtimes[best], JavaMatchKind::kCompatibleHigher});
    }

    return util::Result<JavaSelection>::fail(util::Error::make(
        "java.no_compatible_runtime",
        "未找到满足要求的 Java " + std::to_string(required.major_version) + " 或更高版本",
        "required majorVersion=" + std::to_string(required.major_version)));
}

std::vector<JavaCandidate> enumerate_java_candidates(
    const std::vector<std::filesystem::path>& extra_roots) {
    std::vector<JavaCandidate> candidates;
    std::vector<std::filesystem::path> scanned_roots;

    scan_root_once(platform::data_dir() / L"runtime", JavaSource::kPortableRuntime,
                   &scanned_roots, &candidates);

    append_java_home(&candidates, std::filesystem::path(read_environment(L"JAVA_HOME")),
                     JavaSource::kJavaHome);

    const std::wstring path_value = read_environment(L"PATH");
    size_t begin = 0;
    while (begin <= path_value.size()) {
        const size_t semicolon = path_value.find(L';', begin);
        const size_t end = semicolon == std::wstring::npos ? path_value.size() : semicolon;
        const std::wstring entry = trim_path_entry(path_value.substr(begin, end - begin));
        if (!entry.empty()) {
            append_candidate(&candidates, std::filesystem::path(entry) / L"java.exe",
                             JavaSource::kPath);
        }
        if (semicolon == std::wstring::npos) {
            break;
        }
        begin = semicolon + 1;
    }

    const wchar_t* const kProgramDirectoryVariables[] = {L"ProgramFiles", L"ProgramW6432",
                                                         L"ProgramFiles(x86)"};
    const wchar_t* const kJavaDirectoryNames[] = {L"Java",          L"Eclipse Adoptium",
                                                  L"Microsoft",     L"Amazon Corretto",
                                                  L"Zulu",          L"BellSoft",
                                                  L"Semeru"};
    for (const wchar_t* variable : kProgramDirectoryVariables) {
        const std::wstring program_directory = read_environment(variable);
        if (program_directory.empty()) {
            continue;
        }
        for (const wchar_t* java_directory : kJavaDirectoryNames) {
            scan_root_once(std::filesystem::path(program_directory) / java_directory,
                           JavaSource::kCommonDirectory, &scanned_roots, &candidates);
        }
    }

    // 各固定磁盘根下的常见 JDK 目录（如 D:\JDK\jdk-17 —— 非 Program Files 安装的便携目录）。
    // 只扫两个固定层级（<盘>:\JDK、<盘>:\Java），不做全盘遍历。
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        const wchar_t drive_root[4] = {letter, L':', L'\\', L'\0'};
        if (::GetDriveTypeW(drive_root) != DRIVE_FIXED) {
            continue;
        }
        for (const wchar_t* folder : {L"JDK", L"Java", L"jdk", L"java"}) {
            scan_root_once(std::filesystem::path(drive_root) / folder,
                           JavaSource::kCommonDirectory, &scanned_roots, &candidates);
        }
    }

    const wchar_t* const kRegistryPrefixes[] = {L"SOFTWARE\\", L"SOFTWARE\\WOW6432Node\\"};
    const wchar_t* const kRegistryJavaKeys[] = {
        L"JavaSoft\\JDK",
        L"JavaSoft\\Java Development Kit",
        L"JavaSoft\\JRE",
        L"JavaSoft\\Java Runtime Environment",
        // 非 Oracle 厂商（MSI 布局：<ver>\hotspot\MSI\Path）
        L"Eclipse Adoptium\\JDK",
        L"Eclipse Foundation\\JDK",
        L"Microsoft\\JDK",
        L"Azul Systems\\Zulu",
        L"Amazon Corretto",
        L"BellSoft\\LibericaJDK",
        L"Semeru\\JDK",
    };
    for (const wchar_t* prefix : kRegistryPrefixes) {
        for (const wchar_t* java_key : kRegistryJavaKeys) {
            append_registry_root(std::wstring(prefix) + java_key, &candidates);
        }
    }

    // 用户配置的附加目录（config.json 的 java_scan_dirs，例如 "D:\\JDK"）
    for (const std::filesystem::path& root : extra_roots) {
        scan_root_once(root, JavaSource::kManual, &scanned_roots, &candidates);
    }

    return candidates;
}

std::vector<JavaCandidate> enumerate_java_candidates() {
    return enumerate_java_candidates({});
}

util::Result<JavaRuntime> inspect_java_runtime(const JavaCandidate& candidate, int timeout_ms) {
    if (!platform::file_exists(candidate.executable) ||
        !is_java_executable_name(candidate.executable)) {
        return util::Result<JavaRuntime>::fail(util::Error::make(
            "java.invalid_path", "Java 可执行文件不存在或文件名无效",
            path_utf8(candidate.executable)));
    }

    auto process_result =
        platform::run_process(candidate.executable, {"-version"}, {}, timeout_ms);
    if (process_result.is_error()) {
        return util::Result<JavaRuntime>::fail(util::Error::make(
            "java.version_probe_failed", "无法获取 Java 版本：" + path_utf8(candidate.executable),
            process_result.error().code + ": " + process_result.error().detail));
    }
    if (process_result.value().exit_code != 0) {
        return util::Result<JavaRuntime>::fail(util::Error::make(
            "java.version_probe_failed", "Java 版本查询失败：" + path_utf8(candidate.executable),
            "exit_code=" + std::to_string(process_result.value().exit_code) + "\n" +
                process_result.value().stderr_text + process_result.value().stdout_text));
    }

    const std::string output =
        process_result.value().stderr_text + "\n" + process_result.value().stdout_text;
    auto version = parse_java_version(output);
    if (version.is_error()) {
        return util::Result<JavaRuntime>::fail(util::Error::make(
            version.error().code, "无法识别 Java 版本：" + path_utf8(candidate.executable),
            version.error().detail));
    }
    return util::Result<JavaRuntime>::ok(
        JavaRuntime{normalized_path(candidate.executable), candidate.source,
                    std::move(version.value())});
}

util::Result<JavaRuntime> validate_java_path(const std::filesystem::path& path, int timeout_ms) {
    if (platform::directory_exists(path)) {
        const std::filesystem::path bin_executable = path / L"bin" / L"java.exe";
        if (platform::file_exists(bin_executable)) {
            return inspect_java_runtime({bin_executable, JavaSource::kManual}, timeout_ms);
        }
        const std::filesystem::path direct_executable = path / L"java.exe";
        if (platform::file_exists(direct_executable)) {
            return inspect_java_runtime({direct_executable, JavaSource::kManual}, timeout_ms);
        }
    } else if (platform::file_exists(path) && is_java_executable_name(path)) {
        return inspect_java_runtime({path, JavaSource::kManual}, timeout_ms);
    }

    return util::Result<JavaRuntime>::fail(util::Error::make(
        "java.invalid_path", "请选择有效的 java.exe、javaw.exe、bin 目录或 Java 根目录",
        path_utf8(path)));
}

std::vector<JavaRuntime> detect_java_runtimes(const std::vector<std::filesystem::path>& extra_roots,
                                              int timeout_ms) {
    std::vector<JavaRuntime> runtimes;
    for (const JavaCandidate& candidate : enumerate_java_candidates(extra_roots)) {
        auto inspected = inspect_java_runtime(candidate, timeout_ms);
        if (inspected.is_ok()) {
            runtimes.push_back(std::move(inspected.value()));
        }
    }
    return runtimes;
}

std::vector<std::filesystem::path> scan_java_dirs(
    const std::vector<std::filesystem::path>& roots) {
    std::vector<JavaCandidate> candidates;
    std::vector<std::filesystem::path> scanned_roots;
    for (const std::filesystem::path& root : roots) {
        scan_root_once(root, JavaSource::kManual, &scanned_roots, &candidates);
    }

    std::vector<std::filesystem::path> directories;
    for (const JavaCandidate& candidate : candidates) {
        const std::filesystem::path directory = candidate.executable.parent_path();
        bool duplicate = false;
        for (const std::filesystem::path& existing : directories) {
            if (paths_equal(existing, directory)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            directories.push_back(directory);
        }
    }
    return directories;
}

std::vector<std::filesystem::path> default_java_scan_dirs() {
    std::vector<std::filesystem::path> result;
    auto add = [&result](const std::filesystem::path& path) {
        if (path.empty() || !platform::directory_exists(path)) {
            return;
        }
        const std::filesystem::path normalized = normalized_path(path);
        for (const std::filesystem::path& existing : result) {
            if (paths_equal(existing, normalized)) {
                return;
            }
        }
        result.push_back(normalized);
    };

    // 常见安装目录（与用户提供的排查脚本保持一致）。
    const wchar_t* const kProgramVariables[] = {L"ProgramFiles", L"ProgramW6432",
                                                L"ProgramFiles(x86)"};
    const wchar_t* const kDirectoryNames[] = {
        L"Java", L"Eclipse Adoptium", L"Microsoft", L"Amazon Corretto", L"BellSoft",
        L"Zulu", L"OpenJDK", L"Temurin", L"Semeru"};
    for (const wchar_t* variable : kProgramVariables) {
        const std::wstring base = read_environment(variable);
        if (base.empty()) {
            continue;
        }
        for (const wchar_t* name : kDirectoryNames) {
            add(std::filesystem::path(base) / name);
        }
    }
    const std::wstring local_app_data = read_environment(L"LOCALAPPDATA");
    if (!local_app_data.empty()) {
        add(std::filesystem::path(local_app_data) / L"Programs" / L"Eclipse Adoptium");
        add(std::filesystem::path(local_app_data) / L"Programs" / L"Microsoft");
    }

    // 便携 JDK 常见位置（C:/Java、D:/JDK 等）；只探测固定盘的这两个名字，不遍历。
    for (wchar_t letter = L'C'; letter <= L'E'; ++letter) {
        const wchar_t drive_root[4] = {letter, L':', L'\\', L'\0'};
        if (::GetDriveTypeW(drive_root) != DRIVE_FIXED) {
            continue;
        }
        add(std::filesystem::path(drive_root) / L"Java");
        add(std::filesystem::path(drive_root) / L"JDK");
    }

    // JAVA_HOME 的父目录（脚本里的 Split-Path $env:JAVA_HOME -Parent）。
    const std::filesystem::path java_home(read_environment(L"JAVA_HOME"));
    if (!java_home.empty()) {
        add(java_home.parent_path());
    }

    // PATH 中首个 java.exe 的 JDK 根（脚本里的 Split-Path (Split-Path ...) -Parent）。
    const std::wstring path_value = read_environment(L"PATH");
    size_t begin = 0;
    while (begin <= path_value.size()) {
        const size_t semicolon = path_value.find(L';', begin);
        const size_t end = semicolon == std::wstring::npos ? path_value.size() : semicolon;
        const std::wstring entry = trim_path_entry(path_value.substr(begin, end - begin));
        if (!entry.empty()) {
            const std::filesystem::path bin(entry);
            if (::CompareStringOrdinal(bin.filename().c_str(), -1, L"bin", -1, TRUE) ==
                CSTR_EQUAL) {
                add(bin.parent_path());
            }
        }
        if (semicolon == std::wstring::npos) {
            break;
        }
        begin = semicolon + 1;
    }
    return result;
}

std::vector<JavaRuntime> detect_java_in_dirs(
    const std::vector<std::filesystem::path>& roots, int timeout_ms) {
    std::vector<JavaRuntime> runtimes;
    for (const std::filesystem::path& directory : scan_java_dirs(roots)) {
        const std::filesystem::path executable = directory / L"java.exe";
        if (!platform::file_exists(executable)) {
            continue;
        }
        JavaCandidate candidate;
        candidate.executable = executable;
        candidate.source = JavaSource::kManual;
        util::Result<JavaRuntime> runtime = inspect_java_runtime(candidate, timeout_ms);
        if (runtime.is_ok()) {
            runtimes.push_back(std::move(runtime.value()));
        }
    }
    return runtimes;
}

std::vector<JavaRuntime> detect_java_runtimes(int timeout_ms) {
    return detect_java_runtimes({}, timeout_ms);
}

util::Result<JavaSelection> auto_select_java_runtime(
    const JavaVersionInfo& required, const std::vector<std::filesystem::path>& extra_roots,
    int timeout_ms) {
    if (required.major_version <= 0) {
        return util::Result<JavaSelection>::fail(util::Error::make(
            "java.invalid_requirement", "Minecraft 要求的 Java 主版本无效",
            std::to_string(required.major_version)));
    }
    return select_java_runtime(detect_java_runtimes(extra_roots, timeout_ms), required);
}

util::Result<JavaSelection> auto_select_java_runtime(const JavaVersionInfo& required,
                                                     int timeout_ms) {
    return auto_select_java_runtime(required, {}, timeout_ms);
}

}  // namespace sscl::core
