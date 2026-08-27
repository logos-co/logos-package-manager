// Regression tests for an install that wedges itself permanently, and for the
// staging that replaced the in-place copy.
//
// A module directory left mode 0555 refuses every NEW file, so an install that
// copied over it died at copy_file and the package could never be reinstalled
// -- only `chmod -R u+w` then `rm -rf` recovered it. The 0555 arrives by
// staging a module with a plain `cp -R` out of `nix build .#install`, which
// preserves the Nix store's read-only directory mode. Measured on macOS: it is
// the DIRECTORY that blocks, not the 0444 payload files, because POSIX unlink
// consults only the parent's write bit.
//
// The in-place copy also MERGED, so a failure midway left a mixture that was
// neither install. The install now builds a staging tree beside the
// destination and swaps it in.
#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

void clearReadOnly(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    fs::permissions(root, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
        ec.clear();
    }
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const fs::perms kReadOnlyFile = fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read;
const fs::perms kReadOnlyDir  = kReadOnlyFile | fs::perms::owner_exec |
                                fs::perms::group_exec | fs::perms::others_exec;

} // namespace

class ReadOnlyInstallTest : public ::testing::Test {
protected:
    fs::path base;
    fs::path extractedDir;   // stands in for the temp dir an .lgx unpacks into
    fs::path targetDir;      // stands in for the user modules dir
    fs::path moduleDir;      // where this package installs to

    static constexpr const char* kModuleName = "wedge_module";

    void SetUp() override {
        base = fs::temp_directory_path() / ("lgpm_readonly_" + std::to_string(std::rand()));
        extractedDir = base / "extracted";
        targetDir = base / "install";
        moduleDir = targetDir / kModuleName;
        fs::create_directories(targetDir);
    }

    void TearDown() override {
        // These tests deliberately build read-only trees; clear them first or
        // the cleanup leaves the litter that this bug is about.
        clearReadOnly(base);
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    static json moduleManifest(const std::string& version) {
        json manifest;
        manifest["name"] = kModuleName;
        manifest["type"] = "core";
        manifest["version"] = version;
        manifest["main"] = "payload.so";
        return manifest;
    }

    // Lay out <extractedDir>/<variant>/{manifest.json, payload.so, ...extra},
    // replacing anything a previous call left there.
    fs::path writeExtractedPackage(const std::vector<std::string>& extraFiles = {},
                                   const std::string& payload = "new-payload") {
        std::string variant = PackageManagerLib::platformVariantsToTry().front();
        fs::path variantDir = extractedDir / variant;
        std::error_code ec;
        fs::remove_all(variantDir, ec);
        fs::create_directories(variantDir);

        std::ofstream(variantDir / "manifest.json") << moduleManifest("1.0.0").dump(2);
        std::ofstream(variantDir / "payload.so") << payload;

        for (const auto& rel : extraFiles) {
            fs::path p = variantDir / rel;
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "extra:" << rel;
        }
        return variantDir;
    }

    bool install(std::string& errorMsg) {
        PackageManagerLib pm;
        std::string outName;
        return pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                           /*isCoreModule=*/true, outName, errorMsg);
    }

    // Anything in the install directory that is not the module itself: a
    // staging or retired tree the install failed to clean up.
    std::vector<std::string> strayEntries() {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(targetDir, ec)) {
            std::string n = e.path().filename().string();
            if (n != kModuleName) names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<InstalledPackage> scanInstalled() {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(targetDir.string());
        return pm.getInstalledModules();
    }
};

// The core repro: destination directory 0555, payload files 0444. Before the
// fix this failed with "Failed to copy file from .../libextra.dylib to ...".
TEST_F(ReadOnlyInstallTest, InstallsIntoAReadOnlyDestinationDirectory) {
    writeExtractedPackage({"libextra.dylib"});

    fs::create_directories(moduleDir);
    std::ofstream(moduleDir / "manifest.json") << "{}";
    std::ofstream(moduleDir / "payload.so") << "old-payload";
    fs::permissions(moduleDir / "manifest.json", kReadOnlyFile);
    fs::permissions(moduleDir / "payload.so", kReadOnlyFile);
    fs::permissions(moduleDir, kReadOnlyDir);

    std::string errorMsg;
    ASSERT_TRUE(install(errorMsg)) << errorMsg;

    // The new file is what a read-only directory refuses.
    EXPECT_TRUE(fs::exists(moduleDir / "libextra.dylib"));
    EXPECT_EQ(readFile(moduleDir / "payload.so"), "new-payload");
    EXPECT_TRUE(strayEntries().empty()) << "left a working tree behind";

    // And the install must not leave the tree unwritable for the NEXT one.
    EXPECT_TRUE((fs::status(moduleDir).permissions() & fs::perms::owner_write) != fs::perms::none);
    EXPECT_TRUE((fs::status(moduleDir / "payload.so").permissions() & fs::perms::owner_write) != fs::perms::none);
}

// Read-only FILES under a writable directory never blocked POSIX (unlink reads
// the parent's write bit) but do block Windows deletion. Pinned either way.
TEST_F(ReadOnlyInstallTest, InstallsOverReadOnlyPayloadFiles) {
    writeExtractedPackage();

    fs::create_directories(moduleDir);
    std::ofstream(moduleDir / "payload.so") << "old-payload";
    fs::permissions(moduleDir / "payload.so", kReadOnlyFile);

    std::string errorMsg;
    ASSERT_TRUE(install(errorMsg)) << errorMsg;
    EXPECT_EQ(readFile(moduleDir / "payload.so"), "new-payload");
}

// The same wedge one level down: a staged tree is read-only throughout, so the
// old recursion hit a 0555 subdirectory even when the module dir itself was fine.
TEST_F(ReadOnlyInstallTest, AReadOnlySubdirectoryDoesNotBlockTheCopy) {
    writeExtractedPackage({"qml/inner.qml"});

    fs::create_directories(moduleDir / "qml");
    fs::permissions(moduleDir / "qml", kReadOnlyDir);

    std::string errorMsg;
    ASSERT_TRUE(install(errorMsg)) << errorMsg;
    EXPECT_TRUE(fs::exists(moduleDir / "qml" / "inner.qml"));
}

// The merge's other cost: an upgrade that drops a file used to leave the old
// one installed forever, so a module could keep loading a payload its own
// package no longer ships.
TEST_F(ReadOnlyInstallTest, AnUpgradeRemovesAFileTheNewPackageDropped) {
    writeExtractedPackage({"stale.dylib"});
    std::string errorMsg;
    ASSERT_TRUE(install(errorMsg)) << errorMsg;
    ASSERT_TRUE(fs::exists(moduleDir / "stale.dylib"));

    writeExtractedPackage();   // same package, without stale.dylib
    ASSERT_TRUE(install(errorMsg)) << errorMsg;

    EXPECT_FALSE(fs::exists(moduleDir / "stale.dylib"));
    EXPECT_TRUE(fs::exists(moduleDir / "payload.so"));
    EXPECT_TRUE(strayEntries().empty());
}

#ifndef _WIN32
// A failed copy used to leave its half-written module behind: measured, six of
// eight payload files with no manifest.json and no variant, and nothing in
// lgpm ever cleaned it up. A FIFO in the source is just a copy_file that is
// guaranteed to fail; the assertions are about what survives, not the cause.
TEST_F(ReadOnlyInstallTest, LeavesNothingBehindWhenAFreshInstallFails) {
    fs::path variantDir = writeExtractedPackage();
    ASSERT_EQ(::mkfifo((variantDir / "uncopyable.fifo").string().c_str(), 0644), 0);
    ASSERT_FALSE(fs::exists(moduleDir));

    std::string errorMsg;
    ASSERT_FALSE(install(errorMsg));
    EXPECT_FALSE(errorMsg.empty());
    EXPECT_FALSE(fs::exists(moduleDir)) << "a failed install left a partial module directory behind";
    EXPECT_TRUE(strayEntries().empty()) << "a failed install left its staging tree behind";
}

// ...and a destination that PREDATES the install is not ours to touch at all.
// Staging is what makes this exact: the previous install is not merely still
// present, it is byte-for-byte itself, because nothing was ever copied over it.
TEST_F(ReadOnlyInstallTest, AFailedUpgradeLeavesThePreviousInstallUntouched) {
    fs::path variantDir = writeExtractedPackage();
    ASSERT_EQ(::mkfifo((variantDir / "uncopyable.fifo").string().c_str(), 0644), 0);

    fs::create_directories(moduleDir);
    std::ofstream(moduleDir / "manifest.json") << moduleManifest("0.9.0").dump(2);
    std::ofstream(moduleDir / "payload.so") << "old-payload";
    std::ofstream(moduleDir / "prior.txt") << "from-the-previous-install";

    std::string errorMsg;
    ASSERT_FALSE(install(errorMsg));

    EXPECT_EQ(readFile(moduleDir / "payload.so"), "old-payload") << "the payload was half-replaced";
    EXPECT_EQ(readFile(moduleDir / "prior.txt"), "from-the-previous-install");
    EXPECT_TRUE(strayEntries().empty());

    auto modules = scanInstalled();
    ASSERT_EQ(modules.size(), 1u) << "the previous install stopped scanning as installed";
    EXPECT_EQ(modules[0].name, kModuleName);
    EXPECT_EQ(modules[0].version, "0.9.0") << "the manifest was replaced by the failed upgrade's";
}
#endif
