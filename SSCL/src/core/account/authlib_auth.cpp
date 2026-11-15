#include "core/account/authlib_auth.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/download/mirror_map.hpp"
#include "platform/file_utils.hpp"
#include "platform/http_client.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

std::string trim_copy(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string strip_dashes(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (const char c : id) {
        if (c != '-') {
            out.push_back(c);
        }
    }
    return out;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// authlib 的错误体：{"error":"ForbiddenOperationException","errorMessage":"..."}
std::string error_text(int status, const std::string& body) {
    const json root = json::parse(body, nullptr, false);
    if (!root.is_discarded() && root.is_object()) {
        const std::string message = root.value("errorMessage", "");
        if (!message.empty()) {
            return message;
        }
    }
    switch (status) {
        case 400:
            return "请求被认证服务器拒绝（参数不合法）";
        case 401:
        case 403:
            return "用户名或密码错误，或该账号被服务器拒绝";
        case 404:
            return "认证服务器未提供该接口（请检查服务器地址）";
        default:
            break;
    }
    if (status >= 500) {
        return "认证服务器暂时不可用（HTTP " + std::to_string(status) + "）";
    }
    return "认证失败（HTTP " + std::to_string(status) + "）";
}

util::Result<std::string> post_json(const std::string& url, const std::string& body) {
    platform::HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.body = body;
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Accept", "application/json");
    request.headers.emplace_back("User-Agent", "SSCL/1.0 (authlib-injector)");
    request.connect_timeout_ms = 15000;
    request.receive_timeout_ms = 20000;
    util::Result<platform::HttpResponse> response = platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "authlib.network", "无法连接认证服务器", response.error().detail));
    }
    if (response.value().status_code >= 200 && response.value().status_code < 300) {
        return util::Result<std::string>::ok(std::move(response.value().body));
    }
    return util::Result<std::string>::fail(util::Error::make(
        "authlib.rejected", error_text(response.value().status_code, response.value().body),
        url + " status " + std::to_string(response.value().status_code)));
}

util::Result<std::string> get_text(const std::string& url, int timeout_ms) {
    platform::HttpRequest request;
    request.url = url;
    request.headers.emplace_back("Accept", "application/json");
    request.headers.emplace_back("User-Agent", "SSCL/1.0 (authlib-injector)");
    request.receive_timeout_ms = timeout_ms;
    util::Result<platform::HttpResponse> response = platform::http_request(request);
    if (response.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "authlib.network", "无法下载登录组件", response.error().detail));
    }
    if (response.value().status_code != 200) {
        return util::Result<std::string>::fail(util::Error::make(
            "authlib.network", "下载登录组件失败",
            url + " status " + std::to_string(response.value().status_code)));
    }
    return util::Result<std::string>::ok(std::move(response.value().body));
}

constexpr std::size_t kMinInjectorBytes = 64 * 1024;

}  // namespace

std::string normalize_authlib_server(const std::string& url) {
    std::string text = trim_copy(url);
    if (text.empty()) {
        return {};
    }
    if (!starts_with(text, "http://") && !starts_with(text, "https://")) {
        text = "https://" + text;
    }
    while (!text.empty() && text.back() == '/') {
        text.pop_back();
    }
    // 用户常把完整认证端点粘进来；统一收敛到 API Root。
    if (ends_with(text, "/authserver")) {
        text = text.substr(0, text.size() - std::string("/authserver").size());
    }
    if (ends_with(text, "/authserver/authenticate")) {
        text = text.substr(0, text.size() - std::string("/authserver/authenticate").size());
    }
    return text;
}

util::Result<AccountInfo> authlib_login(const std::string& server_url,
                                        const std::string& username,
                                        const std::string& password) {
    const std::string root = normalize_authlib_server(server_url);
    if (root.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("authlib.bad_server", "请填写认证服务器地址", {}));
    }
    const std::string name = trim_copy(username);
    if (name.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("authlib.bad_name", "请填写邮箱 / 账号", {}));
    }
    if (password.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("authlib.bad_password", "请填写密码", {}));
    }

    json body;
    body["username"] = name;
    body["password"] = password;
    body["clientToken"] = random_uuid_compact();
    body["agent"] = {{"name", "Minecraft"}, {"version", 1}};
    body["requestUser"] = false;

    util::Result<std::string> text = post_json(root + "/authserver/authenticate", body.dump());
    if (text.is_error()) {
        return util::Result<AccountInfo>::fail(std::move(text.error()));
    }
    const json parsed = json::parse(text.value(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return util::Result<AccountInfo>::fail(util::Error::make(
            "authlib.bad_response", "认证服务器返回了无法识别的数据", root));
    }
    const std::string access_token = parsed.value("accessToken", "");
    if (access_token.empty()) {
        return util::Result<AccountInfo>::fail(util::Error::make(
            "authlib.bad_response", "认证响应缺少 accessToken", root));
    }

    AccountInfo account;
    account.type = AccountType::kAuthlibInjector;
    account.server_url = root;
    account.access_token = access_token;
    account.client_token = parsed.value("clientToken", "");
    account.user_type = "mojang";  // authlib-injector 走 legacy 语义

    auto take_profile = [&account, &name](const json& profile) -> bool {
        if (!profile.is_object()) {
            return false;
        }
        const std::string id = profile.value("id", "");
        const std::string profile_name = profile.value("name", "");
        if (id.empty()) {
            return false;
        }
        account.uuid_compact = strip_dashes(id);
        account.username = profile_name.empty() ? name : profile_name;
        return true;
    };

    if (parsed.contains("selectedProfile") && take_profile(parsed["selectedProfile"])) {
        return util::Result<AccountInfo>::ok(std::move(account));
    }
    if (parsed.contains("availableProfiles") && parsed["availableProfiles"].is_array() &&
        !parsed["availableProfiles"].empty() &&
        take_profile(parsed["availableProfiles"][0])) {
        return util::Result<AccountInfo>::ok(std::move(account));
    }
    return util::Result<AccountInfo>::fail(util::Error::make(
        "authlib.no_profile", "该账号在服务器上没有可用角色", "请先在服务器创建角色后重试"));
}

bool authlib_validate(const std::string& server_url, const AccountInfo& account) {
    const std::string root = normalize_authlib_server(server_url);
    if (root.empty() || account.access_token.empty()) {
        return false;
    }
    json body;
    body["accessToken"] = account.access_token;
    if (!account.client_token.empty()) {
        body["clientToken"] = account.client_token;
    }
    util::Result<std::string> result = post_json(root + "/authserver/validate", body.dump());
    return result.is_ok();
}

util::Result<AccountInfo> authlib_refresh(const std::string& server_url,
                                          const AccountInfo& account) {
    const std::string root = normalize_authlib_server(server_url);
    if (root.empty() || account.access_token.empty()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("authlib.relogin", "登录已失效，请重新登录", {}));
    }
    json body;
    body["accessToken"] = account.access_token;
    if (!account.client_token.empty()) {
        body["clientToken"] = account.client_token;
    }
    util::Result<std::string> text = post_json(root + "/authserver/refresh", body.dump());
    if (text.is_error()) {
        return util::Result<AccountInfo>::fail(
            util::Error::make("authlib.relogin", "登录已过期，请重新登录", text.error().detail));
    }
    const json parsed = json::parse(text.value(), nullptr, false);
    AccountInfo updated = account;
    updated.type = AccountType::kAuthlibInjector;
    updated.server_url = root;
    if (!parsed.is_discarded() && parsed.is_object()) {
        const std::string token = parsed.value("accessToken", "");
        if (!token.empty()) {
            updated.access_token = token;
        }
        const std::string client_token = parsed.value("clientToken", "");
        if (!client_token.empty()) {
            updated.client_token = client_token;
        }
        if (parsed.contains("selectedProfile") && parsed["selectedProfile"].is_object()) {
            const json& profile = parsed["selectedProfile"];
            const std::string id = profile.value("id", "");
            const std::string name = profile.value("name", "");
            if (!id.empty()) {
                updated.uuid_compact = strip_dashes(id);
            }
            if (!name.empty()) {
                updated.username = name;
            }
        }
    }
    return util::Result<AccountInfo>::ok(std::move(updated));
}

util::Result<std::filesystem::path> ensure_authlib_injector(
    const std::filesystem::path& data_dir,
    const std::function<void(const std::string&)>& on_stage) {
    const std::filesystem::path jar = data_dir / L"authlib-injector.jar";
    std::error_code ec;
    if (std::filesystem::is_regular_file(jar, ec)) {
        const std::uintmax_t size = std::filesystem::file_size(jar, ec);
        if (!ec && size >= kMinInjectorBytes) {
            return util::Result<std::filesystem::path>::ok(jar);
        }
    }
    if (on_stage) {
        on_stage("正在获取外置登录组件信息…");
    }
    const std::string meta_url = "https://authlib-injector.yushi.moe/artifact/latest.json";
    std::string meta_text;
    std::string last_detail;
    for (const std::string& candidate : build_source_candidates(meta_url)) {
        util::Result<std::string> fetched = get_text(candidate, 15000);
        if (fetched.is_ok()) {
            meta_text = std::move(fetched.value());
            break;
        }
        last_detail = fetched.error().detail;
    }
    if (meta_text.empty()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "authlib.injector_meta", "无法获取外置登录组件信息（网络不可用）", last_detail));
    }
    const json meta = json::parse(meta_text, nullptr, false);
    if (meta.is_discarded() || !meta.is_object()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "authlib.injector_meta", "外置登录组件信息无法解析", meta_url));
    }
    const std::string download_url = meta.value("download_url", "");
    std::string expected_sha256;
    if (meta.contains("checksums") && meta["checksums"].is_object()) {
        expected_sha256 = meta["checksums"].value("sha256", "");
    }
    if (download_url.empty()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "authlib.injector_meta", "外置登录组件缺少下载地址", meta_url));
    }

    if (on_stage) {
        on_stage("正在下载外置登录组件…");
    }
    std::string bytes;
    for (const std::string& candidate : build_source_candidates(download_url)) {
        util::Result<std::string> fetched = get_text(candidate, 60000);
        if (fetched.is_error()) {
            last_detail = fetched.error().detail;
            continue;
        }
        if (fetched.value().size() < kMinInjectorBytes) {
            last_detail = candidate + " 返回内容过小";
            continue;
        }
        if (!expected_sha256.empty()) {
            const std::string actual = util::hash_hex(util::HashAlgorithm::kSha256, fetched.value());
            if (!util::hash_hex_equal(actual, expected_sha256)) {
                last_detail = candidate + " sha256 校验失败";
                continue;
            }
        }
        bytes = std::move(fetched.value());
        break;
    }
    if (bytes.empty()) {
        return util::Result<std::filesystem::path>::fail(util::Error::make(
            "authlib.injector_download", "外置登录组件下载失败", last_detail));
    }
    util::Result<void> created = ::sscl::platform::create_directories(jar.parent_path());
    if (created.is_error()) {
        return util::Result<std::filesystem::path>::fail(std::move(created.error()));
    }
    util::Result<void> written = platform::write_file_atomic(jar, bytes);
    if (written.is_error()) {
        return util::Result<std::filesystem::path>::fail(std::move(written.error()));
    }
    util::log_info("[authlib] injector ready: " + util::path_to_utf8(jar));
    return util::Result<std::filesystem::path>::ok(jar);
}

std::string authlib_javaagent_argument(const AccountInfo& account,
                                       const std::filesystem::path& injector_jar) {
    if (account.type != AccountType::kAuthlibInjector &&
        account.type != AccountType::kNide8) {
        return {};
    }
    const std::string server = normalize_authlib_server(account.server_url);
    if (server.empty() || injector_jar.empty()) {
        return {};
    }
    // 路径可能含空格：用引号包住路径段（launch_command 的 tokenize 会剥引号，
    // 命令行拼接时再整体加引号，Java 侧最终拿到一个完整参数）。
    return "-javaagent:\"" + util::wide_to_utf8(injector_jar.wstring()) + "\"=" + server;
}

}  // namespace sscl::core
