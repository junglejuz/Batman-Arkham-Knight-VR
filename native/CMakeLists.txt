cmake_minimum_required(VERSION 3.20)
project(AKN LANGUAGES C CXX)
# AKN - Arkham Knight native-stereo test (separate from AKVR). Loads as XINPUT9_1_0.dll from the game folder and
# does nothing unless the game is started with -akn (AKVR stays dormant without -akvr / its launcher).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# the workspace's shared references folder: two levels up from akvr/native, one up through the old akvr_native link
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../references/minhook")
  set(REFS "${CMAKE_CURRENT_SOURCE_DIR}/../../references")
else()
  set(REFS "${CMAKE_CURRENT_SOURCE_DIR}/../references")
endif()
add_subdirectory("${REFS}/minhook" "${CMAKE_BINARY_DIR}/_deps/minhook")
add_library(akn SHARED src/xinput_proxy.cpp src/probe.cpp src/dlss.cpp src/afw.cpp src/xinput9_1_0.def)
target_include_directories(akn PRIVATE "${REFS}/minhook/include" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/include")   # DLSS D1: NVIDIA NGX SDK 310.9.1 (from Luma)
target_link_libraries(akn PRIVATE minhook "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib/nvsdk_ngx_d.lib" d3d11)
set_target_properties(akn PROPERTIES OUTPUT_NAME "XINPUT9_1_0" PREFIX "")
