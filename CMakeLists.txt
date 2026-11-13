cmake_minimum_required(VERSION 3.16)
project(foo_super_eq VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Portable header-only core target
add_library(super_eq_core INTERFACE)
target_include_directories(super_eq_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Standalone CLI tool
add_executable(super_eq_cli src/core/super_eq_cli.cpp)
target_link_libraries(super_eq_cli PRIVATE super_eq_core)
if (MSVC)
    target_compile_options(super_eq_cli PRIVATE /W4 /EHsc)
else()
    target_compile_options(super_eq_cli PRIVATE -Wall -Wextra -Werror)
endif()

# Unit & DSP verification test suite
enable_testing()
add_executable(super_eq_tests tests/test_main.cpp)
target_link_libraries(super_eq_tests PRIVATE super_eq_core)
if (MSVC)
    target_compile_options(super_eq_tests PRIVATE /W4 /EHsc)
else()
    target_compile_options(super_eq_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME SuperEqCoreTests COMMAND super_eq_tests)

# Optional Windows foobar2000 v2.0+ component DLL target
set(FB2K_SDK_DIR "" CACHE PATH "Path to extracted foobar2000 SDK root")
set(WTL_INCLUDE_DIR "" CACHE PATH "Path to WTL include directory")
set(FB2K_SDK_LIB_DIR "" CACHE PATH "Path to directory containing built pfc/SDK/helpers/libPPUI/component_client .lib files")

if (WIN32 AND FB2K_SDK_DIR)
    add_library(foo_super_eq SHARED
        src/fb2k/ComponentMain.cpp
        src/fb2k/DspSuperEq.cpp
        src/fb2k/LiveSessionBus.cpp
        src/fb2k/SuperEqEditorView.cpp
        src/fb2k/UiElementSuperEq.cpp
    )

    target_include_directories(foo_super_eq PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FB2K_SDK_DIR}
        ${FB2K_SDK_DIR}/foobar2000
        ${WTL_INCLUDE_DIR}
    )

    target_compile_definitions(foo_super_eq PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )

    target_compile_options(foo_super_eq PRIVATE /W3 /EHsc /utf-8)

    if (FB2K_SDK_LIB_DIR)
        target_link_directories(foo_super_eq PRIVATE ${FB2K_SDK_LIB_DIR})
    endif()

    target_link_libraries(foo_super_eq PRIVATE
        foobar2000_SDK
        foobar2000_sdk_helpers
        foobar2000_component_client
        pfc
        libPPUI
        shared
        d2d1
        dwrite
        uxtheme
        comctl32
        comdlg32
        shlwapi
        gdi32
        user32
    )
endif()
