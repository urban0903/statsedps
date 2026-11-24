cmake_minimum_required(VERSION 3.16)
project(NFSProStreetMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "NFS ProStreet (nfs.exe) is 32-bit x86! Please configure CMake for Win32 / i686 (e.g. -A Win32 for MSVC).")
endif()

add_library(prostreet_hooks SHARED
    hook_dll/prostreet_hooks.cpp
)
target_include_directories(prostreet_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
set_target_properties(prostreet_hooks PROPERTIES PREFIX "" OUTPUT_NAME "prostreet_hooks")

add_executable(NFSProStreetMonitor WIN32
    app_exe/main_app.cpp
)
target_include_directories(NFSProStreetMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(NFSProStreetMonitor PRIVATE user32 gdi32 comdlg32 advapi32)

if(MSVC)
    target_compile_options(prostreet_hooks PRIVATE /W3 /O2 /utf-8)
    target_compile_options(NFSProStreetMonitor PRIVATE /W3 /O2 /utf-8)
    target_compile_definitions(prostreet_hooks PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    target_compile_definitions(NFSProStreetMonitor PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(prostreet_hooks PRIVATE -Wall -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(NFSProStreetMonitor PRIVATE -Wall -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(prostreet_hooks PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(NFSProStreetMonitor PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
endif()
