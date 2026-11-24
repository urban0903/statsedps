cmake_minimum_required(VERSION 3.16)
project(statsEdps VERSION 1.0.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "NFS ProStreet (nfs.exe) is 32-bit x86! Configure CMake for Win32 / i686 (e.g. -A Win32).")
endif()

add_library(statsEdps_hooks SHARED
    hook_dll/statsEdps_hooks.cpp
)
target_include_directories(statsEdps_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
set_target_properties(statsEdps_hooks PROPERTIES PREFIX "" OUTPUT_NAME "statsEdps_hooks")

add_executable(statsEdps WIN32
    app_exe/main_app.cpp
)
target_include_directories(statsEdps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(statsEdps PRIVATE user32 gdi32 comdlg32 advapi32)

if(MSVC)
    target_compile_options(statsEdps_hooks PRIVATE /W3 /O2 /utf-8)
    target_compile_options(statsEdps PRIVATE /W3 /O2 /utf-8)
    target_compile_definitions(statsEdps_hooks PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_compile_definitions(statsEdps PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(statsEdps_hooks PRIVATE -Wall -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(statsEdps PRIVATE -Wall -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(statsEdps_hooks PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(statsEdps PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
endif()
