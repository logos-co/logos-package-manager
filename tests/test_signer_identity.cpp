// Who signed an installed package, and what a `signer` pin does about it.
//
// The evidence is the package's OWN manifest.sig, carried into the install tree
// beside the manifest.json it signs. Every question is settled by an Ed25519
// check against a key the ASKER supplies, so nothing on disk is taken on trust.
//
// Every test drives the REAL install path — a real .lgx built by liblgx, really
// signed, really installed by installPluginFile — rather than fabricating
// evidence in order to read it back.

#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include "package_manager_json.h"
#include <lgx.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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

class SignerIdentityTest : public ::testing::Test {
protected:
    fs::path tempDir, modulesDir, uiPluginsDir, keysDir, keyringDir;

    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("lgpm_signer_" + std::to_string(std::rand()));
        modulesDir   = tempDir / "modules";
        uiPluginsDir = tempDir / "ui_plugins";
        keysDir      = tempDir / "keys";
        keyringDir   = tempDir / "keyring";
        for (auto* d : {&modulesDir, &uiPluginsDir, &keysDir, &keyringDir})
            fs::create_directories(*d);
    }
    void TearDown() override { std::error_code ec; fs::remove_all(tempDir, ec); }

    static std::string libNameFor(const std::string& name) {
#if defined(__APPLE__)
        return name + "_plugin.dylib";
#elif defined(_WIN32)
        return name + "_plugin.dll";
#else
        return name + "_plugin.so";
#endif
    }

    // A structurally valid unsigned .lgx for the host variant. `extraName`/
    // `extraBody` put a file inside variants/<v>/, which install copies wholesale
    // into the install directory — so a package author picks names that land
    // there. `extraReadOnly` ships it 0444, as everything out of the Nix store is.
    fs::path createPackage(const std::string& name,
                           const std::string& version = "1.0.0",
                           const std::string& extraName = "",
                           const std::string& extraBody = "",
                           bool extraReadOnly = false) {
        fs::path lgxPath = tempDir / (name + "-" + version + ".lgx");
        fs::path contentDir = tempDir / (name + "-" + version + "_content");
        fs::create_directories(contentDir);
        const std::string libName = libNameFor(name);
        { std::ofstream f(contentDir / libName); f << "payload " << name << " " << version; }
        if (!extraName.empty()) {
            { std::ofstream f(contentDir / extraName); f << extraBody; }
            if (extraReadOnly) {
                std::error_code ec;
                fs::permissions(contentDir / extraName,
                                fs::perms::owner_read | fs::perms::group_read |
                                    fs::perms::others_read,
                                fs::perm_options::replace, ec);
            }
        }
        {
            std::ofstream mf(contentDir / "manifest.json");
            mf << "{\"name\":\"" << name << "\",\"version\":\"" << version
               << "\",\"type\":\"core\",\"category\":\"test\"}";
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
        return res.success ? lgxPath : fs::path{};
    }

    fs::path generateKey(const std::string& keyName) {
        return lgx_keygen(keyName.c_str(), keysDir.string().c_str()).success
                   ? keysDir / (keyName + ".jwk") : fs::path{};
    }
    std::string readDid(const std::string& keyName) {
        std::ifstream f(keysDir / (keyName + ".did"));
        std::string did; std::getline(f, did); return did;
    }
    bool signPackage(const fs::path& lgxPath, const fs::path& keyPath) {
        return lgx_sign(lgxPath.string().c_str(), keyPath.string().c_str(),
                        nullptr, nullptr).success;
    }
    void anchor(const std::string& keyName, const std::string& localName) {
        lgx_keyring_add(keyringDir.string().c_str(), localName.c_str(),
                        readDid(keyName).c_str(), nullptr, nullptr);
    }
    // The manifest.sig a package carries, read through the same getter install
    // uses — lets a test replay a GENUINE signature somewhere it does not belong.
    std::string signatureOf(const fs::path& lgxPath) {
        lgx_package_t pkg = lgx_load(lgxPath.string().c_str());
        if (!pkg) return {};
        const char* sig = lgx_get_manifest_sig_json(pkg);
        std::string out = sig ? sig : "";
        lgx_free_package(pkg);
        return out;
    }

    PackageManagerLib createPM(SignaturePolicy policy = SignaturePolicy::WARN) {
        PackageManagerLib pm;
        pm.setUserModulesDirectory(modulesDir.string());
        pm.setUserUiPluginsDirectory(uiPluginsDir.string());
        pm.setKeyringDirectory(keyringDir.string());
        pm.setSignaturePolicy(policy);
        return pm;
    }

    // Where a package actually landed. Not simply `modulesDir / name`:
    // installPluginFile routes by the ROOT manifest's `type`, and the C API has
    // no lgx_set_type, so a fixture package lands in ui_plugins even though its
    // in-variant manifest.json says "core". Ask rather than guess.
    fs::path installedDirOf(const std::string& name) const {
        for (const fs::path& base : {modulesDir, uiPluginsDir})
            if (fs::is_directory(base / name)) return base / name;
        return modulesDir / name;
    }

    static std::string slurp(const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss; ss << f.rdbuf(); return ss.str();
    }
    static void overwrite(const fs::path& p, const std::string& body) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc); f << body;
    }
    // Rewrite ONLY the `did` field, leaving a genuine signature by somebody else
    // in place.
    static std::string relabelDid(const std::string& sigJson, const std::string& did) {
        json j = json::parse(sigJson);
        j["did"] = did;
        return j.dump(2);
    }

    // Build, sign, anchor and really install a package; returns the publisher's
    // DID. There is no shortcut — satisfying a pin needs a real signature.
    std::string installSigned(const std::string& name, const std::string& keyName,
                              const std::string& version = "1.0.0") {
        auto lgx = createPackage(name, version);
        if (lgx.empty()) return {};
        auto key = generateKey(keyName);
        if (key.empty()) return {};
        if (!signPackage(lgx, key)) return {};
        anchor(keyName, keyName);
        auto pm = createPM();
        std::string err;
        return pm.installPluginFile(lgx.string(), err).empty() ? std::string{}
                                                              : readDid(keyName);
    }

    // A hand-written installed package: a manifest and nothing else — the shape
    // of every EMBEDDED package (none passes through installPluginFile) and the
    // "no evidence" case.
    void writeInstalled(const std::string& name,
                        const std::vector<json>& deps = {},
                        const std::string& version = "1.0.0") {
        fs::path dir = modulesDir / name;
        fs::create_directories(dir);
        json m{{"name", name}, {"type", "core"}, {"version", version},
               {"dependencies", deps}};
        std::ofstream mf(dir / "manifest.json"); mf << m.dump(2);
    }

    static json objDep(const std::string& name, const std::string& version = "",
                       const std::string& signer = "") {
        json d{{"name", name}};
        if (!version.empty()) d["version"] = version;
        if (!signer.empty())  d["signer"]  = signer;
        return d;
    }
    static const DependencyTreeNode* childNamed(const DependencyTreeNode& root,
                                                const std::string& name) {
        for (const auto& c : root.children) if (c.name == name) return &c;
        return nullptr;
    }
    DependencyStatus statusOf(const std::string& root, const std::string& dep) {
        auto tree = createPM().resolveDependencies(root);
        EXPECT_TRUE(tree.has_value());
        if (!tree) return DependencyStatus::NotInstalled;
        const auto* n = childNamed(*tree, dep);
        EXPECT_NE(n, nullptr);
        return n ? n->status : DependencyStatus::NotInstalled;
    }
};

// 1. CARRYING — the signature must arrive with the bytes it signs

TEST_F(SignerIdentityTest, InstallCarriesTheSignatureBesideTheSignedBytes) {
    auto lgx = createPackage("carry");
    ASSERT_FALSE(lgx.empty());
    auto key = generateKey("pub");
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(signPackage(lgx, key));
    anchor("pub", "publisher");

    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;

    const fs::path dir = installedDirOf("carry");
    ASSERT_TRUE(fs::exists(dir / "manifest.json"));
    ASSERT_TRUE(fs::exists(dir / "manifest.sig"));
    EXPECT_EQ(slurp(dir / "manifest.sig"), signatureOf(lgx));
}

// Holds by construction: extractLgxPackage writes lgx_get_manifest_json(), the
// same expression Package::signPackage() signs. Pinned here because a drift
// would read as a mismatch on every pin in the fleet.
TEST_F(SignerIdentityTest, TheInstalledManifestIsTheBytesThatWereSigned) {
    const std::string did = installSigned("bytes", "pub");
    ASSERT_FALSE(did.empty());
    const fs::path dir = installedDirOf("bytes");

    const std::string installedManifest = slurp(dir / "manifest.json");
    const std::string installedSig      = slurp(dir / "manifest.sig");
    ASSERT_FALSE(installedManifest.empty());
    ASSERT_FALSE(installedSig.empty());

    EXPECT_EQ(lgx_check_manifest_signature(installedManifest.data(),
                                           installedManifest.size(),
                                           installedSig.c_str(), did.c_str()),
              LGX_SIG_CHECK_OK)
        << "the installed manifest is not the manifest that was signed";
}

TEST_F(SignerIdentityTest, AnUnsignedInstallCarriesNoSignature) {
    auto lgx = createPackage("plain");
    ASSERT_FALSE(lgx.empty());
    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;

    EXPECT_FALSE(fs::exists(installedDirOf("plain") / "manifest.sig"));

    // Absent, not "unsigned": deliberately never spelled as a claim.
    auto pkgs = createPM().getInstalledPackages();
    const InstalledPackage* found = nullptr;
    for (const auto& p : pkgs) if (p.name == "plain") found = &p;
    ASSERT_NE(found, nullptr);
    EXPECT_FALSE(found->signerDid.has_value());
}

TEST_F(SignerIdentityTest, ASignedInstallReportsItsSelfAssertedSigner) {
    const std::string did = installSigned("named", "pub");
    ASSERT_FALSE(did.empty());

    auto pkgs = createPM().getInstalledPackages();
    const InstalledPackage* found = nullptr;
    for (const auto& p : pkgs) if (p.name == "named") found = &p;
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->signerDid, std::optional<std::string>(did));
}

// The reported DID is checked against the key it itself names, never taken on
// trust.
TEST_F(SignerIdentityTest, ASignatureThatDoesNotVerifyReportsNoSigner) {
    const std::string did = installSigned("broken", "pub");
    ASSERT_FALSE(did.empty());
    const fs::path dir = installedDirOf("broken");

    json sig = json::parse(slurp(dir / "manifest.sig"));
    sig["signature"] = std::string(86, 'A') + "==";      // right shape, wrong bytes
    overwrite(dir / "manifest.sig", sig.dump(2));

    auto pkgs = createPM().getInstalledPackages();
    const InstalledPackage* found = nullptr;
    for (const auto& p : pkgs) if (p.name == "broken") found = &p;
    ASSERT_NE(found, nullptr);
    EXPECT_FALSE(found->signerDid.has_value());
}

// 2. THE ATTACKS. None is defended against by name; each fails because forging
// a match needs an Ed25519 signature under a key the attacker does not have.

// A1: the package plants its own manifest.sig among its payload files. It is
// read, parsed, checked against the PINNED key, and fails.
TEST_F(SignerIdentityTest, A1_APlantedSignatureNamingThePinnedDidIsRefused) {
    ASSERT_FALSE(generateKey("victim").empty());
    const std::string victimDid = readDid("victim");

    json planted{{"version", 1}, {"algorithm", "ed25519"}, {"did", victimDid},
                 {"linkedDids", json::array()}, {"signer", json::object()},
                 {"signature", std::string(86, 'A') + "=="}};
    auto lgx = createPackage("a1naive", "1.0.0", "manifest.sig", planted.dump(2));
    ASSERT_FALSE(lgx.empty());
    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;

    writeInstalled("app", {objDep("a1naive", "", victimDid)});
    EXPECT_EQ(statusOf("app", "a1naive"), DependencyStatus::SignerMismatch);
}

// Replay, not forgery: a GENUINE signature by the pinned publisher, over some
// other package of theirs. manifest.json carries the Merkle root over the
// payload, so a signature can only ever describe the package it was made for.
TEST_F(SignerIdentityTest, A1_AGenuineSignatureOverOtherBytesIsRefused) {
    auto key = generateKey("victim");
    ASSERT_FALSE(key.empty());
    const std::string victimDid = readDid("victim");

    auto real = createPackage("other");
    ASSERT_FALSE(real.empty());
    ASSERT_TRUE(signPackage(real, key));
    const std::string genuine = signatureOf(real);
    ASSERT_FALSE(genuine.empty());

    auto lgx = createPackage("a1replay", "1.0.0", "manifest.sig", genuine);
    ASSERT_FALSE(lgx.empty());
    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;

    writeInstalled("app", {objDep("a1replay", "", victimDid)});
    EXPECT_EQ(statusOf("app", "a1replay"), DependencyStatus::SignerMismatch);
}

// A2: the record outlives the package it described. copyDirectoryContents
// MERGES into an existing directory, so a reinstall leaves the first install's
// manifest.sig standing. It buys nothing: the manifest underneath it was
// replaced, and the signature is over the manifest.
TEST_F(SignerIdentityTest, A2_AStaleSignatureFromThePreviousInstallIsRefused) {
    auto key = generateKey("first");
    ASSERT_FALSE(key.empty());
    const std::string firstDid = readDid("first");
    anchor("first", "first");

    auto good = createPackage("a2", "1.0.0");
    ASSERT_FALSE(good.empty());
    ASSERT_TRUE(signPackage(good, key));
    auto pm = createPM();
    std::string err;
    ASSERT_FALSE(pm.installPluginFile(good.string(), err).empty()) << err;
    ASSERT_TRUE(fs::exists(installedDirOf("a2") / "manifest.sig"));

    // An unsigned impostor of the same name lands on top of it.
    auto impostor = createPackage("a2", "2.0.0");
    ASSERT_FALSE(impostor.empty());
    ASSERT_FALSE(pm.installPluginFile(impostor.string(), err).empty()) << err;
    // Deliberately NOT cleared: it is refused on its merits below.
    ASSERT_TRUE(fs::exists(installedDirOf("a2") / "manifest.sig"));

    writeInstalled("app", {objDep("a2", "", firstDid)});
    EXPECT_EQ(statusOf("app", "a2"), DependencyStatus::SignerMismatch);
}

// A3: the plant is read-only, which out of the Nix store is ordinary. A
// non-event here because the signature is written during EXTRACTION, and
// Package::extractVariant re-adds owner_write to every file it writes — and a
// surviving plant would still have to verify.
TEST_F(SignerIdentityTest, A3_AReadOnlyPlantedSignatureIsRefused) {
    ASSERT_FALSE(generateKey("victim").empty());
    const std::string victimDid = readDid("victim");

    json planted{{"version", 1}, {"algorithm", "ed25519"}, {"did", victimDid},
                 {"linkedDids", json::array()}, {"signer", json::object()},
                 {"signature", std::string(86, 'A') + "=="}};
    auto lgx = createPackage("a3", "1.0.0", "manifest.sig", planted.dump(2),
                             /*extraReadOnly=*/true);
    ASSERT_FALSE(lgx.empty());
    // The attacker signs with their own key, so install has a real signature to
    // write and the read-only plant is in its way.
    auto akey = generateKey("attacker");
    ASSERT_FALSE(akey.empty());
    ASSERT_TRUE(signPackage(lgx, akey));

    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;

    writeInstalled("app", {objDep("a3", "", victimDid)});
    EXPECT_EQ(statusOf("app", "a3"), DependencyStatus::SignerMismatch);
}

// SWAP: the installed document is self-consistent — attacker's DID, attacker's
// genuine signature, over the victim's real manifest bytes. Any check that takes
// its key from the document agrees with itself.
TEST_F(SignerIdentityTest, SWAP_ASelfConsistentSignatureUnderAnotherKeyIsRefused) {
    auto lgx = createPackage("swap");
    ASSERT_FALSE(lgx.empty());
    auto pkey = generateKey("publisher");
    ASSERT_FALSE(pkey.empty());
    const std::string publisherDid = readDid("publisher");
    ASSERT_TRUE(signPackage(lgx, pkey));
    anchor("publisher", "publisher");

    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;
    const fs::path dir = installedDirOf("swap");

    // The attacker signs the SAME package with their own key.
    fs::path evil = tempDir / "swap-evil.lgx";
    fs::copy_file(lgx, evil);
    auto akey = generateKey("attacker");
    ASSERT_FALSE(akey.empty());
    ASSERT_TRUE(signPackage(evil, akey));
    const std::string attackerDid = readDid("attacker");
    overwrite(dir / "manifest.sig", signatureOf(evil));

    writeInstalled("app", {objDep("swap", "", publisherDid)});
    EXPECT_EQ(statusOf("app", "swap"), DependencyStatus::SignerMismatch);

    // CONTROL: pinned to the attacker it verifies, so the refusal above is not
    // just a broken file.
    writeInstalled("app2", {objDep("swap", "", attackerDid)});
    EXPECT_EQ(statusOf("app2", "swap"), DependencyStatus::Installed);
}

// The decisive one: the attacker rewrites ONLY the `did` field to name the
// publisher, so the document claims exactly the pinned identity. Comparing DIDs
// would accept it; verifying under the PIN's key does not. This is why the pin
// supplies the key rather than the file.
TEST_F(SignerIdentityTest, SWAP_ADidRelabelledToThePinIsStillRefused) {
    auto lgx = createPackage("relabel");
    ASSERT_FALSE(lgx.empty());
    auto pkey = generateKey("publisher");
    ASSERT_FALSE(pkey.empty());
    const std::string publisherDid = readDid("publisher");
    ASSERT_TRUE(signPackage(lgx, pkey));
    anchor("publisher", "publisher");

    std::string err;
    ASSERT_FALSE(createPM().installPluginFile(lgx.string(), err).empty()) << err;
    const fs::path dir = installedDirOf("relabel");

    fs::path evil = tempDir / "relabel-evil.lgx";
    fs::copy_file(lgx, evil);
    auto akey = generateKey("attacker");
    ASSERT_FALSE(akey.empty());
    ASSERT_TRUE(signPackage(evil, akey));

    // The attacker's signature, wearing the publisher's name.
    const std::string relabelled = relabelDid(signatureOf(evil), publisherDid);
    overwrite(dir / "manifest.sig", relabelled);
    ASSERT_EQ(json::parse(slurp(dir / "manifest.sig"))["did"], publisherDid)
        << "precondition: the installed document names the pinned DID";

    writeInstalled("app", {objDep("relabel", "", publisherDid)});
    EXPECT_EQ(statusOf("app", "relabel"), DependencyStatus::SignerMismatch)
        << "a signature relabelled to the pinned DID satisfied the pin";

    // ...and reports no signer at all: it does not verify under the DID it names.
    auto pkgs = createPM().getInstalledPackages();
    const InstalledPackage* found = nullptr;
    for (const auto& p : pkgs) if (p.name == "relabel") found = &p;
    ASSERT_NE(found, nullptr);
    EXPECT_FALSE(found->signerDid.has_value());
}

// Positive control: without it, every refusal above could be a mechanism that
// refuses everything.
TEST_F(SignerIdentityTest, TheRealPublisherSatisfiesTheirOwnPin) {
    const std::string did = installSigned("honest", "pub");
    ASSERT_FALSE(did.empty());
    writeInstalled("app", {objDep("honest", "", did)});
    EXPECT_EQ(statusOf("app", "honest"), DependencyStatus::Installed);
}

// Unknown is tolerated by the default policy, so a pin that is not a did:jwk
// must refuse rather than read as "could not check" — a typo would be waved
// through.
TEST_F(SignerIdentityTest, APinThatIsNotADidIsRefusedRatherThanUnknown) {
    ASSERT_FALSE(installSigned("real", "pub").empty());
    writeInstalled("app", {objDep("real", "", "did:jwk:NOT-A-REAL-DID")});
    EXPECT_EQ(statusOf("app", "real"), DependencyStatus::SignerMismatch);
}

// 3. COMPARING — the pin, its verdicts, and how they rank

TEST_F(SignerIdentityTest, PinnedSignerMismatchIsReported) {
    const std::string mine = installSigned("dep", "mine");
    ASSERT_FALSE(mine.empty());
    ASSERT_FALSE(generateKey("other").empty());
    const std::string otherDid = readDid("other");
    writeInstalled("app", {objDep("dep", "", otherDid)});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
    EXPECT_EQ(dep->requiredSigner, std::optional<std::string>(otherDid));
    EXPECT_EQ(dep->signerDid, std::optional<std::string>(mine));
    // It IS installed, so the row carries the on-disk version.
    EXPECT_EQ(dep->version, "1.0.0");
}

// Every edge in the fleet today is unpinned, so this is the regression guard for
// the whole change.
TEST_F(SignerIdentityTest, NoPinMeansNoSignatureCheck) {
    const std::string mine = installSigned("dep", "mine");
    ASSERT_FALSE(mine.empty());
    writeInstalled("bare",   {json("dep")});                 // plain-string edge
    writeInstalled("ranged", {objDep("dep", "^1.0.0")});     // range, no signer

    for (const char* root : {"bare", "ranged"}) {
        auto tree = createPM().resolveDependencies(root);
        ASSERT_TRUE(tree.has_value()) << root;
        const auto* dep = childNamed(*tree, "dep");
        ASSERT_NE(dep, nullptr) << root;
        EXPECT_EQ(dep->status, DependencyStatus::Installed) << root;
        EXPECT_FALSE(dep->requiredSigner.has_value()) << root;
        // The self-asserted signer still rides along: it is a property of the
        // package, not of the edge.
        EXPECT_EQ(dep->signerDid, std::optional<std::string>(mine)) << root;
    }
}

// THE DESIGN CALL: a pin against a package carrying no signature is
// SignerUnknown by default. Embedded packages never pass through
// installPluginFile, so they can never carry a manifest.sig; under Strict a pin
// on one would be unsatisfiable by construction, forever.
TEST_F(SignerIdentityTest, PinAgainstAnUnsignedPackageIsUnknownByDefault) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("dep");                                    // no signature
    writeInstalled("app", {objDep("dep", "", readDid("mine"))});

    auto pm = createPM();
    ASSERT_EQ(pm.unknownSignerPolicy(), UnknownSignerPolicy::Lenient);
    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerUnknown);
    EXPECT_FALSE(dep->signerDid.has_value());
    EXPECT_TRUE(dep->requiredSigner.has_value());
}

TEST_F(SignerIdentityTest, StrictPolicyTurnsUnknownIntoMismatch) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("dep");
    writeInstalled("app", {objDep("dep", "", readDid("mine"))});

    auto pm = createPM();
    pm.setUnknownSignerPolicy(UnknownSignerPolicy::Strict);
    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
}

// Present but unusable refutes nothing, so it ranks with absence.
TEST_F(SignerIdentityTest, AnUnparseableSignatureRanksWithAbsenceNotRefusal) {
    const std::string did = installSigned("garbled", "pub");
    ASSERT_FALSE(did.empty());
    overwrite(installedDirOf("garbled") / "manifest.sig", "this is not json");

    writeInstalled("app", {objDep("garbled", "", did)});
    EXPECT_EQ(statusOf("app", "garbled"), DependencyStatus::SignerUnknown);

    auto pm = createPM();
    pm.setUnknownSignerPolicy(UnknownSignerPolicy::Strict);
    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    EXPECT_EQ(childNamed(*tree, "garbled")->status, DependencyStatus::SignerMismatch);
}

// A pin cannot be judged against a package that is not there, and "install it"
// is the action either way.
TEST_F(SignerIdentityTest, AbsenceOutranksASignerPin) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("app", {objDep("missing", "", readDid("mine"))});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "missing");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::NotInstalled);
    EXPECT_FALSE(dep->signerDid.has_value());
    EXPECT_TRUE(dep->requiredSigner.has_value());
}

// Identity outranks a range: "requires ^2.0.0, found 1.0.0" would send the user
// hunting for a newer build of a package that is not theirs at ANY version.
TEST_F(SignerIdentityTest, SignerMismatchOutranksVersionMismatch) {
    ASSERT_FALSE(installSigned("dep", "mine", "1.0.0").empty());
    ASSERT_FALSE(generateKey("other").empty());
    const std::string otherDid = readDid("other");
    writeInstalled("app", {objDep("dep", "^2.0.0", otherDid)});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const auto* dep = childNamed(*tree, "dep");
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->status, DependencyStatus::SignerMismatch);
    // Both constraints still travel, so nothing is lost by the ranking.
    EXPECT_EQ(*dep->requiredVersion, "^2.0.0");
    EXPECT_EQ(*dep->requiredSigner, otherDid);
}

// ...but a DEFINITE failure outranks "could not check": missing evidence must
// never mask an actionable rejection.
TEST_F(SignerIdentityTest, VersionMismatchOutranksAnUncheckableSigner) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("dep", {}, "1.0.0");                       // no signature
    writeInstalled("app", {objDep("dep", "^2.0.0", readDid("mine"))});
    EXPECT_EQ(statusOf("app", "dep"), DependencyStatus::VersionMismatch);
}

TEST_F(SignerIdentityTest, ASatisfiedPinDoesNotRescueAVersionMismatch) {
    const std::string mine = installSigned("dep", "mine", "1.0.0");
    ASSERT_FALSE(mine.empty());
    writeInstalled("app", {objDep("dep", "^2.0.0", mine)});
    EXPECT_EQ(statusOf("app", "dep"), DependencyStatus::VersionMismatch);
}

// flatten() is the only projection basecamp's load gate reads, so a mismatch
// reachable only down a deeper edge must survive the BFS dedup.
TEST_F(SignerIdentityTest, DeeperSignerMismatchIsNotMaskedByAShallowerEdge) {
    const std::string mine = installSigned("dep", "mine");
    ASSERT_FALSE(mine.empty());
    ASSERT_FALSE(generateKey("other").empty());
    const std::string otherDid = readDid("other");
    writeInstalled("mid", {objDep("dep", "", otherDid)});
    // BFS reaches the bare root->dep edge first; the constrained one is deeper.
    writeInstalled("app", {json("dep"), json("mid")});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();

    const DependencyTreeNode* row = nullptr;
    int depRows = 0;
    for (const auto& n : flat) if (n.name == "dep") { row = &n; ++depRows; }
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(depRows, 1) << "one row per package is the flat contract";
    EXPECT_EQ(row->status, DependencyStatus::SignerMismatch);
    EXPECT_EQ(*row->requiredSigner, otherDid);
    EXPECT_EQ(row->signerDid, std::optional<std::string>(mine));
}

// Promotion is ordered, not pairwise: a mismatch reached later must still
// overtake one already recorded.
TEST_F(SignerIdentityTest, SignerMismatchPromotesOverARecordedVersionMismatch) {
    ASSERT_FALSE(installSigned("dep", "mine", "1.0.0").empty());
    ASSERT_FALSE(generateKey("other").empty());
    writeInstalled("mid", {objDep("dep", "", readDid("other"))});
    writeInstalled("app", {objDep("dep", "^2.0.0"), json("mid")});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();
    const DependencyTreeNode* row = nullptr;
    for (const auto& n : flat) if (n.name == "dep") row = &n;
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->status, DependencyStatus::SignerMismatch);
}

TEST_F(SignerIdentityTest, UnknownDoesNotPromoteOverARecordedVersionMismatch) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("dep", {}, "1.0.0");                        // no signature
    writeInstalled("mid", {objDep("dep", "", readDid("mine"))});
    writeInstalled("app", {objDep("dep", "^2.0.0"), json("mid")});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    auto flat = tree->flatten();
    const DependencyTreeNode* row = nullptr;
    for (const auto& n : flat) if (n.name == "dep") row = &n;
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->status, DependencyStatus::VersionMismatch);
}

// 4. THE WIRE — what a consumer on the far side actually reads

TEST_F(SignerIdentityTest, SignerStatusesSerialiseWithBothSidesOfTheReport) {
    const std::string mine = installSigned("dep", "mine");
    ASSERT_FALSE(mine.empty());
    ASSERT_FALSE(generateKey("other").empty());
    const std::string otherDid = readDid("other");
    writeInstalled("app", {objDep("dep", "", otherDid)});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    json j = json(*tree);
    ASSERT_EQ(j["children"].size(), 1u);
    const json& dep = j["children"][0];

    EXPECT_EQ(dep["status"], "signer_mismatch");
    EXPECT_EQ(dep["requiredSigner"], otherDid);
    EXPECT_EQ(dep["signerDid"], mine);
    EXPECT_EQ(dep["version"], "1.0.0");
    EXPECT_EQ(dep["installType"], "user");
}

TEST_F(SignerIdentityTest, UnknownSerialisesWithNoSignerDidKey) {
    ASSERT_FALSE(generateKey("mine").empty());
    writeInstalled("dep");
    writeInstalled("app", {objDep("dep", "", readDid("mine"))});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const json dep = json(*tree)["children"][0];
    EXPECT_EQ(dep["status"], "signer_unknown");
    EXPECT_FALSE(dep.contains("signerDid"));
    EXPECT_EQ(dep["version"], "1.0.0");
}

TEST_F(SignerIdentityTest, StatusStringsAreStableAndDistinct) {
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

// The installed manifest.json must be the SIGNED BYTES, and the resolve path
// reads them with readFileBytes(), which is BINARY — so the write must be too.
//
// HONEST LIMIT: on Linux this cannot fail, there being no translation to do. On
// Windows a text-mode write turns '\n' into "\r\n" while the read stays binary,
// so every check returns MISMATCH — for packages the pinned key genuinely signed.
TEST_F(SignerIdentityTest, TheInstalledManifestCarriesNoTextModeTranslation) {
    ASSERT_FALSE(generateKey("mine").empty());
    ASSERT_FALSE(installSigned("dep", "mine").empty());

    const std::string installed = slurp(installedDirOf("dep") / "manifest.json");
    ASSERT_FALSE(installed.empty()) << "no installed manifest.json to inspect";
    EXPECT_EQ(installed.find('\r'), std::string::npos)
        << "the installed manifest.json contains a carriage return, so it was "
           "written through a text-mode stream; on Windows its bytes then "
           "differ from the signed message and no signature can verify";
}

// A constraint that is PRESENT but not a string must fail CLOSED: the raw text
// is carried through, is unparseable as a did:jwk, and lands as SignerMismatch
// naming the offending value.
TEST_F(SignerIdentityTest, AWrongTypedSignerPinFailsClosedRatherThanVanishing) {
    ASSERT_FALSE(generateKey("mine").empty());
    ASSERT_FALSE(installSigned("dep", "mine").empty());
    writeInstalled("app", {json{{"name","dep"},{"signer",42}}});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const json dep = json(*tree)["children"][0];
    EXPECT_EQ(dep["status"], "signer_mismatch")
        << "a malformed signer pin silently became 'no pin' — fail-open on the "
           "one field whose job is to narrow what satisfies the edge";
}

TEST_F(SignerIdentityTest, AWrongTypedVersionRangeFailsClosedRatherThanVanishing) {
    writeInstalled("dep");
    writeInstalled("app", {json{{"name","dep"},{"version",42}}});

    auto tree = createPM().resolveDependencies("app");
    ASSERT_TRUE(tree.has_value());
    const json dep = json(*tree)["children"][0];
    EXPECT_EQ(dep["status"], "version_mismatch");
}

TEST_F(SignerIdentityTest, EverySignerStatusCountsAsOnDisk) {
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::Installed));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::VersionMismatch));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::SignerMismatch));
    EXPECT_TRUE(nodeResolvedToAnInstalledPackage(DependencyStatus::SignerUnknown));
    EXPECT_FALSE(nodeResolvedToAnInstalledPackage(DependencyStatus::NotInstalled));
    EXPECT_FALSE(nodeResolvedToAnInstalledPackage(DependencyStatus::Cycle));
}
