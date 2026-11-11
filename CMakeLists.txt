cmake_minimum_required(VERSION 3.21)
project(PixelForge VERSION 1.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "PixelForge requires Windows 10 or 11.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(pixelforge_core STATIC src/document.cpp src/formats.cpp src/psd.cpp)
target_include_directories(pixelforge_core PUBLIC src)
target_compile_features(pixelforge_core PUBLIC cxx_std_20)
target_compile_definitions(pixelforge_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(pixelforge_core PUBLIC ole32 oleaut32 windowscodecs uuid)

add_executable(PixelForge WIN32 src/main.cpp src/app.cpp src/interface.cpp src/dialogs.cpp src/new_document.cpp
    src/clipboard.cpp src/editing.cpp resources/app.rc)
target_include_directories(PixelForge PRIVATE resources)
target_link_libraries(PixelForge PRIVATE pixelforge_core d2d1 dwrite dwmapi shell32 comdlg32 gdi32 user32)

if(MSVC)
    target_compile_options(pixelforge_core PRIVATE /W4 /utf-8 /permissive-)
    target_compile_options(PixelForge PRIVATE /W4 /utf-8 /permissive-)
    target_link_options(PixelForge PRIVATE /MANIFEST:NO)
endif()

if(MINGW)
    target_link_options(PixelForge PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(PixelForgeTests tests/core_tests.cpp)
    target_link_libraries(PixelForgeTests PRIVATE pixelforge_core psapi)
    if(MSVC)
        target_compile_options(PixelForgeTests PRIVATE /utf-8)
    endif()
    add_test(NAME PixelForgeCore COMMAND PixelForgeTests)
endif()
