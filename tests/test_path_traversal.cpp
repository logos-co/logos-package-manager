// Regression tests for F-006: an unvalidated manifest "name" field used as the
// install destination directory enables path traversal / arbitrary file write
// outside the user modules dir.
//
// The attack: a crafted .lgx ships a manifest.json whose "name" is something
// like "../../evil" or "/abs/path". copyLibraryFromExtracted() reads that name
// and path-joins it onto the target directory. Because std::filesystem treats
// ".." as a real component and an absolute path as a full reset, the copied
// files land outside the sandboxed install directory.
#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

class PathTraversalTest : public ::testing::Test {
protected:
    fs::path base;
    fs::path extractedDir;   // stands in for the temp dir an .lgx unpacks into
    fs::path targetDir;      // stands in for the user modules dir

    void SetUp() override {
        base = fs::temp_directory_path() / ("lgpm_traversal_" + std::to_string(std::rand()));
        extractedDir = base / "extracted";
        targetDir = base / "install";
        fs::create_directories(targetDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    // Lay out an extracted package: <extractedDir>/<variant>/{manifest.json, payload.so}
    // with the supplied manifest "name". Returns the variant directory used.
    fs::path writeExtractedPackage(const std::string& manifestName) {
        // copyLibraryFromExtracted scans platformVariantsToTry() and uses the
        // first variant directory that exists, so create the primary one.
        std::string variant = PackageManagerLib::platformVariantsToTry().front();
        fs::path variantDir = extractedDir / variant;
        fs::create_directories(variantDir);

        json manifest;
        manifest["name"] = manifestName;
        manifest["type"] = "core";
        manifest["version"] = "1.0.0";
        std::ofstream(variantDir / "manifest.json") << manifest.dump(2);

        // A payload that, if the traversal succeeds, would be planted outside
        // targetDir — e.g. an attacker-controlled plugin.
        std::ofstream(variantDir / "payload.so") << "malicious-plugin";
        return variantDir;
    }
};

// Core repro: a "../escaped" name must be rejected and nothing written outside.
TEST_F(PathTraversalTest, RejectsParentTraversalInManifestName) {
    writeExtractedPackage("../escaped_marker");

    PackageManagerLib pm;
    std::string outName, errorMsg;
    bool ok = pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                          /*isCoreModule=*/true, outName, errorMsg);

    EXPECT_FALSE(ok) << "Traversal name should be rejected";
    EXPECT_FALSE(errorMsg.empty());

    // "../escaped_marker" relative to targetDir would land as a sibling of it.
    fs::path escapeTarget = base / "escaped_marker";
    EXPECT_FALSE(fs::exists(escapeTarget))
        << "Files were written outside the target dir at " << escapeTarget;
    EXPECT_FALSE(fs::exists(escapeTarget / "payload.so"));
}

// A longer "../.." chain must also be rejected. The escape target is steered
// back into this test's own (unique, self-cleaning) base dir rather than a
// fixed system path, so a regression can't contaminate another run or collide
// with a real file — but the name still carries multiple ".." segments.
TEST_F(PathTraversalTest, RejectsDeepParentTraversal) {
    // From targetDir (base/install): ".." -> base, "../.." -> temp dir, then
    // "/<base name>/deep_marker" climbs back down to base/deep_marker.
    std::string maliciousName = "../../" + base.filename().string() + "/deep_marker";
    writeExtractedPackage(maliciousName);

    PackageManagerLib pm;
    std::string outName, errorMsg;
    bool ok = pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                          true, outName, errorMsg);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(fs::exists(base / "deep_marker"));
    EXPECT_FALSE(fs::exists(base / "deep_marker" / "payload.so"));
}

// An absolute name resets the join entirely under std::filesystem::operator/.
TEST_F(PathTraversalTest, RejectsAbsolutePathName) {
    fs::path absTarget = base / "abs_escape";
    writeExtractedPackage(absTarget.string());

    PackageManagerLib pm;
    std::string outName, errorMsg;
    bool ok = pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                          true, outName, errorMsg);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(fs::exists(absTarget));
}

// A name carrying a nested separator ("a/b") would create dirs under target;
// reject it too so the install stays a single flat module directory.
TEST_F(PathTraversalTest, RejectsEmbeddedSeparator) {
    writeExtractedPackage("nested/child");

    PackageManagerLib pm;
    std::string outName, errorMsg;
    bool ok = pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                          true, outName, errorMsg);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(fs::exists(targetDir / "nested"));
}

// Sanity: a legitimate name still installs into <targetDir>/<name>.
TEST_F(PathTraversalTest, AcceptsValidName) {
    writeExtractedPackage("good_module");

    PackageManagerLib pm;
    std::string outName, errorMsg;
    bool ok = pm.copyLibraryFromExtracted(extractedDir.string(), targetDir.string(),
                                          true, outName, errorMsg);

    EXPECT_TRUE(ok) << errorMsg;
    EXPECT_EQ(outName, "good_module");
    EXPECT_TRUE(fs::exists(targetDir / "good_module" / "payload.so"));
}

// Direct unit coverage of the validator itself.
TEST(IsValidModuleNameTest, AcceptsPlainNames) {
    EXPECT_TRUE(PackageManagerLib::isValidModuleName("waku_module"));
    EXPECT_TRUE(PackageManagerLib::isValidModuleName("chat-module"));
    EXPECT_TRUE(PackageManagerLib::isValidModuleName("Module.v2"));
}

TEST(IsValidModuleNameTest, RejectsTraversalAndSeparators) {
    EXPECT_FALSE(PackageManagerLib::isValidModuleName(""));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName("."));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName(".."));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName("../etc"));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName("a/b"));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName("/abs"));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName("a\\b"));
    EXPECT_FALSE(PackageManagerLib::isValidModuleName(std::string("nul\0byte", 8)));
}
