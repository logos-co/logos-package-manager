#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <algorithm>
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
                          const std::string& version = "1.0.0",
                          const std::string& main = "default",
                          const std::string& view = "") {
        fs::path moduleDir = tempDir / name;
        fs::create_directories(moduleDir);

        json manifest;
        manifest["name"] = name;
        manifest["type"] = type;
        manifest["version"] = version;
        manifest["description"] = "Test module " + name;
        manifest["category"] = "test";
        if (type == "ui_qml" && main == "default") {
            manifest["main"] = json::object();
        } else if (main == "default") {
            manifest["main"] = name + ".so";
        } else if (!main.empty()) {
            manifest["main"] = main;
        } else if (type == "ui_qml") {
            manifest["main"] = json::object();
        }
        if (!view.empty()) {
            manifest["view"] = view;
        }

        if (type != "ui_qml" && main == "default") {
            std::ofstream(moduleDir / (name + ".so")) << "binary";
        } else if (!main.empty() && main != "default") {
            std::ofstream(moduleDir / main) << "binary";
        }
        if (!view.empty()) {
            fs::create_directories((moduleDir / fs::path(view)).parent_path());
            std::ofstream(moduleDir / view) << "import QtQuick 2.15\nItem {}";
        }

        std::ofstream mf(moduleDir / "manifest.json");
        mf << manifest.dump(2);
    }
};

TEST_F(ScanningTest, EmptyDirectoryReturnsEmptyArray) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    auto modules = pm.getInstalledModules();
    EXPECT_TRUE(modules.empty());
}

TEST_F(ScanningTest, NonExistentDirectoryReturnsEmptyArray) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory("/nonexistent/path");

    auto modules = pm.getInstalledModules();
    EXPECT_TRUE(modules.empty());
}

TEST_F(ScanningTest, GetInstalledModulesFiltersByCore) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    auto modules = pm.getInstalledModules();

    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].name, "core_mod");
}

TEST_F(ScanningTest, GetInstalledUiPluginsFiltersByUiTypes) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");
    createFakeModule("qml_mod", "ui_qml", "1.0.0", "", "qml/Main.qml");

    PackageManagerLib pm;
    pm.setEmbeddedUiPluginsDirectory(tempDir.string());

    auto plugins = pm.getInstalledUiPlugins();

    ASSERT_EQ(plugins.size(), 2u);

    std::vector<std::string> names;
    for (const auto& p : plugins) names.push_back(p.name);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "ui_mod") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "qml_mod") != names.end());
}

TEST_F(ScanningTest, GetInstalledPackagesReturnsAll) {
    createFakeModule("core_mod", "core");
    createFakeModule("ui_mod", "ui");
    createFakeModule("qml_mod", "ui_qml", "1.0.0", "", "qml/Main.qml");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    auto packages = pm.getInstalledPackages();
    EXPECT_EQ(packages.size(), 3u);
}

TEST_F(ScanningTest, ScannedModulesContainManifestFields) {
    createFakeModule("test_mod", "core", "2.1.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    auto modules = pm.getInstalledModules();

    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].name, "test_mod");
    EXPECT_EQ(modules[0].version, "2.1.0");
    EXPECT_EQ(modules[0].type, "core");
    EXPECT_EQ(modules[0].category, "test");
    EXPECT_FALSE(modules[0].installDir.empty());
}

TEST_F(ScanningTest, UiQmlScanKeepsViewAndAllowsEmptyMainFilePath) {
    createFakeModule("qml_mod", "ui_qml", "1.0.0", "", "qml/Main.qml");

    PackageManagerLib pm;
    pm.setEmbeddedUiPluginsDirectory(tempDir.string());

    auto plugins = pm.getInstalledUiPlugins();

    ASSERT_EQ(plugins.size(), 1u);
    EXPECT_EQ(plugins[0].name, "qml_mod");
    EXPECT_EQ(plugins[0].view, "qml/Main.qml");
    EXPECT_TRUE(plugins[0].mainFilePath.empty());
}

TEST_F(ScanningTest, UiQmlScanResolvesBackendMainFilePath) {
    createFakeModule("qml_backend", "ui_qml", "1.0.0", "backend.so", "qml/Main.qml");

    PackageManagerLib pm;
    pm.setEmbeddedUiPluginsDirectory(tempDir.string());

    auto plugins = pm.getInstalledUiPlugins();

    ASSERT_EQ(plugins.size(), 1u);
    EXPECT_EQ(plugins[0].name, "qml_backend");
    EXPECT_EQ(plugins[0].view, "qml/Main.qml");
    EXPECT_NE(plugins[0].mainFilePath.find("backend.so"), std::string::npos);
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

    auto modules = pm.getInstalledModules();
    EXPECT_EQ(modules.size(), 2u);

    std::error_code ec;
    fs::remove_all(dir2, ec);
}

TEST_F(ScanningTest, DirectoryManagement) {
    PackageManagerLib pm;

    pm.setEmbeddedModulesDirectory("/a");
    pm.setUserModulesDirectory("/b");
    pm.setEmbeddedUiPluginsDirectory("/c");
    pm.setUserUiPluginsDirectory("/d");

    EXPECT_EQ(pm.embeddedModulesDirectories(), std::vector<std::string>{"/a"});
    EXPECT_EQ(pm.userModulesDirectory(), "/b");
    EXPECT_EQ(pm.embeddedUiPluginsDirectories(), std::vector<std::string>{"/c"});
    EXPECT_EQ(pm.userUiPluginsDirectory(), "/d");

    auto moduleDirs = pm.allModulesDirectories();
    EXPECT_EQ(moduleDirs.size(), 2u);

    auto uiDirs = pm.allUiPluginsDirectories();
    EXPECT_EQ(uiDirs.size(), 2u);

    auto allDirs = pm.allDirectories();
    EXPECT_EQ(allDirs.size(), 4u);
}

TEST_F(ScanningTest, MultipleEmbeddedDirectories) {
    PackageManagerLib pm;

    pm.setEmbeddedModulesDirectory("/a");
    pm.addEmbeddedModulesDirectory("/b");
    pm.addEmbeddedModulesDirectory("/c");

    auto dirs = pm.embeddedModulesDirectories();
    ASSERT_EQ(dirs.size(), 3u);
    EXPECT_EQ(dirs[0], "/a");
    EXPECT_EQ(dirs[1], "/b");
    EXPECT_EQ(dirs[2], "/c");

    // set clears previous entries
    pm.setEmbeddedModulesDirectory("/x");
    EXPECT_EQ(pm.embeddedModulesDirectories().size(), 1u);
    EXPECT_EQ(pm.embeddedModulesDirectories()[0], "/x");
}

TEST_F(ScanningTest, MultipleEmbeddedUiPluginsDirectories) {
    PackageManagerLib pm;

    pm.setEmbeddedUiPluginsDirectory("/p1");
    pm.addEmbeddedUiPluginsDirectory("/p2");

    auto dirs = pm.embeddedUiPluginsDirectories();
    ASSERT_EQ(dirs.size(), 2u);
    EXPECT_EQ(dirs[0], "/p1");
    EXPECT_EQ(dirs[1], "/p2");
}

TEST_F(ScanningTest, MultipleEmbeddedDirsScanning) {
    fs::path dir2 = fs::temp_directory_path() / ("lgpm_test_emb2_" + std::to_string(std::rand()));
    fs::create_directories(dir2);

    // Module in first embedded dir
    createFakeModule("mod_a", "core");

    // Module in second embedded dir
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
    pm.addEmbeddedModulesDirectory(dir2.string());

    auto modules = pm.getInstalledModules();
    EXPECT_EQ(modules.size(), 2u);

    std::error_code ec;
    fs::remove_all(dir2, ec);
}

TEST_F(ScanningTest, SkipSubdirWithoutManifest) {
    fs::create_directories(tempDir / "no_manifest_module");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(tempDir.string());

    auto packages = pm.getInstalledPackages();
    EXPECT_TRUE(packages.empty());
}
