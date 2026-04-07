#ifndef LGPM_H
#define LGPM_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-specific export macros */
#if defined(_WIN32) || defined(__CYGWIN__)
  #ifdef LGPM_BUILD_SHARED
    #ifdef __GNUC__
      #define LGPM_EXPORT __attribute__((dllexport))
    #else
      #define LGPM_EXPORT __declspec(dllexport)
    #endif
  #else
    #ifdef __GNUC__
      #define LGPM_EXPORT __attribute__((dllimport))
    #else
      #define LGPM_EXPORT __declspec(dllimport)
    #endif
  #endif
#else
  #if __GNUC__ >= 4
    #define LGPM_EXPORT __attribute__((visibility("default")))
  #else
    #define LGPM_EXPORT
  #endif
#endif

/* Opaque handle */
typedef struct lgpm_context_opaque* lgpm_context_t;

/* Result type */
typedef struct {
    bool success;
    const char* error;
} lgpm_result_t;

/* Context lifecycle */
LGPM_EXPORT lgpm_context_t lgpm_create(void);
LGPM_EXPORT void lgpm_free(lgpm_context_t ctx);

/* Configuration — embedded directories (multiple, read-only, set at build time) */
LGPM_EXPORT void lgpm_set_embedded_modules_dir(lgpm_context_t ctx, const char* dir);
LGPM_EXPORT void lgpm_add_embedded_modules_dir(lgpm_context_t ctx, const char* dir);
LGPM_EXPORT void lgpm_set_embedded_ui_plugins_dir(lgpm_context_t ctx, const char* dir);
LGPM_EXPORT void lgpm_add_embedded_ui_plugins_dir(lgpm_context_t ctx, const char* dir);

/* Configuration — user directories (read-write, where new packages are installed) */
LGPM_EXPORT void lgpm_set_user_modules_dir(lgpm_context_t ctx, const char* dir);
LGPM_EXPORT void lgpm_set_user_ui_plugins_dir(lgpm_context_t ctx, const char* dir);

/**
 * Install from local LGX file.
 * Returns installed directory path (caller frees with lgpm_free_string), or NULL on error.
 * If installed_plugin_path is non-NULL, it receives the full path to the installed main file
 * (caller frees with lgpm_free_string).
 * If is_core_module is non-NULL, it receives whether the module type is "core".
 */
LGPM_EXPORT char* lgpm_install_file(lgpm_context_t ctx, const char* lgx_path, bool skip_if_not_newer,
                                     char** installed_plugin_path, bool* is_core_module);

/**
 * Scan installed packages (all types).
 * Returns JSON string of installed packages array (caller frees with lgpm_free_string).
 * Each element contains all manifest.json fields + "installDir" + "mainFilePath".
 */
LGPM_EXPORT char* lgpm_scan_installed(lgpm_context_t ctx, const char** dirs, size_t num_dirs);

/**
 * Get installed core modules (type="core").
 * Returns JSON string array (caller frees with lgpm_free_string).
 * Each element contains all manifest.json fields + "installDir" + "mainFilePath".
 */
LGPM_EXPORT char* lgpm_get_installed_modules(lgpm_context_t ctx);

/**
 * Get installed UI plugins (type="ui" or "ui_qml").
 * Returns JSON string array (caller frees with lgpm_free_string).
 * Each element contains all manifest.json fields + "installDir" + "mainFilePath".
 */
LGPM_EXPORT char* lgpm_get_installed_ui_plugins(lgpm_context_t ctx);

/* Signature policy configuration */

/**
 * Set signature verification policy.
 * @param policy "none", "warn", or "require"
 */
LGPM_EXPORT void lgpm_set_signature_policy(lgpm_context_t ctx, const char* policy);

/**
 * Set keyring directory for trusted key lookup.
 * @param dir Path to keyring directory (NULL for default ~/.config/logos/trusted-keys/)
 */
LGPM_EXPORT void lgpm_set_keyring_path(lgpm_context_t ctx, const char* dir);

/**
 * Enable Trust On First Use (TOFU) for unknown signing keys.
 * When enabled, unknown keys are automatically added to the keyring.
 */
LGPM_EXPORT void lgpm_enable_tofu(lgpm_context_t ctx, bool enabled);

/* Platform variant */
LGPM_EXPORT const char* lgpm_current_variant(void);
LGPM_EXPORT const char** lgpm_variants_to_try(void);

/* Utilities */
LGPM_EXPORT bool lgpm_version_gte(const char* a, const char* b);

/* Memory management */
LGPM_EXPORT void lgpm_free_string(char* str);
LGPM_EXPORT void lgpm_free_string_array(const char** arr);

/* Error handling */
LGPM_EXPORT const char* lgpm_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* LGPM_H */
