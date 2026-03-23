# CLI executable for logos-package-manager
cmake_minimum_required(VERSION 3.14)

set(CLI_SOURCES
    main.cpp
)

add_executable(lgpm ${CLI_SOURCES})

target_include_directories(lgpm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LGX_ROOT}/include
)

target_link_libraries(lgpm PRIVATE
    package_manager_lib
    nlohmann_json::nlohmann_json
    ${LGX_LIB}
)

# Propagate portable build flag to CLI
if(LGPM_PORTABLE_BUILD)
    target_compile_definitions(lgpm PRIVATE LGPM_PORTABLE_BUILD=1)
endif()

set_target_properties(lgpm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Platform-specific RPATH settings
if(APPLE)
    set_target_properties(lgpm PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../lib;${LGX_ROOT}/lib"
    )
elseif(UNIX)
    set_target_properties(lgpm PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/../lib:${LGX_ROOT}/lib"
    )
endif()

install(TARGETS lgpm
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
