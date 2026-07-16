#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <iostream>
#include <map>
#include <set>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "lgx.h"
#include "logos/semver.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::vector<std::string> splitString(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        parts.push_back(token);
    }
    return parts;
}

static std::string joinStrings(const std::vector<std::string>& items, const char* sep) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += sep;
        out += items[i];
    }
    return out;
}

const char* installTypeToString(InstallType t) {
    switch (t) {
        case InstallType::Embedded: return "embedded";
        case InstallType::User:     return "user";
    }
    return "";
}

const char* dependencyStatusToString(DependencyStatus s) {
    switch (s) {
        case DependencyStatus::Installed:    return "installed";
        case DependencyStatus::NotInstalled: return "not_installed";
        case DependencyStatus::Cycle:        return "cycle";
    }
    return "";
}

bool PackageManagerLib::versionGreaterOrEqual(const std::string& a, const std::string& b)
{
    // Delegates to the shared implementation in logos-package. This used to
    // split on '.' and atoi() each component, which parsed "1.0.0-rc1" as
    // 1.0.0 — so a pre-release compared equal to its own release and the
    // skipIfNotNewerVersion gate below refused to install over it.
    return logos::semver::greater_or_equal(a, b);
}

PackageManagerLib::PackageManagerLib()
{
}

PackageManagerLib::~PackageManagerLib()
{
}

void PackageManagerLib::setEmbeddedModulesDirectory(const std::string& dir)
{
    m_embeddedModulesDirs.clear();
    if (!dir.empty()) m_embeddedModulesDirs.push_back(dir);
}

void PackageManagerLib::addEmbeddedModulesDirectory(const std::string& dir)
{
    if (!dir.empty()) m_embeddedModulesDirs.push_back(dir);
}

void PackageManagerLib::setUserModulesDirectory(const std::string& dir)
{
    m_userModulesDir = dir;
}

void PackageManagerLib::setEmbeddedUiPluginsDirectory(const std::string& dir)
{
    m_embeddedUiPluginsDirs.clear();
    if (!dir.empty()) m_embeddedUiPluginsDirs.push_back(dir);
}

void PackageManagerLib::addEmbeddedUiPluginsDirectory(const std::string& dir)
{
    if (!dir.empty()) m_embeddedUiPluginsDirs.push_back(dir);
}

void PackageManagerLib::setUserUiPluginsDirectory(const std::string& dir)
{
    m_userUiPluginsDir = dir;
}

std::vector<std::string> PackageManagerLib::allModulesDirectories() const
{
    std::vector<std::string> dirs = m_embeddedModulesDirs;
    if (!m_userModulesDir.empty()) dirs.push_back(m_userModulesDir);
    return dirs;
}

std::vector<std::string> PackageManagerLib::allUiPluginsDirectories() const
{
    std::vector<std::string> dirs = m_embeddedUiPluginsDirs;
    if (!m_userUiPluginsDir.empty()) dirs.push_back(m_userUiPluginsDir);
    return dirs;
}

std::vector<std::string> PackageManagerLib::allDirectories() const
{
    std::vector<std::string> dirs = m_embeddedModulesDirs;
    if (!m_userModulesDir.empty()) dirs.push_back(m_userModulesDir);
    dirs.insert(dirs.end(), m_embeddedUiPluginsDirs.begin(), m_embeddedUiPluginsDirs.end());
    if (!m_userUiPluginsDir.empty()) dirs.push_back(m_userUiPluginsDir);
    return dirs;
}

std::string PackageManagerLib::installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                                  bool skipIfNotNewerVersion,
                                                  std::string* installedPluginPath,
                                                  bool* isCoreModuleOut)
{
    fs::path sourcePath(pluginPath);
    if (!fs::exists(sourcePath) || !fs::is_regular_file(sourcePath)) {
        errorMsg = "Source plugin file does not exist or is not a file: " + pluginPath;
        return {};
    }

    std::string ext = sourcePath.extension().string();
    // Lowercase the extension
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".lgx") {
        errorMsg = "Only LGX packages are supported. Got: " + sourcePath.extension().string();
        return {};
    }

    // If requested, skip installation when an equal-or-higher version is already present.
    if (skipIfNotNewerVersion) {
        lgx_package_t pkg = lgx_load(pluginPath.c_str());
        if (pkg) {
            const char* rawName = lgx_get_name(pkg);
            const char* rawVersion = lgx_get_version(pkg);
            std::string incomingName = rawName ? rawName : "";
            std::string incomingVersion = rawVersion ? rawVersion : "";
            lgx_free_package(pkg);

            // incomingName comes from the untrusted package; only use it as a
            // path component if it is a valid single-segment module name, or a
            // crafted name ("../x", "/abs") would let this probe arbitrary
            // manifest.json files outside the install dirs (see F-006).
            if (!incomingName.empty() && !incomingVersion.empty() && isValidModuleName(incomingName)) {
                for (const auto& baseDir : allDirectories()) {
                    if (baseDir.empty())
                        continue;
                    fs::path manifestPath = fs::path(baseDir) / incomingName / "manifest.json";
                    if (fs::exists(manifestPath)) {
                        std::ifstream mf(manifestPath);
                        if (mf.is_open()) {
                            try {
                                json doc = json::parse(mf);
                                std::string installedVersion = doc.value("version", "");
                                if (!installedVersion.empty() && versionGreaterOrEqual(installedVersion, incomingVersion)) {
                                    std::string existingDir = (fs::path(baseDir) / incomingName).string();
                                    if (installedPluginPath) *installedPluginPath = existingDir;
                                    if (isCoreModuleOut) *isCoreModuleOut = false;
                                    return existingDir;
                                }
                            } catch (...) {
                                // ignore parse errors
                            }
                        }
                    }
                }
            }
        }
    }

    // Verify signature
    if (m_signaturePolicy != SignaturePolicy::NONE) {
        auto sigResult = verifyPackageSignature(pluginPath);

        if (sigResult.is_signed && !sigResult.signature_valid) {
            errorMsg = "Invalid signature: " + (sigResult.error.empty() ? "unknown error" : sigResult.error);
            return {};
        }
        if (!sigResult.package_valid) {
            errorMsg = "Package validation failed: " + (sigResult.error.empty() ? "unknown error" : sigResult.error);
            return {};
        }
        if (!sigResult.is_signed && m_signaturePolicy == SignaturePolicy::REQUIRE) {
            errorMsg = "Package is unsigned and signature policy requires signatures";
            return {};
        }
        if (sigResult.is_signed && sigResult.trusted_as.empty() &&
            m_signaturePolicy == SignaturePolicy::REQUIRE) {
            errorMsg = "Package signed by untrusted key: " + sigResult.signer_did;
            return {};
        }
        if (!sigResult.is_signed && m_signaturePolicy == SignaturePolicy::WARN) {
            std::cerr << "Warning: Package is unsigned: " << pluginPath << "\n";
        }
    }

    // Create temporary directory for extraction
    std::string tempDir;
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        std::error_code tmpEc;
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::ostringstream oss;
            oss << "lgpm_extract_" << std::hex << dist(gen);
            auto candidate = fs::temp_directory_path(tmpEc) / oss.str();
            if (tmpEc) break;
            if (fs::create_directory(candidate, tmpEc) && !tmpEc) {
                tempDir = candidate.string();
                break;
            }
        }
        if (tempDir.empty()) {
            errorMsg = "Failed to create temporary directory for LGX extraction";
            return {};
        }
    }

    if (!extractLgxPackage(pluginPath, tempDir, errorMsg)) {
        fs::remove_all(tempDir);
        return {};
    }

    // Auto-detect module type from manifest.json in the extracted variant directory
    auto variants = platformVariantsToTry();
    std::string variantDir;
    for (const auto& v : variants) {
        fs::path candidate = fs::path(tempDir) / v;
        if (fs::is_directory(candidate)) {
            variantDir = candidate.string();
            break;
        }
    }

    // Determine module type from manifest.json "type" field
    std::string detectedType;
    if (!variantDir.empty()) {
        fs::path manifestPath = fs::path(variantDir) / "manifest.json";
        if (fs::exists(manifestPath)) {
            std::ifstream mf(manifestPath);
            if (mf.is_open()) {
                try {
                    json doc = json::parse(mf);
                    detectedType = doc.value("type", "");
                } catch (...) {}
            }
        }
    }
    bool isCoreModule = (detectedType == "core");
    if (isCoreModuleOut)
        *isCoreModuleOut = isCoreModule;

    // Always install to user directories
    std::string installDir;
    if (isCoreModule) {
        installDir = m_userModulesDir;
    } else {
        installDir = m_userUiPluginsDir;
    }

    if (installDir.empty()) {
        errorMsg = isCoreModule
            ? "User modules directory is not set. Cannot install plugin."
            : "User UI plugins directory is not set. Cannot install plugin.";
        fs::remove_all(tempDir);
        return {};
    }

    // Create install directory if it doesn't exist
    std::error_code ec;
    fs::create_directories(installDir, ec);
    if (ec) {
        errorMsg = "Failed to create install directory: " + installDir;
        fs::remove_all(tempDir);
        return {};
    }

    std::string installedModuleName;
    if (!copyLibraryFromExtracted(tempDir, installDir, isCoreModule, installedModuleName, errorMsg)) {
        fs::remove_all(tempDir);
        return {};
    }

    // Determine the installed main file path
    if (installedPluginPath) {
        *installedPluginPath = {};
        std::string mainFile;
        bool isQmlPackage = (detectedType == "ui_qml");
        fs::path installedManifestPath = fs::path(installDir) / installedModuleName / "manifest.json";
        if (fs::exists(installedManifestPath)) {
            std::ifstream mf(installedManifestPath);
            if (mf.is_open()) {
                try {
                    json doc = json::parse(mf);
                    if (doc.contains("main")) {
                        if (doc["main"].is_object()) {
                            for (const auto& v : platformVariantsToTry()) {
                                if (doc["main"].contains(v)) {
                                    mainFile = doc["main"][v].get<std::string>();
                                    if (!mainFile.empty())
                                        break;
                                }
                            }
                        } else if (doc["main"].is_string()) {
                            mainFile = doc["main"].get<std::string>();
                        }
                    }
                } catch (...) {}
            }
        }
        if (mainFile.empty() && !isQmlPackage) {
            mainFile = installedModuleName;
        }
        if (!mainFile.empty()) {
            if (!isQmlPackage && mainFile.find('.') == std::string::npos) {
#if defined(__APPLE__)
                mainFile += ".dylib";
#elif defined(_WIN32)
                mainFile += ".dll";
#else
                mainFile += ".so";
#endif
            }
            fs::path mainPath = fs::path(installDir) / installedModuleName / mainFile;
            if (fs::exists(mainPath)) {
                *installedPluginPath = mainPath.string();
            }
        }
    }

    fs::remove_all(tempDir);
    return installDir;
}

namespace {

// Enriched scan record. `installType` tracks whether the winning copy lives
// in an embedded (shipped, read-only) directory or a user-writable one.
struct ScanEntry {
    std::string name;
    std::string type;
    std::string version;
    std::string installDir;
    std::string mainFilePath;
    InstallType installType = InstallType::User;
    std::vector<std::string> dependencies;
    json manifest;  // full manifest.json — used to emit the JSON passthrough
};

static std::string resolveMainFilePath(const json& manifest,
                                       const fs::path& moduleDir,
                                       const std::vector<std::string>& variants)
{
    if (!manifest.contains("main"))
        return {};

    if (manifest["main"].is_object()) {
        const auto& mainObj = manifest["main"];
        for (const auto& variant : variants) {
            if (mainObj.contains(variant)) {
                std::string mainFile = mainObj[variant].get<std::string>();
                if (!mainFile.empty()) {
                    fs::path candidate = moduleDir / mainFile;
                    return fs::exists(candidate) ? candidate.string() : std::string{};
                }
            }
        }
        return {};
    }
    if (manifest["main"].is_string()) {
        std::string mainFile = manifest["main"].get<std::string>();
        if (mainFile.empty())
            return {};
        fs::path candidate = moduleDir / mainFile;
        return fs::exists(candidate) ? candidate.string() : std::string{};
    }
    return {};
}

// Reads the single-line `variant` file that installPluginFile writes into
// every installed module directory, recording which variant was physically
// extracted there. Returns empty if the file is absent (e.g. embedded or
// legacy installs) — callers must treat that as "unknown", not a mismatch.
static std::string readInstalledVariant(const fs::path& moduleDir)
{
    std::ifstream vf(moduleDir / "variant");
    if (!vf.is_open())
        return {};
    std::string variant;
    std::getline(vf, variant);
    // Trim trailing whitespace / CR so the comparison is exact.
    while (!variant.empty() &&
           (variant.back() == '\r' || variant.back() == '\n' ||
            variant.back() == ' '  || variant.back() == '\t')) {
        variant.pop_back();
    }
    return variant;
}

// Enumerate all manifests under the given embedded + user dir lists,
// deduping by package name with user-dir entries winning over embedded ones.
// `types` filter selects by manifest.type; empty = no filter.
static std::map<std::string, ScanEntry> enumerateManifests(
    const std::vector<std::string>& embeddedDirs,
    const std::vector<std::string>& userDirs,
    const std::vector<std::string>& types)
{
    std::map<std::string, ScanEntry> byName;
    auto variants = PackageManagerLib::platformVariantsToTry();

    auto scanOne = [&](const std::string& dirPath, InstallType installType) {
        std::error_code dirEc;
        if (!fs::is_directory(dirPath, dirEc) || dirEc)
            return;

        for (const auto& entry : fs::directory_iterator(
                 dirPath,
                 fs::directory_options::skip_permission_denied,
                 dirEc)) {
            if (dirEc) break;
            std::error_code entryEc;
            if (!entry.is_directory(entryEc) || entryEc)
                continue;

            fs::path manifestPath = entry.path() / "manifest.json";
            std::ifstream manifestFile(manifestPath);
            if (!manifestFile.is_open())
                continue;

            json manifest;
            try {
                manifest = json::parse(manifestFile);
            } catch (...) {
                continue;
            }

            if (!manifest.is_object())
                continue;

            std::string name = manifest.value("name", "");
            if (name.empty())
                continue;

            std::string moduleType = manifest.value("type", "");
            if (!types.empty()) {
                bool matches = false;
                for (const auto& t : types) {
                    if (moduleType == t) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // User always wins — later scans for the same name clobber earlier ones.
            // Since we scan embedded first, then user, the map ends with the correct entry.
            ScanEntry scan;
            scan.name = name;
            scan.type = moduleType;
            scan.version = manifest.value("version", "");
            scan.installDir = entry.path().string();
            scan.installType = installType;
            scan.mainFilePath = resolveMainFilePath(manifest, entry.path(), variants);

            // Surface the variant-mismatch silent-drop (logos-basecamp#191): a
            // module installed for a variant this build does not accept is
            // unloadable on this platform but otherwise scans clean. The
            // installed `variant` file records exactly what was extracted here,
            // so compare it against the supported list and emit one line per
            // affected module (naming the installed variant, the supported list,
            // and the directory) so the cause is greppable. Modules without a
            // variant file (embedded / legacy installs) are left untouched.
            std::string installedVariant = readInstalledVariant(entry.path());
            if (!installedVariant.empty() &&
                std::find(variants.begin(), variants.end(), installedVariant) == variants.end()) {
                std::cerr << "Warning: module '" << name << "' in " << entry.path().string()
                          << " was installed for variant '" << installedVariant
                          << "' which is not supported on this platform and will not be loadable: "
                          << "supported variants [" << joinStrings(variants, ", ") << "].\n";
            }

            if (manifest.contains("dependencies") && manifest["dependencies"].is_array()) {
                for (const auto& d : manifest["dependencies"]) {
                    if (d.is_string()) {
                        std::string depName = d.get<std::string>();
                        if (!depName.empty())
                            scan.dependencies.push_back(depName);
                    }
                }
            }
            scan.manifest = std::move(manifest);

            byName[name] = std::move(scan);
        }
    };

    for (const auto& d : embeddedDirs) scanOne(d, InstallType::Embedded);
    for (const auto& d : userDirs)     scanOne(d, InstallType::User);

    return byName;
}

// Materialise a struct from a scan. Reads the manifest fields that
// downstream consumers actually use (field-use audit: name, version,
// description, type, category, author, license, icon, view, dependencies,
// hashes.root); the synthesised installDir / mainFilePath / installType
// come straight from the scan result. Missing fields default to empty —
// the manifest is already partially validated by enumerateManifests so
// name / type / version are present in any well-formed entry.
static InstalledPackage scanToInstalledPackage(const ScanEntry& scan)
{
    InstalledPackage p;
    p.name        = scan.name;
    p.version     = scan.version;
    p.type        = scan.type;
    p.installType = scan.installType;
    p.installDir  = scan.installDir;
    p.mainFilePath = scan.mainFilePath;
    p.dependencies = scan.dependencies;

    const json& m = scan.manifest;
    p.displayName = m.value("display_name", "");
    p.description = m.value("description", "");
    p.category    = m.value("category", "");
    p.author      = m.value("author", "");
    p.license     = m.value("license", "");
    p.icon        = m.value("icon", "");
    p.view        = m.value("view", "");

    if (m.contains("hashes") && m["hashes"].is_object()) {
        p.hashes.root = m["hashes"].value("root", "");
    }

    return p;
}

static std::vector<InstalledPackage> scansToInstalledPackages(
    const std::map<std::string, ScanEntry>& byName)
{
    std::vector<InstalledPackage> out;
    out.reserve(byName.size());
    for (const auto& [name, scan] : byName) {
        out.push_back(scanToInstalledPackage(scan));
    }
    return out;
}

} // namespace

std::vector<InstalledPackage> PackageManagerLib::getInstalledModules()
{
    auto byName = enumerateManifests(m_embeddedModulesDirs,
                                     m_userModulesDir.empty() ? std::vector<std::string>{}
                                                              : std::vector<std::string>{m_userModulesDir},
                                     {"core"});
    return scansToInstalledPackages(byName);
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledUiPlugins()
{
    auto byName = enumerateManifests(m_embeddedUiPluginsDirs,
                                     m_userUiPluginsDir.empty() ? std::vector<std::string>{}
                                                                : std::vector<std::string>{m_userUiPluginsDir},
                                     {"ui", "ui_qml"});
    return scansToInstalledPackages(byName);
}

std::vector<InstalledPackage> PackageManagerLib::getInstalledPackages()
{
    std::vector<std::string> embeddedDirs = m_embeddedModulesDirs;
    embeddedDirs.insert(embeddedDirs.end(),
                        m_embeddedUiPluginsDirs.begin(), m_embeddedUiPluginsDirs.end());
    std::vector<std::string> userDirs;
    if (!m_userModulesDir.empty())   userDirs.push_back(m_userModulesDir);
    if (!m_userUiPluginsDir.empty()) userDirs.push_back(m_userUiPluginsDir);

    auto byName = enumerateManifests(embeddedDirs, userDirs, {});
    return scansToInstalledPackages(byName);
}

// Helper — enumerate every installed package across all four directory
// categories. Used for cross-category dependency resolution.
static std::map<std::string, ScanEntry> enumerateAllForResolve(
    const std::vector<std::string>& embeddedModulesDirs,
    const std::string& userModulesDir,
    const std::vector<std::string>& embeddedUiPluginsDirs,
    const std::string& userUiPluginsDir)
{
    std::vector<std::string> embedded = embeddedModulesDirs;
    embedded.insert(embedded.end(), embeddedUiPluginsDirs.begin(), embeddedUiPluginsDirs.end());
    std::vector<std::string> users;
    if (!userModulesDir.empty())   users.push_back(userModulesDir);
    if (!userUiPluginsDir.empty()) users.push_back(userUiPluginsDir);
    return enumerateManifests(embedded, users, {});
}

std::optional<DependencyTreeNode> PackageManagerLib::resolveDependencies(const std::string& packageName)
{
    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);

    if (byName.find(packageName) == byName.end())
        return std::nullopt;

    // Recursive walk. Visited-on-path set for cycle detection; the tree is
    // expanded across different branches (diamond shapes) but a cycle
    // through a single branch produces a Cycle leaf to stop descent.
    std::set<std::string> path;
    std::function<DependencyTreeNode(const std::string&)> build = [&](const std::string& name) -> DependencyTreeNode {
        DependencyTreeNode node;
        node.name = name;

        if (path.count(name)) {
            node.status = DependencyStatus::Cycle;
            return node;
        }

        auto it = byName.find(name);
        if (it == byName.end()) {
            node.status = DependencyStatus::NotInstalled;
            return node;
        }

        const auto& scan = it->second;
        node.status      = DependencyStatus::Installed;
        node.version     = scan.version;
        node.installType = scan.installType;

        path.insert(name);
        node.children.reserve(scan.dependencies.size());
        for (const auto& dep : scan.dependencies) {
            node.children.push_back(build(dep));
        }
        path.erase(name);
        return node;
    };

    return build(packageName);
}

std::optional<DependentTreeNode> PackageManagerLib::resolveDependents(const std::string& packageName)
{
    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);

    // Only installed packages get rooted trees — matches resolveDependencies.
    auto rootIt = byName.find(packageName);
    if (rootIt == byName.end())
        return std::nullopt;

    // Reverse adjacency: name → [names that depend on it].
    // Every manifest's dependencies[] entry must reference the canonical
    // manifest.name of its target exactly — no normalization. Packages
    // with incorrect dependency declarations are bugs in those packages.
    std::unordered_map<std::string, std::vector<std::string>> reverseDeps;
    for (const auto& [name, scan] : byName) {
        for (const auto& d : scan.dependencies) {
            reverseDeps[d].push_back(name);
        }
    }

    // Fill a node from its ScanEntry. Reverse edges are synthesised only
    // from installed manifests, so every node reached during the walk is
    // present in byName; no NotInstalled/Cycle status distinction needed.
    auto fillFromScan = [](DependentTreeNode& node, const ScanEntry& scan) {
        node.name        = scan.name;
        node.version     = scan.version;
        node.type        = scan.type;
        node.installType = scan.installType;
        node.installDir  = scan.installDir;
    };

    // Recursive build with on-path cycle detection. A cycle ends the
    // descent silently (node without children) — there's no "Cycle"
    // status to emit because the forward-tree's tri-state status field
    // doesn't apply to reverse trees.
    std::set<std::string> path;
    std::function<DependentTreeNode(const std::string&)> build = [&](const std::string& name) -> DependentTreeNode {
        DependentTreeNode node;
        auto it = byName.find(name);
        if (it != byName.end()) fillFromScan(node, it->second);
        else                    node.name = name;   // unreachable in practice

        if (path.count(name))
            return node;

        auto rit = reverseDeps.find(name);
        if (rit == reverseDeps.end())
            return node;

        path.insert(name);
        node.children.reserve(rit->second.size());
        for (const auto& depender : rit->second) {
            node.children.push_back(build(depender));
        }
        path.erase(name);
        return node;
    };

    return build(packageName);
}

// ---------------------------------------------------------------------------
// DependencyTreeNode::flatten / DependentTreeNode::flatten
// ---------------------------------------------------------------------------
// Both flavours share the same BFS-dedup shape. Kept as two hand-rolled
// copies rather than a template — they walk different node types but are
// each a ~15 line function, and templating would either require putting
// the body in a header or a shared private helper whose generality
// doesn't pay off at two call sites. Children vectors on the returned
// copies are cleared so the flat output is canonical (no hidden
// substructure, no double-counting under iteration).

std::vector<DependencyTreeNode> DependencyTreeNode::flatten() const
{
    std::vector<DependencyTreeNode> out;
    std::unordered_set<std::string> seen;
    std::deque<const DependencyTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependencyTreeNode* n = queue.front();
        queue.pop_front();
        if (!seen.insert(n->name).second) continue;
        DependencyTreeNode copy;
        copy.name        = n->name;
        copy.status      = n->status;
        copy.version     = n->version;
        copy.installType = n->installType;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

std::vector<DependentTreeNode> DependentTreeNode::flatten() const
{
    std::vector<DependentTreeNode> out;
    std::unordered_set<std::string> seen;
    std::deque<const DependentTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependentTreeNode* n = queue.front();
        queue.pop_front();
        if (!seen.insert(n->name).second) continue;
        DependentTreeNode copy;
        copy.name        = n->name;
        copy.version     = n->version;
        copy.type        = n->type;
        copy.installType = n->installType;
        copy.installDir  = n->installDir;
        out.push_back(std::move(copy));
        for (const auto& c : n->children) queue.push_back(&c);
    }
    return out;
}

UninstallResult PackageManagerLib::uninstallPackage(const std::string& packageName)
{
    UninstallResult result;

    auto byName = enumerateAllForResolve(m_embeddedModulesDirs, m_userModulesDir,
                                         m_embeddedUiPluginsDirs, m_userUiPluginsDir);
    auto it = byName.find(packageName);
    if (it == byName.end()) {
        result.errorMsg = "Package not found: " + packageName;
        return result;
    }

    const auto& scan = it->second;
    if (scan.installType == InstallType::Embedded) {
        result.errorMsg = "Cannot uninstall embedded package: " + packageName;
        return result;
    }
    if (scan.installDir.empty()) {
        result.errorMsg = "Package has no install directory: " + packageName;
        return result;
    }

    std::error_code ec;
    // Guard against anything weird happening with the resolved path: require
    // that the directory lives under one of the configured user directories.
    fs::path toDelete = fs::absolute(scan.installDir, ec);
    if (ec || !fs::exists(toDelete, ec)) {
        result.errorMsg = "Install directory missing: " + scan.installDir;
        return result;
    }
    bool insideUserDir = false;
    for (const auto* d : { &m_userModulesDir, &m_userUiPluginsDir }) {
        if (d->empty()) continue;
        std::error_code pec;
        fs::path userRoot = fs::absolute(*d, pec);
        if (pec) continue;
        auto rel = fs::relative(toDelete, userRoot, pec);
        if (pec) continue;
        std::string rs = rel.string();
        if (!rs.empty() && rs.rfind("..", 0) != 0) {
            insideUserDir = true;
            break;
        }
    }
    if (!insideUserDir) {
        result.errorMsg = "Refusing to uninstall path outside user directories: " + toDelete.string();
        return result;
    }

    std::uintmax_t removed = fs::remove_all(toDelete, ec);
    if (ec) {
        result.errorMsg = "Failed to remove install directory: " + ec.message();
        return result;
    }

    result.success = true;
    result.removedFiles.push_back(toDelete.string());
    (void)removed;
    return result;
}

std::string PackageManagerLib::currentPlatformVariant()
{
#if defined(__APPLE__)
    #if defined(__aarch64__)
        return "darwin-arm64";
    #else
        return "darwin-x86_64";
    #endif
#elif defined(__linux__)
    #if defined(__x86_64__)
        return "linux-x86_64";
    #elif defined(__aarch64__)
        return "linux-arm64";
    #else
        return "linux-x86";
    #endif
#elif defined(_WIN32)
    #if defined(_M_X64) || defined(__x86_64__)
        return "windows-x86_64";
    #else
        return "windows-x86";
    #endif
#else
    return "unknown";
#endif
}

std::vector<std::string> PackageManagerLib::platformVariantsToTry()
{
    std::string primary = currentPlatformVariant();
    std::vector<std::string> variants;
    variants.push_back(primary);

    if (primary == "linux-x86_64") {
        variants.push_back("linux-amd64");
    } else if (primary == "linux-amd64") {
        variants.push_back("linux-x86_64");
    } else if (primary == "linux-arm64") {
        variants.push_back("linux-aarch64");
    } else if (primary == "linux-aarch64") {
        variants.push_back("linux-arm64");
    }

#ifndef LGPM_PORTABLE_BUILD
    std::vector<std::string> devVariants;
    for (const auto& variant : variants) {
        devVariants.push_back(variant + "-dev");
    }
    variants = devVariants;
#endif

    return variants;
}

bool PackageManagerLib::isValidModuleName(const std::string& name)
{
    // A module name becomes a single directory component under the install
    // directory, so anything that isn't a plain leaf segment is rejected. This
    // blocks the path-traversal vectors that std::filesystem::operator/ would
    // otherwise honor: ".." escapes upward, an absolute path resets the join,
    // and embedded separators create nested or sibling directories.
    if (name.empty() || name == "." || name == "..")
        return false;

    for (char c : name) {
        // '/' and '\\' are path separators; ':' is a Windows drive separator
        // ("C:foo" is drive-relative); '\0' truncates the path at the C ABI.
        if (c == '/' || c == '\\' || c == ':' || c == '\0')
            return false;
    }

    return true;
}

bool PackageManagerLib::copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg)
{
    if (!fs::is_directory(srcDir)) {
        errorMsg = "Source directory does not exist: " + srcDir;
        return false;
    }

    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec) {
        errorMsg = "Failed to create destination directory: " + destDir;
        return false;
    }

    std::error_code iterEc;
    for (const auto& entry : fs::directory_iterator(srcDir, fs::directory_options::skip_permission_denied, iterEc)) {
        if (iterEc) {
            errorMsg = "Failed to iterate source directory: " + srcDir;
            return false;
        }
        fs::path destPath = fs::path(destDir) / entry.path().filename();

        std::error_code statusEc;
        if (entry.is_directory(statusEc) && !statusEc) {
            if (!copyDirectoryContents(entry.path().string(), destPath.string(), errorMsg)) {
                return false;
            }
        } else if (!statusEc) {
            if (fs::exists(destPath, statusEc) && !statusEc) {
                fs::remove(destPath, ec);
                if (ec) {
                    errorMsg = "Failed to remove existing file: " + destPath.string();
                    return false;
                }
            }
            fs::copy_file(entry.path(), destPath, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                errorMsg = "Failed to copy file from " + entry.path().string() + " to " + destPath.string();
                return false;
            }
        }
    }

    return true;
}

bool PackageManagerLib::extractLgxPackage(const std::string& lgxPath, const std::string& outputDir, std::string& errorMsg)
{
    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        errorMsg = std::string("Failed to load LGX package: ") + lgx_get_last_error();
        return false;
    }

    auto variants = platformVariantsToTry();
    std::string matchedVariant;

    for (const auto& v : variants) {
        if (lgx_has_variant(pkg, v.c_str())) {
            matchedVariant = v;
            break;
        }
    }

    if (matchedVariant.empty()) {
        // Collect the variants the package actually provides so the failure is
        // self-explanatory. The common cause is a dev/portable mismatch: a
        // package built for "<host>-dev" installed by a portable build that only
        // accepts "<host>" (or vice versa). Naming both lists turns a silent
        // "Package does not contain variant" into an actionable line.
        std::vector<std::string> available;
        const char** pkgVariants = lgx_get_variants(pkg);
        if (pkgVariants) {
            for (size_t i = 0; pkgVariants[i] != nullptr; ++i)
                available.push_back(pkgVariants[i]);
            lgx_free_string_array(pkgVariants);
        }
        std::string availableList = available.empty() ? "none" : joinStrings(available, ", ");
        std::cerr << "Warning: package '" << lgxPath
                  << "' has no variant matching this platform: tried ["
                  << joinStrings(variants, ", ") << "], package provides ["
                  << availableList << "].\n";
        errorMsg = "Package does not contain variant for platform: " + variants.front()
                 + " (package provides: " + availableList + ")";
        lgx_free_package(pkg);
        return false;
    }

    lgx_result_t result = lgx_extract(pkg, matchedVariant.c_str(), outputDir.c_str());

    if (!result.success) {
        errorMsg = std::string("Failed to extract variant: ") + (result.error ? result.error : "unknown error");
        lgx_free_package(pkg);
        return false;
    }

    // Write the full root manifest.json from the LGX package into the extracted variant directory
    fs::path variantOutputDir = fs::path(outputDir) / matchedVariant;
    fs::path manifestPath = variantOutputDir / "manifest.json";

    const char* manifestJson = lgx_get_manifest_json(pkg);
    if (!manifestJson) {
        errorMsg = std::string("Failed to get manifest JSON from LGX package: ") + lgx_get_last_error();
        lgx_free_package(pkg);
        return false;
    }

    {
        std::ofstream mf(manifestPath);
        if (!mf.is_open()) {
            errorMsg = "Failed to write manifest.json to: " + manifestPath.string();
            lgx_free_package(pkg);
            return false;
        }
        mf << manifestJson;
        if (!mf.good()) {
            errorMsg = "Failed to write data to manifest.json at: " + manifestPath.string();
            lgx_free_package(pkg);
            return false;
        }
    }

    // Write a "variant" text file containing the installed variant name
    {
        fs::path variantFilePath = variantOutputDir / "variant";
        std::ofstream vf(variantFilePath);
        if (vf.is_open()) {
            vf << matchedVariant;
        }
    }

    lgx_free_package(pkg);
    return true;
}

void PackageManagerLib::setSignaturePolicy(SignaturePolicy policy)
{
    m_signaturePolicy = policy;
}

void PackageManagerLib::setKeyringDirectory(const std::string& dir)
{
    m_keyringDir = dir;
}

SignatureVerificationResult PackageManagerLib::verifyPackageSignature(const std::string& lgxPath)
{
    SignatureVerificationResult result;
    const char* krDir = m_keyringDir.empty() ? nullptr : m_keyringDir.c_str();
    lgx_signature_info_t info = lgx_verify_signature(lgxPath.c_str(), krDir);

    result.is_signed = info.is_signed;
    result.signature_valid = info.signature_valid;
    result.package_valid = info.package_valid;
    if (info.signer_did) result.signer_did = info.signer_did;
    if (info.signer_name) result.signer_name = info.signer_name;
    if (info.signer_url) result.signer_url = info.signer_url;
    if (info.trusted_as) result.trusted_as = info.trusted_as;
    if (info.error) result.error = info.error;

    lgx_free_signature_info(info);
    return result;
}

bool PackageManagerLib::copyLibraryFromExtracted(const std::string& extractedDir, const std::string& targetDir,
                                                  bool /* isCoreModule */, std::string& outModuleName, std::string& errorMsg)
{
    auto variants = platformVariantsToTry();
    std::string variantDir;

    for (const auto& v : variants) {
        fs::path candidate = fs::path(extractedDir) / v;
        if (fs::is_directory(candidate)) {
            variantDir = candidate.string();
            break;
        }
    }

    if (variantDir.empty()) {
        std::string variantList;
        for (size_t i = 0; i < variants.size(); ++i) {
            if (i > 0) variantList += ", ";
            variantList += variants[i];
        }
        errorMsg = "Extracted variant directory not found for: " + variantList;
        return false;
    }

    // Determine the module name from manifest.json "name" field
    std::string moduleName;
    fs::path manifestPath = fs::path(variantDir) / "manifest.json";
    if (fs::exists(manifestPath)) {
        std::ifstream mf(manifestPath);
        if (mf.is_open()) {
            try {
                json doc = json::parse(mf);
                moduleName = doc.value("name", "");
            } catch (...) {}
        }
    }

    // Fall back to the first library file's base name if manifest name is unavailable
    if (moduleName.empty()) {
        std::vector<std::string> extensions;
#if defined(__APPLE__)
        extensions.push_back(".dylib");
#elif defined(_WIN32)
        extensions.push_back(".dll");
#else
        extensions.push_back(".so");
#endif
        for (const auto& entry : fs::directory_iterator(variantDir)) {
            if (!entry.is_regular_file())
                continue;
            for (const auto& ext : extensions) {
                if (entry.path().extension() == ext) {
                    moduleName = entry.path().stem().string();
                    break;
                }
            }
            if (!moduleName.empty())
                break;
        }
        if (moduleName.empty()) {
            errorMsg = "No library files found and no name in manifest for: " + variantDir;
            return false;
        }
    }

    // The module name (whether from the manifest or the library-file fallback)
    // is about to become a directory component under targetDir. Reject anything
    // that isn't a single plain segment before the join, or a crafted manifest
    // "name" like "../../x" or "/abs" would let the copy escape targetDir and
    // plant files (e.g. an auto-loaded plugin) anywhere on disk — F-006.
    if (!isValidModuleName(moduleName)) {
        errorMsg = "Invalid module name in manifest: " + moduleName;
        return false;
    }

    outModuleName = moduleName;
    std::string moduleSubDir = (fs::path(targetDir) / moduleName).string();

    // Defense in depth: even with the name validated, confirm the resolved
    // destination is lexically contained within targetDir before any write
    // happens. weakly_canonical resolves "."/".." without requiring the path to
    // exist, so this runs ahead of copyDirectoryContents.
    {
        fs::path canonicalTarget = fs::weakly_canonical(fs::path(targetDir));
        fs::path canonicalSub = fs::weakly_canonical(fs::path(moduleSubDir));
        auto rel = fs::relative(canonicalSub, canonicalTarget);
        if (rel.empty() || rel.native().rfind("..", 0) == 0) {
            errorMsg = "Resolved install path escapes target directory: " + moduleSubDir;
            return false;
        }
    }

    if (!copyDirectoryContents(variantDir, moduleSubDir, errorMsg)) {
        return false;
    }

    return true;
}
