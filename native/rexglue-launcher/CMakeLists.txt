# ao4_launcher - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ao4_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(AO4LAUNCHER_SOURCES
    src/main.cpp
    src/windows_fault_trace.cpp
    src/module_unload_hook.cpp
    src/splash_skip_hook.cpp
    # Port menu (F4, public test build) shares the settings model with AO4Launcher.exe.
    ../ao4-frontend/src/config.cpp
    ../ao4-frontend/src/json_lite.cpp
    ../ao4-frontend/src/partner_icons.cpp
)

if(WIN32)
    add_executable(ao4_launcher WIN32 ${AO4LAUNCHER_SOURCES})
else()
    add_executable(ao4_launcher ${AO4LAUNCHER_SOURCES})
endif()

rexglue_setup_target(ao4_launcher)
target_link_libraries(ao4_launcher PRIVATE bcrypt)
set_source_files_properties(../ao4-frontend/src/partner_icons.cpp PROPERTIES INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue-sdk-source-0.10.0/thirdparty/stb)

# Clang's SIMDe fallback emits C23 roundevenf, absent from the Windows UCRT.
# Keep this compatibility code outside generated files. SSE4.1 is required.
if(WIN32 AND TARGET ao4_launcher_AO4_Splash)
    target_sources(ao4_launcher_AO4_Splash PRIVATE src/windows_roundeven.cpp)
    target_link_options(ao4_launcher_AO4_Splash PRIVATE /DEBUG:FULL)
endif()
if(WIN32)
    add_executable(ao4_roundeven_check EXCLUDE_FROM_ALL
        src/windows_roundeven.cpp src/windows_roundeven_check.cpp)
    add_executable(ao4_input_check EXCLUDE_FROM_ALL src/input_check.cpp)
    target_link_libraries(ao4_input_check PRIVATE rex::runtime)
    add_executable(ao4_weapon_button_check EXCLUDE_FROM_ALL src/weapon_button_check.cpp)
    # This diagnostic constructs SDK assignment objects. Use the matching
    # source-build layout, not the older installed SDK's inline constructors.
    target_include_directories(ao4_weapon_button_check BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue-sdk-source-0.10.0/include)
    target_link_libraries(ao4_weapon_button_check PRIVATE rex::runtime)
    add_executable(ao4_xnet_random_check EXCLUDE_FROM_ALL src/xnet_random_check.cpp)
    target_link_libraries(ao4_xnet_random_check PRIVATE rex::runtime)
endif()
