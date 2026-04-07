#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <iostream>
#include <nlohmann/json.hpp>
#include "lgx.h"

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

bool PackageManagerLib::versionGreaterOrEqual(const std::string& a, const std::string& b)
{
    auto ap = splitString(a, '.');
    auto bp = splitString(b, '.');
    size_t len = std::max(ap.size(), bp.size());
    for (size_t i = 0; i < len; ++i) {
        int av = i < ap.size() ? std::atoi(ap[i].c_str()) : 0;
        int bv = i < bp.size() ? std::atoi(bp[i].c_str()) : 0;
        if (av != bv)
            return av > bv;
    }
    return true; // equal
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

            if (!incomingName.empty() && !incomingVersion.empty()) {
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
        if (sigResult.is_signed && sigResult.trusted_as.empty()) {
            if (m_tofuEnabled) {
                const char* krDir = m_keyringDir.empty() ? nullptr : m_keyringDir.c_str();
                const char* dispName = sigResult.signer_name.empty() ? nullptr : sigResult.signer_name.c_str();
                const char* url = sigResult.signer_url.empty() ? nullptr : sigResult.signer_url.c_str();
                lgx_keyring_add(krDir, "auto-trusted", sigResult.signer_did.c_str(), dispName, url);
            } else if (m_signaturePolicy == SignaturePolicy::REQUIRE) {
                errorMsg = "Package signed by unknown key and TOFU is disabled";
                return {};
            }
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
        std::string mainFile;
        bool isQmlPackage = (detectedType == "ui_qml");
        fs::path installedManifestPath = fs::path(installDir) / installedModuleName / "manifest.json";
        if (fs::exists(installedManifestPath)) {
            std::ifstream mf(installedManifestPath);
            if (mf.is_open()) {
                try {
                    json doc = json::parse(mf);
                    if (doc.contains("main") && doc["main"].is_object()) {
                        for (const auto& v : platformVariantsToTry()) {
                            if (doc["main"].contains(v)) {
                                mainFile = doc["main"][v].get<std::string>();
                                if (!mainFile.empty())
                                    break;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
        if (mainFile.empty()) {
            mainFile = installedModuleName;
        }
        // For core and ui packages, append platform-specific library extension
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

    fs::remove_all(tempDir);
    return installDir;
}

// Shared scanning logic — scans directories for installed packages matching given types.
// Returns JSON array; each element is a copy of manifest.json + "installDir" + "mainFilePath".
static std::string scanInstalledByTypes(const std::vector<std::string>& dirs,
                                         const std::vector<std::string>& types)
{
    json results = json::array();
    auto variants = PackageManagerLib::platformVariantsToTry();

    for (const auto& dirPath : dirs) {
        std::error_code dirEc;
        if (!fs::is_directory(dirPath, dirEc) || dirEc)
            continue;

        for (const auto& entry : fs::directory_iterator(dirPath, fs::directory_options::skip_permission_denied, dirEc)) {
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

            // Filter by type if types list is non-empty
            if (!types.empty()) {
                std::string moduleType = manifest.value("type", "");
                bool matches = false;
                for (const auto& t : types) {
                    if (moduleType == t) { matches = true; break; }
                }
                if (!matches) continue;
            }

            // Resolve mainFilePath from the "main" field
            std::string mainFilePath;
            if (manifest.contains("main")) {
                if (manifest["main"].is_object()) {
                    auto mainObj = manifest["main"];
                    for (const auto& variant : variants) {
                        if (mainObj.contains(variant)) {
                            std::string mainFile = mainObj[variant].get<std::string>();
                            if (!mainFile.empty()) {
                                fs::path candidate = entry.path() / mainFile;
                                if (fs::exists(candidate)) {
                                    mainFilePath = candidate.string();
                                }
                                break;
                            }
                        }
                    }
                } else if (manifest["main"].is_string()) {
                    std::string mainFile = manifest["main"].get<std::string>();
                    if (!mainFile.empty()) {
                        fs::path candidate = entry.path() / mainFile;
                        if (fs::exists(candidate)) {
                            mainFilePath = candidate.string();
                        }
                    }
                }
            }

            // Start with a copy of all manifest fields
            json info = manifest;
            info["installDir"] = entry.path().string();
            info["mainFilePath"] = mainFilePath;

            results.push_back(info);
        }
    }

    return results.dump();
}

std::string PackageManagerLib::getInstalledModules()
{
    return scanInstalledByTypes(allModulesDirectories(), {"core"});
}

std::string PackageManagerLib::getInstalledUiPlugins()
{
    return scanInstalledByTypes(allUiPluginsDirectories(), {"ui", "ui_qml"});
}

std::string PackageManagerLib::getInstalledPackages()
{
    return scanInstalledByTypes(allDirectories(), {});
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
        errorMsg = "Package does not contain variant for platform: " + variants.front();
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

void PackageManagerLib::setTofuEnabled(bool enabled)
{
    m_tofuEnabled = enabled;
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

    outModuleName = moduleName;
    std::string moduleSubDir = (fs::path(targetDir) / moduleName).string();

    if (!copyDirectoryContents(variantDir, moduleSubDir, errorMsg)) {
        return false;
    }

    return true;
}
