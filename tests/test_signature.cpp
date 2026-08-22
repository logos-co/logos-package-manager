#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include "test_support.h"
#include <lgx.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/**
 * Helper to get the preferred platform variant string.
 * Uses platformVariantsToTry() so the variant matches what install expects
 * (includes -dev suffix for non-portable builds).
 */
static std::string currentVariant() {
    auto variants = PackageManagerLib::platformVariantsToTry();
    return variants.empty() ? "unknown" : variants.front();
}

/**
 * Test fixture for signature-related tests.
 * Creates temp directories and provides helpers to build LGX test packages.
 */
class SignatureTest : public ::testing::Test {
protected:
    fs::path tempDir;
    fs::path modulesDir;
    fs::path uiPluginsDir;
    fs::path keysDir;
    fs::path keyringDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("lgpm_sig_test_" + std::to_string(std::rand()));
        modulesDir = tempDir / "modules";
        uiPluginsDir = tempDir / "ui_plugins";
        keysDir = tempDir / "keys";
        keyringDir = tempDir / "keyring";

        fs::create_directories(modulesDir);
        fs::create_directories(uiPluginsDir);
        fs::create_directories(keysDir);
        fs::create_directories(keyringDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    /**
     * Create a minimal unsigned LGX package with the given name.
     * Adds a fake .so/.dylib file for the current platform variant.
     * Returns the path to the .lgx file.
     */
    fs::path createUnsignedPackage(const std::string& name,
                                    const std::string& type = "core",
                                    const std::string& version = "1.0.0") {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);

        // Create a fake library file
#if defined(__APPLE__)
        std::string libName = name + "_plugin.dylib";
#elif defined(_WIN32)
        std::string libName = name + "_plugin.dll";
#else
        std::string libName = name + "_plugin.so";
#endif
        {
            std::ofstream f(contentDir / libName);
            f << "fake library content";
        }

        // Create a manifest.json inside the content directory
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n"
               << "  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"" << version << "\",\n"
               << "  \"type\": \"" << type << "\",\n"
               << "  \"description\": \"Test package " << name << "\",\n"
               << "  \"category\": \"test\"\n"
               << "}";
        }

        // Create LGX package
        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};

        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};

        // Set version and description on the package manifest
        lgx_set_version(pkg, version.c_str());
        lgx_set_description(pkg, ("Test package " + name).c_str());

        // Add variant with the content directory
        res = lgx_add_variant(pkg, currentVariant().c_str(),
                              contentDir.string().c_str(), libName.c_str());
        if (!res.success) {
            lgx_free_package(pkg);
            return {};
        }

        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);

        if (!res.success) return {};
        return lgxPath;
    }

    /**
     * Create a minimal unsigned LGX package whose only variant is the given
     * (typically non-host) variant name. Used to exercise the install-time
     * variant-mismatch path. Returns the path to the .lgx file.
     */
    fs::path createPackageWithVariant(const std::string& name,
                                      const std::string& variant) {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);

        // Match the host library extension, like createUnsignedPackage.
#if defined(__APPLE__)
        std::string libName = name + "_plugin.dylib";
#elif defined(_WIN32)
        std::string libName = name + "_plugin.dll";
#else
        std::string libName = name + "_plugin.so";
#endif
        {
            std::ofstream f(contentDir / libName);
            f << "fake library content";
        }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n"
               << "  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"1.0.0\",\n"
               << "  \"type\": \"core\",\n"
               << "  \"category\": \"test\"\n"
               << "}";
        }

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        res = lgx_add_variant(pkg, variant.c_str(),
                              contentDir.string().c_str(), libName.c_str());
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    /**
     * Generate a keypair and return the .jwk path.
     */
    fs::path generateKey(const std::string& keyName) {
        lgx_result_t res = lgx_keygen(keyName.c_str(), keysDir.string().c_str());
        if (!res.success) return {};
        return keysDir / (keyName + ".jwk");
    }

    /**
     * Sign a package with the given key. Returns true on success.
     */
    bool signPackage(const fs::path& lgxPath, const fs::path& keyPath,
                     const std::string& signerName = "",
                     const std::string& signerUrl = "") {
        lgx_result_t res = lgx_sign(
            lgxPath.string().c_str(),
            keyPath.string().c_str(),
            signerName.empty() ? nullptr : signerName.c_str(),
            signerUrl.empty() ? nullptr : signerUrl.c_str()
        );
        return res.success;
    }

    /**
     * Read the DID string from a .did file generated by keygen.
     */
    std::string readDid(const std::string& keyName) {
        fs::path didPath = keysDir / (keyName + ".did");
        std::ifstream f(didPath);
        std::string did;
        std::getline(f, did);
        return did;
    }

    /**
     * Create a PackageManagerLib configured with our temp directories.
     */
    PackageManagerLib createPM(SignaturePolicy policy = SignaturePolicy::WARN) {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(modulesDir.string());
        pm.setUserUiPluginsDirectory(uiPluginsDir.string());
        pm.setKeyringDirectory(keyringDir.string());
        pm.setSignaturePolicy(policy);
        return pm;
    }
};

// =============================================================================
// Signature Policy Configuration
// =============================================================================

TEST_F(SignatureTest, DefaultPolicyIsWarn) {
    PackageManagerLib pm;
    EXPECT_EQ(pm.signaturePolicy(), SignaturePolicy::WARN);
}

TEST_F(SignatureTest, SetPolicyNone) {
    PackageManagerLib pm;
    pm.setSignaturePolicy(SignaturePolicy::NONE);
    EXPECT_EQ(pm.signaturePolicy(), SignaturePolicy::NONE);
}

TEST_F(SignatureTest, SetPolicyRequire) {
    PackageManagerLib pm;
    pm.setSignaturePolicy(SignaturePolicy::REQUIRE);
    EXPECT_EQ(pm.signaturePolicy(), SignaturePolicy::REQUIRE);
}

TEST_F(SignatureTest, SetKeyringDirectory) {
    PackageManagerLib pm;
    EXPECT_TRUE(pm.keyringDirectory().empty());
    pm.setKeyringDirectory("/some/path");
    EXPECT_EQ(pm.keyringDirectory(), "/some/path");
}

// =============================================================================
// Signature Verification (standalone)
// =============================================================================

TEST_F(SignatureTest, VerifyUnsignedPackage) {
    auto lgxPath = createUnsignedPackage("test_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM();
    auto result = pm.verifyPackageSignature(lgxPath.string());

    EXPECT_FALSE(result.is_signed);
    EXPECT_FALSE(result.signature_valid);
    EXPECT_TRUE(result.signer_did.empty());
}

TEST_F(SignatureTest, VerifySignedPackage) {
    auto lgxPath = createUnsignedPackage("test_signed");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("testkey");
    ASSERT_FALSE(keyPath.empty());

    ASSERT_TRUE(signPackage(lgxPath, keyPath, "Test Signer", "https://example.com"));

    auto pm = createPM();
    auto result = pm.verifyPackageSignature(lgxPath.string());

    EXPECT_TRUE(result.is_signed);
    EXPECT_TRUE(result.signature_valid);
    EXPECT_TRUE(result.package_valid);
    EXPECT_FALSE(result.signer_did.empty());
    EXPECT_EQ(result.signer_name, "Test Signer");
    EXPECT_EQ(result.signer_url, "https://example.com");
    // Not in keyring, so not trusted
    EXPECT_TRUE(result.trusted_as.empty());
}

TEST_F(SignatureTest, VerifySignedPackageTrustedKey) {
    auto lgxPath = createUnsignedPackage("test_trusted");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("trustkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    // Add key to keyring
    std::string did = readDid("trustkey");
    ASSERT_FALSE(did.empty());
    lgx_result_t addRes = lgx_keyring_add(
        keyringDir.string().c_str(), "trusted-publisher",
        did.c_str(), "Trusted Publisher", nullptr);
    ASSERT_TRUE(addRes.success);

    auto pm = createPM();
    auto result = pm.verifyPackageSignature(lgxPath.string());

    EXPECT_TRUE(result.is_signed);
    EXPECT_TRUE(result.signature_valid);
    EXPECT_TRUE(result.package_valid);
    EXPECT_EQ(result.trusted_as, "trusted-publisher");
}

TEST_F(SignatureTest, VerifySignedPackageWithSignerMetadata) {
    auto lgxPath = createUnsignedPackage("test_meta");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("metakey");
    ASSERT_FALSE(keyPath.empty());

    ASSERT_TRUE(signPackage(lgxPath, keyPath, "Logos Foundation", "https://logos.co"));

    auto pm = createPM();
    auto result = pm.verifyPackageSignature(lgxPath.string());

    EXPECT_TRUE(result.is_signed);
    EXPECT_TRUE(result.signature_valid);
    EXPECT_EQ(result.signer_name, "Logos Foundation");
    EXPECT_EQ(result.signer_url, "https://logos.co");
    // DID should start with did:jwk:
    EXPECT_TRUE(result.signer_did.substr(0, 8) == "did:jwk:");
}

// =============================================================================
// Install Flow: Policy NONE (skip all signature checks)
// =============================================================================

TEST_F(SignatureTest, InstallUnsignedWithPolicyNone) {
    auto lgxPath = createUnsignedPackage("pkg_none_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_FALSE(result.empty()) << "Install should succeed: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

TEST_F(SignatureTest, InstallSignedWithPolicyNone) {
    auto lgxPath = createUnsignedPackage("pkg_none_signed");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("nonekey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_FALSE(result.empty()) << "Install should succeed: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

// =============================================================================
// Install Flow: Policy WARN (accept unsigned with warning, reject invalid)
// =============================================================================

TEST_F(SignatureTest, InstallUnsignedWithPolicyWarn) {
    auto lgxPath = createUnsignedPackage("pkg_warn_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    // Should succeed (with a warning printed to stderr)
    EXPECT_FALSE(result.empty()) << "Install should succeed with warning: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

TEST_F(SignatureTest, InstallSignedValidUntrustedWithPolicyWarn) {
    auto lgxPath = createUnsignedPackage("pkg_warn_untrusted");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("warnkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    // Should succeed (signature valid, just not trusted)
    EXPECT_FALSE(result.empty()) << "Install should succeed: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

TEST_F(SignatureTest, InstallSignedValidTrustedWithPolicyWarn) {
    auto lgxPath = createUnsignedPackage("pkg_warn_trusted");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("warntrust");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    // Trust the key
    std::string did = readDid("warntrust");
    ASSERT_FALSE(did.empty());
    lgx_keyring_add(keyringDir.string().c_str(), "publisher",
                    did.c_str(), nullptr, nullptr);

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_FALSE(result.empty()) << "Install should succeed: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

// =============================================================================
// Install Flow: Policy REQUIRE (reject unsigned and untrusted)
// =============================================================================

TEST_F(SignatureTest, InstallUnsignedWithPolicyRequireRejected) {
    auto lgxPath = createUnsignedPackage("pkg_req_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_TRUE(result.empty()) << "Install should fail for unsigned package";
    EXPECT_FALSE(errorMsg.empty());
    EXPECT_NE(errorMsg.find("unsigned"), std::string::npos);
}

TEST_F(SignatureTest, InstallSignedUntrustedWithPolicyRequireRejected) {
    auto lgxPath = createUnsignedPackage("pkg_req_untrusted");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("reqkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_TRUE(result.empty()) << "Install should fail for untrusted signer";
    EXPECT_FALSE(errorMsg.empty());
    EXPECT_NE(errorMsg.find("untrusted"), std::string::npos);
}

TEST_F(SignatureTest, InstallSignedTrustedWithPolicyRequireAccepted) {
    auto lgxPath = createUnsignedPackage("pkg_req_trusted");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("reqtrust");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    // Trust the key
    std::string did = readDid("reqtrust");
    ASSERT_FALSE(did.empty());
    lgx_keyring_add(keyringDir.string().c_str(), "req-publisher",
                    did.c_str(), nullptr, nullptr);

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_FALSE(result.empty()) << "Install should succeed for trusted signer: " << errorMsg;
    EXPECT_TRUE(errorMsg.empty());
}

// =============================================================================
// Install Flow: Installed files present
// =============================================================================

TEST_F(SignatureTest, InstallCreatesModuleDirectory) {
    auto lgxPath = createUnsignedPackage("install_dir_test");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);

    EXPECT_FALSE(result.empty()) << "Install failed: " << errorMsg;
    EXPECT_TRUE(fs::is_directory(result));
}

// =============================================================================
// Install Flow: Error cases
// =============================================================================

TEST_F(SignatureTest, InstallNonExistentFileReturnsError) {
    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result = pm.installPluginFile("/nonexistent/package.lgx", errorMsg);

    EXPECT_TRUE(result.empty());
    EXPECT_FALSE(errorMsg.empty());
}

TEST_F(SignatureTest, InstallNonLgxFileReturnsError) {
    fs::path txtPath = tempDir / "notapackage.txt";
    { std::ofstream f(txtPath); f << "not a package"; }

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(txtPath.string(), errorMsg);

    EXPECT_TRUE(result.empty());
    EXPECT_FALSE(errorMsg.empty());
    EXPECT_NE(errorMsg.find("LGX"), std::string::npos);
}

// =============================================================================
// Install Flow: Version skip logic
// =============================================================================

TEST_F(SignatureTest, SkipInstallIfAlreadyInstalled) {
    auto lgxPath = createUnsignedPackage("skip_test", "core", "2.0.0");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;

    // First install
    std::string result1 = pm.installPluginFile(lgxPath.string(), errorMsg);
    ASSERT_FALSE(result1.empty()) << "First install failed: " << errorMsg;

    // Second install with skipIfNotNewerVersion
    errorMsg.clear();
    std::string result2 = pm.installPluginFile(lgxPath.string(), errorMsg, true);
    EXPECT_FALSE(result2.empty()) << "Second install should return existing: " << errorMsg;
}

TEST_F(SignatureTest, SkipInstallOlderVersion) {
    // Install v2.0.0 first
    auto lgxPath2 = createUnsignedPackage("ver_test", "core", "2.0.0");
    ASSERT_FALSE(lgxPath2.empty());

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;

    std::string result1 = pm.installPluginFile(lgxPath2.string(), errorMsg);
    ASSERT_FALSE(result1.empty()) << "First install failed: " << errorMsg;

    // Try to install v1.0.0 with skip flag
    auto lgxPath1 = createUnsignedPackage("ver_test", "core", "1.0.0");
    ASSERT_FALSE(lgxPath1.empty());

    errorMsg.clear();
    std::string result2 = pm.installPluginFile(lgxPath1.string(), errorMsg, true);
    // Should return the existing (v2.0.0) directory without error
    EXPECT_FALSE(result2.empty());
}

// =============================================================================
// Keyring Integration
// =============================================================================

TEST_F(SignatureTest, KeyringAddAndVerify) {
    auto lgxPath = createUnsignedPackage("keyring_test");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("krkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    std::string did = readDid("krkey");
    ASSERT_FALSE(did.empty());

    // Verify before adding to keyring
    auto pm = createPM();
    auto result1 = pm.verifyPackageSignature(lgxPath.string());
    EXPECT_TRUE(result1.is_signed);
    EXPECT_TRUE(result1.signature_valid);
    EXPECT_TRUE(result1.trusted_as.empty());

    // Add to keyring
    lgx_keyring_add(keyringDir.string().c_str(), "my-key",
                    did.c_str(), "Test Name", "https://test.com");

    // Verify after adding to keyring
    auto result2 = pm.verifyPackageSignature(lgxPath.string());
    EXPECT_TRUE(result2.is_signed);
    EXPECT_TRUE(result2.signature_valid);
    EXPECT_EQ(result2.trusted_as, "my-key");
}

TEST_F(SignatureTest, KeyringRemoveAndVerify) {
    auto lgxPath = createUnsignedPackage("keyring_rm_test");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("rmkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    std::string did = readDid("rmkey");
    ASSERT_FALSE(did.empty());

    // Add and verify trusted
    lgx_keyring_add(keyringDir.string().c_str(), "rm-key",
                    did.c_str(), nullptr, nullptr);

    auto pm = createPM();
    auto result1 = pm.verifyPackageSignature(lgxPath.string());
    EXPECT_EQ(result1.trusted_as, "rm-key");

    // Remove and verify no longer trusted
    lgx_keyring_remove(keyringDir.string().c_str(), "rm-key");

    auto result2 = pm.verifyPackageSignature(lgxPath.string());
    EXPECT_TRUE(result2.is_signed);
    EXPECT_TRUE(result2.signature_valid);
    EXPECT_TRUE(result2.trusted_as.empty());
}

TEST_F(SignatureTest, KeyringListKeys) {
    auto keyPath1 = generateKey("listkey1");
    auto keyPath2 = generateKey("listkey2");
    ASSERT_FALSE(keyPath1.empty());
    ASSERT_FALSE(keyPath2.empty());

    std::string did1 = readDid("listkey1");
    std::string did2 = readDid("listkey2");
    ASSERT_FALSE(did1.empty());
    ASSERT_FALSE(did2.empty());

    lgx_keyring_add(keyringDir.string().c_str(), "key-one",
                    did1.c_str(), "Publisher One", "https://one.com");
    lgx_keyring_add(keyringDir.string().c_str(), "key-two",
                    did2.c_str(), "Publisher Two", nullptr);

    lgx_keyring_list_t list = lgx_keyring_list(keyringDir.string().c_str());
    ASSERT_EQ(list.count, 2u);

    // Collect names
    std::set<std::string> names;
    for (size_t i = 0; i < list.count; ++i) {
        names.insert(list.keys[i].name);
    }
    EXPECT_TRUE(names.count("key-one"));
    EXPECT_TRUE(names.count("key-two"));

    lgx_free_keyring_list(list);
}

// =============================================================================
// Install Flow: variant mismatch logging (logos-basecamp#191)
// =============================================================================

TEST_F(SignatureTest, InstallVariantMismatchFailsAndLogs) {
    // Package provides only a variant this build never accepts.
    auto lgxPath = createPackageWithVariant("variant_mismatch_pkg",
                                            "totally-bogus-platform");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result;
    std::string logged;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg);
        logged = cap.str();
    }

    // Install must fail with an explanatory error that names the provided variant.
    EXPECT_TRUE(result.empty());
    EXPECT_NE(errorMsg.find("variant"), std::string::npos) << errorMsg;
    EXPECT_NE(errorMsg.find("totally-bogus-platform"), std::string::npos) << errorMsg;

    // ...and a warning line must be logged naming the package and both variant lists.
    EXPECT_NE(logged.find("Warning"), std::string::npos) << logged;
    EXPECT_NE(logged.find("totally-bogus-platform"), std::string::npos) << logged;
    EXPECT_NE(logged.find(lgxPath.string()), std::string::npos) << logged;
}

// =============================================================================
// The trust-anchor gate: what the three policy levels SAY, not just what they
// decide.
//
// Install gating is the TRUST-ANCHOR POLICY: a package is authorised by an
// ACTIVE anchor validating it, and the only anchor set is the local keyring,
// which nothing enters except by an explicit user act. A signer DID in a
// package, a catalog entry, or a downloaded key is a self-assertion and
// establishes nothing. (The separate, weaker question "which same-named
// candidate did the author mean" is dependency disambiguation, and it lives in
// logos-package-downloader's resolver. The two are not the same check and
// neither substitutes for the other.)
//
// The outcomes must therefore be a UNIFORM table, not three different noise
// levels:
//
//   policy    unsigned            invalid sig   valid, unanchored   anchored
//   NONE      install, silent     install       install             install
//   WARN      install + warn      REFUSE        install + WARN      install
//   REQUIRE   refuse              refuse        refuse, names DID    install
//
// The "valid, unanchored" cell at WARN is what these tests add. It used to be
// TOTALLY SILENT — quieter than the unsigned case — so the worse posture
// produced less noise than the better one.
// =============================================================================

TEST_F(SignatureTest, WarnPolicyDiagnosesASignerNoAnchorValidates) {
    auto lgxPath = createUnsignedPackage("pkg_warn_unanchored");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("mallory");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath, "Mallory Publishing"));
    const std::string did = readDid("mallory");
    ASSERT_FALSE(did.empty());

    // Keyring is empty: no active anchor validates this signature.
    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result;
    std::string logged;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg);
        logged = cap.str();
    }

    // WARN still installs — that is the policy's whole point...
    EXPECT_FALSE(result.empty()) << "Install should succeed at WARN: " << errorMsg;
    // ...but it must not do so in silence.
    EXPECT_FALSE(logged.empty())
        << "a package signed by a key no anchor validates installed with no diagnostic";
    EXPECT_NE(logged.find("Warning"), std::string::npos) << logged;
    // The DID is the only actionable thing in the message: it is what the user
    // would add to their keyring if they decided to trust this publisher.
    EXPECT_NE(logged.find(did), std::string::npos)
        << "diagnostic does not name the signer DID; logged: " << logged;
}

TEST_F(SignatureTest, WarnPolicyIsQuietWhenAnAnchorDoesValidateTheSigner) {
    // The control that makes the test above mean something: the warning is
    // about the ANCHOR SET, not about being signed.
    auto lgxPath = createUnsignedPackage("pkg_warn_anchored");
    ASSERT_FALSE(lgxPath.empty());

    auto keyPath = generateKey("anchored");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath, "Anchored Publisher"));
    const std::string did = readDid("anchored");
    ASSERT_FALSE(did.empty());
    ASSERT_TRUE(lgx_keyring_add(keyringDir.string().c_str(), "anchored-publisher",
                                did.c_str(), nullptr, nullptr).success);

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result;
    std::string logged;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg);
        logged = cap.str();
    }

    EXPECT_FALSE(result.empty()) << errorMsg;
    EXPECT_TRUE(logged.empty())
        << "an anchored signer produced a diagnostic; logged: " << logged;
}

TEST_F(SignatureTest, WarnPolicyStillDiagnosesAnUnsignedPackage) {
    // Regression guard on the neighbouring cell: adding the unanchored
    // diagnostic must not disturb the unsigned one.
    auto lgxPath = createUnsignedPackage("pkg_warn_still_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    std::string result;
    std::string logged;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg);
        logged = cap.str();
    }
    EXPECT_FALSE(result.empty()) << errorMsg;
    EXPECT_NE(logged.find("unsigned"), std::string::npos) << logged;
}

TEST_F(SignatureTest, NonePolicySaysNothingAboutTrustAtAll) {
    // NONE governs TRUST only, and at NONE there is nothing to say about it.
    auto lgxPath = createUnsignedPackage("pkg_none_quiet");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("nonekey2");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    auto pm = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    std::string result;
    std::string logged;
    {
        CerrCapture cap;
        result = pm.installPluginFile(lgxPath.string(), errorMsg);
        logged = cap.str();
    }
    EXPECT_FALSE(result.empty()) << errorMsg;
    EXPECT_TRUE(logged.empty()) << logged;
}

TEST_F(SignatureTest, RequirePolicyStillRefusesAnUnanchoredSignerAndNamesTheDid) {
    // The decision half of the same asymmetry: at REQUIRE the anchor set
    // changes the DECISION; at WARN it changes only the DIAGNOSTIC. That is
    // the correct asymmetry and it is deliberate.
    auto lgxPath = createUnsignedPackage("pkg_req_unanchored");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("reqmallory");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));
    const std::string did = readDid("reqmallory");
    ASSERT_FALSE(did.empty());

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    std::string result = pm.installPluginFile(lgxPath.string(), errorMsg);
    EXPECT_TRUE(result.empty()) << "REQUIRE installed a package no anchor validates";
    EXPECT_NE(errorMsg.find(did), std::string::npos) << errorMsg;
}

// A catalog's or a package's own claim about its signer is NOT an anchor.
// There is no API here that promotes one, and there must never be: the keyring
// is the only anchor set, and it is populated only by an explicit user act.
TEST_F(SignatureTest, PackageSelfAssertedSignerNameDoesNotBecomeAnAnchor) {
    auto lgxPath = createUnsignedPackage("pkg_self_asserted");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("selfassert");
    ASSERT_FALSE(keyPath.empty());
    // The package claims a reassuring publisher name and URL. Both are
    // attacker-controlled strings inside the package being judged.
    ASSERT_TRUE(signPackage(lgxPath, keyPath, "Logos Core Team",
                            "https://logos.co"));

    PackageManagerLib pm;
    pm.setKeyringDirectory(keyringDir.string());
    auto verified = pm.verifyPackageSignature(lgxPath.string());
    EXPECT_TRUE(verified.is_signed);
    EXPECT_TRUE(verified.signature_valid);
    EXPECT_EQ(verified.signer_name, "Logos Core Team");
    // ...and it is still anchored to nothing.
    EXPECT_TRUE(verified.trusted_as.empty())
        << "a self-asserted signer name became a trust anchor: " << verified.trusted_as;

    // REQUIRE must refuse it regardless of how the package describes itself.
    pm.setUserModulesDirectory(modulesDir.string());
    pm.setUserUiPluginsDirectory(uiPluginsDir.string());
    pm.setSignaturePolicy(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    EXPECT_TRUE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;
}
