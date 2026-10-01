cmake_minimum_required(VERSION 3.20)
project(AKVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Dynamic CRT (/MD) — matches the OpenXR loader's build (its CMake forces /MD),
# so everything links. Needs the VC++ redist, which the game + dev machine have.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Reference repos live in the sibling ../references folder (cloned by Claude).
set(REFS "${CMAKE_CURRENT_SOURCE_DIR}/../references")
if(NOT EXISTS "${REFS}/minhook/CMakeLists.txt")
    message(FATAL_ERROR "Expected sibling folder ../references with minhook/imgui clones. Run the reference-clone step first.")
endif()

# --- MinHook (from reference clone, built static) ---
add_subdirectory("${REFS}/minhook" "${CMAKE_BINARY_DIR}/_deps/minhook")

# --- OpenXR loader (static) from reference clone ---
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REFS}/OpenXR-SDK-Source" "${CMAKE_BINARY_DIR}/_deps/openxr")

# --- Dear ImGui core + Win32/DX11 backends (from reference clone) ---
set(IMGUI_DIR "${REFS}/imgui")
add_library(imgui_dx11 STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp")
target_include_directories(imgui_dx11 PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
# We drive gamepad navigation ourselves (reading the controller through our own
# hook so the game can be made to ignore it in menu mode) — so turn OFF the Win32
# backend's built-in gamepad reader, else it fights our fed input.
target_compile_definitions(imgui_dx11 PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# --- The mod: selectable bootstrap, identical VR implementation ---
set(AKVR_PROXY "version" CACHE STRING "Bootstrap DLL: version or dinput8")
set_property(CACHE AKVR_PROXY PROPERTY STRINGS version dinput8)
if(NOT AKVR_PROXY STREQUAL "version" AND NOT AKVR_PROXY STREQUAL "dinput8")
    message(FATAL_ERROR "AKVR_PROXY must be version or dinput8")
endif()
add_library(akvr SHARED
    src/dllmain.cpp
    src/vrmode.cpp
    src/earlyres.cpp
    src/${AKVR_PROXY}_proxy.cpp
    src/hooks.cpp
    src/present_probe.cpp
    src/frameid.cpp
    src/geo11conv.cpp
    src/hudprobe.cpp
    src/hudsplit.cpp
    src/camera.cpp
    src/xr.cpp
    src/openvr_beacon.cpp
    src/gamepad.cpp
    src/${AKVR_PROXY}.def
    vendor/kiero/kiero.cpp)
target_include_directories(akvr PRIVATE
    vendor
    "${REFS}/minhook/include"
    "${REFS}/openvr/headers")
target_compile_definitions(akvr PRIVATE XR_USE_GRAPHICS_API_D3D11 XR_USE_PLATFORM_WIN32)
target_link_libraries(akvr PRIVATE minhook imgui_dx11 openxr_loader d3d11 dxgi xinput9_1_0
    "${REFS}/openvr/lib/win64/openvr_api.lib")
set_target_properties(akvr PROPERTIES OUTPUT_NAME "${AKVR_PROXY}" PREFIX "")

# Optional: auto-copy the DLL to the game folder after every successful build.
# Set AKVR_GAME_DIR to your Batman Arkham Knight Win64 folder, or leave it empty
# to skip the copy. The game must NOT be running or the copy will fail.
set(AKVR_GAME_DIR "E:/Games/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64"
    CACHE PATH "Path to BatmanAK.exe's folder (for post-build copy)")
if(AKVR_GAME_DIR AND EXISTS "${AKVR_GAME_DIR}")
    if(AKVR_PROXY STREQUAL "version")
        set(AKVR_OTHER_PROXY "dinput8")
    else()
        set(AKVR_OTHER_PROXY "version")
    endif()
    if(EXISTS "${AKVR_GAME_DIR}/${AKVR_OTHER_PROXY}.dll")
        message(FATAL_ERROR "Another proxy (${AKVR_OTHER_PROXY}.dll) is installed. Disable auto-deployment with -DAKVR_GAME_DIR= or explicitly switch loaders; never install both.")
    endif()
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/deploy_postbuild.bat" "@echo off\ncopy /Y \"%~1\" \"%~2\" >nul 2>&1\nif errorlevel 1 echo Deploy skipped: file locked or game running\nexit /b 0\n")
    add_custom_command(TARGET akvr POST_BUILD
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/deploy_postbuild.bat" "$<TARGET_FILE:akvr>" "${AKVR_GAME_DIR}/${AKVR_PROXY}.dll"
        COMMENT "Deploying ${AKVR_PROXY}.dll to ${AKVR_GAME_DIR} if possible"
    )
endif()
