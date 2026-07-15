cmake_minimum_required(VERSION 3.24)
project(TaskbarWidgetsNative VERSION 0.1.0 LANGUAGES CXX)

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
target_link_libraries(TaskbarWidgetsHook PRIVATE ole32 oleaut32 runtimeobject gdi32 gdiplus)

add_executable(TaskbarWidgetsMediaHelper media-helper/media_helper.cpp)
set_target_properties(TaskbarWidgetsMediaHelper PROPERTIES
    OUTPUT_NAME "TaskbarWidgets.MediaHelper"
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES)
target_compile_definitions(TaskbarWidgetsMediaHelper PRIVATE
    UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(TaskbarWidgetsMediaHelper PRIVATE ole32 oleaut32 runtimeobject gdi32 gdiplus)

include(CTest)
if(BUILD_TESTING)
    add_executable(TaskbarWidgetsNativeTests tests/layout_tests.cpp)
    set_target_properties(TaskbarWidgetsNativeTests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES)
    target_include_directories(TaskbarWidgetsNativeTests PRIVATE taskbar-hook)
    add_test(NAME TaskbarWidgetsNativeTests COMMAND TaskbarWidgetsNativeTests)
endif()
