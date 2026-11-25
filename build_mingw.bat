@echo off
chcp 65001 >nul
setlocal

echo ============================================================
echo   statsEdps - MinGW-w64 x86 (32-bit) Build Script
echo ============================================================

set "CXX="
where i686-w64-mingw32-g++ >nul 2>&1 && set "CXX=i686-w64-mingw32-g++"
if not defined CXX ( where g++ >nul 2>&1 && set "CXX=g++" )
if not defined CXX ( echo [ERROR] 32-bit g++ not found! & pause & exit /b 1 )

set "RC="
where i686-w64-mingw32-windres >nul 2>&1 && set "RC=i686-w64-mingw32-windres"
if not defined RC ( where windres >nul 2>&1 && set "RC=windres" )

if not exist bin mkdir bin

set "DLL_RES="
set "EXE_RES="
if defined RC (
    %RC% -F pe-i386 -I common -DSTATSEDPS_IS_DLL common/version.rc -O coff -o bin/ver_dll.o
    %RC% -F pe-i386 -I common common/version.rc -O coff -o bin/ver_exe.o
    set "DLL_RES=bin/ver_dll.o"
    set "EXE_RES=bin/ver_exe.o"
)

%CXX% -m32 -std=c++17 -O2 -shared -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -Icommon hook_dll/statsEdps_hooks.cpp %DLL_RES% ^
      -o bin/statsEdps_hooks.dll -static -static-libgcc -static-libstdc++ -luser32 -lkernel32
if errorlevel 1 ( echo [ERROR] DLL build failed! & pause & exit /b 1 )

%CXX% -m32 -std=c++17 -O2 -mwindows -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -Icommon app_exe/main_app.cpp %EXE_RES% ^
      -o bin/statsEdps.exe -static -static-libgcc -static-libstdc++ ^
      -luser32 -lgdi32 -lcomdlg32 -ladvapi32 -lkernel32
if errorlevel 1 ( echo [ERROR] EXE build failed! & pause & exit /b 1 )

del /q bin\*.o 2>nul
echo SUCCESS: bin\statsEdps.exe + bin\statsEdps_hooks.dll
pause
