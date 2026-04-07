#pragma once

#include <string>
#include <vector>

enum class SignaturePolicy {
    NONE,    // Accept all packages without checking signatures
    WARN,    // Accept unsigned packages with a warning (default)
    REQUIRE  // Reject unsigned packages and packages with unknown signers
};

struct SignatureVerificationResult {
    bool is_signed = false;
    bool signature_valid = false;
    bool package_valid = false;
    std::string signer_did;    // did:jwk:... string
    std::string signer_name;   // self-asserted display name
    std::string signer_url;    // self-asserted URL
    std::string trusted_as;    // keyring name if trusted, empty otherwise
    std::string error;         // error message if any
};

class PackageManagerLib
{
public:
    PackageManagerLib();
    ~PackageManagerLib();

    // Directory management — embedded directories (multiple, read-only at runtime)
    void setEmbeddedModulesDirectory(const std::string& dir);
    void addEmbeddedModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void addEmbeddedUiPluginsDirectory(const std::string& dir);

    std::vector<std::string> embeddedModulesDirectories() const { return m_embeddedModulesDirs; }
    std::vector<std::string> embeddedUiPluginsDirectories() const { return m_embeddedUiPluginsDirs; }

    // Directory management — user directories (single, writable, where new packages are installed)
    void setUserModulesDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);

    std::string userModulesDirectory() const { return m_userModulesDir; }
    std::string userUiPluginsDirectory() const { return m_userUiPluginsDir; }

    // All modules directories (embedded + user) for scanning
    std::vector<std::string> allModulesDirectories() const;
    // All UI plugins directories (embedded + user) for scanning
    std::vector<std::string> allUiPluginsDirectories() const;
    // All directories (modules + UI plugins) for scanning
    std::vector<std::string> allDirectories() const;

    // Install from local LGX file
    // Returns installed path, or empty string on error (sets errorMsg).
    // If installedPluginPath is non-null, receives the full path to the installed main file.
    // If isCoreModule is non-null, receives whether the module type is "core".
    std::string installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                  bool skipIfNotNewerVersion = false,
                                  std::string* installedPluginPath = nullptr,
                                  bool* isCoreModule = nullptr);

    // Module scanning — returns JSON string (array of module objects)
    // Each object contains all manifest.json fields + "installDir" + "mainFilePath"
    std::string getInstalledModules();
    std::string getInstalledUiPlugins();
    std::string getInstalledPackages();

    // LGX extraction and installation helpers
    bool extractLgxPackage(const std::string& lgxPath, const std::string& outputDir, std::string& errorMsg);
    bool copyLibraryFromExtracted(const std::string& extractedDir, const std::string& targetDir,
                                  bool isCoreModule, std::string& outModuleName, std::string& errorMsg);

    // Variant selection (platform detection)
    static std::string currentPlatformVariant();
    static std::vector<std::string> platformVariantsToTry();

    // Signature policy configuration
    void setSignaturePolicy(SignaturePolicy policy);
    SignaturePolicy signaturePolicy() const { return m_signaturePolicy; }
    void setKeyringDirectory(const std::string& dir);
    std::string keyringDirectory() const { return m_keyringDir; }
    void setTofuEnabled(bool enabled);
    bool tofuEnabled() const { return m_tofuEnabled; }

    // Standalone signature verification
    SignatureVerificationResult verifyPackageSignature(const std::string& lgxPath);

    // Utilities
    static bool versionGreaterOrEqual(const std::string& a, const std::string& b);
    static bool copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg);

private:
    std::vector<std::string> m_embeddedModulesDirs;
    std::string m_userModulesDir;
    std::vector<std::string> m_embeddedUiPluginsDirs;
    std::string m_userUiPluginsDir;
    SignaturePolicy m_signaturePolicy = SignaturePolicy::WARN;
    std::string m_keyringDir;
    bool m_tofuEnabled = false;
};
