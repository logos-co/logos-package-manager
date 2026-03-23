# logos-package-manager tests

add_executable(lgpm_tests
    test_version.cpp
    test_variant.cpp
    test_scanning.cpp
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
