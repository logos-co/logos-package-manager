#pragma once

// Opt-in JSON helpers for the PackageManagerLib struct types.
//
// The public `package_manager_lib.h` deliberately keeps nlohmann::json out
// of its include graph — callers that only need structs (Qt module
// wrapper, library tests) shouldn't pay the parse cost or transitively
// depend on the JSON library. Include this header only at sites that
// need JSON output (the C ABI shim, the `lgpm --json` CLI flag, etc.).
//
// The emitted JSON matches the legacy wire format that used to be
// returned from PackageManagerLib directly, so the C ABI surface is
// preserved byte-for-byte across the refactor.

#include "package_manager_lib.h"

#include <nlohmann/json.hpp>

void to_json(nlohmann::json& j, const Hashes& h);
void to_json(nlohmann::json& j, const InstalledPackage& p);
void to_json(nlohmann::json& j, const DependencyTreeNode& n);
void to_json(nlohmann::json& j, const DependentTreeNode& n);
