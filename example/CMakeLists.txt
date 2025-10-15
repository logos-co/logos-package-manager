cmake_minimum_required(VERSION 3.14)

project(PackageManagerPluginLoader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

add_executable(plugin_loader plugin_loader.cpp)

target_link_libraries(plugin_loader PRIVATE 
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

# Ensure the executable can locate Qt frameworks used by plugins at runtime
if(APPLE)
    # Embed build-time rpaths to Qt lib dirs so dyld finds frameworks for plugins
    set_target_properties(plugin_loader PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        BUILD_RPATH "$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>;$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::RemoteObjects>"
        INSTALL_RPATH "$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>;$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::RemoteObjects>"
    )
else()
    set_target_properties(plugin_loader PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()


