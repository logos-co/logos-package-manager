#pragma once

#include <string>
#include <vector>

class PackageManagerLib
{
public:
    PackageManagerLib();
    ~PackageManagerLib();

    // Directory management
    void setEmbeddedModulesDirectory(const std::string& dir);
    void setUserModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);

    std::string embeddedModulesDirectory() const { return m_embeddedModulesDir; }
    std::string userModulesDirectory() const { return m_userModulesDir; }
    std::string embeddedUiPluginsDirectory() const { return m_embeddedUiPluginsDir; }
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

    // Utilities
    static bool versionGreaterOrEqual(const std::string& a, const std::string& b);
    static bool copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg);

private:
    std::string m_embeddedModulesDir;
    std::string m_userModulesDir;
    std::string m_embeddedUiPluginsDir;
    std::string m_userUiPluginsDir;
};
