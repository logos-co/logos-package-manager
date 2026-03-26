#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

class ScanningTest : public ::testing::Test {
protected:
    fs::path tempDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("lgpm_test_" + std::to_string(std::rand()));
        fs::create_directories(tempDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    // Create a fake installed module directory with manifest.json
    void createFakeModule(const std::string& name, const std::string& type,
                          const std::string& version = "1.0.0") {
        fs::path moduleDir = tempDir / name;
        fs::create_directories(moduleDir);

        json manifest;
        manifest["name"] = name;
        manifest["type"] = type;
        manifest["version"] = version;
        manifest["description"] = "Test module " + name;
        manifest["category"] = "test";
        manifest["main"] = name + ".so";

        std::ofstream mf(moduleDir / "manifest.json");
        mf << manifest.dump(2);
    }
};

TEST_F(ScanningTest, EmptyDirectoryReturnsEmptyArray) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    std::string result = pm.getInstalledModules();
    json modules = json::parse(result);
    EXPECT_TRUE(modules.is_array());
    EXPECT_TRUE(modules.empty());
}

TEST_F(ScanningTest, NonExistentDirectoryReturnsEmptyArray) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory("/nonexistent/path");

    std::string result = pm.getInstalledModules();
    json modules = json::parse(result);
    EXPECT_TRUE(modules.empty());
}

TEST_F(ScanningTest, GetInstalledModulesFiltersByCore) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    std::string result = pm.getInstalledModules();
    json modules = json::parse(result);

    ASSERT_EQ(modules.size(), 1);
    EXPECT_EQ(modules[0]["name"], "core_mod");
}

TEST_F(ScanningTest, GetInstalledUiPluginsFiltersByUiTypes) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");
    createFakeModule("qml_mod", "ui_qml");

    PackageManagerLib pm;
    pm.setEmbeddedUiPluginsDirectory(tempDir.string());

    std::string result = pm.getInstalledUiPlugins();
    json plugins = json::parse(result);

    ASSERT_EQ(plugins.size(), 2);

    std::vector<std::string> names;
    for (const auto& p : plugins) names.push_back(p["name"]);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "ui_mod") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "qml_mod") != names.end());
}

TEST_F(ScanningTest, GetInstalledPackagesReturnsAll) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");
    createFakeModule("qml_mod", "ui_qml");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    std::string result = pm.getInstalledPackages();
    json packages = json::parse(result);

    EXPECT_EQ(packages.size(), 3);
}

TEST_F(ScanningTest, ScannedModulesContainManifestFields) {
    createFakeModule("test_mod", "core", "2.1.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    std::string result = pm.getInstalledModules();
    json modules = json::parse(result);

    ASSERT_EQ(modules.size(), 1);
    EXPECT_EQ(modules[0]["name"], "test_mod");
    EXPECT_EQ(modules[0]["version"], "2.1.0");
    EXPECT_EQ(modules[0]["type"], "core");
    EXPECT_EQ(modules[0]["category"], "test");
    EXPECT_FALSE(modules[0]["installDir"].get<std::string>().empty());
}

TEST_F(ScanningTest, MultipleDirectoriesCombined) {
    fs::path dir2 = fs::temp_directory_path() / ("lgpm_test2_" + std::to_string(std::rand()));
    fs::create_directories(dir2);

    // Module in first dir
    createFakeModule("mod_a", "core");

    // Module in second dir
    fs::path modB = dir2 / "mod_b";
    fs::create_directories(modB);
    json manifest;
    manifest["name"] = "mod_b";
    manifest["type"] = "core";
    manifest["version"] = "1.0.0";
    std::ofstream mf(modB / "manifest.json");
    mf << manifest.dump(2);
    mf.close();

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());
    pm.setUserModulesDirectory(dir2.string());

    std::string result = pm.getInstalledModules();
    json modules = json::parse(result);

    EXPECT_EQ(modules.size(), 2);

    std::error_code ec;
    fs::remove_all(dir2, ec);
}

TEST_F(ScanningTest, DirectoryManagement) {
    PackageManagerLib pm;

    pm.setEmbeddedModulesDirectory("/a");
    pm.setUserModulesDirectory("/b");
    pm.setEmbeddedUiPluginsDirectory("/c");
    pm.setUserUiPluginsDirectory("/d");

    EXPECT_EQ(pm.embeddedModulesDirectory(), "/a");
    EXPECT_EQ(pm.userModulesDirectory(), "/b");
    EXPECT_EQ(pm.embeddedUiPluginsDirectory(), "/c");
    EXPECT_EQ(pm.userUiPluginsDirectory(), "/d");

    auto moduleDirs = pm.allModulesDirectories();
    EXPECT_EQ(moduleDirs.size(), 2);

    auto uiDirs = pm.allUiPluginsDirectories();
    EXPECT_EQ(uiDirs.size(), 2);

    auto allDirs = pm.allDirectories();
    EXPECT_EQ(allDirs.size(), 4);
}

TEST_F(ScanningTest, SkipSubdirWithoutManifest) {
    fs::create_directories(tempDir / "no_manifest_module");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    std::string result = pm.getInstalledPackages();
    json packages = json::parse(result);
    EXPECT_TRUE(packages.empty());
}
