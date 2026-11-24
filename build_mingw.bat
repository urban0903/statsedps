@echo off
chcp 65001 >nul
setlocal

echo ============================================================
echo   statsEdps v1.0.5 - MinGW-w64 x86 (32-bit) Build Script
echo ============================================================

set "CXX="
where i686-w64-mingw32-g++ >nul 2>&1 && set "CXX=i686-w64-mingw32-g++"
if not defined CXX ( where g++ >nul 2>&1 && set "CXX=g++" )
if not defined CXX ( echo [ERROR] 32-bit g++ not found! & pause & exit /b 1 )

if not exist bin mkdir bin

%CXX% -m32 -std=c++17 -O2 -shared -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -Icommon hook_dll/statsEdps_hooks.cpp ^
      -o bin/statsEdps_hooks.dll -static -static-libgcc -static-libstdc++ -luser32 -lkernel32
if errorlevel 1 ( echo [ERROR] DLL build failed! & pause & exit /b 1 )

%CXX% -m32 -std=c++17 -O2 -mwindows -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -Icommon app_exe/main_app.cpp ^
      -o bin/statsEdps.exe -static -static-libgcc -static-libstdc++ ^
      -luser32 -lgdi32 -lcomdlg32 -ladvapi32 -lkernel32
if errorlevel 1 ( echo [ERROR] EXE build failed! & pause & exit /b 1 )

echo SUCCESS: bin\statsEdps.exe + bin\statsEdps_hooks.dll
pause
