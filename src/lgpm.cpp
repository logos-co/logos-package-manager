#include "lgpm.h"
#include "package_manager_lib.h"
#include "package_manager_json.h"
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

static thread_local std::string g_last_error;

static void set_error(const std::string& err) {
    g_last_error = err;
}

static char* to_c_string(const std::string& s) {
    if (s.empty()) return nullptr;
    char* result = static_cast<char*>(malloc(s.size() + 1));
    if (result) {
        memcpy(result, s.c_str(), s.size() + 1);
    }
    return result;
}

// Serialise a vector of structs through nlohmann's ADL to_json hooks in
// package_manager_json.h. Kept in one place so every C-ABI accessor emits
// the same JSON shape.
template <typename T>
static std::string structsToJsonString(const std::vector<T>& v) {
    return nlohmann::json(v).dump();
}

// Project a vector of tree nodes to their .name field. Shared by the
// recursive (tree->flatten()) and non-recursive (tree->children) cases.
template <typename Node>
static std::vector<std::string> nodeNames(const std::vector<Node>& nodes) {
    std::vector<std::string> out;
    out.reserve(nodes.size());
    for (const auto& n : nodes) out.push_back(n.name);
    return out;
}

// Flat name-only dependency walk used by lgpm_get_module_dependencies.
// Wraps resolveDependencies so the library no longer ships a parallel
// name-projecting API — the C ABI consumer gets its name list built here.
static std::vector<std::string> dependenciesFlat(PackageManagerLib& lib,
                                                 const std::string& name,
                                                 bool recursive) {
    auto tree = lib.resolveDependencies(name);
    if (!tree) return {};
    return nodeNames(recursive ? tree->flatten() : tree->children);
}

// Flat name-only reverse-edge walk used by lgpm_get_module_dependents.
// Thin projection over resolveDependents.
static std::vector<std::string> dependentsFlat(PackageManagerLib& lib,
                                                const std::string& name,
                                                bool recursive) {
    auto tree = lib.resolveDependents(name);
    if (!tree) return {};
    return nodeNames(recursive ? tree->flatten() : tree->children);
}

struct lgpm_context_opaque {
    PackageManagerLib lib;
};

extern "C" {

lgpm_context_t lgpm_create(void) {
    return new lgpm_context_opaque();
}

void lgpm_free(lgpm_context_t ctx) {
    delete ctx;
}

void lgpm_set_embedded_modules_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.setEmbeddedModulesDirectory(dir);
}

void lgpm_add_embedded_modules_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.addEmbeddedModulesDirectory(dir);
}

void lgpm_set_embedded_ui_plugins_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.setEmbeddedUiPluginsDirectory(dir);
}

void lgpm_add_embedded_ui_plugins_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.addEmbeddedUiPluginsDirectory(dir);
}

void lgpm_set_user_modules_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.setUserModulesDirectory(dir);
}

void lgpm_set_user_ui_plugins_dir(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.setUserUiPluginsDirectory(dir);
}

char* lgpm_install_file(lgpm_context_t ctx, const char* lgx_path, bool skip_if_not_newer,
                         char** installed_plugin_path, bool* is_core_module) {
    if (!ctx || !lgx_path) {
        set_error("Invalid arguments");
        return nullptr;
    }

    std::string errorMsg;
    std::string pluginPath;
    bool coreModule = false;

    std::string result = ctx->lib.installPluginFile(
        lgx_path, errorMsg, skip_if_not_newer,
        installed_plugin_path ? &pluginPath : nullptr,
        is_core_module ? &coreModule : nullptr
    );

    if (result.empty()) {
        set_error(errorMsg);
        return nullptr;
    }

    if (installed_plugin_path) {
        *installed_plugin_path = to_c_string(pluginPath);
    }
    if (is_core_module) {
        *is_core_module = coreModule;
    }

    return to_c_string(result);
}

char* lgpm_scan_installed(lgpm_context_t ctx, const char** dirs, size_t num_dirs) {
    if (!ctx) {
        set_error("Invalid context");
        return nullptr;
    }
    (void)dirs; (void)num_dirs;
    return to_c_string(structsToJsonString(ctx->lib.getInstalledPackages()));
}

char* lgpm_get_installed_modules(lgpm_context_t ctx) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }
    return to_c_string(structsToJsonString(ctx->lib.getInstalledModules()));
}

char* lgpm_get_installed_ui_plugins(lgpm_context_t ctx) {
    if (!ctx) { set_error("Invalid context"); return nullptr; }
    return to_c_string(structsToJsonString(ctx->lib.getInstalledUiPlugins()));
}

static const char** names_to_c_array(const std::vector<std::string>& names) {
    const char** result = static_cast<const char**>(
        malloc((names.size() + 1) * sizeof(const char*)));
    if (!result) return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        result[i] = to_c_string(names[i]);
    }
    result[names.size()] = nullptr;
    return result;
}

const char** lgpm_get_module_dependencies(lgpm_context_t ctx,
                                          const char* module_name,
                                          bool recursive) {
    if (!ctx || !module_name) {
        set_error("Invalid arguments");
        return nullptr;
    }
    return names_to_c_array(dependenciesFlat(ctx->lib, module_name, recursive));
}

const char** lgpm_get_module_dependents(lgpm_context_t ctx,
                                        const char* module_name,
                                        bool recursive) {
    if (!ctx || !module_name) {
        set_error("Invalid arguments");
        return nullptr;
    }
    return names_to_c_array(dependentsFlat(ctx->lib, module_name, recursive));
}

void lgpm_set_signature_policy(lgpm_context_t ctx, const char* policy) {
    if (!ctx || !policy) return;
    std::string p(policy);
    if (p == "none") ctx->lib.setSignaturePolicy(SignaturePolicy::NONE);
    else if (p == "warn") ctx->lib.setSignaturePolicy(SignaturePolicy::WARN);
    else if (p == "require") ctx->lib.setSignaturePolicy(SignaturePolicy::REQUIRE);
}

void lgpm_set_keyring_path(lgpm_context_t ctx, const char* dir) {
    if (ctx && dir) ctx->lib.setKeyringDirectory(dir);
}

const char* lgpm_current_variant(void) {
    static thread_local std::string variant = PackageManagerLib::currentPlatformVariant();
    return variant.c_str();
}

const char** lgpm_variants_to_try(void) {
    auto variants = PackageManagerLib::platformVariantsToTry();

    const char** result = static_cast<const char**>(malloc((variants.size() + 1) * sizeof(const char*)));
    if (!result) return nullptr;

    for (size_t i = 0; i < variants.size(); ++i) {
        result[i] = to_c_string(variants[i]);
    }
    result[variants.size()] = nullptr;

    return result;
}

bool lgpm_version_gte(const char* a, const char* b) {
    if (!a || !b) return false;
    return PackageManagerLib::versionGreaterOrEqual(a, b);
}

void lgpm_free_string(char* str) {
    free(str);
}

void lgpm_free_string_array(const char** arr) {
    if (!arr) return;
    for (size_t i = 0; arr[i]; ++i) {
        free(const_cast<char*>(arr[i]));
    }
    free(const_cast<char**>(arr));
}

const char* lgpm_get_last_error(void) {
    return g_last_error.c_str();
}

} // extern "C"
