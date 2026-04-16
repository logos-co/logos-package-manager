#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

class UninstallTest : public ::testing::Test {
protected:
    fs::path embeddedDir;
    fs::path userDir;

    void SetUp() override {
        auto base = fs::temp_directory_path() / ("lgpm_uninst_" + std::to_string(std::rand()));
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
                       const std::string& type = "core") {
        fs::path dir = parentDir / name;
        fs::create_directories(dir);

        json manifest;
        manifest["name"] = name;
        manifest["type"] = type;
        manifest["version"] = "1.0.0";

        std::ofstream mf(dir / "manifest.json");
        mf << manifest.dump(2);

        // Drop a fake payload so remove_all has something to remove.
        std::ofstream(dir / (name + ".so")) << "binary";
    }
};

TEST_F(UninstallTest, UninstallsUserInstalledPackage) {
    writeManifest(userDir, "usermod");

    PackageManagerLib pm;
    pm.setUserModulesDirectory(userDir.string());

    UninstallResult r = pm.uninstallPackage("usermod");
    EXPECT_TRUE(r.success) << r.errorMsg;
    EXPECT_FALSE(fs::exists(userDir / "usermod"));
}

TEST_F(UninstallTest, RefusesEmbeddedPackage) {
    writeManifest(embeddedDir, "embed_mod");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());

    UninstallResult r = pm.uninstallPackage("embed_mod");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.errorMsg.find("embedded"), std::string::npos);
    EXPECT_TRUE(fs::exists(embeddedDir / "embed_mod"));
}

TEST_F(UninstallTest, UnknownPackageFails) {
    PackageManagerLib pm;
    pm.setUserModulesDirectory(userDir.string());

    UninstallResult r = pm.uninstallPackage("nope");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.errorMsg.find("not found"), std::string::npos);
}

TEST_F(UninstallTest, UserWinsWhenSameNameInBothDirs) {
    // Same package exists in embedded AND user — user wins, uninstall succeeds.
    writeManifest(embeddedDir, "same");
    writeManifest(userDir, "same");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(embeddedDir.string());
    pm.setUserModulesDirectory(userDir.string());

    UninstallResult r = pm.uninstallPackage("same");
    EXPECT_TRUE(r.success) << r.errorMsg;
    EXPECT_FALSE(fs::exists(userDir / "same"));
    EXPECT_TRUE(fs::exists(embeddedDir / "same"));
}
