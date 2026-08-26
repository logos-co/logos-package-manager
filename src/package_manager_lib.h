#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class SignaturePolicy {
    NONE,    // Accept all packages without checking signatures
    WARN,    // Accept unsigned packages with a warning (default)
    REQUIRE  // Reject unsigned packages and packages with unknown signers
};

// Where a scanned package lives. "embedded" is a read-only, shipped-with-app
// directory; "user" is the writable directory new installs go to. When the
// same package name exists in both, user wins at scan time.
enum class InstallType {
    Embedded,
    User,
};

// Whether a dependency is currently installed, absent, part of a cycle,
// installed at a version its dependant refused, or installed but published by
// somebody other than the publisher its dependant named.
//
// New enumerators are APPENDED, never inserted. This enum crosses a shared
// library boundary — logos-package-manager-module compiles against this header
// and links libpackage_manager_lib at run time — so the numeric value of an
// existing enumerator is ABI.
enum class DependencyStatus {
    Installed,
    NotInstalled,
    Cycle,
    // The package IS installed, but its version does not satisfy the semver
    // range the depending manifest declared for this edge. Distinct from
    // NotInstalled on purpose: the two call for different remedies (install it
    // vs. change a version), and only absence can be asserted without reading
    // a constraint. `version` and `installType` are populated on such a node —
    // the version actually present is the whole point of the report.
    VersionMismatch,
    // A package by that name IS installed, and it is provably NOT the one the
    // dependant named: the key whose signature this install was verified
    // against is not the `signer` DID on the edge.
    //
    // This is an IDENTITY answer, not a trust one. `my_module` published by me
    // and `my_module` published by anybody else are different packages that
    // happen to share a name, and telling them apart is the same kind of
    // question as "is this package called my_module" — squarely the scanner's
    // job. Whether a given publisher may be installed AT ALL is the separate
    // question the trust-anchor gate in installPluginFile answers, and neither
    // check substitutes for the other:
    //
    //   trust anchor : does any active anchor vouch for this key?  -> may it
    //                                                                 be installed
    //   signer pin   : is this the key the dependant named?        -> is it the
    //                                                                 same package
    //
    // `version` and `installType` are populated — something is on disk.
    SignerMismatch,
    // The edge names a `signer`, a package by that name is installed, and
    // NOTHING RECORDS WHO PUBLISHED IT. Absence of evidence, not evidence of
    // mismatch, and reported as its own status rather than folded into either
    // neighbour — see `UnknownSignerPolicy` for the whole argument and for the
    // one line that flips it.
    //
    // `version` and `installType` are populated — something is on disk.
    SignerUnknown,
};

// What to report for an edge that pins a `signer` when the installed package
// has NO recorded publisher at all.
//
// ── The design call, stated where it is made ──────────────────────────────
//
// An installed package records its publisher only when installPluginFile
// VERIFIED a signature at install time (the `signer` sidecar, mirroring the
// `variant` one). Three populations legitimately have no such record:
//
//   1. EMBEDDED packages — shipped with the app, placed by the build, never
//      passed through installPluginFile at all. These can never acquire a
//      sidecar; the same is already true of the `variant` file, whose reader
//      documents "absent (e.g. embedded or legacy installs)".
//   2. Packages installed BEFORE this change existed.
//   3. Packages installed under SignaturePolicy::NONE, where no verification
//      happened, and unsigned packages installed under WARN.
//
// Reporting those as SignerMismatch would be fail-closed, and it is wrong for
// population 1 specifically: a pin on an embedded dependency would be
// unsatisfiable BY CONSTRUCTION, forever, with no action the user could take.
// That is not a strict policy, it is a broken one. It is also a different
// sentence to say to a user — "this is a different publisher's package" is an
// accusation; "nobody recorded who published this" is a gap.
//
// Measured before choosing (2026-08-26, whole workspace): 110 metadata.json,
// 65 declared dependency edges, ZERO object-form entries — so zero signer
// pins and zero version ranges — zero manifest.sig anywhere, and the shared
// release workflow publishes `signing_mode: none`. The number of packages
// either option changes TODAY is zero, under both. So the choice is not about
// blast radius; it decides whether pins can be adopted at all, and Lenient is
// the only option under which they can.
//
// THE RESIDUAL, stated because Lenient does not close it. An UNSIGNED package
// records nothing, so an unsigned impostor installed under a pinned name is
// reported SignerUnknown and does not block. The pin catches a different
// publisher; it does not catch "no publisher". That gap is bounded — under
// SignaturePolicy::REQUIRE an unsigned package cannot be installed at all, so
// it exists only under WARN/NONE, where the whole signature story is already
// advisory. The clean fix is not Strict (which also condemns embedded and
// legacy installs) but recording an explicit "verified: unsigned" OBSERVATION,
// which would separate "we looked and it is unsigned" from "nobody looked" and
// let the first be judged. That is a change to what install records, not to
// this policy, and is deliberately not made here.
//
// Default: Lenient. Flip the whole build by changing this initialiser, or one
// instance with setUnknownSignerPolicy(UnknownSignerPolicy::Strict).
enum class UnknownSignerPolicy {
    // No record -> DependencyStatus::SignerUnknown. Surfaced, does not claim a
    // mismatch it cannot prove.
    Lenient,
    // No record -> DependencyStatus::SignerMismatch. Fail closed: only a
    // package whose signature was verified against the pinned key satisfies
    // the pin.
    Strict,
};

struct SignatureVerificationResult {
    bool is_signed = false;
    bool signature_valid = false;
    bool package_valid = false;
    std::string signer_did;    // did:jwk:... string
    std::string signer_name;   // self-asserted display name
    std::string signer_url;    // self-asserted URL
    std::string trusted_as;    // keyring name if trusted, empty otherwise
    std::string error;         // error message if any
};

struct UninstallResult {
    bool success = false;
    std::string errorMsg;
    std::vector<std::string> removedFiles;   // paths removed (top-level dir)
};

// Nested to mirror the manifest.json shape. Only `root` is read today, but
// keeping the struct leaves room for additional hash fields without a
// subsequent refactor.
struct Hashes {
    std::string root;            // Merkle root over the package contents
};

// One entry of a manifest's `dependencies` array. Mirrors lgx::Dependency
// (logos-package src/core/manifest.h) and the LGX spec's "Dependency entries"
// section, which defines two on-disk forms:
//
//   plain string  "waku_module"                     -> { name }
//   object        { "name": "waku_module",
//                   "version"?: "^1.2.0",           -> npm-style semver range
//                   "signer"?:  "did:jwk:..." }     -> publisher DID pin
//
// The distinction is NOT cosmetic: the scan used to accept only the string
// form, so an object entry lost THE EDGE ITSELF (not merely its constraint)
// and vanished from resolveDependencies / resolveDependents. Every consumer
// that needs a bare name reads `.name`, which both forms populate.
//
// Implicitly constructible from a string so `dependencies.push_back("dep")`
// and brace-init from a name list keep working unchanged.
struct PackageDependency {
    std::string name;                       // required; canonical package name
    std::optional<std::string> version;     // semver range; absent = any version
    std::optional<std::string> signer;      // did:jwk:...; absent = any signer

    PackageDependency() = default;
    PackageDependency(std::string n) : name(std::move(n)) {}   // NOLINT: intended implicit
    PackageDependency(const char* n) : name(n) {}              // NOLINT: intended implicit

    // True when only `name` is set — the entry round-trips as a plain string.
    bool isSimple() const { return !version.has_value() && !signer.has_value(); }

    // Single-line human form used by `lgpm info` and error messages.
    std::string toString() const {
        std::string s = name;
        if (version) s += " " + *version;
        if (signer)  s += " [signer=" + *signer + "]";
        return s;
    }

    bool operator==(const PackageDependency& o) const {
        return name == o.name && version == o.version && signer == o.signer;
    }
    bool operator!=(const PackageDependency& o) const { return !(*this == o); }
};

// A scanned, installed package. Mirrors the manifest.json fields plus the
// resolved install location. `icon` and `view` are optional; `view` is only
// meaningful for ui_qml packages (the QML entry point).
struct InstalledPackage {
    std::string name;
    std::string displayName;
    std::string version;
    std::string description;
    std::string type;                       // "core" | "ui" | "ui_qml"
    std::string category;
    std::string author;
    std::string license;
    std::string icon;
    std::string view;
    std::string manifestVersion;
    // Dependency NAMES — one per manifest `dependencies[]` entry, in declared
    // order, for BOTH the string and the object form. This is the edge set the
    // dependency graph is built from, so an object-form entry must appear here
    // exactly like a string one.
    std::vector<std::string> dependencies;
    // The subset of those entries that declared a version range and/or a signer
    // DID, carrying the constraint verbatim. Empty for a package whose
    // dependencies are all bare names — which is every package in the workspace
    // today, which is why the scan dropping object entries went unnoticed.
    //
    // Kept separate from `dependencies` (rather than widening its element type)
    // so the name-only wire format and every existing consumer are unchanged;
    // the semantic evaluation of these constraints belongs to the resolver in
    // logos-package-downloader and, at load time, to the runtime.
    std::vector<PackageDependency> dependencyConstraints;
    // WHO PUBLISHED THIS COPY — the DID of the key whose signature over this
    // package's manifest was actually VERIFIED when it was installed. Read
    // back from the `signer` sidecar installPluginFile writes next to
    // `variant`; see readInstalledSigner in the .cpp for the write/read pair.
    //
    // Three states, and they are three, not two:
    //   nullopt        nothing recorded — UNKNOWN. Embedded packages (never
    //                  installed through installPluginFile), installs
    //                  predating the sidecar, installs under
    //                  SignaturePolicy::NONE, and unsigned packages all land
    //                  here. NOT "unsigned": the value is missing, which is a
    //                  different fact from a package we looked at and found
    //                  unsigned.
    //   non-empty DID  an OBSERVATION. Only written when
    //                  signature_valid && !signer_did.empty(); a package whose
    //                  manifest.sig names a DID but does NOT verify records
    //                  nothing, because signer_did alone is a CLAIM (it is
    //                  populated before verification in
    //                  logos-package's Package::verifySignature).
    //   empty string   never produced; the reader collapses it to nullopt so a
    //                  truncated sidecar cannot read as an identity.
    //
    // Distinct from every `signer` in `dependencyConstraints`, which are PINS
    // this package declares about OTHERS. This is the one field that says
    // something about THIS package, and it is the only one a pin can be
    // checked against.
    //
    // Which is why the sidecar behind it is written AUTHORITATIVELY: install
    // rewrites it on an observation and DELETES it when it has none, in the
    // install directory, after the copy. Without both halves the value is not
    // evidence at all — `variants/<v>/` is copied wholesale, so a package can
    // ship a file by that name, and copyDirectoryContents merges rather than
    // replaces, so a previous install's record would outlive the package it
    // described. Either one lets a package wear an identity nobody verified.
    std::optional<std::string> observedSigner;
    Hashes hashes;
    InstallType installType;
    std::string installDir;
    std::string mainFilePath;
};

// A node in the forward dependency tree produced by resolveDependencies.
// Recursion stops at NotInstalled and Cycle nodes (no manifest available);
// for those, `version` is empty and `installType` is unspecified.
struct DependencyTreeNode {
    std::string name;
    DependencyStatus status;
    // Empty for NotInstalled and Cycle; the version actually installed for
    // both Installed and VersionMismatch.
    std::string version;
    InstallType installType;                // meaningful on the same two states
    // The constraint the PARENT declared on this edge, verbatim, absent when
    // the parent named the dependency without one (and always absent on the
    // root, which no edge points at). BOTH are judged: `requiredVersion`
    // against `version`, `requiredSigner` against `observedSigner`, and
    // `status` reports whichever failed.
    //
    // `requiredSigner` used to be carried as data only, on the grounds that
    // "who may sign a dependency is a trust decision that does not belong to
    // the scanner". That reasoning was backwards, and it is worth writing down
    // why so it is not reintroduced. A signer pin is what separates
    // `my_module` published by me from `my_module` published by anybody else.
    // When the pin is present it is as much a part of the dependency's IDENTITY
    // as its name, and resolving identity is exactly what this walk does. The
    // sentence was true of a DIFFERENT question — may this publisher be
    // installed at all — which is authorization, is answered by the
    // trust-anchor gate in installPluginFile against the local keyring, and
    // never reaches here. Skipping the identity check because the trust check
    // belongs elsewhere conflated the two and did neither.
    std::optional<std::string> requiredVersion;
    std::optional<std::string> requiredSigner;
    // Who actually published what is installed under this name — see
    // InstalledPackage::observedSigner. Populated on any node that resolved to
    // an installed package (whatever its status), absent on NotInstalled and
    // Cycle nodes, and absent when nothing recorded a publisher.
    //
    // Travels next to `requiredSigner` so a report can name BOTH sides:
    // "requires <pin>, published by <observed>" is actionable, and either half
    // alone is not.
    std::optional<std::string> observedSigner;
    std::vector<DependencyTreeNode> children;

    // BFS enumeration of descendants (this node is excluded), deduplicated
    // by name so diamonds and cycles don't produce repeats. The `children`
    // vectors on returned copies are left empty — consumers iterate the
    // flat output without double-counting.
    //
    // Note the dedup interacts with the per-edge constraint fields: when two
    // parents depend on the same package under different constraints, the flat
    // list keeps whichever edge BFS reached first — EXCEPT that a later edge
    // which judges the installed package MORE HARSHLY promotes the row and
    // carries its own constraint. A package satisfies its dependants only if it
    // satisfies all of them, and BFS reaches the shallowest edge first, which
    // in today's fleet is almost always an unconstrained one; without the
    // promotion a mismatch one level down was silently dropped from the only
    // projection the flat API and basecamp's load gate ever read. Callers that
    // must see EVERY constraint on a package still walk the tree instead of
    // flattening it — the flat row reports one failing constraint, not all of
    // them.
    std::vector<DependencyTreeNode> flatten() const;
};

// A node in the reverse dependency tree produced by resolveDependents.
// Every node is an installed package (reverse edges are synthesised only
// from installed manifests), so the legacy "status" field used by the
// forward tree has no counterpart here.
struct DependentTreeNode {
    std::string name;
    std::string version;
    std::string type;
    InstallType installType;
    std::string installDir;
    std::vector<DependentTreeNode> children;

    // Same semantics as DependencyTreeNode::flatten — BFS descendants,
    // name-deduped, children cleared on returned copies.
    std::vector<DependentTreeNode> flatten() const;
};

const char* installTypeToString(InstallType t);
const char* dependencyStatusToString(DependencyStatus s);

// Did this node resolve to a package that is actually ON DISK?
//
// True for every status except NotInstalled and Cycle. It decides whether
// `version` / `installType` carry meaning on a serialised node, and it is the
// reason to have a predicate rather than an `||` chain at each site: the chain
// was written as `Installed || VersionMismatch`, so each newly appended
// edge-decided status (SignerMismatch, SignerUnknown — both of which resolved
// to a real installed package) would have started reporting an empty version
// at two call sites in two repositories, quietly.
bool nodeResolvedToAnInstalledPackage(DependencyStatus s);

class PackageManagerLib
{
public:
    PackageManagerLib();
    ~PackageManagerLib();

    // Directory management — embedded directories (multiple, read-only at runtime)
    void setEmbeddedModulesDirectory(const std::string& dir);
    void addEmbeddedModulesDirectory(const std::string& dir);
    void setEmbeddedUiPluginsDirectory(const std::string& dir);
    void addEmbeddedUiPluginsDirectory(const std::string& dir);

    std::vector<std::string> embeddedModulesDirectories() const { return m_embeddedModulesDirs; }
    std::vector<std::string> embeddedUiPluginsDirectories() const { return m_embeddedUiPluginsDirs; }

    // Directory management — user directories (single, writable, where new packages are installed)
    void setUserModulesDirectory(const std::string& dir);
    void setUserUiPluginsDirectory(const std::string& dir);

    std::string userModulesDirectory() const { return m_userModulesDir; }
    std::string userUiPluginsDirectory() const { return m_userUiPluginsDir; }

    // All modules directories (embedded + user) for scanning
    std::vector<std::string> allModulesDirectories() const;
    // All UI plugins directories (embedded + user) for scanning
    std::vector<std::string> allUiPluginsDirectories() const;
    // All directories (modules + UI plugins) for scanning
    std::vector<std::string> allDirectories() const;

    // Install from local LGX file
    // Returns the install ROOT (the configured user modules / UI plugins
    // directory), or empty string on error (sets errorMsg).
    //
    // If installedPluginPath is non-null, receives the path identifying what
    // was installed — see resolveInstalledPackagePath() for the exact rule.
    // It is NEVER empty when this function reports success, so callers may
    // use it as the "something was installed" signal. It is the installed
    // main FILE when the package ships one, and the installed module
    // DIRECTORY otherwise (a QML-only ui_qml package legitimately has no
    // backend library, so its manifest carries an empty "main").
    //
    // If isCoreModule is non-null, receives whether the module type is "core".
    std::string installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                  bool skipIfNotNewerVersion = false,
                                  std::string* installedPluginPath = nullptr,
                                  bool* isCoreModule = nullptr);

    // Decide what installPluginFile() reports for a package that has just
    // been copied into `moduleDir` (= <installRoot>/<moduleName>). `variants`
    // is normally platformVariantsToTry().
    //
    // Returns <moduleDir>/<main> when the installed manifest.json names a main
    // file for this platform AND that file is present; otherwise returns
    // `moduleDir` itself. Only returns empty when `moduleDir` does not exist.
    //
    // The directory fallback is the fix for a real defect: a QML-only ui_qml
    // package has "main": {}, so this used to yield an empty string on a
    // completely successful install. Callers treat an empty path as failure —
    // logos-package-manager-ui rendered a red RETRY, and
    // package_manager_module skipped its uiPluginFileInstalled event, so the
    // freshly installed plugin only appeared after an app restart.
    //
    // Exposed publicly so the behaviour can be unit-tested directly: the
    // "main": {} shape cannot be produced through liblgx's C API (lgx_add_variant
    // only allows a main-less directory variant when the package manifest
    // already declares type "ui_qml", and there is no lgx_set_type()).
    static std::string resolveInstalledPackagePath(const std::string& moduleDir,
                                                   const std::vector<std::string>& variants);

    // Module scanning — returns the scanned packages as populated structs.
    // Each struct carries all manifest.json fields plus the resolved
    // install location and install type. For ui_qml packages, `view` is
    // the required QML entry point and `mainFilePath` is backend-only
    // metadata that may be empty.
    //
    // When the same package name appears in both an embedded and the user
    // directory, the user-directory copy wins and the embedded entry is
    // dropped from the result.
    //
    // Serialization to JSON is the caller's responsibility — see
    // package_manager_json.h for to_json hooks that match the legacy wire
    // format used by the C ABI and the `lgpm --json` CLI output.
    std::vector<InstalledPackage> getInstalledModules();
    std::vector<InstalledPackage> getInstalledUiPlugins();
    std::vector<InstalledPackage> getInstalledPackages();

    // Dependency resolution over the currently-installed package set.
    //
    // resolveDependencies — walks the forward dep tree rooted at
    // `packageName`. Recursion stops at NotInstalled and Cycle nodes (we
    // don't have a manifest for them). Returns `std::nullopt` if
    // `packageName` is itself absent from the installed set.
    std::optional<DependencyTreeNode> resolveDependencies(const std::string& packageName);

    // resolveDependents — walks the reverse dep tree rooted at
    // `packageName`. The returned root carries its own manifest metadata;
    // `children` are its direct dependents; deeper levels are transitive.
    // Returns `std::nullopt` if `packageName` is itself absent from the
    // installed set. Callers that want a flat list build it via
    // `tree->flatten()`.
    std::optional<DependentTreeNode> resolveDependents(const std::string& packageName);

    // Uninstall a previously-installed package. Refuses if the resolved
    // install lives in an embedded directory. Best-effort recursive
    // delete; does not touch runtime load state.
    UninstallResult uninstallPackage(const std::string& packageName);

    // LGX extraction and installation helpers
    bool extractLgxPackage(const std::string& lgxPath, const std::string& outputDir, std::string& errorMsg);
    bool copyLibraryFromExtracted(const std::string& extractedDir, const std::string& targetDir,
                                  bool isCoreModule, std::string& outModuleName, std::string& errorMsg);

    // Variant selection (platform detection)
    // Install for a platform OTHER than the one we are running on.
    //
    // Needed for cross-builds: the Nix install bundler runs lgpm on the Linux
    // builder to lay out a Windows package. Without this it fail-closes with
    // "Package does not contain variant for platform: linux-x86_64 (package
    // provides: windows-x86_64)" -- correct behaviour, and the protection is
    // worth keeping, so this override is EXPLICIT and never inferred. Empty
    // string restores the compiled-in platform.
    static void setPlatformVariantOverride(const std::string& variant);
    static std::string platformVariantOverride();

    static std::string currentPlatformVariant();
    static std::vector<std::string> platformVariantsToTry();

    // Signature policy configuration
    void setSignaturePolicy(SignaturePolicy policy);
    SignaturePolicy signaturePolicy() const { return m_signaturePolicy; }
    void setKeyringDirectory(const std::string& dir);
    std::string keyringDirectory() const { return m_keyringDir; }

    // What resolveDependencies reports for a signer pin against an installed
    // package with no recorded publisher — THE flip for the design call
    // documented on UnknownSignerPolicy. Default Lenient.
    void setUnknownSignerPolicy(UnknownSignerPolicy policy);
    UnknownSignerPolicy unknownSignerPolicy() const { return m_unknownSignerPolicy; }

    // Standalone signature verification
    SignatureVerificationResult verifyPackageSignature(const std::string& lgxPath);

    // WHAT COUNTS AS AN OBSERVATION of a package's publisher.
    //
    // The rule, and the whole rule: a DID is recorded only when the Ed25519
    // signature over the manifest actually VERIFIED. `signature_valid`, never
    // `is_signed`, and never `signer_did` on its own — logos-package populates
    // signer_did straight out of manifest.sig BEFORE running the check
    // (Package::verifySignature), so until the check passes it is a CLAIM the
    // package makes about itself, and anybody can write any DID into a
    // manifest.sig. Recording a claim would let a forged signature mint that
    // publisher identity for the install, and every pin comparison downstream
    // would then be comparing against attacker-chosen input. That is exactly
    // the defect fixed alongside this in logos-package-downloader's
    // index→file signer binding, which read is_signed and signer_did and
    // never signature_valid.
    //
    // Returns nullopt for: unsigned, signature failed, DID empty. All three
    // read back as "nothing recorded" — the honest answer in every case is
    // that we do not know who published it.
    //
    // A pure function of the verification result, and PUBLIC, for the reason
    // signerPinMatches is public in logos-package-downloader: it is the
    // invariant itself rather than one call site's spelling of it. Today the
    // trust-anchor gate happens to refuse a failed-signature package before
    // this is ever consulted with one, so the two spellings cannot be told
    // apart end to end — which is precisely why the rule needs to be testable
    // on its own. WARN means "warn, do not refuse"; the day it stops refusing
    // this case, a claim-based rule would start minting identities silently.
    static std::optional<std::string> observedSignerFrom(
        const SignatureVerificationResult& result);

    // The name of the sidecar file, inside an installed package directory,
    // that records the DID of the key whose signature was verified when the
    // package was installed. Exactly the shape of the `variant` sidecar next
    // to it: one line, no payload meaning, written by install, read by scan.
    //
    // Exposed so tests and tooling can assert on the file without hardcoding
    // the name in two places.
    //
    // The name is RESERVED: install overwrites or deletes it after the payload
    // has been copied, so whatever a package ships under it is discarded. A
    // package that could write here could name its own publisher.
    static const char* observedSignerFileName();

    // Read that sidecar out of an installed package directory.
    //
    // nullopt means UNKNOWN — the file is absent (embedded install, an install
    // predating the sidecar, an install under SignaturePolicy::NONE) or holds
    // nothing usable. It never means "unsigned": a package we verified and
    // found unsigned records nothing, exactly like one we never looked at, and
    // this reader deliberately does not invent a distinction the file cannot
    // carry. Callers MUST treat nullopt as an absence of evidence.
    static std::optional<std::string> readInstalledSigner(const std::string& moduleDir);

    // Utilities
    static bool versionGreaterOrEqual(const std::string& a, const std::string& b);
    static bool copyDirectoryContents(const std::string& srcDir, const std::string& destDir, std::string& errorMsg);

    // Validates a module name that came from an untrusted manifest before it is
    // used as a path component when installing. A name is only safe to join onto
    // an install directory if it is a single, plain path segment: rejects empty,
    // ".", "..", anything containing a path separator ('/' or '\\'), a NUL byte,
    // or a leading drive separator. This is the guard against F-006 (path
    // traversal via the manifest "name" field).
    static bool isValidModuleName(const std::string& name);

private:
    std::vector<std::string> m_embeddedModulesDirs;
    std::string m_userModulesDir;
    std::vector<std::string> m_embeddedUiPluginsDirs;
    std::string m_userUiPluginsDir;
    SignaturePolicy m_signaturePolicy = SignaturePolicy::WARN;
    // THE FLIP for the unknown-signer design call. Change this initialiser to
    // ::Strict to fail closed build-wide; see UnknownSignerPolicy.
    UnknownSignerPolicy m_unknownSignerPolicy = UnknownSignerPolicy::Lenient;
    std::string m_keyringDir;
};
