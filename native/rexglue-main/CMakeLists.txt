# ao4_main - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ao4_main LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(AO4MAIN_SOURCES
    src/main.cpp
)

# Job-system diagnostics (forced includes around stores/CAS of the job queues) from the earlier
# crash hunt. Off for release/test builds: they add string compares to every queue CAS.
option(AO4_JOB_DIAG "Build the job-system store/CAS/dispatch observers" OFF)
if(WIN32 AND AO4_JOB_DIAG)
    # Targeted header-store diagnostics without editing generated files.
    # All original stores remain unchanged.
    foreach(AO4_STORE_UNIT IN ITEMS 80 149 153 237 295)
        set_source_files_properties(generated/ao4.main/ao4_main_recomp.${AO4_STORE_UNIT}.cpp PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/job_store_observer.h")
    endforeach()
    foreach(AO4_QUEUE_UNIT IN ITEMS 0 70 83 97 104 289 298)
        set_source_files_properties(generated/ao4.main/ao4_main_recomp.${AO4_QUEUE_UNIT}.cpp PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/job_atomic_observer.h")
    endforeach()
    set_source_files_properties(generated/ao4.main/ao4_main_recomp.70.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/job_dispatch_observer.h")
endif()
if(WIN32)
    add_executable(ao4_main WIN32 ${AO4MAIN_SOURCES})
else()
    add_executable(ao4_main ${AO4MAIN_SOURCES})
endif()

rexglue_setup_target(ao4_main)
# The SDK adds -msse4.1 only for NOT MSVC, so clang-cl builds of the recompiled
# code fall back to SIMDe's scalar emulation of pshufb/blendv/round (A15 profile:
# ~59 % of the voice DSP time in simde_mm_shuffle_epi8). Opt-in until validated.
option(AO4_HOST_SSE41 "Compile recompiled guest code with SSE4.1 (clang-cl)" OFF)
if(AO4_HOST_SSE41 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(ao4_main_recomp PRIVATE -msse4.1)
endif()
if(WIN32)
    target_sources(ao4_main_recomp PRIVATE
        ../rexglue-launcher/src/windows_roundeven.cpp)
    # Reuse the same compiled AOT objects as a loadable native module.
    add_library(ao4_main_native SHARED src/module_exports.cpp src/job_queue_trace.cpp src/job_node_history.cpp src/soldier_observer.cpp src/role_observer.cpp src/qol_fixes.cpp src/stability_guard.cpp src/texture_lifetime_trace.cpp src/aim_source_observer.cpp src/lens_query_trace.cpp src/net_observer.cpp src/weapon_action_trace.cpp src/audio_sample_trace.cpp src/fps_experiment.cpp
 src/fps_dtprobe.cpp
        $<TARGET_OBJECTS:ao4_main_recomp>)
    target_include_directories(ao4_main_native PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} generated/ao4.main)
    target_link_libraries(ao4_main_native PRIVATE rex::runtime)
    target_link_options(ao4_main_native PRIVATE /DEBUG:FULL)
    if(AO4_HOST_SSE41 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(ao4_main_native PRIVATE -msse4.1)
    endif()
    # Isolated diagnostic: original generated queue routines, synthetic test
    # jobs only. This executable is never used by the game launcher.
    add_executable(ao4_queue_check EXCLUDE_FROM_ALL src/queue_check.cpp
        $<TARGET_OBJECTS:ao4_main_recomp>)
    target_include_directories(ao4_queue_check PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} generated/ao4.main)
    target_link_libraries(ao4_queue_check PRIVATE rex::runtime)
endif()
