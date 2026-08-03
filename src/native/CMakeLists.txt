cmake_minimum_required(VERSION 3.24)
project(TaskbarWidgetsNative VERSION 0.5.34 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Taskbar Widgets native components require Windows x64")
endif()

# The hook contains user-facing Unicode literals. MSVC otherwise interprets
# UTF-8 source files with the machine's active ANSI code page, which can turn
# characters such as the U+2022 bullet separator into mojibake.
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(TaskbarWidgetsHook SHARED taskbar-hook/taskbar_widgets_hook.cpp)
set_target_properties(TaskbarWidgetsHook PROPERTIES
    OUTPUT_NAME "TaskbarWidgets.Hook"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES)
target_compile_definitions(TaskbarWidgetsHook PRIVATE
    UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00 _WIN32_IE=0x0A00)
target_link_libraries(TaskbarWidgetsHook PRIVATE
    ole32 oleaut32 runtimeobject gdi32 gdiplus uiautomationcore)

add_executable(TaskbarWidgetsMediaHelper media-helper/media_helper.cpp)
set_target_properties(TaskbarWidgetsMediaHelper PROPERTIES
    OUTPUT_NAME "TaskbarWidgets.MediaHelper"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES)
target_compile_definitions(TaskbarWidgetsMediaHelper PRIVATE
    UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(TaskbarWidgetsMediaHelper PRIVATE ole32 oleaut32 runtimeobject gdi32 gdiplus uuid mmdevapi)

if(DEFINED WEBVIEW2_SDK_ROOT)
    set(WEBVIEW2_INCLUDE "${WEBVIEW2_SDK_ROOT}/build/native/include")
    set(WEBVIEW2_STATIC_LIBRARY "${WEBVIEW2_SDK_ROOT}/build/native/x64/WebView2LoaderStatic.lib")
    if(NOT EXISTS "${WEBVIEW2_INCLUDE}/WebView2.h" OR NOT EXISTS "${WEBVIEW2_STATIC_LIBRARY}")
        message(FATAL_ERROR "WEBVIEW2_SDK_ROOT does not contain the pinned native WebView2 SDK")
    endif()

    add_executable(TaskbarWidgetsRenderHost WIN32 render-host/render_host.cpp)
    set_target_properties(TaskbarWidgetsRenderHost PROPERTIES
        OUTPUT_NAME "TaskbarWidgets.RenderHost"
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES)
    target_include_directories(TaskbarWidgetsRenderHost PRIVATE "${WEBVIEW2_INCLUDE}")
    target_compile_definitions(TaskbarWidgetsRenderHost PRIVATE
        UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00
        TASKBAR_WIDGETS_VERSION="${PROJECT_VERSION}")
    target_link_libraries(TaskbarWidgetsRenderHost PRIVATE
        "${WEBVIEW2_STATIC_LIBRARY}"
        ole32 oleaut32 runtimeobject user32 gdi32 shlwapi shell32 advapi32
        psapi dcomp d3d11 dxgi windowsapp)
else()
    message(STATUS "WEBVIEW2_SDK_ROOT was not supplied; RenderHost target is disabled")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(TaskbarWidgetsNativeTests tests/layout_tests.cpp)
    set_target_properties(TaskbarWidgetsNativeTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)
    target_include_directories(TaskbarWidgetsNativeTests PRIVATE taskbar-hook)
    add_test(NAME TaskbarWidgetsNativeTests COMMAND TaskbarWidgetsNativeTests)
endif()
