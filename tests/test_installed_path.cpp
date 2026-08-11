// Tests for the path installPluginFile() reports back through its
// `installedPluginPath` out-parameter.
//
// Regression context: a QML-only ui_qml package (no backend library, so its
// manifest carries "main": {}) installed correctly but left this out-param
// EMPTY. Callers use it as the "something was installed" signal —
// package_manager_module gated its uiPluginFileInstalled event on it and put
// it in response["path"], and logos-package-manager-ui treats an empty "path"
// as failure — so a perfectly good install was rendered as a red RETRY and the
// plugin only showed up after an app restart.
//
// The "main": {} shape cannot be produced through liblgx's C API: lgx_add_variant
// only accepts a main-less directory variant when the package manifest already
// declares type "ui_qml", and there is no lgx_set_type(). So the manifest-shape
// matrix is driven directly through PackageManagerLib::resolveInstalledPackagePath(),
// and the end-to-end .lgx install tests cover the paths liblgx can express.

#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include "test_support.h"
#include <lgx.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string hostVariant() {
    auto variants = PackageManagerLib::platformVariantsToTry();
    return variants.empty() ? "unknown" : variants.front();
}

// The extension resolveInstalledPackagePath() appends when it synthesises a
// main file name from the module name. Windows is tested first deliberately —
// a platform chain that tests it last is how Windows ends up on the Unix branch.
#if defined(_WIN32)
constexpr const char* kLibExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kLibExt = ".dylib";
#else
constexpr const char* kLibExt = ".so";
#endif

void writeFile(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

} // namespace

// =============================================================================
// resolveInstalledPackagePath() — manifest-shape matrix
// =============================================================================

class InstalledPathTest : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        root = fs::temp_directory_path() / ("lgpm_instpath_" + std::to_string(std::rand()));
        fs::create_directories(root);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // Creates <root>/<name>/manifest.json with the given JSON body.
    fs::path makeModuleDir(const std::string& name, const json& manifest) {
        fs::path dir = root / name;
        fs::create_directories(dir);
        std::ofstream mf(dir / "manifest.json");
        mf << manifest.dump(2);
        return dir;
    }

    std::string resolve(const fs::path& dir) {
        return PackageManagerLib::resolveInstalledPackagePath(
            dir.string(), PackageManagerLib::platformVariantsToTry());
    }
};

// THE regression: ui_qml with an empty "main" map must report the module
// directory, not an empty string.
TEST_F(InstalledPathTest, UiQmlWithEmptyMainReportsModuleDirectory) {
    json m;
    m["name"] = "hello_ui";
    m["version"] = "1.0.0";
    m["type"] = "ui_qml";
    m["view"] = "Main.qml";
    m["main"] = json::object();
    fs::path dir = makeModuleDir("hello_ui", m);
    writeFile(dir / "Main.qml", "import QtQuick\nItem {}\n");
    writeFile(dir / "qmldir", "module hello_ui\n");

    EXPECT_EQ(resolve(dir), dir.string());
}

// Same, with "main" absent from the manifest entirely.
TEST_F(InstalledPathTest, UiQmlWithNoMainKeyReportsModuleDirectory) {
    json m;
    m["name"] = "hello_ui";
    m["version"] = "1.0.0";
    m["type"] = "ui_qml";
    m["view"] = "Main.qml";
    fs::path dir = makeModuleDir("hello_ui", m);
    writeFile(dir / "Main.qml", "import QtQuick\nItem {}\n");

    EXPECT_EQ(resolve(dir), dir.string());
}

// A QML-only package must NOT have a main file guessed from its module name,
// even when a same-named library happens to sit in the directory.
TEST_F(InstalledPathTest, UiQmlDoesNotSynthesiseMainFromModuleName) {
    json m;
    m["name"] = "hello_ui";
    m["version"] = "1.0.0";
    m["type"] = "ui_qml";
    m["main"] = json::object();
    fs::path dir = makeModuleDir("hello_ui", m);
    writeFile(dir / (std::string("hello_ui") + kLibExt), "not the entry point");

    EXPECT_EQ(resolve(dir), dir.string());
}

// A ui_qml package that DOES ship a backend keeps reporting the backend file —
// this is the shape every pre-existing ui_qml package (wallet_ui, accounts_ui)
// has, and it must not regress to the directory.
TEST_F(InstalledPathTest, UiQmlWithBackendReportsMainFile) {
    const std::string lib = std::string("hello_ui_plugin") + kLibExt;
    json m;
    m["name"] = "hello_ui";
    m["version"] = "1.0.0";
    m["type"] = "ui_qml";
    m["main"] = { { hostVariant(), lib } };
    fs::path dir = makeModuleDir("hello_ui", m);
    writeFile(dir / lib, "fake backend");

    EXPECT_EQ(resolve(dir), (dir / lib).string());
}

TEST_F(InstalledPathTest, CoreModuleReportsMainFile) {
    const std::string lib = std::string("waku_module_plugin") + kLibExt;
    json m;
    m["name"] = "waku_module";
    m["version"] = "1.0.0";
    m["type"] = "core";
    m["main"] = { { hostVariant(), lib } };
    fs::path dir = makeModuleDir("waku_module", m);
    writeFile(dir / lib, "fake plugin");

    EXPECT_EQ(resolve(dir), (dir / lib).string());
}

// "main" as a plain string (legacy single-variant manifests).
TEST_F(InstalledPathTest, StringMainReportsMainFile) {
    const std::string lib = std::string("legacy_plugin") + kLibExt;
    json m;
    m["name"] = "legacy";
    m["version"] = "1.0.0";
    m["type"] = "core";
    m["main"] = lib;
    fs::path dir = makeModuleDir("legacy", m);
    writeFile(dir / lib, "fake plugin");

    EXPECT_EQ(resolve(dir), (dir / lib).string());
}

// Non-ui_qml with no "main": the module name is used as the base name and the
// platform extension appended.
TEST_F(InstalledPathTest, CoreWithoutMainSynthesisesFromModuleName) {
    json m;
    m["name"] = "guessed";
    m["version"] = "1.0.0";
    m["type"] = "core";
    fs::path dir = makeModuleDir("guessed", m);
    const std::string lib = std::string("guessed") + kLibExt;
    writeFile(dir / lib, "fake plugin");

    EXPECT_EQ(resolve(dir), (dir / lib).string());
}

// A declared main that is not in the payload is a packaging defect, not an
// install failure: report the directory AND say so on stderr rather than
// silently handing back an empty path.
TEST_F(InstalledPathTest, DeclaredButMissingMainFallsBackToDirectoryAndWarns) {
    json m;
    m["name"] = "broken";
    m["version"] = "1.0.0";
    m["type"] = "core";
    m["main"] = { { hostVariant(), std::string("ghost") + kLibExt } };
    fs::path dir = makeModuleDir("broken", m);

    std::string out;
    {
        CerrCapture cap;
        out = resolve(dir);
        EXPECT_NE(cap.str().find("has no main file at"), std::string::npos)
            << "stderr was: " << cap.str();
    }
    EXPECT_EQ(out, dir.string());
}

// A module directory with no manifest at all still resolves to the directory.
TEST_F(InstalledPathTest, NoManifestFallsBackToDirectory) {
    fs::path dir = root / "bare";
    fs::create_directories(dir);
    CerrCapture cap;  // suppress the packaging warning
    EXPECT_EQ(resolve(dir), dir.string());
}

// Only a directory that does not exist yields an empty result.
TEST_F(InstalledPathTest, MissingDirectoryYieldsEmpty) {
    CerrCapture cap;
    EXPECT_TRUE(resolve(root / "does_not_exist").empty());
}

// =============================================================================
// End-to-end installPluginFile() — the out-param on a real .lgx
// =============================================================================

class InstallOutParamTest : public ::testing::Test {
protected:
    fs::path tempDir;
    fs::path modulesDir;
    fs::path uiPluginsDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("lgpm_outparam_" + std::to_string(std::rand()));
        modulesDir = tempDir / "modules";
        uiPluginsDir = tempDir / "ui_plugins";
        fs::create_directories(modulesDir);
        fs::create_directories(uiPluginsDir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    // Builds a .lgx whose single (host) variant is `contentDir`, declaring
    // `mainName` as its main file. `mainName` need not exist in contentDir —
    // that is exactly the "declared but missing" case we want to exercise.
    fs::path createPackage(const std::string& name,
                           const std::string& mainName,
                           bool includeMainFile,
                           const std::string& version = "1.0.0") {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);
        writeFile(contentDir / "Main.qml", "import QtQuick\nItem {}\n");
        if (includeMainFile)
            writeFile(contentDir / mainName, "fake library content");

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        lgx_set_version(pkg, version.c_str());
        res = lgx_add_variant(pkg, hostVariant().c_str(),
                              contentDir.string().c_str(), mainName.c_str());
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    PackageManagerLib createPM() {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(modulesDir.string());
        pm.setUserUiPluginsDirectory(uiPluginsDir.string());
        pm.setSignaturePolicy(SignaturePolicy::NONE);
        return pm;
    }
};

TEST_F(InstallOutParamTest, ReportsMainFileWhenPackageShipsOne) {
    const std::string lib = std::string("with_main_plugin") + kLibExt;
    auto lgxPath = createPackage("with_main", lib, /*includeMainFile=*/true);
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM();
    std::string errorMsg;
    std::string installedPath;
    bool isCore = true;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg, false,
                                              &installedPath, &isCore);

    ASSERT_FALSE(result.empty()) << errorMsg;
    EXPECT_FALSE(isCore);
    EXPECT_EQ(installedPath, (uiPluginsDir / "with_main" / lib).string());
    EXPECT_TRUE(fs::exists(installedPath));
}

// The install succeeds and the out-param must still identify the package.
// Before the fix this came back empty, which every caller reads as failure.
TEST_F(InstallOutParamTest, ReportsModuleDirectoryWhenMainFileIsAbsent) {
    const std::string lib = std::string("no_main_plugin") + kLibExt;
    auto lgxPath = createPackage("no_main", lib, /*includeMainFile=*/false);
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM();
    std::string errorMsg;
    std::string installedPath;
    std::string result;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg, false,
                                      &installedPath, nullptr);
    }

    ASSERT_FALSE(result.empty()) << errorMsg;
    EXPECT_FALSE(installedPath.empty())
        << "a successful install must never report an empty path";
    EXPECT_EQ(installedPath, (uiPluginsDir / "no_main").string());
    EXPECT_TRUE(fs::is_directory(installedPath));
}

// The skipIfNotNewerVersion early return used to hardcode isCoreModule=false,
// so skipping an already-current CORE module made package_manager_module emit
// uiPluginFileInstalled for it. The type now comes off the installed manifest.
TEST_F(InstallOutParamTest, SkippedInstallReportsTypeFromInstalledManifest) {
    // Pre-seed a newer copy of a CORE module in the user modules directory.
    fs::path existing = modulesDir / "already_here";
    fs::create_directories(existing);
    {
        json m;
        m["name"] = "already_here";
        m["version"] = "2.0.0";
        m["type"] = "core";
        std::ofstream mf(existing / "manifest.json");
        mf << m.dump(2);
    }

    const std::string lib = std::string("already_here_plugin") + kLibExt;
    auto lgxPath = createPackage("already_here", lib, true, "1.0.0");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM();
    std::string errorMsg;
    std::string installedPath;
    bool isCore = false;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg, /*skip=*/true,
                                              &installedPath, &isCore);

    EXPECT_EQ(result, existing.string());
    EXPECT_EQ(installedPath, existing.string());
    EXPECT_TRUE(isCore) << "a skipped core install must not report itself as a UI plugin";
}
