#include "core/launch/version_json.hpp"

#include <algorithm>
#include <regex>
#include <utility>

#include <nlohmann/json.hpp>

namespace sscl::core {

namespace {

using nlohmann::json;

std::string json_string(const json& node, const char* key) {
    // const operator[] 对缺失键抛异常，必须先检查
    if (!node.is_object() || !node.contains(key)) {
        return {};
    }
    const json& value = node.at(key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

std::vector<Rule> parse_rules(const json& node) {
    std::vector<Rule> rules;
    if (!node.is_array()) {
        return rules;
    }
    for (const json& entry : node) {
        Rule rule;
        rule.action = json_string(entry, "action");
        if (entry.contains("os") && entry["os"].is_object()) {
            const json& os = entry["os"];
            rule.has_os = true;
            rule.os.name = json_string(os, "name");
            rule.os.version = json_string(os, "version");
            rule.os.arch = json_string(os, "arch");
        }
        if (entry.contains("features") && entry["features"].is_object()) {
            for (auto it = entry["features"].begin(); it != entry["features"].end(); ++it) {
                if (it.value().is_boolean()) {
                    rule.features[it.key()] = it.value().get<bool>();
                }
            }
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

LibraryArtifact parse_artifact(const json& node) {
    LibraryArtifact artifact;
    artifact.path = json_string(node, "path");
    artifact.url = json_string(node, "url");
    artifact.sha1 = json_string(node, "sha1");
    if (node.contains("size") && node["size"].is_number_integer()) {
        artifact.size = node["size"].get<std::int64_t>();
    }
    return artifact;
}

void parse_argument_entries(const json& values, ArgumentList* out) {
    if (!values.is_array()) {
        return;
    }
    for (const json& entry : values) {
        RuledValues ruled;
        if (entry.is_string()) {
            ruled.values.push_back(entry.get<std::string>());
        } else if (entry.is_object()) {
            ruled.rules = parse_rules(entry.contains("rules") ? entry["rules"] : json::array());
            const json& value = entry.contains("value") ? entry["value"] : json();
            if (value.is_string()) {
                ruled.values.push_back(value.get<std::string>());
            } else if (value.is_array()) {
                for (const json& item : value) {
                    if (item.is_string()) {
                        ruled.values.push_back(item.get<std::string>());
                    }
                }
            }
        }
        if (!ruled.values.empty() || !ruled.rules.empty()) {
            out->entries.push_back(std::move(ruled));
        }
    }
}

// PCL 的 JObject.Merge 语义：对象递归合并，数组/标量由后者替换。
void merge_json_objects(json& target, const json& source) {
    if (target.is_object() && source.is_object()) {
        for (auto it = source.begin(); it != source.end(); ++it) {
            if (target.contains(it.key())) {
                merge_json_objects(target[it.key()], it.value());
            } else {
                target[it.key()] = it.value();
            }
        }
        return;
    }
    target = source;
}

// HMCL 版本：用 patches 代替 inheritsFrom。按 priority 升序合并后，id 设为实例名，
// 并删除 inheritsFrom（PCL ModMinecraft.vb:427-456）。
void normalize_hmcl_patches(json& root) {
    if (!root.is_object() || !root.contains("patches") || !root["patches"].is_array() ||
        root.contains("time")) {
        return;
    }
    std::vector<json> patches;
    for (const json& patch : root["patches"]) {
        if (patch.is_object()) {
            patches.push_back(patch);
        }
    }
    std::stable_sort(patches.begin(), patches.end(), [](const json& a, const json& b) {
        auto priority = [](const json& value) {
            if (!value.contains("priority")) {
                return 0.0;
            }
            if (value["priority"].is_number()) {
                return value["priority"].get<double>();
            }
            if (value["priority"].is_string()) {
                try {
                    return std::stod(value["priority"].get<std::string>());
                } catch (...) {
                    return 0.0;
                }
            }
            return 0.0;
        };
        return priority(a) < priority(b);
    });
    json merged;
    for (const json& patch : patches) {
        merge_json_objects(merged, patch);
    }
    if (!merged.is_object()) {
        return;
    }
    const std::string root_id = json_string(root, "id");
    const std::string merged_id = json_string(merged, "id");
    merged["id"] = root_id.empty() ? merged_id : root_id;
    merged.erase("inheritsFrom");
    root = std::move(merged);
}

std::string normalize_os_name(std::string name) {    if (name == "win" || name == "windows") {
        return "windows";
    }
    if (name == "mac" || name == "osx" || name == "macos") {
        return "osx";
    }
    return name;  // linux / 其它原样
}

bool regex_full_match(const std::string& pattern, const std::string& value) {
    try {
        return std::regex_match(value, std::regex(pattern));
    } catch (const std::regex_error&) {
        return false;  // 非法正则：规则不生效
    }
}

bool rule_matches(const Rule& rule, const OsInfo& os,
                  const std::map<std::string, bool>& features) {
    if (rule.has_os) {
        std::string name = rule.os.name;
        std::string arch = rule.os.arch;
        // "win-x86" 复合形式（name-arch 两段）
        const std::size_t dash = name.find('-');
        if (dash != std::string::npos && dash > 0) {
            const std::string arch_part = name.substr(dash + 1);
            name = name.substr(0, dash);
            if (arch.empty()) {
                arch = arch_part;
            }
        }
        if (!name.empty() && normalize_os_name(name) != normalize_os_name(os.name)) {
            return false;
        }
        if (!rule.os.version.empty() && !regex_full_match(rule.os.version, os.version)) {
            return false;
        }
        if (!arch.empty() && !regex_full_match(arch, os.arch)) {
            return false;
        }
    }
    for (const auto& [key, expected] : rule.features) {
        const auto it = features.find(key);
        if (it == features.end() || it->second != expected) {
            return false;
        }
    }
    return true;
}

}  // namespace

util::Result<VersionJson> parse_version_json(const std::string& json_text) {
    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return util::Result<VersionJson>::fail(
            util::Error::make("version.parse_failed", "版本 JSON 解析失败", {}));
    }
    normalize_hmcl_patches(root);

    VersionJson version;
    version.id = json_string(root, "id");
    if (version.id.empty()) {
        // Version.java:494：id 为空解析失败
        return util::Result<VersionJson>::fail(
            util::Error::make("version.missing_id", "版本 JSON 缺少 id", {}));
    }
    version.inherits_from = json_string(root, "inheritsFrom");
    version.main_class = json_string(root, "mainClass");
    version.minecraft_arguments = json_string(root, "minecraftArguments");
    version.jar = json_string(root, "jar");
    version.assets = json_string(root, "assets");
    version.type = json_string(root, "type");
    version.client_version = json_string(root, "clientVersion");
    version.release_time = json_string(root, "releaseTime");
    version.time = json_string(root, "time");

    // SSCL 元数据（D-037）：用于版本隔离/Mod 目标推断；未知字段忽略。
    if (root.contains("sscl") && root["sscl"].is_object()) {
        const json& sscl = root["sscl"];
        version.sscl.has = true;
        version.sscl.loader = json_string(sscl, "loader");
        version.sscl.loader_version = json_string(sscl, "loaderVersion");
        version.sscl.minecraft_version = json_string(sscl, "minecraftVersion");
        version.sscl.installer_url = json_string(sscl, "installerUrl");
    }

    if (root.contains("minimumLauncherVersion") &&
        root["minimumLauncherVersion"].is_number_integer()) {
        version.minimum_launcher_version = root["minimumLauncherVersion"].get<int>();
    }

    if (root.contains("arguments") && root["arguments"].is_object()) {
        version.has_arguments = true;
        parse_argument_entries(root["arguments"].value("game", json::array()),
                               &version.game_arguments);
        parse_argument_entries(root["arguments"].value("jvm", json::array()),
                               &version.jvm_arguments);
    }

    if (root.contains("libraries") && root["libraries"].is_array()) {
        for (const json& entry : root["libraries"]) {
            Library library;
            library.name = json_string(entry, "name");
            library.url = json_string(entry, "url");  // Maven 基址（Fabric 库只有 name+url）
            if (entry.contains("natives") && entry["natives"].is_object()) {
                for (auto it = entry["natives"].begin(); it != entry["natives"].end(); ++it) {
                    if (it.value().is_string()) {
                        library.natives[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (entry.contains("downloads") && entry["downloads"].is_object()) {
                const json& downloads = entry["downloads"];
                if (downloads.contains("artifact") && downloads["artifact"].is_object()) {
                    library.has_artifact = true;
                    library.artifact = parse_artifact(downloads["artifact"]);
                }
                if (downloads.contains("classifiers") && downloads["classifiers"].is_object()) {
                    for (auto it = downloads["classifiers"].begin();
                         it != downloads["classifiers"].end(); ++it) {
                        if (it.value().is_object()) {
                            library.classifiers[it.key()] = parse_artifact(it.value());
                        }
                    }
                }
            }
            if (entry.contains("extract") && entry["extract"].is_object()) {
                library.has_extract = true;
                const json& exclude = entry["extract"].contains("exclude")
                                          ? entry["extract"]["exclude"]
                                          : json::array();
                if (exclude.is_array()) {
                    for (const json& item : exclude) {
                        if (item.is_string()) {
                            library.extract.exclude.push_back(item.get<std::string>());
                        }
                    }
                }
            }
            library.rules = parse_rules(entry.contains("rules") ? entry["rules"] : json::array());
            library.hint = json_string(entry, "hint");
            // 本地生成库：声明了 artifact 但 url 为空（Forge/NeoForge 的 forge-<v>-client.jar 由处理器产出）
            library.local_only = library.has_artifact && library.artifact.url.empty();
            version.libraries.push_back(std::move(library));
        }
    }

    if (root.contains("assetIndex") && root["assetIndex"].is_object()) {
        version.has_asset_index = true;
        version.asset_index.id = json_string(root["assetIndex"], "id");
        version.asset_index.url = json_string(root["assetIndex"], "url");
        version.asset_index.sha1 = json_string(root["assetIndex"], "sha1");
    }

    if (root.contains("downloads") && root["downloads"].is_object()) {
        const json& downloads = root["downloads"];
        if (downloads.contains("client") && downloads["client"].is_object()) {
            version.has_downloads = true;
            version.downloads.has_client = true;
            version.downloads.client.url = json_string(downloads["client"], "url");
            version.downloads.client.sha1 = json_string(downloads["client"], "sha1");
            if (downloads["client"].contains("size") &&
                downloads["client"]["size"].is_number_integer()) {
                version.downloads.client.size = downloads["client"]["size"].get<std::int64_t>();
            }
        }
        // 官方映射（Forge 的 DOWNLOAD_MOJMAPS 处理器以它替代联网下载）
        if (downloads.contains("client_mappings") && downloads["client_mappings"].is_object()) {
            version.has_downloads = true;
            version.downloads.has_client_mappings = true;
            version.downloads.client_mappings.url =
                json_string(downloads["client_mappings"], "url");
            version.downloads.client_mappings.sha1 =
                json_string(downloads["client_mappings"], "sha1");
            if (downloads["client_mappings"].contains("size") &&
                downloads["client_mappings"]["size"].is_number_integer()) {
                version.downloads.client_mappings.size =
                    downloads["client_mappings"]["size"].get<std::int64_t>();
            }
        }
    }

    // 安装器处理器（install_profile.json 风格；仅安装期使用）
    if (root.contains("processors") && root["processors"].is_array()) {
        version.has_processors = true;
        for (const json& entry : root["processors"]) {
            if (!entry.is_object()) {
                continue;
            }
            InstallerProcessor processor;
            processor.jar = json_string(entry, "jar");
            if (processor.jar.empty()) {
                continue;  // 无 jar 的处理器无意义（ForgeNewInstallProfile.validate 同款）
            }
            if (entry.contains("sides") && entry["sides"].is_array()) {
                for (const json& side : entry["sides"]) {
                    if (side.is_string()) {
                        processor.sides.push_back(side.get<std::string>());
                    }
                }
            }
            if (entry.contains("classpath") && entry["classpath"].is_array()) {
                for (const json& item : entry["classpath"]) {
                    if (item.is_string()) {
                        processor.classpath.push_back(item.get<std::string>());
                    }
                }
            }
            if (entry.contains("args") && entry["args"].is_array()) {
                for (const json& item : entry["args"]) {
                    if (item.is_string()) {
                        processor.args.push_back(item.get<std::string>());
                    }
                }
            }
            if (entry.contains("outputs") && entry["outputs"].is_object()) {
                for (auto it = entry["outputs"].begin(); it != entry["outputs"].end(); ++it) {
                    if (it.value().is_string()) {
                        processor.outputs[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            version.processors.push_back(std::move(processor));
        }
    }

    // 安装器数据（{KEY: {client, server}}）
    if (root.contains("data") && root["data"].is_object()) {
        version.has_installer_data = true;
        for (auto it = root["data"].begin(); it != root["data"].end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }
            SidedData sided;
            sided.client = json_string(it.value(), "client");
            sided.server = json_string(it.value(), "server");
            version.installer_data[it.key()] = std::move(sided);
        }
    }

    if (root.contains("logging") && root["logging"].is_object() &&
        root["logging"].contains("client") && root["logging"]["client"].is_object()) {
        const json& client = root["logging"]["client"];
        version.has_logging = true;
        version.logging.argument = json_string(client, "argument");
        if (client.contains("file") && client["file"].is_object()) {
            version.logging.file.id = json_string(client["file"], "id");
            version.logging.file.url = json_string(client["file"], "url");
            version.logging.file.sha1 = json_string(client["file"], "sha1");
            if (client["file"].contains("size") && client["file"]["size"].is_number_integer()) {
                version.logging.file.size = client["file"]["size"].get<std::int64_t>();
            }
        }
    }

    if (root.contains("javaVersion") && root["javaVersion"].is_object()) {
        version.has_java_version = true;
        version.java_version.component = json_string(root["javaVersion"], "component");
        if (root["javaVersion"].contains("majorVersion") &&
            root["javaVersion"]["majorVersion"].is_number_integer()) {
            version.java_version.major_version = root["javaVersion"]["majorVersion"].get<int>();
        }
    }

    return util::Result<VersionJson>::ok(std::move(version));
}

VersionJson merge_version(const VersionJson& child, const VersionJson& parent) {
    VersionJson merged;

    // id / type / time：子优先（version-json.md §2）
    merged.id = child.id.empty() ? parent.id : child.id;
    merged.type = child.type.empty() ? parent.type : child.type;
    merged.client_version =
        child.client_version.empty() ? parent.client_version : child.client_version;
    merged.release_time = child.release_time.empty() ? parent.release_time : child.release_time;
    merged.time = child.time.empty() ? parent.time : child.time;

    // minecraftArguments：子非空用子，否则用父
    merged.minecraft_arguments =
        child.minecraft_arguments.empty() ? parent.minecraft_arguments : child.minecraft_arguments;

    // arguments：**父的列表在前、子的追加在后**（与 libraries 顺序相反！）
    if (child.has_arguments && parent.has_arguments) {
        merged.has_arguments = true;
        merged.jvm_arguments.entries = parent.jvm_arguments.entries;
        merged.jvm_arguments.entries.insert(merged.jvm_arguments.entries.end(),
                                            child.jvm_arguments.entries.begin(),
                                            child.jvm_arguments.entries.end());
        merged.game_arguments.entries = parent.game_arguments.entries;
        merged.game_arguments.entries.insert(merged.game_arguments.entries.end(),
                                             child.game_arguments.entries.begin(),
                                             child.game_arguments.entries.end());
    } else if (child.has_arguments) {
        merged.has_arguments = true;
        merged.jvm_arguments = child.jvm_arguments;
        merged.game_arguments = child.game_arguments;
    } else if (parent.has_arguments) {
        merged.has_arguments = true;
        merged.jvm_arguments = parent.jvm_arguments;
        merged.game_arguments = parent.game_arguments;
    }

    // mainClass：子非空用子，否则用父
    merged.main_class = child.main_class.empty() ? parent.main_class : child.main_class;

    // jar：子优先；两者都无则用 id（Version.java:315）
    merged.jar = !child.jar.empty() ? child.jar : parent.jar;
    if (merged.jar.empty()) {
        merged.jar = merged.id;
    }

    // assets / assetIndex：子优先
    merged.assets = child.assets.empty() ? parent.assets : child.assets;
    if (child.has_asset_index) {
        merged.has_asset_index = true;
        merged.asset_index = child.asset_index;
    } else if (parent.has_asset_index) {
        merged.has_asset_index = true;
        merged.asset_index = parent.asset_index;
    }

    // javaVersion：子优先
    if (child.has_java_version) {
        merged.has_java_version = true;
        merged.java_version = child.java_version;
    } else if (parent.has_java_version) {
        merged.has_java_version = true;
        merged.java_version = parent.java_version;
    }

    // sscl 元数据：子优先（加载器子版本覆盖父版本）
    if (child.sscl.has) {
        merged.sscl = child.sscl;
    } else if (parent.sscl.has) {
        merged.sscl = parent.sscl;
    }

    // libraries：**子的列表在前、父的追加在后**
    merged.libraries = child.libraries;
    merged.libraries.insert(merged.libraries.end(), parent.libraries.begin(),
                            parent.libraries.end());

    // 安装器数据 / 处理器：子优先（同一条版本链通常只有一层带安装器的版本）
    if (child.has_processors) {
        merged.has_processors = true;
        merged.processors = child.processors;
    } else if (parent.has_processors) {
        merged.has_processors = true;
        merged.processors = parent.processors;
    }
    if (child.has_installer_data) {
        merged.has_installer_data = true;
        merged.installer_data = child.installer_data;
    } else if (parent.has_installer_data) {
        merged.has_installer_data = true;
        merged.installer_data = parent.installer_data;
    }

    // downloads / logging：整个 map，子非空直接用子（不逐项合并）
    if (child.has_downloads) {
        merged.has_downloads = true;
        merged.downloads = child.downloads;
    } else if (parent.has_downloads) {
        merged.has_downloads = true;
        merged.downloads = parent.downloads;
    }
    if (child.has_logging) {
        merged.has_logging = true;
        merged.logging = child.logging;
    } else if (parent.has_logging) {
        merged.has_logging = true;
        merged.logging = parent.logging;
    }

    // minimumLauncherVersion：取 max
    merged.minimum_launcher_version =
        (std::max)(child.minimum_launcher_version, parent.minimum_launcher_version);

    return merged;
}

bool rules_allow(const std::vector<Rule>& rules, const OsInfo& os,
                 const std::map<std::string, bool>& features) {
    if (rules.empty()) {
        return true;
    }
    bool allowed = false;  // 有规则时初值 DISALLOW
    for (const Rule& rule : rules) {
        if (rule_matches(rule, os, features)) {
            allowed = (rule.action == "allow");
        }
    }
    return allowed;
}

std::vector<std::string> collect_arguments(const ArgumentList& list, const OsInfo& os,
                                           const std::map<std::string, bool>& features) {
    std::vector<std::string> out;
    for (const RuledValues& entry : list.entries) {
        if (rules_allow(entry.rules, os, features)) {
            out.insert(out.end(), entry.values.begin(), entry.values.end());
        }
    }
    return out;
}

std::string select_native_classifier(const Library& library, const std::string& os_name,
                                     bool is_64bit) {
    const auto it = library.natives.find(os_name);
    if (it == library.natives.end()) {
        return {};
    }
    std::string classifier = it->second;
    const std::string placeholder = "${arch}";
    const std::size_t pos = classifier.find(placeholder);
    if (pos != std::string::npos) {
        classifier.replace(pos, placeholder.size(), is_64bit ? "64" : "32");
    }
    return classifier;
}

std::string library_relative_path(const std::string& maven_name) {
    // group:artifact:version[:classifier][@ext]
    // @ext 形式（如 maven 坐标 :mappings@tsrg）在 Forge 安装器里常见（knowledge/mod-loader-install.md §6）
    std::string coordinate = maven_name;
    std::string extension = "jar";
    const std::size_t at = coordinate.rfind('@');
    if (at != std::string::npos && at + 1 < coordinate.size()) {
        extension = coordinate.substr(at + 1);
        coordinate.resize(at);
    }
    if (extension.empty()) {
        extension = "jar";
    }

    std::vector<std::string> parts;
    std::string current;
    for (const char c : coordinate) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    if (parts.size() < 3) {
        return {};
    }
    std::string group = parts[0];
    for (char& c : group) {
        if (c == '.') {
            c = '/';
        }
    }
    const std::string& artifact = parts[1];
    const std::string& version = parts[2];
    const std::string classifier = parts.size() >= 4 ? parts[3] : std::string();

    std::string file = artifact + "-" + version;
    if (!classifier.empty()) {
        file += "-" + classifier;
    }
    file += "." + extension;
    return group + "/" + artifact + "/" + version + "/" + file;
}

}  // namespace sscl::core
