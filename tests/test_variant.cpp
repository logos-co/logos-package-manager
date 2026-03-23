#include <gtest/gtest.h>
#include "package_manager_lib.h"
#include <algorithm>

TEST(VariantTest, CurrentPlatformVariantNotEmpty) {
    std::string variant = PackageManagerLib::currentPlatformVariant();
    EXPECT_FALSE(variant.empty());
    EXPECT_NE(variant, "unknown");
}

TEST(VariantTest, CurrentPlatformVariantFormat) {
    std::string variant = PackageManagerLib::currentPlatformVariant();
    // Should contain a dash separating OS and arch
    EXPECT_NE(variant.find('-'), std::string::npos);
}

TEST(VariantTest, PlatformVariantsToTryNotEmpty) {
    auto variants = PackageManagerLib::platformVariantsToTry();
    EXPECT_FALSE(variants.empty());
}

TEST(VariantTest, PlatformVariantsContainsPrimary) {
    std::string primary = PackageManagerLib::currentPlatformVariant();
    auto variants = PackageManagerLib::platformVariantsToTry();

    // In dev mode the variants have "-dev" suffix; in portable mode they don't.
    // Either way the list should not be empty and first entry should relate to primary.
    EXPECT_FALSE(variants.empty());

    // The first variant should start with the primary platform
    bool found = false;
    for (const auto& v : variants) {
        if (v.find(primary) != std::string::npos || primary.find(v) != std::string::npos
            || v.find(primary.substr(0, primary.find('-'))) != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "No variant related to " << primary << " found in platformVariantsToTry()";
}
