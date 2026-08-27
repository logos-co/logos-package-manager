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

// Windows refuses to delete a file carrying FILE_ATTRIBUTE_READONLY. POSIX
// consults only the PARENT DIRECTORY's write bit, so a payload file shipped
// mode 0444 removes fine on Linux and macOS and denies on Windows -- which is
// why this was invisible until a package was installed there.
//
// Everything in the Nix store is 0444, and nix-bundle-lgx copies a module's
// `icon:` straight out of it, so every .lgx with an icon carries at least one
// read-only file. Measured on the extracted payload: the root `hello.svg` was
// "ReadOnly, Archive" while both DLLs, the manifests, the qmldir and even
// `icons/hello.svg` were plain "Archive".
//
// `fs::permissions` is the portable spelling of "clear the read-only bit" --
// deliberately NOT <windows.h>, whose `interface` macro and TokenSource
// enumerator have twice broken unrelated files in this codebase.
static void clearReadOnlyRecursive(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    fs::permissions(root, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
        ec.clear();   // one stubborn entry must not abandon the rest
    }
}

// Remove a tree without ever throwing. The THROWING overload of remove_all is
// what turned a failed temp-directory cleanup into an uncaught
// std::filesystem_error: in the lgpm CLI it reached terminate(), and inside the
// package_manager module it unwound out of the call so the install reply was
// never sent -- the files were all on disk and the UI still showed "Retry".
//
// A temp directory that will not delete is untidy, never a reason to fail an
// install that has already succeeded.
static bool removeTreeQuietly(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (!ec) return true;
    clearReadOnlyRecursive(p);          // second attempt, attributes cleared
    ec.clear();
    fs::remove_all(p, ec);
    if (ec) {
        std::cerr << "Warning: could not remove " << p.string() << ": " << ec.message()
                  << " (leaving it behind; this does not affect the install)" << std::endl;
        return false;
    }
    return true;
}

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
        case DependencyStatus::Installed:       return "installed";
        case DependencyStatus::NotInstalled:    return "not_installed";
        case DependencyStatus::Cycle:           return "cycle";
        case DependencyStatus::VersionMismatch: return "version_mismatch";
        case DependencyStatus::SignerMismatch:  return "signer_mismatch";
        case DependencyStatus::SignerUnknown:   return "signer_unknown";
    }
    return "";
}

namespace {

// How harshly an EDGE judged the package it points at. Higher wins, and the
// ranking is the single authority on two questions that must agree: which
// constraint a node reports when one edge fails more than one of them
// (resolveDependencies), and which edge's verdict survives the flat list's
// name dedup (DependencyTreeNode::flatten).
//
//   Installed(0)       no constraint failed
//   SignerUnknown(1)   a pin could not be checked — missing evidence
//   VersionMismatch(2) a range definitely failed
//   SignerMismatch(3)  identity definitely failed: not this package at all
//
// Missing evidence ranks BELOW a definite failure, so "we could not tell who
// published this" never masks a range rejection the user can actually act on.
// Identity ranks above a range because a range means nothing until you know
// which package you are ranging over — "requires ^2.0.0, found 1.0.0" sends
// somebody hunting for a newer build of a package that is not theirs at any
// version.
//
// NotInstalled and Cycle score -1: they are properties of the package or of
// the graph rather than verdicts an edge reached, so they neither promote nor
// are promoted. Absence outranks everything, and it is enforced by returning
// early before any of this runs.
int edgeVerdictSeverity(DependencyStatus s)
{
    switch (s) {
        case DependencyStatus::Installed:       return 0;
        case DependencyStatus::SignerUnknown:   return 1;
        case DependencyStatus::VersionMismatch: return 2;
        case DependencyStatus::SignerMismatch:  return 3;
        case DependencyStatus::NotInstalled:
        case DependencyStatus::Cycle:           return -1;
    }
    return -1;
}

} // namespace

bool nodeResolvedToAnInstalledPackage(DependencyStatus s) {
    // Named by exclusion here on purpose, and it is the one place that is
    // right: the question is "is anything on disk", and exactly two statuses
    // mean there is not. A new edge-decided status is by definition about a
    // package that IS installed, so it should be admitted by default — the
    // opposite of the classify-by-naming rule that applies when the question
    // is "does this block".
    return s != DependencyStatus::NotInstalled && s != DependencyStatus::Cycle;
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
                                    // Read the type off the manifest we just parsed. This used to
                                    // be hardcoded false, so a skipped CORE install reported
                                    // isCoreModule=false and package_manager_module emitted
                                    // uiPluginFileInstalled for a core module — Basecamp then
                                    // rescanned UI plugins instead of refreshing core modules.
                                    if (isCoreModuleOut) *isCoreModuleOut = (doc.value("type", "") == "core");
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

    // ── The trust-anchor gate: may this package be installed at all? ────────
    //
    // This is AUTHORIZATION, and the ACTIVE ANCHOR SET is the only thing that
    // grants it. The anchor set is the local keyring alone (m_keyringDir,
    // managed by `lgx keyring` and addTrustedKey/removeTrustedKey), and nothing
    // enters it except by an explicit user act. A signer DID carried in the
    // package, advertised by a catalog entry, listed in a repository's
    // `trustedSigners`, or arriving with a downloaded key is a SELF-ASSERTION
    // and establishes no anchor — see logos-package-downloader's
    // Repository::trustedSignerDids, which is parsed and deliberately never
    // consulted.
    //
    // Do not confuse this with a dependency's `signer` pin. That pin
    // DISAMBIGUATES among same-named candidates — is this the same package? —
    // and satisfying it authorises nothing. It is checked by the catalog
    // resolver (logos-package-downloader, PackageDownloaderLib::
    // signerPinMatches) when choosing what to fetch, and against the installed
    // set by resolveDependencies in this file. It never reaches this function.
    // Both checks are needed; neither substitutes for the other.
    //
    // This block decides ONLY whether the install may proceed. It used to also
    // establish the installed package's publisher identity, on the reasoning
    // that the .lgx is deleted after extraction so this is the last moment
    // anybody knows who signed it. That reasoning was wrong in its premise:
    // the signature is a FILE, and extractLgxPackage now carries it into the
    // install tree beside the bytes it covers, so the question can be asked
    // again at any time by anyone, against a key of their own choosing.
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
        if (m_signaturePolicy == SignaturePolicy::WARN) {
            // Every negative outcome that WARN nevertheless installs gets a
            // line. This used to warn ONLY about the unsigned case, so a
            // package carrying a valid signature from a key NO ACTIVE ANCHOR
            // VALIDATES installed in total silence — quieter than an unsigned
            // one, i.e. the worse posture produced less noise than the better.
            // At WARN the anchor set changes the DIAGNOSTIC; at REQUIRE it
            // changes the DECISION (above). That asymmetry is the point of
            // having two levels, and it only works if both levels speak.
            if (!sigResult.is_signed) {
                std::cerr << "Warning: Package is unsigned: " << pluginPath << "\n";
            } else if (sigResult.trusted_as.empty()) {
                // Name the DID: it is the one actionable thing here — what the
                // user would hand to `lgx keyring add` if they decided to
                // trust this publisher. signer_name/signer_url are the
                // package's own claims about itself and are reported as such,
                // never as corroboration.
                std::cerr << "Warning: Package is signed by a key no trusted anchor validates: "
                          << pluginPath << "\n"
                          << "  signer DID: "
                          << (sigResult.signer_did.empty() ? "(none reported)" : sigResult.signer_did)
                          << "\n";
                if (!sigResult.signer_name.empty()) {
                    std::cerr << "  signer claims to be: " << sigResult.signer_name
                              << " (self-asserted, not verified)\n";
                }
                std::cerr << "  checked against keyring: "
                          << (m_keyringDir.empty() ? "(default)" : m_keyringDir) << "\n";
            }
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
        removeTreeQuietly(tempDir);
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
        removeTreeQuietly(tempDir);
        return {};
    }

    // Create install directory if it doesn't exist
    std::error_code ec;
    fs::create_directories(installDir, ec);
    if (ec) {
        errorMsg = "Failed to create install directory: " + installDir;
        removeTreeQuietly(tempDir);
        return {};
    }

    std::string installedModuleName;
    if (!copyLibraryFromExtracted(tempDir, installDir, isCoreModule, installedModuleName, errorMsg)) {
        removeTreeQuietly(tempDir);
        return {};
    }

    // Determine the path we report for what was just installed. This is the
    // main file when the package ships one and the module directory otherwise;
    // it is never empty here, because copyLibraryFromExtracted() has just
    // created that directory.
    if (installedPluginPath) {
        *installedPluginPath = resolveInstalledPackagePath(
            (fs::path(installDir) / installedModuleName).string(),
            platformVariantsToTry());
    }

    removeTreeQuietly(tempDir);
    return installDir;
}

std::string PackageManagerLib::resolveInstalledPackagePath(const std::string& moduleDir,
                                                           const std::vector<std::string>& variants)
{
    fs::path dir(moduleDir);
    const std::string moduleName = dir.filename().string();

    std::string mainFile;
    bool isQmlPackage = false;

    fs::path manifestPath = dir / "manifest.json";
    if (fs::exists(manifestPath)) {
        std::ifstream mf(manifestPath);
        if (mf.is_open()) {
            try {
                json doc = json::parse(mf);
                isQmlPackage = (doc.value("type", "") == "ui_qml");
                if (doc.contains("main")) {
                    if (doc["main"].is_object()) {
                        for (const auto& v : variants) {
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

    // Only non-ui_qml packages get a main file synthesised from the module
    // name: a QML-only ui_qml package has no backend library at all, and
    // guessing "<name>.so" for it would just miss.
    if (mainFile.empty() && !isQmlPackage)
        mainFile = moduleName;

    if (!mainFile.empty()) {
        if (!isQmlPackage && mainFile.find('.') == std::string::npos) {
            // Windows first: this chain is the shape that has repeatedly let a
            // Windows build fall through to the Unix branch.
#if defined(_WIN32)
            mainFile += ".dll";
#elif defined(__APPLE__)
            mainFile += ".dylib";
#else
            mainFile += ".so";
#endif
        }
        fs::path mainPath = dir / mainFile;
        if (fs::exists(mainPath))
            return mainPath.string();

        // The manifest named a main file that is not in the payload. The copy
        // itself succeeded, so this is a packaging defect rather than an
        // install failure — report the directory (below) but say so, instead
        // of silently handing back an empty path that reads as "install
        // failed". ui_qml is excluded: for those the name is only a guess.
        if (!isQmlPackage) {
            std::cerr << "Warning: package '" << moduleName << "' has no main file at "
                      << mainPath.string() << " after install; the package declares '"
                      << mainFile << "' but it is not in the payload. Reporting the "
                      << "module directory instead.\n";
        }
    }

    std::error_code ec;
    if (fs::is_directory(dir, ec) && !ec)
        return dir.string();
    return {};
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
    // Declared dependency entries, string and object form alike (see
    // PackageDependency). The graph walks `.name`; the constraints ride along
    // for consumers that evaluate them.
    std::vector<PackageDependency> dependencies;
    // The DID the installed manifest.sig names, once that signature has been
    // checked against the key the DID itself carries — see
    // InstalledPackage::signerDid. For display. nullopt = no usable signature.
    std::optional<std::string> signerDid;
    // The raw material a signer PIN is judged against, kept verbatim because
    // that is the only form in which it means anything.
    //
    // manifestBytes is the file as it sits on disk, byte for byte — NOT
    // scan.manifest re-serialised. An Ed25519 signature covers exact bytes,
    // and nlohmann round-tripping is not byte-preserving (key order, spacing,
    // number formatting), so a re-dump would fail to verify a perfectly good
    // signature and every pin in the fleet would read as a mismatch.
    std::string manifestBytes;
    std::optional<std::string> manifestSigJson;   // nullopt = no manifest.sig
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

// Read a whole file as bytes. nullopt when it is not there or cannot be read;
// an EMPTY file reads as an empty string, which is a different answer and the
// callers below rely on the distinction.
static std::optional<std::string> readFileBytes(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    if (f.bad())
        return std::nullopt;
    return buf.str();
}

// The DID an installed signature names, reported only if that signature really
// verifies under the key that DID carries.
//
// This is a SELF-check and it is labelled one on purpose. did:jwk embeds its
// own public key, so the document supplies both the claim and the means to
// check it, and the two agree whenever a single author wrote both. What it
// therefore proves is narrow but real: somebody holding SOME key produced a
// genuine Ed25519 signature over exactly these manifest bytes. That is enough
// to print, and it is not plantable — a package cannot ship this without a
// private key, which is the property the old sidecar had to be defended into
// having.
//
// What it CANNOT do is settle identity, and no amount of care here would make
// it able to. Comparing this value to a pin would compare the pin against a
// string the package chose, checked against a key the package also chose. See
// resolveDependencies.
static std::optional<std::string> selfAssertedSignerDid(
    const std::string& manifestBytes,
    const std::optional<std::string>& manifestSigJson)
{
    if (!manifestSigJson)
        return std::nullopt;

    std::string did;
    try {
        json sig = json::parse(*manifestSigJson);
        if (!sig.is_object() || !sig.contains("did") || !sig["did"].is_string())
            return std::nullopt;
        did = sig["did"].get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
    if (did.empty())
        return std::nullopt;

    if (lgx_check_manifest_signature(manifestBytes.data(), manifestBytes.size(),
                                     manifestSigJson->c_str(), did.c_str())
        != LGX_SIG_CHECK_OK) {
        return std::nullopt;
    }
    return did;
}

// Read one line out of a sidecar file inside an installed package directory,
// trimmed. Returns empty when the file is absent or holds nothing.
static std::string readSidecarLine(const fs::path& moduleDir, const char* fileName)
{
    std::ifstream f(moduleDir / fileName);
    if (!f.is_open())
        return {};
    std::string line;
    std::getline(f, line);
    // Trim trailing whitespace / CR so the comparison is exact.
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' ||
            line.back() == ' '  || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

// Reads the single-line `variant` file that installPluginFile writes into
// every installed module directory, recording which variant was physically
// extracted there. Returns empty if the file is absent (e.g. embedded or
// legacy installs) — callers must treat that as "unknown", not a mismatch.
static std::string readInstalledVariant(const fs::path& moduleDir)
{
    return readSidecarLine(moduleDir, "variant");
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
            // Slurped rather than streamed into the parser because the RAW
            // bytes are load-bearing here: they are the signed message. Parsing
            // from the string leaves both views available and costs one copy of
            // a file we were reading anyway.
            std::optional<std::string> manifestBytes = readFileBytes(manifestPath);
            if (!manifestBytes)
                continue;

            json manifest;
            try {
                manifest = json::parse(*manifestBytes);
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
            // The package's own signature over the manifest just read, if it
            // shipped one. Both halves are kept verbatim: the pin check needs
            // the exact bytes and the exact signature, and neither survives a
            // round trip through a parser.
            scan.manifestBytes   = std::move(*manifestBytes);
            scan.manifestSigJson = readFileBytes(entry.path() / "manifest.sig");
            // The display value, and the ONLY thing that is decided here.
            // Checking the signature against the key its own DID carries proves
            // a real key signed these exact bytes — enough to say "signed by
            // <did>" honestly, and deliberately not enough to say the package
            // is who it claims. WHICH identity is right is a question only an
            // outside reference can answer, and the pin block in
            // resolveDependencies answers it there, with the pin's own key.
            scan.signerDid = selfAssertedSignerDid(scan.manifestBytes, scan.manifestSigJson);

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

            // A `dependencies[]` entry is either a plain string or an object
            // carrying the same name plus the constraints an installer resolves
            // by (see the LGX spec's "Dependency entries", and lgx::Dependency
            // in logos-package). Both forms declare THE SAME EDGE.
            //
            // This used to accept only `d.is_string()` with no else branch, so
            // an object entry was skipped entirely — the graph lost the edge,
            // not merely its constraint. resolveDependencies under-reported,
            // resolveDependents under-reported (so an uninstall never warned
            // about a dependent that declared its need in object form), and
            // basecamp's missing-dependency marker went green on a module whose
            // dependency was not installed at all. No manifest in the workspace
            // uses the object form today, which is exactly why it went unseen.
            if (manifest.contains("dependencies") && manifest["dependencies"].is_array()) {
                for (const auto& d : manifest["dependencies"]) {
                    PackageDependency dep;
                    if (d.is_string()) {
                        dep.name = d.get<std::string>();
                    } else if (d.is_object() && d.contains("name") && d["name"].is_string()) {
                        dep.name = d["name"].get<std::string>();
                        if (d.contains("version") && d["version"].is_string())
                            dep.version = d["version"].get<std::string>();
                        if (d.contains("signer") && d["signer"].is_string())
                            dep.signer = d["signer"].get<std::string>();
                    }
                    // Neither form, or a name that is present but empty: the
                    // entry declares no edge. SAY SO — dropping it silently is
                    // the exact failure this loop was just fixed for, one notch
                    // down. Without this line a hand-edited or build-time
                    // embedded manifest can lose a dependency with no
                    // diagnostic anywhere in the system, and the only symptom
                    // is an uninstall that fails to warn, or a basecamp row
                    // that shows green while its dependency is absent.
                    //
                    // Severity is low BECAUSE the `lgpm install` path cannot
                    // reach it — logos-package rejects both shapes ahead of us:
                    // Manifest::fromJson returns nullopt on a non-string /
                    // non-object entry or an object without a string `name`
                    // (so Package::load fails outright), and Manifest::validate
                    // adds "Dependency with empty name" for the rest. What is
                    // NOT covered is every manifest that bypasses that path:
                    // an embedded install written at build time, and anything
                    // edited in place afterwards. Those are exactly the cases
                    // nobody is watching, which is why this warns rather than
                    // trusting the upstream gate.
                    if (dep.name.empty()) {
                        std::cerr << "Warning: package '" << name << "' in "
                                  << entry.path().string()
                                  << " has a malformed dependencies[] entry " << d.dump()
                                  << " (expected a non-empty name string, or an object with a "
                                  << "string 'name'); the dependency edge is ignored.\n";
                        continue;
                    }
                    scan.dependencies.push_back(std::move(dep));
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
    p.signerDid = scan.signerDid;
    // Split the scanned entries into the name list every consumer already
    // reads and the constrained subset. Built here, in one place, so the two
    // views cannot drift.
    p.dependencies.reserve(scan.dependencies.size());
    for (const auto& d : scan.dependencies) {
        p.dependencies.push_back(d.name);
        if (!d.isSimple())
            p.dependencyConstraints.push_back(d);
    }

    const json& m = scan.manifest;
    p.displayName = m.value("display_name", "");
    p.description = m.value("description", "");
    p.category    = m.value("category", "");
    p.author      = m.value("author", "");
    p.license     = m.value("license", "");
    p.icon        = m.value("icon", "");
    p.view        = m.value("view", "");
    p.manifestVersion = m.value("manifestVersion", "");

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
    //
    // Takes the whole PackageDependency rather than the name alone: the range
    // and the signer live on the EDGE, not on the package, so they have to
    // travel with the recursion to be evaluated at the node they constrain.
    // Passing only `dep.name` here is what made the constraint unreachable.
    std::set<std::string> path;
    std::function<DependencyTreeNode(const PackageDependency&)> build =
        [&](const PackageDependency& dep) -> DependencyTreeNode {
        DependencyTreeNode node;
        node.name = dep.name;
        // Recorded before any early return, so a constraint is visible even on
        // an edge whose status is decided without consulting it.
        node.requiredVersion = dep.version;
        node.requiredSigner  = dep.signer;

        if (path.count(dep.name)) {
            node.status = DependencyStatus::Cycle;
            return node;
        }

        auto it = byName.find(dep.name);
        if (it == byName.end()) {
            // ABSENCE OUTRANKS MISMATCH, deliberately. A range can only be
            // judged against a version we actually have, and "install it" is
            // the action either way — reporting version_mismatch for a package
            // that is not there would name the weaker fact and point the user
            // at the wrong fix. The declared range still rides along on
            // `requiredVersion` so a caller can say which version to install.
            node.status = DependencyStatus::NotInstalled;
            return node;
        }

        const auto& scan = it->second;
        node.version        = scan.version;
        node.installType    = scan.installType;
        node.signerDid      = scan.signerDid;
        // An absent range means the parent declared no constraint and anything
        // installed satisfies it. A range that does not PARSE is treated as
        // unsatisfied rather than ignored: silently dropping a typo'd range
        // would fail open, and `lgx verify` already rejects the syntax upstream
        // (logos::semver::valid_range), so reaching here with one means the
        // manifest bypassed that gate and deserves to be visible.
        node.status = (!dep.version || logos::semver::satisfies(scan.version, *dep.version))
                          ? DependencyStatus::Installed
                          : DependencyStatus::VersionMismatch;

        // ── The signer pin: IS THIS THE SAME PACKAGE? ──────────────────────
        //
        // Evaluated when and ONLY when the edge carries a pin. An unpinned
        // edge does not care who published its dependency and must keep
        // resolving byte-identically to before — which is every edge in the
        // fleet today.
        //
        // THE PIN SUPPLIES THE KEY. This is the whole mechanism, and it is the
        // one thing here that is fatal to get backwards, so it is spelled out:
        //
        //   1. take the PINNED did and extract its Ed25519 public key
        //      (did:jwk embeds it, so no keyring is involved and none is
        //      needed — the keyring answers a different question, whether a
        //      key is ANCHORED, and it is asked at install time)
        //   2. verify the installed manifest.sig over the installed
        //      manifest.json bytes WITH THAT KEY
        //   3. verifies => the pinned key signed this package => same package
        //      fails    => it did not                         => SignerMismatch
        //
        // The DID written inside the installed manifest.sig is NOT consulted.
        // Reading it, comparing it to the pin, and then verifying with that
        // same DID's key would prove only that the file agrees with itself,
        // which any attacker can arrange: swap the signature and swap the DID
        // beside it, sign with a key you own, and every check passes. Making
        // the key come from the pin means forging a match requires an Ed25519
        // signature under a key the attacker does not have.
        //
        // What this replaced was a `signer` sidecar the installer wrote,
        // recording the DID it had verified — an ASSERTION, trusted because
        // the installer made it. Three separate forgeries worked against it
        // (a package shipping its own record, since variants/<v>/ is copied
        // wholesale; a record outliving its package, since the copy merges;
        // and a read-only plant that Windows would not let the installer
        // clear), and each needed its own defence. None of them survives here,
        // and not because anything defends against them: a planted, inherited
        // or unclearable manifest.sig is simply a file that does not verify
        // under the pinned key. The signature also binds the PAYLOAD, not just
        // the name — manifest.json carries the Merkle root over the package
        // contents — so a stale signature cannot describe different bytes.
        //
        // WHAT THIS DELIBERATELY DOES NOT DO: re-hash the installed payload.
        //
        // The signed manifest carries a Merkle tree over the package contents
        // (hashes.root, hashes.variants, hashes["variants/<v>"]), so verifying
        // the signature transitively establishes what the payload SHOULD hash
        // to, and re-hashing what is on disk would turn this into a tamper
        // check as well as an identity check. That is genuinely newly
        // possible, and it still does not belong here, for two reasons.
        //
        // COST. Checking the signature is ~450us and is CONSTANT: the message
        // is a ~600-byte manifest whatever the package weighs. Re-hashing is
        // linear in the payload — SHA-256 runs at ~1 GB/s on this hardware, so
        // ~2.4ms for a 2.6 MB module and ~47ms for a 50 MB one, before any
        // file I/O. resolveDependencies walks every edge and basecamp calls it
        // on refresh, so that is a per-frame cost that grows with what the
        // user has installed.
        //
        // CORRECTNESS, which is the stronger reason. The tree is computed over
        // TAR ENTRY PATHS and the install tree is FLATTENED and ADDED TO:
        // `variants/<v>/x.so` becomes `<name>/x.so`, `assets/` is merged into
        // the same directory, and extractLgxPackage synthesises manifest.json,
        // manifest.sig and `variant` there. Measured on a real package, the
        // leaf hash covers ONE file while the install directory holds four.
        // Re-deriving it means re-prefixing paths and excluding exactly the
        // files install invented — a mapping that is easy to get subtly wrong
        // and whose failure mode is a false accusation of tampering.
        //
        // So: a separate, deliberately-tested verb (`lgpm verify <package>`),
        // run on demand, not an implicit cost on a resolve that today answers
        // a question about identity.
        //
        // Combined with the range verdict through edgeVerdictSeverity rather
        // than by assignment, because ONE EDGE can fail both constraints and
        // the node reports one status. A signer mismatch outranks a version
        // mismatch ("requires ^2.0.0, found 1.0.0" sends the user after a
        // newer build of a package that is not theirs at any version), while a
        // signer we merely could not CHECK ranks below one — missing evidence
        // must never mask an actionable rejection. Absence outranks all of it
        // and has already returned above.
        //
        // Taking the max also means a satisfied pin never IMPROVES a status:
        // the right publisher at the wrong version is still the wrong version.
        if (dep.signer) {
            DependencyStatus signerVerdict = DependencyStatus::Installed;
            if (!scan.manifestSigJson) {
                // No signature installed at all. Nothing was proved and
                // nothing disproved — see UnknownSignerPolicy for why the
                // default does not block.
                signerVerdict = (m_unknownSignerPolicy == UnknownSignerPolicy::Strict)
                                    ? DependencyStatus::SignerMismatch
                                    : DependencyStatus::SignerUnknown;
            } else {
                switch (lgx_check_manifest_signature(
                            scan.manifestBytes.data(), scan.manifestBytes.size(),
                            scan.manifestSigJson->c_str(), dep.signer->c_str())) {
                case LGX_SIG_CHECK_OK:
                    break;                       // the pinned key signed this
                case LGX_SIG_CHECK_MISMATCH:
                    signerVerdict = DependencyStatus::SignerMismatch;
                    break;
                case LGX_SIG_CHECK_BAD_DID:
                    // The PIN is not a did:jwk. A pin nobody can parse must
                    // never read as satisfied, and it must not read as
                    // "unknown" either: unknown is tolerated by the default
                    // policy, so a typo'd pin would be waved through — the
                    // exact fail-open this mechanism exists to prevent. The
                    // fault is in the depending manifest, so say which.
                    std::cerr << "Warning: dependency '" << dep.name << "' is pinned to '"
                              << *dep.signer << "', which is not a did:jwk carrying an "
                              << "Ed25519 key; the pin cannot be satisfied by anything.\n";
                    signerVerdict = DependencyStatus::SignerMismatch;
                    break;
                case LGX_SIG_CHECK_UNUSABLE:
                    // A manifest.sig is present but is not a usable signature
                    // document. It refutes nothing, so it ranks with absence
                    // rather than with a refusal.
                    signerVerdict = (m_unknownSignerPolicy == UnknownSignerPolicy::Strict)
                                        ? DependencyStatus::SignerMismatch
                                        : DependencyStatus::SignerUnknown;
                    break;
                }
            }
            if (edgeVerdictSeverity(signerVerdict) > edgeVerdictSeverity(node.status))
                node.status = signerVerdict;
        }

        // Say it once, at the one moment it is decided. SignerMismatch is
        // definitive, rare, and the remedy (this is somebody else's package)
        // is not guessable from a bare load failure — the exact class of fact
        // this scanner keeps being fixed for dropping silently.
        //
        // SignerUnknown deliberately does NOT warn: it is the expected state
        // for every embedded package and every unsigned one, and a warning
        // that fires on the normal case trains readers to ignore the channel.
        // It travels on the status and on `lgpm info` instead.
        if (node.status == DependencyStatus::SignerMismatch && dep.signer) {
            std::cerr << "Warning: dependency '" << dep.name << "' is pinned to signer "
                      << *dep.signer << " but the installed package in " << scan.installDir
                      << " was not signed by that key (it is signed by "
                      << (scan.signerDid ? *scan.signerDid
                                         : std::string("no key whose signature verifies"))
                      << "); it is a different package under the same name.\n";
        }

        path.insert(dep.name);
        node.children.reserve(scan.dependencies.size());
        for (const auto& child : scan.dependencies) {
            node.children.push_back(build(child));
        }
        path.erase(dep.name);
        return node;
    };

    // The root is not pointed at by any edge, so it carries no constraint.
    return build(PackageDependency(packageName));
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
            reverseDeps[d.name].push_back(name);
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
    std::unordered_map<std::string, std::size_t> indexByName;
    std::deque<const DependencyTreeNode*> queue;
    for (const auto& c : children) queue.push_back(&c);
    while (!queue.empty()) {
        const DependencyTreeNode* n = queue.front();
        queue.pop_front();
        auto [slot, inserted] = indexByName.emplace(n->name, out.size());
        if (!inserted) {
            // Already reported under this name. One row per package stays the
            // contract — every flat consumer counts on it — so this does not
            // append a second row. But a package satisfies its dependants only
            // if it satisfies ALL of them, and the row we already have came
            // from whichever edge BFS reached first, which means the SHALLOWEST
            // one: a package named both directly by the root (bare, as every
            // manifest in the fleet does today) and by a dependency that
            // constrains it was reported "installed" and the rejection was
            // dropped. That loss is invisible in the tree — resolveDependencies
            // shows the mismatch, resolveFlatDependencies did not — and the
            // flat list is the only projection basecamp's load gate reads.
            //
            // So a later edge that judges the package MORE HARSHLY than the
            // recorded one promotes the row, carrying its constraint so the
            // report can name what failed. Ordered by edgeVerdictSeverity, so
            // adding a status to the vocabulary means placing it in that one
            // ranking rather than extending a chain of pairwise ifs — the
            // original rule was written as the single pair
            // `Installed -> VersionMismatch`, which would silently have kept
            // dropping a deeper SIGNER mismatch exactly the way it once
            // dropped a deeper version one.
            //
            // Only the edge-decided statuses participate: NotInstalled and
            // Cycle score -1, so they neither promote nor are promoted (an
            // absent package is absent on every edge).
            //
            // `signerDid` is NOT carried across: it is a property of the
            // installed package, identical on every edge that reaches it,
            // whereas requiredVersion/requiredSigner belong to the promoting
            // edge and must travel with it.
            DependencyTreeNode& recorded = out[slot->second];
            const int recordedRank = edgeVerdictSeverity(recorded.status);
            const int candidateRank = edgeVerdictSeverity(n->status);
            if (recordedRank >= 0 && candidateRank > recordedRank) {
                recorded.status          = n->status;
                recorded.requiredVersion = n->requiredVersion;
                recorded.requiredSigner  = n->requiredSigner;
            }
            continue;
        }
        DependencyTreeNode copy;
        copy.name            = n->name;
        copy.status          = n->status;
        copy.version         = n->version;
        copy.installType     = n->installType;
        copy.requiredVersion = n->requiredVersion;
        copy.requiredSigner  = n->requiredSigner;
        copy.signerDid       = n->signerDid;
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

    // Same read-only trap as the temp cleanup above, but here the failure is
    // load-bearing rather than cosmetic: an installed package whose icon came
    // out of the Nix store carries FILE_ATTRIBUTE_READONLY, so on Windows every
    // uninstall and every upgrade failed with
    //   "Failed to remove install directory: Access is denied"
    // while the same call succeeds on POSIX, which needs only the parent
    // directory's write bit. Clear the attributes and retry before reporting.
    std::uintmax_t removed = fs::remove_all(toDelete, ec);
    if (ec) {
        clearReadOnlyRecursive(toDelete);
        ec.clear();
        removed = fs::remove_all(toDelete, ec);
    }
    if (ec) {
        result.errorMsg = "Failed to remove install directory: " + ec.message();
        return result;
    }

    result.success = true;
    result.removedFiles.push_back(toDelete.string());
    (void)removed;
    return result;
}

namespace {
// Deliberately a plain global rather than something inferred from the
// environment: a silent platform switch would defeat the fail-closed check that
// stops a Windows package being installed as a Linux one.
std::string g_platformVariantOverride;
}

void PackageManagerLib::setPlatformVariantOverride(const std::string& variant)
{
    g_platformVariantOverride = variant;
}

std::string PackageManagerLib::platformVariantOverride()
{
    return g_platformVariantOverride;
}

std::string PackageManagerLib::currentPlatformVariant()
{
    if (!g_platformVariantOverride.empty()) {
        return g_platformVariantOverride;
    }

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

namespace {

// The one place a variant-name SPELLING is enumerated.
//
// A variant name is "<os>-<architecture>", and both halves have more than one
// live spelling in this ecosystem. Canonical vocabulary is the one
// logos-module-builder's lib/resolvePlatforms.nix pins -- os in
// {linux, darwin, windows}, architecture in {x86_64, aarch64} -- so the first
// entry of every row below is the canonical spelling and the rest are legacy
// ones that some producer actually writes:
//
//   nix-bundle-lgx/flake.nix:52                   darwin-amd64  linux-amd64
//                                                 darwin-arm64  linux-arm64
//   nix-bundle-logos-module-install/flake.nix:49  darwin-x86_64 linux-x86_64
//                                                 darwin-arm64  linux-arm64
//
// Those two producers disagree with each other while the SECOND CONSUMES THE
// FIRST -- it bundles a .lgx the first named and then calls this package
// manager with --platform spelled its own way -- so this consumer is where the
// disagreement has to be absorbed.
//
// Only the ARCHITECTURE is aliased. The OS half is matched verbatim on
// purpose: aliasing it would weaken the fail-closed check that stops a Windows
// package being installed as a macOS one, and "macos" (logos-release-set's
// release-ASSET naming) is a different namespace that is translated to
// "darwin" before it ever reaches an .lgx.
//
// Adding a spelling here is safe -- it only ever widens what an EXISTING
// package resolves to, and rows are per-architecture so a new OS inherits
// every alias without a new code path. Removing one is not: variant names sit
// inside the signed hash tree (logos-package writes hashes["variants/<name>"]),
// so a published package can never be renamed on disk. Alias on read.
const std::vector<std::vector<std::string>>& architectureSpellings()
{
    static const std::vector<std::vector<std::string>> kSpellings = {
        { "x86_64",  "amd64" },
        { "aarch64", "arm64" },
    };
    return kSpellings;
}

} // namespace

std::vector<std::string> PackageManagerLib::platformVariantsToTry()
{
    const std::string primary = currentPlatformVariant();
    std::vector<std::string> variants;

    // The host's own spelling always leads: it is what diagnostics print as
    // "this machine", and what errorMsg names as the platform that was wanted.
    variants.push_back(primary);

    // Split on the LAST '-' so the architecture is the trailing component
    // whatever the OS half turns out to contain.
    const std::string::size_type sep = primary.rfind('-');
    if (sep != std::string::npos) {
        const std::string os = primary.substr(0, sep);
        const std::string arch = primary.substr(sep + 1);
        for (const auto& row : architectureSpellings()) {
            if (std::find(row.begin(), row.end(), arch) == row.end())
                continue;
            for (const auto& spelling : row) {
                if (spelling != arch)
                    variants.push_back(os + "-" + spelling);
            }
            break;
        }
    }
    // An architecture no row knows degrades to "this host's spelling only",
    // which is the pre-alias behaviour and still fail-closed.

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

    // ── Carry the signature to where the signed bytes are going ────────────
    //
    // manifest.json above is written from lgx_get_manifest_json(), which
    // returns getManifest().toJson() — the SAME expression Package::signPackage
    // signs. So the manifest that lands in the install tree is byte-identical
    // to the one the signature covers, by construction rather than by luck,
    // and a detached Ed25519 check over the installed file is meaningful
    // offline, long after the .lgx is deleted.
    //
    // Without this the install tree kept the signed bytes and threw away the
    // signature over them, so nothing on disk could answer "who published
    // this" and the answer had to be ASSERTED by the installer instead — a
    // record trusted because the installer wrote it, which then needed
    // defending against everything that can write a file. Evidence that needs
    // defending is not evidence. This copy asserts nothing: it moves the
    // package's own document, verbatim, and every question about it is settled
    // later by arithmetic over a key the ASKER supplies.
    //
    // That is also why this belongs here rather than after the payload copy.
    // Package::extractVariant re-adds owner_write to every file it writes
    // (package.cpp: "NEVER extract a file the caller cannot overwrite"), so a
    // package shipping its own read-only variants/<v>/manifest.sig cannot
    // block the real one — the trap that needed a bespoke read-only dance when
    // this was written into the install directory after the copy simply does
    // not exist on this path.
    //
    // A package that ships no signature leaves nothing here, and an installed
    // package with no manifest.sig reads as "nobody knows", exactly as before.
    if (const char* sigJson = lgx_get_manifest_sig_json(pkg)) {
        fs::path sigPath = variantOutputDir / "manifest.sig";
        std::ofstream sf(sigPath, std::ios::binary | std::ios::trunc);
        if (!sf.is_open() || !(sf << sigJson) || !sf.good()) {
            // Not fatal: manifest.json is what the install NEEDS (type, name),
            // while the signature is evidence, and its absence is already a
            // defined and safe state. But a signed package that lands without
            // its signature silently reads as unsigned forever, so say it.
            std::cerr << "Warning: package '" << lgxPath << "' is signed, but its signature "
                      << "could not be written to " << sigPath.string()
                      << "; the installed copy will read as having no known publisher.\n";
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

void PackageManagerLib::setUnknownSignerPolicy(UnknownSignerPolicy policy)
{
    m_unknownSignerPolicy = policy;
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
        // Compare the first COMPONENT rather than doing a string prefix test.
        // fs::path::native() is std::wstring on Windows, so rfind("..", 0) does
        // not even compile there -- and the prefix test was imprecise anyway: it
        // also matched a legitimate directory literally named "..foo", rejecting
        // a path that does not actually escape. Component comparison is portable
        // and exact. (Short-circuits, so *rel.begin() is only reached when rel
        // is non-empty.)
        if (rel.empty() || *rel.begin() == fs::path("..")) {
            errorMsg = "Resolved install path escapes target directory: " + moduleSubDir;
            return false;
        }
    }

    if (!copyDirectoryContents(variantDir, moduleSubDir, errorMsg)) {
        return false;
    }

    return true;
}
