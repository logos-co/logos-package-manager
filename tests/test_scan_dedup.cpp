#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

class ScanDedupTest : public ::testing::Test {
protected:
    fs::path embeddedDir;
    fs::path userDir;

    void SetUp() override {
        auto base = fs::temp_directory_path() / ("lgpm_dedup_" + std::to_string(std::rand()));
        embeddedDir = base / "embedded";
        userDir = base / "user";
        fs::create_directories(embeddedDir);
        fs::create_directories(userDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(embeddedDir.parent_path(), ec);
    }

    void writeManifest(const fs::path& parentDir,
                       const std::string& name,
                       const std::string& version,
                       const std::string& type = "core") {
        fs::path dir = parentDir / name;
        fs::create_directories(dir);

        json manifest;
        manifest["name"] = name;
        manifest["type"] = type;
        manifest["version"] = version;

        std::ofstream mf(dir / "manifest.json");
        mf << manifest.dump(2);
    }
};

TEST_F(ScanDedupTest, NewerUserCopyWinsOverEmbedded) {
    writeManifest(embeddedDir, "widget", "1.0.0");
    writeManifest(userDir,     "widget", "2.0.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.setUserModulesDirectory(userDir.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].name, "widget");
    EXPECT_EQ(modules[0].version, "2.0.0");
    EXPECT_EQ(modules[0].installType, InstallType::User);
}

TEST_F(ScanDedupTest, EmbeddedOnlyShowsAsEmbedded) {
    writeManifest(embeddedDir, "builtin", "1.0.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].installType, InstallType::Embedded);
}

TEST_F(ScanDedupTest, UserOnlyShowsAsUser) {
    writeManifest(userDir, "installed", "1.0.0");

    PackageManagerLib pm;
    pm.setUserModulesDirectory(userDir.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].installType, InstallType::User);
}

TEST_F(ScanDedupTest, DedupWorksForGetInstalledPackages) {
    writeManifest(embeddedDir, "shared", "1.0.0", "core");
    writeManifest(userDir,     "shared", "2.0.0", "core");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.setUserModulesDirectory(userDir.string());

    auto pkgs = pm.getInstalledPackages();
    ASSERT_EQ(pkgs.size(), 1u);
    EXPECT_EQ(pkgs[0].version, "2.0.0");
    EXPECT_EQ(pkgs[0].installType, InstallType::User);
}

// An app upgrade can ship a newer embedded copy than the one the user
// installed; the older user copy must not shadow it.
TEST_F(ScanDedupTest, NewerEmbeddedCopyWinsOverOlderUserCopy) {
    writeManifest(embeddedDir, "widget", "2.0.0");
    writeManifest(userDir,     "widget", "1.0.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.setUserModulesDirectory(userDir.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].version, "2.0.0");
    EXPECT_EQ(modules[0].installType, InstallType::Embedded);
}

TEST_F(ScanDedupTest, UserCopyWinsATie) {
    writeManifest(embeddedDir, "widget", "1.0.0");
    writeManifest(userDir,     "widget", "1.0.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.setUserModulesDirectory(userDir.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].installType, InstallType::User);
}

// liblogos registers every modules dir as embedded, in the frontend's order;
// that order must not decide which version loads.
TEST_F(ScanDedupTest, NewestWinsAcrossEmbeddedDirsWhateverTheirOrder) {
    const fs::path second = embeddedDir.parent_path() / "embedded2";
    fs::create_directories(second);
    writeManifest(embeddedDir, "widget", "3.0.2");
    writeManifest(second,      "widget", "3.0.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.addEmbeddedModulesDirectory(second.string());

    auto modules = pm.getInstalledModules();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].version, "3.0.2");
    EXPECT_EQ(fs::path(modules[0].installDir), embeddedDir / "widget");
}
