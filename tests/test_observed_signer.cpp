// Who published an installed package, and what a `signer` pin does about it.
//
// Two halves, and the first is the load-bearing one:
//
//   1. RECORDING. Until this change, nothing anywhere on disk said who
//      published an installed package. Every signer string in an install tree
//      was a PIN some dependant declared, never an observation. These tests
//      pin what the `signer` sidecar records — and, at least as importantly,
//      what it refuses to record.
//   2. COMPARING. A pin is checked against that recording, when and only when
//      a pin is present.
//
// The recording tests drive the REAL install path end to end: a real .lgx
// built by liblgx, really signed with a really generated Ed25519 key, really
// anchored in a real keyring, really installed by installPluginFile. Nothing
// here fabricates a sidecar to then read it back.

#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include "package_manager_json.h"
#include <lgx.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::string currentVariant() {
    auto variants = PackageManagerLib::platformVariantsToTry();
    return variants.empty() ? "unknown" : variants.front();
}
}  // namespace

class ObservedSignerTest : public ::testing::Test {
protected:
    fs::path tempDir;
    fs::path modulesDir;
    fs::path uiPluginsDir;
    fs::path keysDir;
    fs::path keyringDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("lgpm_obssig_" + std::to_string(std::rand()));
        modulesDir   = tempDir / "modules";
        uiPluginsDir = tempDir / "ui_plugins";
        keysDir      = tempDir / "keys";
        keyringDir   = tempDir / "keyring";
        fs::create_directories(modulesDir);
        fs::create_directories(uiPluginsDir);
        fs::create_directories(keysDir);
        fs::create_directories(keyringDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    // A minimal, structurally valid, unsigned .lgx for the host variant.
    fs::path createPackage(const std::string& name, const std::string& version = "1.0.0") {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);

#if defined(__APPLE__)
        const std::string libName = name + "_plugin.dylib";
#elif defined(_WIN32)
        const std::string libName = name + "_plugin.dll";
#else
        const std::string libName = name + "_plugin.so";
#endif
        { std::ofstream f(contentDir / libName); f << "fake library content"; }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n"
               << "  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"" << version << "\",\n"
               << "  \"type\": \"core\",\n"
               << "  \"category\": \"test\"\n"
               << "}";
        }

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        lgx_set_version(pkg, version.c_str());
        res = lgx_add_variant(pkg, currentVariant().c_str(),
                              contentDir.string().c_str(), libName.c_str());
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    fs::path generateKey(const std::string& keyName) {
        lgx_result_t res = lgx_keygen(keyName.c_str(), keysDir.string().c_str());
        if (!res.success) return {};
        return keysDir / (keyName + ".jwk");
    }

    std::string readDid(const std::string& keyName) {
        std::ifstream f(keysDir / (keyName + ".did"));
        std::string did;
        std::getline(f, did);
        return did;
    }

    bool signPackage(const fs::path& lgxPath, const fs::path& keyPath) {
        return lgx_sign(lgxPath.string().c_str(), keyPath.string().c_str(),
                        nullptr, nullptr).success;
    }

    void anchor(const std::string& keyName, const std::string& localName) {
        const std::string did = readDid(keyName);
        lgx_keyring_add(keyringDir.string().c_str(), localName.c_str(),
                        did.c_str(), nullptr, nullptr);
    }

    // A package carrying a manifest.sig that NAMES a real DID but does not
    // verify against it — the forged-signature shape.
    //
    // Built by signing honestly and then mutating the manifest underneath the
    // signature: `lgx_set_version` writes straight into the manifest without
    // clearing manifest.sig, and manifest.json is excluded from the content
    // Merkle tree, so the package stays structurally valid (package_valid) and
    // keeps its signature block (is_signed, signer_did = the real DID) while
    // the Ed25519 check over the manifest bytes now fails (!signature_valid).
    //
    // That is exactly the vector this whole gate exists for: signer_did is
    // populated by logos-package BEFORE verification, so anything comparing
    // signer_did alone would accept this.
    fs::path createForgedSignaturePackage(const std::string& name,
                                          const std::string& keyName) {
        fs::path lgxPath = createPackage(name);
        if (lgxPath.empty()) return {};
        fs::path keyPath = generateKey(keyName);
        if (keyPath.empty()) return {};
        if (!signPackage(lgxPath, keyPath)) return {};

        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        lgx_set_version(pkg, "9.9.9");            // signature now covers stale bytes
        lgx_result_t res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    // A package whose VARIANT PAYLOAD carries an extra file of the author's
    // choosing. `variants/<v>/` is copied wholesale into the install
    // directory, so a package author picks the names that land there — which
    // is the whole reason the sidecar namespace has to be defended.
    fs::path createPackageWithPayloadFile(const std::string& name,
                                          const std::string& fileName,
                                          const std::string& content,
                                          const std::string& version = "1.0.0") {
        fs::path lgxPath = tempDir / (name + ".lgx");
        fs::path contentDir = tempDir / (name + "_content");
        fs::create_directories(contentDir);

#if defined(__APPLE__)
        const std::string libName = name + "_plugin.dylib";
#elif defined(_WIN32)
        const std::string libName = name + "_plugin.dll";
#else
        const std::string libName = name + "_plugin.so";
#endif
        { std::ofstream f(contentDir / libName); f << "fake library content"; }
        { std::ofstream f(contentDir / fileName); f << content; }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\n"
               << "  \"name\": \"" << name << "\",\n"
               << "  \"version\": \"" << version << "\",\n"
               << "  \"type\": \"core\",\n"
               << "  \"category\": \"test\"\n"
               << "}";
        }

        lgx_result_t res = lgx_create(lgxPath.string().c_str(), name.c_str());
        if (!res.success) return {};
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        lgx_set_version(pkg, version.c_str());
        res = lgx_add_variant(pkg, currentVariant().c_str(),
                              contentDir.string().c_str(), libName.c_str());
        if (!res.success) { lgx_free_package(pkg); return {}; }
        res = lgx_save(pkg, lgxPath.string().c_str());
        lgx_free_package(pkg);
        if (!res.success) return {};
        return lgxPath;
    }

    PackageManagerLib createPM(SignaturePolicy policy = SignaturePolicy::WARN) {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(modulesDir.string());
        pm.setUserUiPluginsDirectory(uiPluginsDir.string());
        pm.setKeyringDirectory(keyringDir.string());
        pm.setSignaturePolicy(policy);
        return pm;
    }

    // Where a package actually landed.
    //
    // Not simply `modulesDir / name`: installPluginFile routes by the `type`
    // in the ROOT manifest, and the root manifest liblgx synthesises for a
    // package built through lgx_create/lgx_add_variant carries no type (there
    // is no lgx_set_type in the C API), so a fixture package installs into the
    // UI-plugins directory even though its in-variant manifest.json says
    // "core". That is an accident of what liblgx lets a test build, not
    // anything the code under test decides, so the fixture asks where the file
    // is rather than asserting against a guess. Falls back to the modules
    // directory so a "nothing was installed" assertion still names one path.
    fs::path installedDirOf(const std::string& name) const {
        for (const fs::path& base : {modulesDir, uiPluginsDir})
            if (fs::is_directory(base / name)) return base / name;
        return modulesDir / name;
    }

    // Hand-write an installed package directory, for the resolver half. Same
    // shape enumerateManifests reads; `observedSigner` empty means NO sidecar,
    // i.e. an embedded or pre-sidecar install.
    void writeInstalled(const std::string& name,
                        const std::vector<json>& deps = {},
                        const std::string& version = "1.0.0",
                        const std::string& observedSigner = "") {
        fs::path dir = modulesDir / name;
        fs::create_directories(dir);
        json m;
        m["name"] = name;
        m["type"] = "core";
        m["version"] = version;
        m["dependencies"] = deps;
        std::ofstream mf(dir / "manifest.json");
        mf << m.dump(2);
        mf.close();
        if (!observedSigner.empty()) {
            std::ofstream sf(dir / PackageManagerLib::observedSignerFileName());
            sf << observedSigner;
        }
    }

    static json objDep(const std::string& name,
                       const std::string& version = "",
                       const std::string& signer = "") {
        json d;
        d["name"] = name;
        if (!version.empty()) d["version"] = version;
        if (!signer.empty())  d["signer"]  = signer;
        return d;
    }

    static const DependencyTreeNode* childNamed(const DependencyTreeNode& root,
                                                const std::string& name) {
        for (const auto& c : root.children)
            if (c.name == name) return &c;
        return nullptr;
    }
};

// =============================================================================
// 1. RECORDING — what install writes, and what it refuses to write
// =============================================================================

// The positive control this whole change is built on. Before it, a package
// could be signed, its key anchored, and installed under --require-signatures,
// and afterwards NOTHING in the install tree named the publisher.
TEST_F(ObservedSignerTest, SignedAndAnchoredInstallRecordsTheVerifiedDid) {
    auto lgxPath = createPackage("obs_signed");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("obskey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));
    anchor("obskey", "publisher");
    const std::string did = readDid("obskey");
    ASSERT_FALSE(did.empty());

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    // The sidecar exists, next to `variant`, holding the DID we verified.
    const fs::path sidecar =
        installedDirOf("obs_signed") / PackageManagerLib::observedSignerFileName();
    ASSERT_TRUE(fs::exists(sidecar)) << "no observed-signer sidecar at " << sidecar;
    EXPECT_EQ(PackageManagerLib::readInstalledSigner(installedDirOf("obs_signed").string()),
              std::optional<std::string>(did));

    // ...and the scan surfaces it, which is how everything downstream sees it.
    auto pkgs = pm.getInstalledPackages();
    ASSERT_EQ(pkgs.size(), 1u);
    ASSERT_TRUE(pkgs[0].observedSigner.has_value());
    EXPECT_EQ(*pkgs[0].observedSigner, did);
    EXPECT_EQ(json(pkgs[0])["observedSigner"].get<std::string>(), did);
}

// An unsigned package records NOTHING. Not an empty sidecar, not a marker —
// nothing, so it reads back as "unknown" rather than as an identity.
TEST_F(ObservedSignerTest, UnsignedInstallRecordsNothing) {
    auto lgxPath = createPackage("obs_unsigned");
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    EXPECT_FALSE(fs::exists(installedDirOf("obs_unsigned") /
                            PackageManagerLib::observedSignerFileName()));
    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("obs_unsigned").string()).has_value());

    auto pkgs = pm.getInstalledPackages();
    ASSERT_EQ(pkgs.size(), 1u);
    EXPECT_FALSE(pkgs[0].observedSigner.has_value());
    // Absent key, not an empty one: a reader must be able to tell "nothing
    // recorded" from a recorded value without guessing.
    EXPECT_FALSE(json(pkgs[0]).contains("observedSigner"));
}

// THE RECORDING RULE, on its own.
//
// Driven directly rather than only end to end, and that is not laziness: the
// trust-anchor gate refuses a failed-signature package before the write site
// is ever reached with one, so `signature_valid` and `is_signed` produce the
// SAME installed tree today and no end-to-end test can tell them apart. That
// was measured — a build with the rule weakened to `is_signed && !signer_did
// .empty()` passed every test in this file. The rule still has to be the
// strict one, and it still has to be pinned, because the install gate is a
// SEPARATE decision that can move: WARN means "warn, do not refuse", and the
// day it stops refusing this case a claim-based rule starts minting publisher
// identities from attacker-written strings with nothing to notice.
//
// So: the property lives on the predicate, where it is reachable.
TEST_F(ObservedSignerTest, OnlyAVerifiedSignatureCountsAsAnObservation) {
    SignatureVerificationResult r;
    r.package_valid = true;

    // THE FORGERY: a manifest.sig that names a DID and does not verify. This
    // is the case a claim-based rule gets wrong, and the one shape of input an
    // attacker fully controls — the DID is just a string in a file.
    r.is_signed = true;
    r.signature_valid = false;
    r.signer_did = "did:jwk:VICTIM";
    EXPECT_FALSE(PackageManagerLib::observedSignerFrom(r).has_value())
        << "a manifest.sig that merely NAMES a DID was recorded as an observation";

    // Unsigned: nothing to observe.
    r.is_signed = false;
    r.signature_valid = false;
    r.signer_did.clear();
    EXPECT_FALSE(PackageManagerLib::observedSignerFrom(r).has_value());

    // Signed and valid but no DID reported — nothing usable to record.
    r.is_signed = true;
    r.signature_valid = true;
    r.signer_did.clear();
    EXPECT_FALSE(PackageManagerLib::observedSignerFrom(r).has_value());

    // The one case that IS an observation.
    r.signer_did = "did:jwk:REAL";
    EXPECT_EQ(PackageManagerLib::observedSignerFrom(r),
              std::optional<std::string>("did:jwk:REAL"));

    // Trust is a different question and does not enter into it: a valid
    // signature from a key no anchor validates is still an observation.
    r.trusted_as.clear();
    EXPECT_EQ(PackageManagerLib::observedSignerFrom(r),
              std::optional<std::string>("did:jwk:REAL"));
}

// ...and the same property against a REAL forged package, so the premise the
// predicate test rests on is not just asserted: liblgx really does report
// (is_signed, !signature_valid, signer_did = the named DID) for a manifest.sig
// that names a DID it does not verify under, which is what makes signer_did
// alone attacker-controlled.
TEST_F(ObservedSignerTest, ForgedSignatureRecordsNothingNotTheClaimedDid) {
    auto lgxPath = createForgedSignaturePackage("obs_forged", "forgedkey");
    ASSERT_FALSE(lgxPath.empty());
    const std::string claimedDid = readDid("forgedkey");
    ASSERT_FALSE(claimedDid.empty());

    // Precondition: the package really does claim that DID and really does
    // fail verification. Without this the test could pass for the wrong reason.
    auto pm = createPM(SignaturePolicy::WARN);
    auto sig = pm.verifyPackageSignature(lgxPath.string());
    ASSERT_TRUE(sig.is_signed);
    ASSERT_TRUE(sig.package_valid) << "forgery must stay structurally valid: " << sig.error;
    ASSERT_FALSE(sig.signature_valid);
    ASSERT_EQ(sig.signer_did, claimedDid) << "the claimed DID must be the pinned one";
    // The rule, applied to what a real forged package really produces.
    EXPECT_FALSE(PackageManagerLib::observedSignerFrom(sig).has_value());

    // Under NONE nothing is verified at all, so nothing may be recorded from
    // a package the library never looked at.
    auto pmNone = createPM(SignaturePolicy::NONE);
    std::string errorMsg;
    ASSERT_FALSE(pmNone.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;
    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("obs_forged").string()).has_value())
        << "an unverified install recorded a publisher";

    // Under WARN the install is refused outright — and still records nothing.
    std::error_code ec;
    fs::remove_all(installedDirOf("obs_forged"), ec);
    std::string warnError;
    EXPECT_TRUE(pm.installPluginFile(lgxPath.string(), warnError).empty());
    EXPECT_FALSE(fs::exists(installedDirOf("obs_forged") /
                            PackageManagerLib::observedSignerFileName()));
}

// A valid signature from a key NO ANCHOR VALIDATES is still an OBSERVATION.
// Trust and identity are different questions: WARN installs this package, and
// we know exactly who published it even though nobody vouches for them.
TEST_F(ObservedSignerTest, ValidButUnanchoredSignerIsStillObserved) {
    auto lgxPath = createPackage("obs_untrusted");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("untrustedkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));   // deliberately NOT anchored

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    EXPECT_EQ(PackageManagerLib::readInstalledSigner(installedDirOf("obs_untrusted").string()),
              std::optional<std::string>(readDid("untrustedkey")));
}

// Backward compatibility, stated as a property of the READER: an install
// directory with no sidecar yields "unknown", and so does a truncated one. An
// empty string must never come back, or it could compare against a pin or be
// misread as "observed to be unsigned".
TEST_F(ObservedSignerTest, MissingOrEmptySidecarReadsAsUnknownNotEmptyString) {
    writeInstalled("legacy_pkg");                        // no sidecar at all
    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("legacy_pkg").string()).has_value());

    fs::create_directories(installedDirOf("truncated_pkg"));
    { std::ofstream f(installedDirOf("truncated_pkg") /
                      PackageManagerLib::observedSignerFileName()); }   // zero bytes
    auto readBack = PackageManagerLib::readInstalledSigner(
        installedDirOf("truncated_pkg").string());
    EXPECT_FALSE(readBack.has_value())
        << "an empty sidecar read back as the identity '" << readBack.value_or("") << "'";
}

// The sidecar is a SIDECAR: it lands next to `variant`, in the installed
// package directory, and does not disturb the payload.
TEST_F(ObservedSignerTest, SidecarSitsBesideTheVariantFile) {
    auto lgxPath = createPackage("obs_sidecar");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("sidecarkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    const fs::path dir = installedDirOf("obs_sidecar");
    EXPECT_TRUE(fs::exists(dir / "variant"));
    EXPECT_TRUE(fs::exists(dir / PackageManagerLib::observedSignerFileName()));
    EXPECT_TRUE(fs::exists(dir / "manifest.json"));
}


// =============================================================================
// 1b. RECORDING — the sidecar is an OBSERVATION, so only install may write it
//
// Both tests below are about one property: what the `signer` file says is
// what THIS install verified, and nothing else can put a value there. A
// recording that anybody but the verifier can write is not evidence, and a
// pin compared against it is theatre.
// =============================================================================

// ATTACK 1 — the package plants the record itself.
//
// `variants/<v>/` is copied wholesale into the install directory, so a package
// author chooses filenames that land there. An UNSIGNED package that simply
// ships a file called `signer` containing somebody else's DID would otherwise
// be reported as published by that somebody. No key, no signature, no keyring:
// the pin is satisfied by naming the answer.
TEST_F(ObservedSignerTest, APackageCannotPlantItsOwnSignerRecord) {
    auto victimKey = generateKey("victimkey");
    ASSERT_FALSE(victimKey.empty());
    const std::string victimDid = readDid("victimkey");
    ASSERT_FALSE(victimDid.empty());

    // Nothing signs this package. It just carries the name it wants to wear.
    auto lgxPath = createPackageWithPayloadFile(
        "obs_planted", PackageManagerLib::observedSignerFileName(), victimDid);
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    // Nothing was verified, so nothing is recorded — the planted file must not
    // survive as an identity.
    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("obs_planted").string()).has_value())
        << "a package planted its own observed-signer record";

    auto pkgs = pm.getInstalledPackages();
    ASSERT_EQ(pkgs.size(), 1u);
    EXPECT_FALSE(pkgs[0].observedSigner.has_value());

    // ...and the pin it was aimed at must not be satisfied by it.
    writeInstalled("obs_planted_dependant", {objDep("obs_planted", "", victimDid)});
    auto tree = pm.resolveDependencies("obs_planted_dependant");
    ASSERT_TRUE(tree.has_value());
    const auto* child = childNamed(*tree, "obs_planted");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->status, DependencyStatus::SignerUnknown);
}

// ATTACK 2 — the record outlives the package it described.
//
// copyDirectoryContents MERGES into an existing install directory; it does not
// clear it. So a second install that verifies nothing leaves the FIRST
// install's `signer` file in place, and an impostor inherits the identity of
// whoever published the package it replaced. The record has to describe the
// bytes that are there now, which means an install with no observation must
// ERASE any earlier one rather than leave it standing.
TEST_F(ObservedSignerTest, AnInstallThatVerifiesNothingErasesTheEarlierRecord) {
    auto lgxPath = createPackage("obs_replaced");
    ASSERT_FALSE(lgxPath.empty());
    auto keyPath = generateKey("replacedkey");
    ASSERT_FALSE(keyPath.empty());
    ASSERT_TRUE(signPackage(lgxPath, keyPath));
    anchor("replacedkey", "publisher");
    const std::string did = readDid("replacedkey");

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;
    ASSERT_EQ(PackageManagerLib::readInstalledSigner(installedDirOf("obs_replaced").string()),
              std::optional<std::string>(did));

    // A different, UNSIGNED package by the same name lands on top of it.
    auto impostor = createPackageWithPayloadFile("obs_replaced", "impostor.txt", "x");
    ASSERT_FALSE(impostor.empty());
    auto pmWarn = createPM(SignaturePolicy::WARN);
    ASSERT_FALSE(pmWarn.installPluginFile(impostor.string(), errorMsg).empty()) << errorMsg;

    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("obs_replaced").string()).has_value())
        << "an unsigned reinstall inherited the previous publisher's identity";

    writeInstalled("obs_replaced_dependant", {objDep("obs_replaced", "", did)});
    auto tree = pmWarn.resolveDependencies("obs_replaced_dependant");
    ASSERT_TRUE(tree.has_value());
    const auto* child = childNamed(*tree, "obs_replaced");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->status, DependencyStatus::SignerUnknown);
}

// The same plant, shipped READ-ONLY.
//
// Stated plainly about what this test can and cannot catch: on POSIX it passes
// with or without the permission handling, because deleting a file consults
// the PARENT DIRECTORY's write bit and not the file's. It discriminates on
// Windows, which refuses both to delete a FILE_ATTRIBUTE_READONLY file and to
// reopen it for write — so without clearing the bit the planted value survives
// there and nowhere else. That is the exact shape of the read-only payload bug
// clearReadOnlyRecursive was already written for (every .lgx with an icon
// carries a 0444 file, because nix-bundle-lgx copies it out of the Nix store),
// which is why the guard is here rather than waiting for a Windows report.
TEST_F(ObservedSignerTest, AReadOnlyPlantedSignerRecordIsStillDiscarded) {
    auto victimKey = generateKey("rovictim");
    ASSERT_FALSE(victimKey.empty());
    const std::string victimDid = readDid("rovictim");

    auto lgxPath = createPackageWithPayloadFile(
        "obs_ro_planted", PackageManagerLib::observedSignerFileName(), victimDid);
    ASSERT_FALSE(lgxPath.empty());

    auto pm = createPM(SignaturePolicy::WARN);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;

    const fs::path landed =
        installedDirOf("obs_ro_planted") / PackageManagerLib::observedSignerFileName();
    std::error_code ec;
    fs::permissions(landed, fs::perms::owner_read | fs::perms::group_read |
                            fs::perms::others_read,
                    fs::perm_options::replace, ec);

    // Reinstall over it. Nothing verifies, so the record must end up saying
    // nothing — deleted where that is possible, emptied where it is not.
    ASSERT_FALSE(pm.installPluginFile(lgxPath.string(), errorMsg).empty()) << errorMsg;
    EXPECT_FALSE(PackageManagerLib::readInstalledSigner(
                     installedDirOf("obs_ro_planted").string()).has_value())
        << "a read-only planted record survived a reinstall";
}

// The control for both: a reinstall that DOES verify replaces the record
// rather than erasing it, so the fix cannot be "always delete the sidecar".
TEST_F(ObservedSignerTest, AVerifiedReinstallReplacesTheRecordWithTheNewPublisher) {
    auto first = createPackage("obs_rotated");
    ASSERT_FALSE(first.empty());
    auto keyOne = generateKey("rotone");
    ASSERT_TRUE(signPackage(first, keyOne));
    anchor("rotone", "pub_one");

    auto pm = createPM(SignaturePolicy::REQUIRE);
    std::string errorMsg;
    ASSERT_FALSE(pm.installPluginFile(first.string(), errorMsg).empty()) << errorMsg;
    ASSERT_EQ(PackageManagerLib::readInstalledSigner(installedDirOf("obs_rotated").string()),
              std::optional<std::string>(readDid("rotone")));

    // Same name, republished by a second anchored publisher.
    fs::remove(tempDir / "obs_rotated.lgx");
    fs::remove_all(tempDir / "obs_rotated_content");
    auto second = createPackage("obs_rotated", "1.1.0");
    ASSERT_FALSE(second.empty());
    auto keyTwo = generateKey("rottwo");
    ASSERT_TRUE(signPackage(second, keyTwo));
    anchor("rottwo", "pub_two");
    ASSERT_FALSE(pm.installPluginFile(second.string(), errorMsg).empty()) << errorMsg;

    EXPECT_EQ(PackageManagerLib::readInstalledSigner(installedDirOf("obs_rotated").string()),
              std::optional<std::string>(readDid("rottwo")));
}

// =============================================================================
// 2. COMPARING — what a `signer` pin does about the recording
// =============================================================================

// The whole point, in one test: same name, different publisher, and the
// scanner says so.
TEST_F(ObservedSignerTest, PinnedSignerMismatchIsReported) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("app", {objDep("dep", "", "did:jwk:SOMEBODY_ELSE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
    ASSERT_TRUE(dep->requiredSigner.has_value());
    EXPECT_EQ(*dep->requiredSigner, "did:jwk:SOMEBODY_ELSE");
    ASSERT_TRUE(dep->observedSigner.has_value());
    EXPECT_EQ(*dep->observedSigner, "did:jwk:MINE");
    // It IS installed — the report must carry what is on disk, or the reader
    // cannot say "published by X" at all.
    EXPECT_EQ(dep->version, "1.0.0");
}

TEST_F(ObservedSignerTest, PinnedSignerMatchIsInstalled) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("app", {objDep("dep", "", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::Installed);
}

// WHEN AND ONLY WHEN a pin is present. An unpinned edge must resolve exactly
// as before — which is every edge in the fleet today, so this is the
// regression guard for the entire change.
TEST_F(ObservedSignerTest, NoPinMeansTheSignerIsNeverConsulted) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("bare", {json("dep")});                 // plain-string edge
    writeInstalled("ranged", {objDep("dep", "^1.0.0")});   // range, no signer

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    for (const char* root : {"bare", "ranged"}) {
        auto tree = pm.resolveDependencies(root);
        ASSERT_TRUE(tree.has_value()) << root;
        const auto* dep = childNamed(*tree, "dep");
        ASSERT_NE(dep, nullptr) << root;
        EXPECT_EQ(dep->status, DependencyStatus::Installed) << root;
        EXPECT_FALSE(dep->requiredSigner.has_value()) << root;
        // The observation still rides along — it is a property of the package,
        // not of the edge, and a caller may want to display it.
        EXPECT_EQ(dep->observedSigner, std::optional<std::string>("did:jwk:MINE")) << root;
    }
}

// THE DESIGN CALL, pinned. A pin against a package with no recorded publisher
// is SignerUnknown by default: absence of evidence, reported as its own thing.
TEST_F(ObservedSignerTest, PinAgainstUnrecordedSignerIsUnknownByDefault) {
    writeInstalled("dep");                                        // no sidecar
    writeInstalled("app", {objDep("dep", "", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());
    ASSERT_EQ(pm.unknownSignerPolicy(), UnknownSignerPolicy::Lenient);

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerUnknown);
    EXPECT_FALSE(dep->observedSigner.has_value());
    ASSERT_TRUE(dep->requiredSigner.has_value());
    EXPECT_EQ(*dep->requiredSigner, "did:jwk:MINE");
}

// ...and the flip really flips it, in one call, with no other change.
TEST_F(ObservedSignerTest, StrictPolicyTurnsUnknownIntoMismatch) {
    writeInstalled("dep");
    writeInstalled("app", {objDep("dep", "", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());
    pm.setUnknownSignerPolicy(UnknownSignerPolicy::Strict);

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
}

// Absence still outranks everything: a pin cannot be judged against a package
// that is not there, and "install it" is the action either way.
TEST_F(ObservedSignerTest, AbsenceOutranksASignerPin) {
    writeInstalled("app", {objDep("missing", "", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "missing");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::NotInstalled);
    EXPECT_FALSE(dep->observedSigner.has_value());
    // The pin still rides along, so a caller can say which publisher to get it
    // from — the same courtesy `requiredVersion` gets on an absent row.
    ASSERT_TRUE(dep->requiredSigner.has_value());
}

// Identity outranks a range. "requires ^2.0.0, found 1.0.0" would send the
// user hunting for a newer build of a package that is not theirs at ANY
// version; which package it is has to be settled first.
TEST_F(ObservedSignerTest, SignerMismatchOutranksVersionMismatch) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("app", {objDep("dep", "^2.0.0", "did:jwk:SOMEBODY_ELSE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
    // Both constraints still travel, so nothing is lost by the ranking.
    EXPECT_EQ(*dep->requiredVersion, "^2.0.0");
    EXPECT_EQ(*dep->requiredSigner, "did:jwk:SOMEBODY_ELSE");
}

// ...but a DEFINITE version failure outranks a signer we merely could not
// check. Missing evidence must never mask an actionable rejection.
TEST_F(ObservedSignerTest, VersionMismatchOutranksAnUncheckableSigner) {
    writeInstalled("dep", {}, "1.0.0");                   // no sidecar
    writeInstalled("app", {objDep("dep", "^2.0.0", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::VersionMismatch);
}

// A satisfied pin never IMPROVES a status. The right publisher at the wrong
// version is still the wrong version.
TEST_F(ObservedSignerTest, ASatisfiedPinDoesNotRescueAVersionMismatch) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("app", {objDep("dep", "^2.0.0", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::VersionMismatch);
}

// The flat projection is the ONLY one basecamp's load gate reads, so a
// mismatch reachable only down a deeper edge must survive the BFS dedup —
// the same failure mode already fixed for version ranges, one status along.
TEST_F(ObservedSignerTest, DeeperSignerMismatchIsNotMaskedByAShallowerEdge) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("mid", {objDep("dep", "", "did:jwk:SOMEBODY_ELSE")});
    // BFS reaches the bare root->dep edge first; the constrained one is deeper.
    writeInstalled("app", {json("dep"), json("mid")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();

    const DependencyTreeNode* row = nullptr;
    int depRows = 0;
    for (const auto& n : flat) if (n.name == "dep") { row = &n; ++depRows; }
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(depRows, 1) << "one row per package is the flat contract";
    EXPECT_EQ(row->status, DependencyStatus::SignerMismatch);
    EXPECT_EQ(*row->requiredSigner, "did:jwk:SOMEBODY_ELSE");
    // The observation is a property of the package and survives the promotion.
    EXPECT_EQ(row->observedSigner, std::optional<std::string>("did:jwk:MINE"));
}

// ...and promotion is ordered, not pairwise: a signer mismatch reached later
// must still overtake an already-recorded VERSION mismatch.
TEST_F(ObservedSignerTest, SignerMismatchPromotesOverARecordedVersionMismatch) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("mid", {objDep("dep", "", "did:jwk:SOMEBODY_ELSE")});
    writeInstalled("app", {objDep("dep", "^2.0.0"), json("mid")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();
    const DependencyTreeNode* row = nullptr;
    for (const auto& n : flat) if (n.name == "dep") row = &n;
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->status, DependencyStatus::SignerMismatch);
}

// ...while "we could not check" must NOT overtake a definite rejection.
TEST_F(ObservedSignerTest, UnknownDoesNotPromoteOverARecordedVersionMismatch) {
    writeInstalled("dep", {}, "1.0.0");                    // no sidecar
    writeInstalled("mid", {objDep("dep", "", "did:jwk:MINE")});
    writeInstalled("app", {objDep("dep", "^2.0.0"), json("mid")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();
    const DependencyTreeNode* row = nullptr;
    for (const auto& n : flat) if (n.name == "dep") row = &n;
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->status, DependencyStatus::VersionMismatch);
}

// =============================================================================
// 3. THE WIRE — what a consumer on the far side actually reads
// =============================================================================

TEST_F(ObservedSignerTest, SignerStatusesSerialiseWithBothSidesOfTheReport) {
    writeInstalled("dep", {}, "1.0.0", "did:jwk:MINE");
    writeInstalled("app", {objDep("dep", "", "did:jwk:SOMEBODY_ELSE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    json j = json(*tree);
    ASSERT_EQ(j["children"].size(), 1u);
    const json& dep = j["children"][0];

    EXPECT_EQ(dep["status"], "signer_mismatch");
    EXPECT_EQ(dep["requiredSigner"], "did:jwk:SOMEBODY_ELSE");
    EXPECT_EQ(dep["observedSigner"], "did:jwk:MINE");
    // A signer-mismatched package IS installed, so the row must carry what is
    // on disk — the `Installed || VersionMismatch` chain this replaced would
    // have emitted an empty version here.
    EXPECT_EQ(dep["version"], "1.0.0");
    EXPECT_EQ(dep["installType"], "user");
}

TEST_F(ObservedSignerTest, UnknownSerialisesWithNoObservedSignerKey) {
    writeInstalled("dep");
    writeInstalled("app", {objDep("dep", "", "did:jwk:MINE")});

    PackageManagerLib pm;
    pm.setUserModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const json dep = json(*tree)["children"][0];
    EXPECT_EQ(dep["status"], "signer_unknown");
    EXPECT_FALSE(dep.contains("observedSigner"));
    EXPECT_EQ(dep["version"], "1.0.0");
}

TEST_F(ObservedSignerTest, StatusStringsAreStableAndDistinct) {
    EXPECT_STREQ(dependencyStatusToString(DependencyStatus::SignerMismatch), "signer_mismatch");
    EXPECT_STREQ(dependencyStatusToString(DependencyStatus::SignerUnknown),  "signer_unknown");
    // Appended, never inserted: the numeric values cross a shared-library
    // boundary into logos-package-manager-module.
    EXPECT_EQ(static_cast<int>(DependencyStatus::Installed),       0);
    EXPECT_EQ(static_cast<int>(DependencyStatus::NotInstalled),    1);
    EXPECT_EQ(static_cast<int>(DependencyStatus::Cycle),           2);
    EXPECT_EQ(static_cast<int>(DependencyStatus::VersionMismatch), 3);
    EXPECT_EQ(static_cast<int>(DependencyStatus::SignerMismatch),  4);
    EXPECT_EQ(static_cast<int>(DependencyStatus::SignerUnknown),   5);
}

TEST_F(ObservedSignerTest, EverySignerStatusCountsAsOnDisk) {
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::Installed));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::VersionMismatch));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::SignerMismatch));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::SignerUnknown));
    EXPECT_FALSE(nodeResolvedToAnInstalledPackage(DependencyStatus::NotInstalled));
    EXPECT_FALSE(nodeResolvedToAnInstalledPackage(DependencyStatus::Cycle));
}
