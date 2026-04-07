#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "package_manager_lib.h"

namespace fs = std::filesystem;

using json = nlohmann::json;

// Parse "--key=value" or "--key value" style options.
// Returns true and sets `value` if the arg matches the given key.
static bool parseOption(const std::vector<std::string>& args, size_t& i,
                        const std::string& key, std::string& value) {
    const std::string& arg = args[i];
    if (arg == key && i + 1 < args.size()) {
        value = args[++i];
        return true;
    }
    if (arg.rfind(key + "=", 0) == 0) {
        value = arg.substr(key.size() + 1);
        return true;
    }
    return false;
}

static void printHelp() {
    std::cout << "lgpm - Logos Package Manager CLI\n"
              << "\n"
              << "Usage: lgpm [options] <command> [arguments]\n"
              << "\n"
              << "Commands:\n"
              << "  install --file <path>   Install from a local LGX file\n"
              << "  install --dir <path>    Install all LGX files in a directory\n"
              << "  list                    List installed packages\n"
              << "  info <package>          Show installed package info\n"
              << "\n"
              << "Options:\n"
              << "  --modules-dir <path>    Set core modules directory\n"
              << "  --ui-plugins-dir <path> Set UI plugins directory\n"
              << "  --file <path>           LGX file path (for install command)\n"
              << "  --dir <path>            Directory of LGX files (for install command)\n"
              << "  --json                  Output in JSON format\n"
              << "  --allow-unsigned        Accept unsigned packages without warning\n"
              << "  --require-signatures    Reject unsigned packages\n"
              << "  --tofu                  Trust unknown signing keys on first use\n"
              << "  --keyring <path>        Override keyring directory\n"
              << "  -h, --help              Show this help message\n"
              << "  -v, --version           Show version information\n";
}

static int cmdInstallFile(PackageManagerLib& pm, const std::string& filePath) {
    std::cout << "Installing from file: " << filePath << "..." << std::flush;

    std::string errorMsg;
    std::string installedPath = pm.installPluginFile(filePath, errorMsg);

    if (installedPath.empty()) {
        std::cout << " FAILED\n";
        std::cerr << "Error: " << errorMsg << "\n";
        return 1;
    }

    std::cout << " done\n";
    std::cout << "Installed to: " << installedPath << "\n";
    return 0;
}

static int cmdInstallDir(PackageManagerLib& pm, const std::string& dirPath) {
    if (!fs::is_directory(dirPath)) {
        std::cerr << "Error: not a directory: " << dirPath << "\n";
        return 1;
    }

    std::vector<fs::path> lgxFiles;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.is_regular_file() && entry.path().extension() == ".lgx") {
            lgxFiles.push_back(entry.path());
        }
    }

    if (lgxFiles.empty()) {
        std::cerr << "No .lgx files found in: " << dirPath << "\n";
        return 1;
    }

    std::sort(lgxFiles.begin(), lgxFiles.end());

    int failures = 0;
    for (const auto& lgxPath : lgxFiles) {
        std::cout << "Installing " << lgxPath.filename().string() << "..." << std::flush;

        std::string errorMsg;
        std::string installedPath = pm.installPluginFile(lgxPath.string(), errorMsg);

        if (installedPath.empty()) {
            std::cout << " FAILED\n";
            std::cerr << "  Error: " << errorMsg << "\n";
            ++failures;
        } else {
            std::cout << " done\n";
        }
    }

    std::cout << "\nInstalled " << (lgxFiles.size() - failures) << "/" << lgxFiles.size() << " package(s)";
    if (failures > 0) std::cout << " (" << failures << " failed)";
    std::cout << "\n";

    return failures > 0 ? 1 : 0;
}

static void printInstalledTable(const json& modules) {
    printf("%-30s %-15s %-10s %-15s\n", "NAME", "VERSION", "TYPE", "CATEGORY");
    std::cout << std::string(70, '-') << "\n";

    for (const auto& mod : modules) {
        printf("%-30s %-15s %-10s %-15s\n",
               mod.value("name", "").c_str(),
               mod.value("version", "").c_str(),
               mod.value("type", "").c_str(),
               mod.value("category", "").c_str());
    }
}

static int cmdListInstalled(PackageManagerLib& pm, bool jsonOutput) {
    std::string modulesJson = pm.getInstalledPackages();
    json modules = json::parse(modulesJson);

    if (modules.empty() && pm.allDirectories().empty()) {
        std::cerr << "Error: no directories specified. Use --modules-dir and/or --ui-plugins-dir\n";
        return 1;
    }

    if (modules.empty()) {
        std::cout << "No installed modules found\n";
        return 0;
    }

    if (jsonOutput) {
        std::cout << modules.dump(2) << "\n";
    } else {
        std::cout << "Found " << modules.size() << " installed module(s):\n\n";
        printInstalledTable(modules);
    }

    return 0;
}

static int cmdInfo(PackageManagerLib& pm, const std::string& packageName, bool jsonOutput) {
    std::string modulesJson = pm.getInstalledPackages();
    json modules = json::parse(modulesJson);

    json found;
    for (const auto& mod : modules) {
        if (mod.value("name", "") == packageName) {
            found = mod;
            break;
        }
    }

    if (found.is_null()) {
        std::cerr << "Error: installed package '" << packageName << "' not found\n";
        return 1;
    }

    if (jsonOutput) {
        std::cout << found.dump(2) << "\n";
    } else {
        std::cout << "Name: " << found.value("name", "") << "\n";
        std::cout << "Version: " << found.value("version", "") << "\n";
        std::cout << "Description: " << found.value("description", "") << "\n";
        std::cout << "Type: " << found.value("type", "") << "\n";
        std::cout << "Category: " << found.value("category", "") << "\n";
        std::cout << "Author: " << found.value("author", "") << "\n";
        std::cout << "Directory: " << found.value("installDir", "") << "\n";

        if (found.contains("dependencies") && found["dependencies"].is_array() && !found["dependencies"].empty()) {
            std::cout << "Dependencies: ";
            bool first = true;
            for (const auto& d : found["dependencies"]) {
                if (!first) std::cout << ", ";
                std::cout << d.get<std::string>();
                first = false;
            }
            std::cout << "\n";
        } else {
            std::cout << "Dependencies: none\n";
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string modulesDir, uiPluginsDir, filePath, installDir, command, keyringDir;
    std::vector<std::string> positionalArgs;
    bool jsonOutput = false;
    bool allowUnsigned = false;
    bool requireSignatures = false;
    bool tofu = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-h" || args[i] == "--help") {
            printHelp();
            return 0;
        } else if (args[i] == "-v" || args[i] == "--version") {
            std::cout << "lgpm version 1.0.0\n";
            return 0;
        } else if (parseOption(args, i, "--modules-dir", modulesDir)) {
        } else if (parseOption(args, i, "--ui-plugins-dir", uiPluginsDir)) {
        } else if (parseOption(args, i, "--file", filePath)) {
        } else if (parseOption(args, i, "--dir", installDir)) {
        } else if (parseOption(args, i, "--keyring", keyringDir)) {
        } else if (args[i] == "--json") {
            jsonOutput = true;
        } else if (args[i] == "--allow-unsigned") {
            allowUnsigned = true;
        } else if (args[i] == "--require-signatures") {
            requireSignatures = true;
        } else if (args[i] == "--tofu") {
            tofu = true;
        } else {
            positionalArgs.push_back(args[i]);
        }
    }

    if (positionalArgs.empty()) {
        printHelp();
        return 1;
    }

    command = positionalArgs[0];
    positionalArgs.erase(positionalArgs.begin());

    PackageManagerLib pm;

    if (!modulesDir.empty())
        pm.setUserModulesDirectory(modulesDir);
    if (!uiPluginsDir.empty())
        pm.setUserUiPluginsDirectory(uiPluginsDir);

    // Signature policy
    if (requireSignatures)
        pm.setSignaturePolicy(SignaturePolicy::REQUIRE);
    else if (allowUnsigned)
        pm.setSignaturePolicy(SignaturePolicy::NONE);

    if (!keyringDir.empty())
        pm.setKeyringDirectory(keyringDir);
    if (tofu)
        pm.setTofuEnabled(true);

    if (command == "install") {
        if (!filePath.empty()) {
            return cmdInstallFile(pm, filePath);
        }
        if (!installDir.empty()) {
            return cmdInstallDir(pm, installDir);
        }
        std::cerr << "Error: install requires --file <path> or --dir <path>\n";
        return 1;
    } else if (command == "list") {
        return cmdListInstalled(pm, jsonOutput);
    } else if (command == "info") {
        if (positionalArgs.empty()) {
            std::cerr << "Error: info requires a package name\n";
            return 1;
        }
        return cmdInfo(pm, positionalArgs[0], jsonOutput);
    } else {
        std::cerr << "Error: unknown command '" << command << "'\n";
        std::cerr << "Run 'lgpm --help' for usage information\n";
        return 1;
    }
}
