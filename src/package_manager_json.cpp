#include "package_manager_json.h"

void to_json(nlohmann::json& j, const Hashes& h)
{
    j = nlohmann::json::object();
    j["root"] = h.root;
}

void to_json(nlohmann::json& j, const InstalledPackage& p)
{
    j = nlohmann::json::object();
    j["name"]         = p.name;
    j["version"]      = p.version;
    j["description"]  = p.description;
    j["type"]         = p.type;
    j["category"]     = p.category;
    j["author"]       = p.author;
    j["license"]      = p.license;
    j["icon"]         = p.icon;
    j["view"]         = p.view;
    j["manifestVersion"] = p.manifestVersion;
    j["dependencies"] = p.dependencies;
    j["hashes"]       = p.hashes;
    j["installType"]  = installTypeToString(p.installType);
    j["installDir"]   = p.installDir;
    j["mainFilePath"] = p.mainFilePath;
}

void to_json(nlohmann::json& j, const DependencyTreeNode& n)
{
    j = nlohmann::json::object();
    j["name"]     = n.name;
    j["status"]   = dependencyStatusToString(n.status);
    // For Cycle and NotInstalled nodes, installType has no meaningful value;
    // emit the empty string to match the legacy wire format produced when
    // the lib returned JSON directly.
    if (n.status == DependencyStatus::Installed) {
        j["version"]     = n.version;
        j["installType"] = installTypeToString(n.installType);
    } else {
        j["version"]     = "";
        j["installType"] = "";
    }
    j["children"] = n.children;
}

void to_json(nlohmann::json& j, const DependentTreeNode& n)
{
    j = nlohmann::json::object();
    j["name"]        = n.name;
    j["version"]     = n.version;
    j["type"]        = n.type;
    j["installType"] = installTypeToString(n.installType);
    j["installDir"]  = n.installDir;
    j["children"]    = n.children;
}
