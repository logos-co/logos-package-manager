#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

class DependencyResolutionTest : public ::testing::Test {
protected:
    fs::path modulesDir;
    fs::path uiPluginsDir;

    void SetUp() override {
        auto base = fs::temp_directory_path() / ("lgpm_dep_" + std::to_string(std::rand()));
        modulesDir = base / "modules";
        uiPluginsDir = base / "ui_plugins";
        fs::create_directories(modulesDir);
        fs::create_directories(uiPluginsDir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(modulesDir.parent_path(), ec);
    }

    void writeManifest(const fs::path& parentDir,
                       const std::string& name,
                       const std::string& type,
                       const std::vector<std::string>& deps = {},
                       const std::string& version = "1.0.0") {
        fs::path dir = parentDir / name;
        fs::create_directories(dir);

        json manifest;
        manifest["name"] = name;
        manifest["type"] = type;
        manifest["version"] = version;
        manifest["dependencies"] = deps;

        std::ofstream mf(dir / "manifest.json");
        mf << manifest.dump(2);
    }

    // Helpers mirroring the flat name-list projections that the C ABI and
    // CLI build on top of resolveDependencies / resolveDependents. Tests
    // that used to exercise the library's getDependencies / getDependents
    // now exercise these projections — the library no longer ships a
    // parallel name-only API, but the semantics are preserved.
    template <typename Node>
    static std::vector<std::string> nodeNames(const std::vector<Node>& nodes) {
        std::vector<std::string> out;
        out.reserve(nodes.size());
        for (const auto& n : nodes) out.push_back(n.name);
        return out;
    }

    static std::vector<std::string> dependenciesFlat(PackageManagerLib& pm,
                                                      const std::string& name,
                                                      bool recursive) {
        auto tree = pm.resolveDependencies(name);
        if (!tree) return {};
        return nodeNames(recursive ? tree->flatten() : tree->children);
    }

    static std::vector<std::string> dependentsFlat(PackageManagerLib& pm,
                                                    const std::string& name,
                                                    bool recursive) {
        auto tree = pm.resolveDependents(name);
        if (!tree) return {};
        return nodeNames(recursive ? tree->flatten() : tree->children);
    }
};

TEST_F(DependencyResolutionTest, ResolveLinearChain) {
    // A -> B -> C
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"c"});
    writeManifest(modulesDir, "c", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("a");
    ASSERT_TRUE(tree);
    EXPECT_EQ(tree->name, "a");
    EXPECT_EQ(tree->status, DependencyStatus::Installed);
    ASSERT_EQ(tree->children.size(), 1u);

    const auto& bNode = tree->children[0];
    EXPECT_EQ(bNode.name, "b");
    EXPECT_EQ(bNode.status, DependencyStatus::Installed);
    ASSERT_EQ(bNode.children.size(), 1u);

    const auto& cNode = bNode.children[0];
    EXPECT_EQ(cNode.name, "c");
    EXPECT_EQ(cNode.status, DependencyStatus::Installed);
    EXPECT_EQ(cNode.children.size(), 0u);
}

TEST_F(DependencyResolutionTest, MissingDependencyReturnsNotInstalledLeaf) {
    // A -> B (missing)
    writeManifest(modulesDir, "a", "core", {"b"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("a");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    EXPECT_EQ(tree->children[0].name, "b");
    EXPECT_EQ(tree->children[0].status, DependencyStatus::NotInstalled);
    EXPECT_EQ(tree->children[0].children.size(), 0u);
}

TEST_F(DependencyResolutionTest, CrossCategoryDependency) {
    // UI plugin depends on a core module — must find across directories.
    writeManifest(uiPluginsDir, "ui_a", "ui_qml", {"core_b"});
    writeManifest(modulesDir, "core_b", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    pm.setEmbeddedUiPluginsDirectory(uiPluginsDir.string());

    auto tree = pm.resolveDependencies("ui_a");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    EXPECT_EQ(tree->children[0].name, "core_b");
    EXPECT_EQ(tree->children[0].status, DependencyStatus::Installed);
}

TEST_F(DependencyResolutionTest, CycleIsDetectedAndMarked) {
    // A -> B -> A
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"a"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("a");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    const auto& bNode = tree->children[0];
    ASSERT_EQ(bNode.children.size(), 1u);
    EXPECT_EQ(bNode.children[0].name, "a");
    EXPECT_EQ(bNode.children[0].status, DependencyStatus::Cycle);
}

TEST_F(DependencyResolutionTest, UnknownRootReturnsNullopt) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    EXPECT_FALSE(pm.resolveDependencies("nope").has_value());
}

TEST_F(DependencyResolutionTest, ResolveDependentsRootHasDirectChildren) {
    writeManifest(modulesDir, "a", "core", {"c"});
    writeManifest(modulesDir, "b", "core", {"c"});
    writeManifest(modulesDir, "c", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependents("c");
    ASSERT_TRUE(tree);
    EXPECT_EQ(tree->name, "c");
    ASSERT_EQ(tree->children.size(), 2u);

    std::set<std::string> names;
    for (const auto& ch : tree->children) names.insert(ch.name);
    EXPECT_TRUE(names.count("a"));
    EXPECT_TRUE(names.count("b"));
}

TEST_F(DependencyResolutionTest, ResolveDependentsTreeExposesTransitiveViaChildren) {
    // A -> B -> C; D -> C
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"c"});
    writeManifest(modulesDir, "c", "core", {});
    writeManifest(modulesDir, "d", "core", {"c"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependents("c");
    ASSERT_TRUE(tree);

    // Direct dependents of c: b and d. a only reaches c transitively via b.
    std::set<std::string> direct;
    for (const auto& ch : tree->children) direct.insert(ch.name);
    EXPECT_EQ(direct.size(), 2u);
    EXPECT_TRUE(direct.count("b"));
    EXPECT_TRUE(direct.count("d"));
    EXPECT_FALSE(direct.count("a"));

    // Flatten returns every descendant (direct + transitive), deduped.
    auto all = tree->flatten();
    std::set<std::string> allNames;
    for (const auto& n : all) allNames.insert(n.name);
    EXPECT_EQ(allNames.size(), 3u);
    EXPECT_TRUE(allNames.count("a"));
    EXPECT_TRUE(allNames.count("b"));
    EXPECT_TRUE(allNames.count("d"));
}

TEST_F(DependencyResolutionTest, ResolveDependentsEmptyWhenNoDependents) {
    writeManifest(modulesDir, "a", "core", {});
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    auto tree = pm.resolveDependents("a");
    ASSERT_TRUE(tree);
    EXPECT_TRUE(tree->children.empty());
    EXPECT_TRUE(tree->flatten().empty());
}

TEST_F(DependencyResolutionTest, ResolveDependentsUnknownRootReturnsNullopt) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    EXPECT_FALSE(pm.resolveDependents("nope").has_value());
}

TEST_F(DependencyResolutionTest, ResolveDependentsCycleTerminatesFlatten) {
    // A -> B and B -> A — reverse edges form a cycle.
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"a"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependents("a");
    ASSERT_TRUE(tree);
    EXPECT_EQ(tree->name, "a");

    // A child node whose name matches the root is the cycle break — the
    // builder inserts it as an empty-children leaf so descent stops.
    // flatten() dedupes by name, so "b" appears exactly once; whether
    // "a" re-surfaces via the cycle edge is implementation-defined
    // (same latitude as DependenciesFlat_CycleSafe above). We only
    // assert that the walk terminates and that "b" is present.
    auto all = tree->flatten();
    std::set<std::string> names;
    for (const auto& n : all) names.insert(n.name);
    EXPECT_TRUE(names.count("b"));
    EXPECT_EQ(all.size(), names.size());   // no duplicates
}

// ---------------------------------------------------------------------------
// Flat name-only projections over resolveDependencies / resolveDependents.
// The library no longer ships getDependencies / getDependents; these tests
// target the name-projection helper callers build at the boundary.
// ---------------------------------------------------------------------------

TEST_F(DependencyResolutionTest, DependenciesFlat_UnknownName_ReturnsEmpty) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    EXPECT_TRUE(dependenciesFlat(pm, "nope", false).empty());
    EXPECT_TRUE(dependenciesFlat(pm, "nope", true).empty());
}

TEST_F(DependencyResolutionTest, DependentsFlat_UnknownName_ReturnsEmpty) {
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    EXPECT_TRUE(dependentsFlat(pm, "nope", false).empty());
    EXPECT_TRUE(dependentsFlat(pm, "nope", true).empty());
}

TEST_F(DependencyResolutionTest, DependenciesFlat_NoDeps_ReturnsEmpty) {
    writeManifest(modulesDir, "a", "core", {});
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    EXPECT_TRUE(dependenciesFlat(pm, "a", false).empty());
    EXPECT_TRUE(dependenciesFlat(pm, "a", true).empty());
}

TEST_F(DependencyResolutionTest, DependenciesFlat_DirectVsRecursive) {
    // a -> b -> c
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"c"});
    writeManifest(modulesDir, "c", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto direct = dependenciesFlat(pm, "a", false);
    ASSERT_EQ(direct.size(), 1u);
    EXPECT_EQ(direct[0], "b");

    auto transitive = dependenciesFlat(pm, "a", true);
    std::set<std::string> asSet(transitive.begin(), transitive.end());
    EXPECT_EQ(asSet.size(), 2u);
    EXPECT_TRUE(asSet.count("b"));
    EXPECT_TRUE(asSet.count("c"));
}

TEST_F(DependencyResolutionTest, DependentsFlat_DirectVsRecursive) {
    // a -> b -> c; d -> c
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"c"});
    writeManifest(modulesDir, "c", "core", {});
    writeManifest(modulesDir, "d", "core", {"c"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto direct = dependentsFlat(pm, "c", false);
    std::set<std::string> directSet(direct.begin(), direct.end());
    EXPECT_EQ(directSet.size(), 2u);
    EXPECT_TRUE(directSet.count("b"));
    EXPECT_TRUE(directSet.count("d"));
    EXPECT_FALSE(directSet.count("a"));   // a is only a transitive dependent

    auto transitive = dependentsFlat(pm, "c", true);
    std::set<std::string> transitiveSet(transitive.begin(), transitive.end());
    EXPECT_EQ(transitiveSet.size(), 3u);
    EXPECT_TRUE(transitiveSet.count("a"));
    EXPECT_TRUE(transitiveSet.count("b"));
    EXPECT_TRUE(transitiveSet.count("d"));
}

TEST_F(DependencyResolutionTest, DependenciesFlat_DiamondRecursiveDedups) {
    // a -> b, a -> c, b -> d, c -> d
    writeManifest(modulesDir, "a", "core", {"b", "c"});
    writeManifest(modulesDir, "b", "core", {"d"});
    writeManifest(modulesDir, "c", "core", {"d"});
    writeManifest(modulesDir, "d", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto deps = dependenciesFlat(pm, "a", true);
    std::set<std::string> asSet(deps.begin(), deps.end());
    EXPECT_EQ(deps.size(), asSet.size());   // no duplicates
    EXPECT_EQ(asSet.size(), 3u);
    EXPECT_TRUE(asSet.count("b"));
    EXPECT_TRUE(asSet.count("c"));
    EXPECT_TRUE(asSet.count("d"));
}

TEST_F(DependencyResolutionTest, DependentsFlat_DiamondRecursiveDedups) {
    // a -> b, a -> c, b -> d, c -> d
    writeManifest(modulesDir, "a", "core", {"b", "c"});
    writeManifest(modulesDir, "b", "core", {"d"});
    writeManifest(modulesDir, "c", "core", {"d"});
    writeManifest(modulesDir, "d", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto dependents = dependentsFlat(pm, "d", true);
    std::set<std::string> asSet(dependents.begin(), dependents.end());
    EXPECT_EQ(dependents.size(), asSet.size());   // no duplicates
    EXPECT_EQ(asSet.size(), 3u);
    EXPECT_TRUE(asSet.count("a"));
    EXPECT_TRUE(asSet.count("b"));
    EXPECT_TRUE(asSet.count("c"));
}

TEST_F(DependencyResolutionTest, DependenciesFlat_CycleSafe) {
    // a -> b -> a (cycle)
    writeManifest(modulesDir, "a", "core", {"b"});
    writeManifest(modulesDir, "b", "core", {"a"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto deps = dependenciesFlat(pm, "a", true);
    std::set<std::string> asSet(deps.begin(), deps.end());
    // The flat walk starts from a's children and dedups — b appears, and
    // the cycle back to a is pruned (either because the resolver marks
    // that node as Cycle and we still include its name in the flat list,
    // or the seen-set filters it out; either way we end up with b and
    // optionally a from the cycle edge).
    EXPECT_TRUE(asSet.count("b"));
}

TEST_F(DependencyResolutionTest, DependenciesFlat_NonRecursiveIncludesUninstalled) {
    // a -> missing (declared but not installed)
    writeManifest(modulesDir, "a", "core", {"missing"});
    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto deps = dependenciesFlat(pm, "a", false);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "missing");

    // Recursive also lists "missing" but can't expand it further.
    auto transitive = dependenciesFlat(pm, "a", true);
    ASSERT_EQ(transitive.size(), 1u);
    EXPECT_EQ(transitive[0], "missing");
}

TEST_F(DependencyResolutionTest, DependenciesFlat_CrossCategory) {
    // ui_a depends on core_b — must resolve across dir categories.
    writeManifest(uiPluginsDir, "ui_a", "ui_qml", {"core_b"});
    writeManifest(modulesDir, "core_b", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());
    pm.setEmbeddedUiPluginsDirectory(uiPluginsDir.string());

    auto deps = dependenciesFlat(pm, "ui_a", false);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "core_b");
}

// ---------------------------------------------------------------------------
// Object-form dependency entries.
//
// The LGX spec (logos-package docs/spec.md, "Dependency entries") allows each
// element of `dependencies[]` to be either a plain string or an object
// {name, version?, signer?}. The scan used to accept only the string form with
// no else branch, so an object entry lost THE EDGE ITSELF: it vanished from
// resolveDependencies, from resolveDependents (an uninstall stopped warning
// about a dependent that declared its need in object form), and from the
// installed-package `dependencies` array basecamp derives its missing-dep
// marker from. No workspace manifest uses the object form, which is why
// nothing caught it — so these tests write the object form directly.
// ---------------------------------------------------------------------------

// Writes a manifest whose `dependencies` array is supplied verbatim, so a test
// can mix plain-string and object entries exactly as an .lgx manifest may.
static void writeManifestRawDeps(const fs::path& parentDir,
                                 const std::string& name,
                                 const json& deps,
                                 const std::string& version = "1.0.0") {
    fs::path dir = parentDir / name;
    fs::create_directories(dir);
    json manifest;
    manifest["name"] = name;
    manifest["type"] = "core";
    manifest["version"] = version;
    manifest["dependencies"] = deps;
    std::ofstream mf(dir / "manifest.json");
    mf << manifest.dump(2);
}

TEST_F(DependencyResolutionTest, ObjectFormDependencyKeepsForwardEdge) {
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ json{{"name", "lib"}, {"version", "^1.2.0"}} }));
    writeManifest(modulesDir, "lib", "core", {}, "1.5.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    EXPECT_EQ(tree->children[0].name, "lib");
    EXPECT_EQ(tree->children[0].status, DependencyStatus::Installed);
}

TEST_F(DependencyResolutionTest, ObjectFormDependencyKeepsReverseEdge) {
    // The uninstall-safety direction: removing "lib" must still report "app".
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ json{{"name", "lib"}, {"version", "^1.2.0"}} }));
    writeManifest(modulesDir, "lib", "core", {}, "1.5.0");

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependents("lib");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    EXPECT_EQ(tree->children[0].name, "app");
}

TEST_F(DependencyResolutionTest, ObjectFormMissingDependencyIsStillReportedMissing) {
    // The basecamp red-cross path: an object-form dep that is NOT installed
    // must surface as not_installed, not as "no dependencies at all".
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ json{{"name", "absent"}, {"version", "^2.0.0"}} }));

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto tree = pm.resolveDependencies("app");
    ASSERT_TRUE(tree);
    ASSERT_EQ(tree->children.size(), 1u);
    EXPECT_EQ(tree->children[0].name, "absent");
    EXPECT_EQ(tree->children[0].status, DependencyStatus::NotInstalled);
}

TEST_F(DependencyResolutionTest, MixedStringAndObjectDependenciesBothResolve) {
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ "plain",
                                       json{{"name", "ranged"}, {"version", ">=1.0.0"}},
                                       json{{"name", "pinned"},
                                            {"signer", "did:jwk:eyJrdHkiOiJPS1AifQ"}} }));
    writeManifest(modulesDir, "plain",  "core", {});
    writeManifest(modulesDir, "ranged", "core", {});
    writeManifest(modulesDir, "pinned", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto deps = dependenciesFlat(pm, "app", false);
    std::set<std::string> asSet(deps.begin(), deps.end());
    EXPECT_EQ(asSet.size(), 3u);
    EXPECT_TRUE(asSet.count("plain"));
    EXPECT_TRUE(asSet.count("ranged"));
    EXPECT_TRUE(asSet.count("pinned"));
}

TEST_F(DependencyResolutionTest, ObjectFormConstraintsAreCarriedNotJustTheName) {
    // The name alone restores the graph; the range and signer are what make the
    // constraint reachable to anything that evaluates it.
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ "plain",
                                       json{{"name", "ranged"}, {"version", "^1.2.0"}},
                                       json{{"name", "pinned"},
                                            {"version", ">=0.5.0"},
                                            {"signer", "did:jwk:eyJrdHkiOiJPS1AifQ"}} }));

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto pkgs = pm.getInstalledPackages();
    auto it = std::find_if(pkgs.begin(), pkgs.end(),
                           [](const InstalledPackage& p) { return p.name == "app"; });
    ASSERT_NE(it, pkgs.end());

    // Every edge is present in the name list, in declared order.
    ASSERT_EQ(it->dependencies.size(), 3u);
    EXPECT_EQ(it->dependencies[0], "plain");
    EXPECT_EQ(it->dependencies[1], "ranged");
    EXPECT_EQ(it->dependencies[2], "pinned");

    // Only the constrained entries appear in the constraint list.
    ASSERT_EQ(it->dependencyConstraints.size(), 2u);
    EXPECT_EQ(it->dependencyConstraints[0].name, "ranged");
    ASSERT_TRUE(it->dependencyConstraints[0].version.has_value());
    EXPECT_EQ(*it->dependencyConstraints[0].version, "^1.2.0");
    EXPECT_FALSE(it->dependencyConstraints[0].signer.has_value());

    EXPECT_EQ(it->dependencyConstraints[1].name, "pinned");
    ASSERT_TRUE(it->dependencyConstraints[1].version.has_value());
    EXPECT_EQ(*it->dependencyConstraints[1].version, ">=0.5.0");
    ASSERT_TRUE(it->dependencyConstraints[1].signer.has_value());
    EXPECT_EQ(*it->dependencyConstraints[1].signer, "did:jwk:eyJrdHkiOiJPS1AifQ");
}

TEST_F(DependencyResolutionTest, PlainDependenciesCarryNoConstraints) {
    writeManifest(modulesDir, "app", "core", {"lib"});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto pkgs = pm.getInstalledPackages();
    auto it = std::find_if(pkgs.begin(), pkgs.end(),
                           [](const InstalledPackage& p) { return p.name == "app"; });
    ASSERT_NE(it, pkgs.end());
    ASSERT_EQ(it->dependencies.size(), 1u);
    EXPECT_EQ(it->dependencies[0], "lib");
    EXPECT_TRUE(it->dependencyConstraints.empty());
}

TEST_F(DependencyResolutionTest, MalformedDependencyEntriesDeclareNoEdge) {
    // A number, a null, and an object without a string `name` are not
    // dependency entries in either form — they must not synthesise an edge.
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ 42, nullptr, json::object(),
                                       json{{"name", 7}}, "", json{{"name", ""}},
                                       "real" }));
    writeManifest(modulesDir, "real", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    auto deps = dependenciesFlat(pm, "app", false);
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "real");
}

TEST_F(DependencyResolutionTest, MalformedDependencyEntryWarnsOnStderr) {
    // A dropped edge with no diagnostic is the failure mode this whole change
    // is about, so the one remaining drop — a malformed entry — must announce
    // itself. Without the warning, a manifest that bypasses lgx::Manifest
    // validation (an embedded install written at build time, or a hand-edited
    // manifest.json) loses a dependency and nothing anywhere says so.
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ json{{"name", 7}}, "good" }));
    writeManifest(modulesDir, "good", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    testing::internal::CaptureStderr();
    auto deps = dependenciesFlat(pm, "app", false);
    std::string err = testing::internal::GetCapturedStderr();

    // The good edge still resolves...
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "good");

    // ...and the bad one is reported rather than dropped in silence. The
    // message must name the package and the offending entry, because the
    // manifest that produced it is not the one the user is looking at.
    EXPECT_NE(err.find("malformed dependencies[] entry"), std::string::npos) << err;
    EXPECT_NE(err.find("app"), std::string::npos) << err;
    EXPECT_NE(err.find("\"name\":7"), std::string::npos) << err;
}

TEST_F(DependencyResolutionTest, WellFormedDependenciesWarnAboutNothing) {
    // Invariance guard: the warning must fire ONLY for entries that declare no
    // edge. A manifest mixing both legal forms has to stay silent, or the
    // diagnostic becomes noise and gets muted.
    writeManifestRawDeps(modulesDir, "app",
                         json::array({ "plain",
                                       json{{"name", "ranged"}, {"version", "^1.2.0"}} }));
    writeManifest(modulesDir, "plain",  "core", {});
    writeManifest(modulesDir, "ranged", "core", {});

    PackageManagerLib pm;
    pm.setEmbeddedModulesDirectory(modulesDir.string());

    testing::internal::CaptureStderr();
    (void)dependenciesFlat(pm, "app", false);
    std::string err = testing::internal::GetCapturedStderr();

    // Deliberately asserts the warning ONLY, with no edge-count assertion, so
    // this holds identically before and after the fix — a guard that only
    // starts holding once the fix lands is not a guard. That the two entries
    // actually resolve to two edges is MixedStringAndObjectDependenciesBothResolve's
    // job.
    EXPECT_EQ(err.find("malformed dependencies[] entry"), std::string::npos) << err;
}
