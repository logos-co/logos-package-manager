# logos-package-manager

C++ library and CLI for local Logos package management — installing `.lgx` packages, scanning installed modules, platform variant selection, and LGX extraction.

This repo handles **local operations** only. It does not fetch packages from the network — that is the responsibility of [logos-package-downloader](https://github.com/logos-co/logos-package-downloader).

## Library API

```cpp
#include <package_manager_lib.h>

PackageManagerLib pm;

// Configure embedded directories (multiple, read-only at runtime)
pm.setEmbeddedModulesDirectory("/path/to/embedded/modules");       // clears + sets first
pm.addEmbeddedModulesDirectory("/path/to/more/embedded/modules");  // appends additional
pm.setEmbeddedUiPluginsDirectory("/path/to/embedded/plugins");
pm.addEmbeddedUiPluginsDirectory("/path/to/more/embedded/plugins");

// Configure user directories (single, writable, where new packages are installed)
pm.setUserModulesDirectory("/path/to/user/modules");
pm.setUserUiPluginsDirectory("/path/to/user/plugins");

// Install from a local .lgx file (auto-detects type, installs to user directory)
std::string errorMsg;
std::string installedPath = pm.installPluginFile("/path/to/module.lgx", errorMsg);
// skipIfNotNewer: skip installation if an equal-or-newer version is already installed
std::string path = pm.installPluginFile("/path/to/module.lgx", errorMsg, /*skipIfNotNewer=*/true);

// Scan installed packages (returns JSON array string)
// Each entry includes all manifest.json fields + "installDir" + "mainFilePath"
std::string modules = pm.getInstalledModules();      // type=core only
std::string uiPlugins = pm.getInstalledUiPlugins();   // type=ui,ui_qml only
std::string all = pm.getInstalledPackages();           // all types

// Platform variant helpers
std::string variant = PackageManagerLib::currentPlatformVariant();     // e.g. "darwin-arm64"
std::vector<std::string> variants = PackageManagerLib::platformVariantsToTry(); // ordered fallback list

// Low-level LGX extraction
pm.extractLgxPackage("/path/to/module.lgx", "/output/dir", errorMsg);
pm.copyLibraryFromExtracted("/extracted/dir", "/target/dir",
                            /*isCoreModule=*/true, outModuleName, errorMsg);

// Signature verification policy
pm.setSignaturePolicy(SignaturePolicy::WARN);     // NONE, WARN (default), REQUIRE
pm.setKeyringDirectory("/path/to/trusted-keys");  // default: ~/.config/logos/trusted-keys/

// Standalone signature verification
SignatureVerificationResult sigInfo = pm.verifyPackageSignature("/path/to/module.lgx");
// sigInfo.is_signed, sigInfo.signature_valid, sigInfo.package_valid,
// sigInfo.signer_did, sigInfo.signer_name, sigInfo.signer_url, sigInfo.trusted_as, sigInfo.error

// Utilities
bool newer = PackageManagerLib::versionGreaterOrEqual("1.2.0", "1.1.0");
PackageManagerLib::copyDirectoryContents("/src", "/dst", errorMsg);
```

### C API

A C-compatible API is available via `lgpm.h` for use from non-C++ consumers:

```c
#include <lgpm.h>

lgpm_context_t ctx = lgpm_create();
lgpm_set_embedded_modules_dir(ctx, "/path/to/embedded/modules");
lgpm_add_embedded_modules_dir(ctx, "/path/to/more/embedded/modules");
lgpm_set_user_modules_dir(ctx, "/path/to/user/modules");

// Signature policy
lgpm_set_signature_policy(ctx, "require");  // "none", "warn", "require"
lgpm_set_keyring_path(ctx, "/path/to/trusted-keys");
lgpm_enable_tofu(ctx, true);

char* result = lgpm_install_file(ctx, "/path/to/module.lgx", false, NULL, NULL);
lgpm_free_string(result);
lgpm_free(ctx);
```

## CLI (`lgpm`)

```
lgpm [options] <command> [arguments]

Commands:
  install --file <path>       Install from a local LGX file
  install --dir <path>        Install all LGX files in a directory
  list                        List installed packages
  info <package>              Show installed package details

Options:
  --modules-dir <path>        Target directory for core modules
  --ui-plugins-dir <path>     Target directory for UI plugins
  --json                      Output in JSON format
  --allow-unsigned            Accept unsigned packages without warning
  --require-signatures        Reject unsigned packages
  --tofu                      Trust unknown signing keys on first use
  --keyring <path>            Override keyring directory
  -h, --help                  Show help
  -v, --version               Show version
```

### Examples

```bash
# Install a local .lgx package
lgpm --modules-dir ./modules install --file ./waku_module.lgx

# Install all .lgx files in a directory
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins install --dir ./packages/

# List installed modules
lgpm --modules-dir ./modules --ui-plugins-dir ./plugins list

# Show package info (JSON)
lgpm --modules-dir ./modules info waku_module --json
```

## Building

```bash
nix build                        # library + CLI
nix build .#lib                  # library only
nix build .#cli                  # CLI only
```

## Testing

```bash
nix flake check                  # run all tests
nix build .#tests                # build and run tests
```

## Dependencies

- `logos-package` — LGX format library
- `nlohmann_json` — JSON parsing
