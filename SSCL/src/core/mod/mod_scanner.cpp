#include "core/mod/mod_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include <nlohmann/json.hpp>

#include "platform/file_utils.hpp"
#include "util/hash.hpp"
#include "util/log.hpp"
#include "util/string_utils.hpp"

namespace sscl::core {

namespace {

using nlohmann::json;

bool ends_with(const std::string& text, const char* suffix) {
    const std::string tail(suffix);
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

std::string lower_ascii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string strip_suffix(const std::string& name, const std::string& lower,
                         const char* suffix) {
    const std::string tail(suffix);
    if (ends_with(lower, suffix)) {
        return name.substr(0, name.size() - tail.size());
    }
    return name;
}

}  // namespace

std::string mod_enabled_name(const std::string& file_name) {
    const std::string lower = lower_ascii(file_name);
    std::string result = strip_suffix(file_name, lower, ".disabled");
    if (result == file_name) {
        // 同一个文件不会同时以 .disabled 和 .old 结尾；PCL 的 Replace 全局替换仅影响
        // 文件名中间误含后缀的情况，这里按“去掉状态后缀”实现。
        result = strip_suffix(file_name, lower, ".old");
    }
    return result;
}

bool mod_file_is_enabled(const std::string& file_name) {
    const std::string lower = lower_ascii(file_name);
    return ends_with(lower, ".jar") || ends_with(lower, ".zip") || ends_with(lower, ".litemod");
}

bool mod_file_is_old(const std::string& file_name) {
    return ends_with(lower_ascii(file_name), ".old");
}

std::vector<LocalModEntry> list_local_mods(const std::filesystem::path& mods_dir,
                                           bool include_old) {
    std::vector<LocalModEntry> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(mods_dir, ec)) {
        return result;
    }
    for (std::filesystem::directory_iterator it(mods_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = util::path_to_utf8(it->path().filename());
        const std::string lower = lower_ascii(name);
        const bool old = ends_with(lower, ".old");
        if (old && !include_old) {
            continue;
        }
        const std::string enabled_name = mod_enabled_name(name);
        if (!mod_file_is_enabled(enabled_name)) {
            continue;
        }
        LocalModEntry entry;
        entry.file_name = name;
        entry.full_path = it->path();
        entry.enabled_name = enabled_name;
        entry.size = static_cast<std::uint64_t>(it->file_size(ec));
        entry.disabled = !old && ends_with(lower, ".disabled");
        entry.old = old;
        entry.enabled = mod_file_is_enabled(name);
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const LocalModEntry& a, const LocalModEntry& b) {
        return lower_ascii(a.file_name) < lower_ascii(b.file_name);
    });
    return result;
}

std::vector<LocalModEntry> scan_pcl_mods(const std::filesystem::path& mods_dir) {
    std::vector<LocalModEntry> all = list_local_mods(mods_dir, /*include_old=*/true);
    std::vector<LocalModEntry> result;
    result.reserve(all.size());
    for (LocalModEntry& entry : all) {
        const std::string key = lower_ascii(entry.enabled_name);
        auto found = std::find_if(result.begin(), result.end(), [&key](const LocalModEntry& item) {
            return lower_ascii(item.enabled_name) == key;
        });
        if (found == result.end()) {
            result.push_back(std::move(entry));
            continue;
        }
        // PCL：同名启用/禁用并存时保留启用版本；都禁用时保留先遇到的。
        if (!found->enabled && entry.enabled) {
            *found = std::move(entry);
        }
    }
    std::sort(result.begin(), result.end(), [](const LocalModEntry& a, const LocalModEntry& b) {
        return lower_ascii(a.file_name) < lower_ascii(b.file_name);
    });
    return result;
}

util::Result<void> set_mod_enabled(const std::filesystem::path& file, bool enable) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return util::Result<void>::fail(util::Error::make(
            "mod.file_missing", "Mod 文件不存在", util::path_to_utf8(file)));
    }
    const std::string file_name = util::path_to_utf8(file.filename());
    const bool currently_enabled = mod_file_is_enabled(file_name);
    if (enable == currently_enabled) {
        return util::Result<void>::ok();
    }
    const std::filesystem::path target =
        enable ? file.parent_path() / util::path_from_utf8(mod_enabled_name(file_name))
               : file.parent_path() /
                     util::path_from_utf8(
                         file_name +
                         (std::filesystem::is_regular_file(
                              file.parent_path() / util::path_from_utf8(file_name + ".old"), ec)
                              ? ".old"
                              : ".disabled"));
    if (std::filesystem::exists(target, ec)) {
        util::Result<std::string> source_hash =
            util::hash_file_hex(util::HashAlgorithm::kSha1, file);
        util::Result<std::string> target_hash =
            util::hash_file_hex(util::HashAlgorithm::kSha1, target);
        if (source_hash.is_error() || target_hash.is_error()) {
            return util::Result<void>::fail(util::Error::make(
                "mod.file_unreadable", "无法比较 Mod 文件", util::path_to_utf8(file)));
        }
        if (!util::hash_hex_equal(source_hash.value(), target_hash.value())) {
            return util::Result<void>::fail(util::Error::make(
                "mod.conflict", "同时存在启用和禁用的同名 Mod，且内容不同",
                util::path_to_utf8(target)));
        }
        // 内容相同：删除源文件，保留目标状态。
        std::filesystem::remove(file, ec);
        if (ec) {
            return util::Result<void>::fail(util::Error::make(
                "mod.rename_failed", "删除重复 Mod 失败", util::path_to_utf8(file)));
        }
        return util::Result<void>::ok();
    }
    std::filesystem::rename(file, target, ec);
    if (ec) {
        return util::Result<void>::fail(util::Error::make(
            "mod.rename_failed", "重命名 Mod 失败（可能被游戏占用）",
            util::path_to_utf8(file)));
    }
    return util::Result<void>::ok();
}

util::Result<int> import_mod_files(const std::filesystem::path& mods_dir,
                                   const std::vector<std::filesystem::path>& files) {
    util::Result<void> created = ::sscl::platform::create_directories(mods_dir);
    if (created.is_error()) {
        return util::Result<int>::fail(std::move(created.error()));
    }
    int imported = 0;
    for (const std::filesystem::path& source : files) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) {
            continue;
        }
        std::string name = mod_enabled_name(util::path_to_utf8(source.filename()));
        if (name.find('.') == std::string::npos) {
            name += ".jar";
        }
        if (!mod_file_is_enabled(name)) {
            // PCL 接受 jar/litemod/zip；其他扩展名拒绝。
            continue;
        }
        const std::filesystem::path target = mods_dir / util::path_from_utf8(name);
        if (std::filesystem::equivalent(source, target, ec)) {
            continue;  // 不能删除自身
        }
        ec.clear();
        if (std::filesystem::exists(target, ec)) {
            std::filesystem::remove(target, ec);
            if (ec) {
                continue;
            }
        }
        std::filesystem::copy_file(source, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            ++imported;
        }
    }
    return util::Result<int>::ok(imported);
}

ModHashCache::ModHashCache(std::filesystem::path cache_file)
    : m_file(std::move(cache_file)) {
    if (enabled()) {
        util::Result<void> loaded = load();
        if (loaded.is_error()) {
            util::log_warn("[modhash] 读取缓存失败（忽略）：" + loaded.error().detail);
        }
    }
}

util::Result<void> ModHashCache::load() {
    util::Result<std::string> text = platform::read_file(m_file);
    if (text.is_error()) {
        return util::Result<void>::ok();
    }
    const json root = json::parse(text.value(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<void>::ok();
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.value().is_string()) {
            m_entries[it.key()] = it.value().get<std::string>();
        }
    }
    return util::Result<void>::ok();
}

std::string ModHashCache::key_for(const std::filesystem::path& file, std::uint64_t size,
                                  std::int64_t mtime) const {
    return util::wide_to_utf8(file.wstring()) + "|" + std::to_string(size) + "|" +
           std::to_string(mtime);
}

util::Result<std::string> ModHashCache::sha1(const std::filesystem::path& file) {
    std::error_code ec;
    const std::uint64_t size = static_cast<std::uint64_t>(std::filesystem::file_size(file, ec));
    if (ec) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_unreadable", "无法读取 Mod 文件", util::path_to_utf8(file)));
    }
    const std::filesystem::file_time_type ftime = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_unreadable", "无法读取 Mod 文件时间", util::path_to_utf8(file)));
    }
    const std::int64_t mtime = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count());
    const std::string key = key_for(file, size, mtime);
    const auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        return util::Result<std::string>::ok(it->second);
    }
    util::Result<std::string> digest =
        util::hash_file_hex(util::HashAlgorithm::kSha1, file);
    if (digest.is_error()) {
        return util::Result<std::string>::fail(util::Error::make(
            "mod.file_unreadable", "无法计算 Mod 文件哈希", util::path_to_utf8(file)));
    }
    m_entries[key] = digest.value();
    m_dirty = true;
    return digest;
}

util::Result<std::uint32_t> ModHashCache::curseforge_fingerprint(
    const std::filesystem::path& file) {
    std::error_code ec;
    const std::uint64_t size = static_cast<std::uint64_t>(std::filesystem::file_size(file, ec));
    if (ec) {
        return util::Result<std::uint32_t>::fail(util::Error::make(
            "mod.file_unreadable", "无法读取 Mod 文件", util::path_to_utf8(file)));
    }
    const std::filesystem::file_time_type ftime = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return util::Result<std::uint32_t>::fail(util::Error::make(
            "mod.file_unreadable", "无法读取 Mod 文件时间", util::path_to_utf8(file)));
    }
    const std::int64_t mtime = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count());
    const std::string key = key_for(file, size, mtime) + "|C";
    const auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        try {
            return util::Result<std::uint32_t>::ok(
                static_cast<std::uint32_t>(std::stoul(it->second)));
        } catch (...) {
        }
    }
    util::Result<std::uint32_t> fingerprint = util::curseforge_fingerprint(file);
    if (fingerprint.is_error()) {
        return util::Result<std::uint32_t>::fail(util::Error::make(
            "mod.file_unreadable", "无法计算 CurseForge 指纹", util::path_to_utf8(file)));
    }
    m_entries[key] = std::to_string(fingerprint.value());
    m_dirty = true;
    return fingerprint;
}

util::Result<void> ModHashCache::save() {
    if (!enabled() || !m_dirty) {
        return util::Result<void>::ok();
    }
    util::Result<void> created = ::sscl::platform::create_directories(m_file.parent_path());
    if (created.is_error()) {
        return created;
    }
    json root = json::object();
    for (const auto& entry : m_entries) {
        root[entry.first] = entry.second;
    }
    util::Result<void> written = platform::write_file_atomic(m_file, root.dump());
    if (written.is_ok()) {
        m_dirty = false;
    }
    return written;
}

}  // namespace sscl::core
