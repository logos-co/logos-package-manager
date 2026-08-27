# logos-package-manager tests

add_executable(lgpm_tests
    test_version.cpp
    test_variant.cpp
    test_scanning.cpp
    test_scan_dedup.cpp
    test_signature.cpp
    test_installed_path.cpp
    test_dependency_resolution.cpp
    test_signer_identity.cpp
    test_uninstall.cpp
    test_path_traversal.cpp
    test_readonly_install.cpp
)

# Signature tests need lgx.h and liblgx
target_include_directories(lgpm_tests PRIVATE ${LGX_ROOT}/include)
find_library(LGX_LIB_TEST lgx PATHS ${LGX_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
target_link_libraries(lgpm_tests PRIVATE ${LGX_LIB_TEST})

# Ensure liblgx.dylib and libpackage_manager_lib are findable at runtime
# (needed for gtest_discover_tests which runs the binary during build)
set_target_properties(lgpm_tests PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${LGX_ROOT}/lib;${CMAKE_BINARY_DIR}/lib"
)

if(TARGET GTest::gtest)
    target_link_libraries(lgpm_tests PRIVATE
        package_manager_lib
        GTest::gtest
        GTest::gtest_main
    )
else()
    target_link_libraries(lgpm_tests PRIVATE
        package_manager_lib
        gtest
        gtest_main
    )
endif()

gtest_discover_tests(lgpm_tests)
