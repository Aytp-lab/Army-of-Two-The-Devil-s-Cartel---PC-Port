# AO4 frontend for the public test build: first-run setup (player supplies their own ISO),
# launcher with all settings, profile manager. No game data is part of this project.
cmake_minimum_required(VERSION 3.25)
project(ao4_frontend LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # AO4Launcher.exe runs without the VC++ redistributable
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

add_library(ao4_setup STATIC src/game_setup.cpp)
target_include_directories(ao4_setup PUBLIC src)
target_link_libraries(ao4_setup PUBLIC bcrypt)

add_executable(ao4_setup_check src/setup_check.cpp)
target_link_libraries(ao4_setup_check PRIVATE ao4_setup)

# Settings model, texts (DE/EN), game start.
add_library(ao4_frontend_core STATIC src/i18n.cpp src/json_lite.cpp src/config.cpp src/launch.cpp src/partner_icons.cpp)
target_include_directories(ao4_frontend_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../tools/rexglue-sdk-source-0.10.0/thirdparty/stb)
target_include_directories(ao4_frontend_core PUBLIC src)
target_link_libraries(ao4_frontend_core PUBLIC bcrypt iphlpapi ws2_32)

# Dear ImGui from the SDK's thirdparty folder (MIT), Win32 + DX11 backends.
set(AO4_IMGUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tools/rexglue-sdk-source-0.10.0/thirdparty/imgui")
add_library(ao4_imgui STATIC
  ${AO4_IMGUI_DIR}/imgui.cpp ${AO4_IMGUI_DIR}/imgui_draw.cpp ${AO4_IMGUI_DIR}/imgui_tables.cpp
  ${AO4_IMGUI_DIR}/imgui_widgets.cpp ${AO4_IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${AO4_IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(ao4_imgui PUBLIC ${AO4_IMGUI_DIR} ${AO4_IMGUI_DIR}/backends)
target_link_libraries(ao4_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_executable(AO4Launcher WIN32 src/launcher_main.cpp src/launcher.rc)
target_link_libraries(AO4Launcher PRIVATE ao4_setup ao4_frontend_core ao4_imgui comdlg32 shell32 user32)
target_link_options(AO4Launcher PRIVATE /MANIFEST:NO)  # own manifest in launcher.rc

add_executable(ao4_icon_check src/icon_check.cpp)
target_link_libraries(ao4_icon_check PRIVATE ao4_frontend_core)

# Diagnosis: sample one thread of the running game (not packaged).
add_executable(ao4_thread_sampler src/thread_sampler.cpp)
