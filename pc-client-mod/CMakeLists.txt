cmake_minimum_required(VERSION 3.20)
project(MegaMixMP CXX C)

option(MP_DEV_TOOLS "RE/debug instrumentation (watchpoints, keylog, vk/dik taps)" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # /MT static CRT

add_library(MegaMixMP SHARED
    src/mod.cpp
    src/Config.cpp
    src/MpFont.cpp
    src/Stubs.cpp
    src/platform/MpLog.cpp
    src/platform/NxShim.cpp
    src/platform/MpSock.cpp
    src/game/diva.cpp
    src/game/MpSelPc.cpp
    src/mp/MpNet.cpp
    src/mp/MpLobby.cpp
    src/mp/MpMenu.cpp
    src/mp/MpInput.cpp
    src/mp/MpUi.cpp
    src/mp/MpPanelPc.cpp
    src/mp/MpOverlayPc.cpp
    src/mp/MpAvatarStub.cpp
    src/mp/MpSpr.cpp
    third_party/safetyhook/src/safetyhook.cpp
    third_party/safetyhook/src/Zydis.c
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(MegaMixMP PRIVATE
    src
    third_party
    third_party/imgui
    third_party/imgui/backends
)

target_compile_definitions(MegaMixMP PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
    NOMINMAX
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    $<$<BOOL:${MP_DEV_TOOLS}>:MP_DEV_TOOLS>
)

target_compile_options(MegaMixMP PRIVATE /W3 /wd4530 /utf-8)
set_source_files_properties(third_party/safetyhook/src/Zydis.c PROPERTIES LANGUAGE C)

target_link_libraries(MegaMixMP PRIVATE
    d3d11 dxgi ws2_32 iphlpapi xinput
)

set_target_properties(MegaMixMP PROPERTIES
    OUTPUT_NAME "MegaMixMP"
    SUFFIX ".dll"
)
