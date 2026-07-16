#include <gtest/gtest.h>
#include "package_manager_lib.h"

TEST(VersionTest, EqualVersions) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.0.0"));
}

TEST(VersionTest, GreaterMajor) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("2.0.0", "1.0.0"));
}

TEST(VersionTest, GreaterMinor) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.2.0", "1.1.0"));
}

TEST(VersionTest, GreaterPatch) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.2", "1.0.1"));
}

TEST(VersionTest, LesserMajor) {
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "2.0.0"));
}

TEST(VersionTest, LesserMinor) {
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.1.0"));
}

TEST(VersionTest, LesserPatch) {
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.0.1"));
}

TEST(VersionTest, DifferentLengths) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.0"));
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0", "1.0.0"));
}

TEST(VersionTest, ShortVersionGreater) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("2.0", "1.0.0"));
}

TEST(VersionTest, EmptyVersions) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("", ""));
}

// ─────────────────────────── pre-releases ───────────────────────────
//
// versionGreaterOrEqual now delegates to the shared implementation in
// logos-package (include/logos/semver.hpp). It used to split on '.' and atoi()
// each component, so "1.0.0-rc1" parsed as 1.0.0 and every pre-release compared
// EQUAL to its own release — meaning `install --skip-if-not-newer` refused to
// upgrade a 1.0.0-rc1 to the real 1.0.0.
//
// Exhaustive precedence coverage lives in logos-package's test_semver.cpp; these
// pin the behaviour this gate actually depends on.

TEST(VersionTest, ReleaseIsNewerThanItsPreRelease) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.0.0-rc.1"));
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc.1", "1.0.0"));
}

// The exact regression the old atoi() split produced: a single-token
// pre-release with NO dot. "1.0.0-rc1" split on '.' gave ["1","0","0-rc1"],
// and atoi("0-rc1") == 0, so it compared EQUAL to "1.0.0". "rc.1" (with a dot)
// wouldn't have reproduced it — the third component would have been a clean
// "0". Keep this distinct case so a future regression here is caught.
TEST(VersionTest, SingleTokenPreReleaseIsNewerThanRelease) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "1.0.0-rc1"));
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc1", "1.0.0"));
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-beta2", "1.0.0-beta1"));
}

// The reported bug: numeric pre-release identifiers compare numerically, so
// rc.11 is newer than rc.2 (a plain string compare says the opposite).
TEST(VersionTest, NumericPreReleaseIdentifiersCompareNumerically) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc.11", "1.0.0-rc.2"));
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc.2", "1.0.0-rc.11"));
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-beta.11", "1.0.0-beta.2"));
}

TEST(VersionTest, PreReleaseOrderingAcrossIdentifiers) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-beta", "1.0.0-alpha"));
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc.1", "1.0.0-beta.11"));
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("1.0.0-alpha", "1.0.0-beta"));
}

TEST(VersionTest, EqualPreReleasesAreGreaterOrEqual) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0-rc.1", "1.0.0-rc.1"));
}

// Build metadata is ignored for precedence (spec §10), so these are "equal" and
// the gate treats an incoming build as not-newer.
TEST(VersionTest, BuildMetadataIgnored) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0+build.2", "1.0.0+build.1"));
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0+build.1", "1.0.0+build.2"));
}

// Comparison stays lenient about partial versions (see DifferentLengths above);
// unparseable junk sorts below anything real, so a garbage installed version
// never blocks a genuine upgrade.
TEST(VersionTest, UnparseableVersionsSortLowest) {
    EXPECT_TRUE(PackageManagerLib::versionGreaterOrEqual("1.0.0", "banana"));
    EXPECT_FALSE(PackageManagerLib::versionGreaterOrEqual("banana", "1.0.0"));
}
