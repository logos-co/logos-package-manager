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

// Whether a dependency is currently installed, absent, or part of a cycle.
enum class DependencyStatus {
    Installed,
    NotInstalled,
    Cycle,
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

// A scanned, installed package. Mirrors the manifest.json fields plus the
// resolved install location. `icon` and `view` are optional; `view` is only
// meaningful for ui_qml packages (the QML entry point).
struct InstalledPackage {
    std::string name;
    std::string version;
    std::string description;
    std::string type;                       // "core" | "ui" | "ui_qml"
    std::string category;
    std::string author;
    std::string license;
    std::string icon;
    std::string view;
    std::vector<std::string> dependencies;
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
    std::string version;                    // empty unless status == Installed
    InstallType installType;                // meaningful only if status == Installed
    std::vector<DependencyTreeNode> children;

    // BFS enumeration of descendants (this node is excluded), deduplicated
    // by name so diamonds and cycles don't produce repeats. The `children`
    // vectors on returned copies are left empty — consumers iterate the
    // flat output without double-counting.
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
    // Returns installed path, or empty string on error (sets errorMsg).
    // If installedPluginPath is non-null, receives the full path to the
    // installed main file. For ui_qml packages this is the optional backend
    // plugin path only and may be empty.
    // If isCoreModule is non-null, receives whether the module type is "core".
    std::string installPluginFile(const std::string& pluginPath, std::string& errorMsg,
                                  bool skipIfNotNewerVersion = false,
                                  std::string* installedPluginPath = nullptr,
                                  bool* isCoreModule = nullptr);

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
    static std::string currentPlatformVariant();
    static std::vector<std::string> platformVariantsToTry();

    // Signature policy configuration
    void setSignaturePolicy(SignaturePolicy policy);
    SignaturePolicy signaturePolicy() const { return m_signaturePolicy; }
    void setKeyringDirectory(const std::string& dir);
    std::string keyringDirectory() const { return m_keyringDir; }

    // Standalone signature verification
    SignatureVerificationResult verifyPackageSignature(const std::string& lgxPath);

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
    std::string m_keyringDir;
};
