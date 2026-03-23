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
