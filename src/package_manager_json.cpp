#include "package_manager_json.h"

void to_json(nlohmann::json& j, const Hashes& h)
{
    j = nlohmann::json::object();
    j["root"] = h.root;
}

// Emits the entry in the same minimal form the LGX manifest uses: a bare name
// serialises as a plain string, a constrained one as an object. Matches
// lgx::Manifest::toJson so a manifest round-trips through lgpm unchanged.
void to_json(nlohmann::json& j, const PackageDependency& d)
{
    if (d.isSimple()) {
        j = d.name;
        return;
    }
    j = nlohmann::json::object();
    j["name"] = d.name;
    if (d.version) j["version"] = *d.version;
    if (d.signer)  j["signer"]  = *d.signer;
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
    // `dependencies` stays a flat array of NAMES — the shape every consumer
    // and doctest already reads. Object-form manifest entries now contribute
    // their name here like any other edge (they used to be dropped entirely).
    j["dependencies"] = p.dependencies;
    // Additive: present only when at least one entry declared a version range
    // or a signer DID, so today's manifests serialise byte-identically.
    if (!p.dependencyConstraints.empty())
        j["dependencyConstraints"] = p.dependencyConstraints;
    // The DID the installed manifest.sig names, once checked against its own
    // embedded key — see InstalledPackage::signerDid. Additive, and emitted
    // only when a usable signature is installed, so an unsigned or embedded
    // package serialises byte-identically to before and a reader can tell "no
    // signature" (key absent) from a signed one. A key present with an empty
    // value would be neither.
    if (p.signerDid)
        j["signerDid"] = *p.signerDid;
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
    // the lib returned JSON directly. VersionMismatch resolved to a real
    // installed package, so it carries both — the version actually present is
    // the half of the report `requiredVersion` is compared against.
    if (nodeResolvedToAnInstalledPackage(n.status)) {
        j["version"]     = n.version;
        j["installType"] = installTypeToString(n.installType);
    } else {
        j["version"]     = "";
        j["installType"] = "";
    }
    // Additive, and absent unless the parent edge declared one — so a tree of
    // bare-name dependencies, which is every package in the workspace today,
    // serialises byte-identically to before.
    if (n.requiredVersion) j["requiredVersion"] = *n.requiredVersion;
    if (n.requiredSigner)  j["requiredSigner"]  = *n.requiredSigner;
    // The other half of a signer report: who the installed package's own
    // signature says signed it. Absent when no usable signature is installed —
    // which is what makes a `signer_unknown` row legible without a second
    // lookup. NOT the field `status` was derived from: the verdict comes from
    // verifying that signature against `requiredSigner`'s key, so these two
    // can differ without the row being inconsistent.
    if (n.signerDid)  j["signerDid"]  = *n.signerDid;
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
