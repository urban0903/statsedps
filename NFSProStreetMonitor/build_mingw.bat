@echo off
chcp 65001 >nul
setlocal

echo ============================================================
echo   NFS ProStreet Monitor - MinGW-w64 x86 (32-bit) Build
echo ============================================================

set "CXX="
where i686-w64-mingw32-g++ >nul 2>&1 && set "CXX=i686-w64-mingw32-g++"
if not defined CXX (
    where g++ >nul 2>&1 && set "CXX=g++"
)
if not defined CXX (
    echo [ERROR] 32-bit MinGW-w64 g++ not found in PATH!
    echo Install MSYS2 (mingw-w64-i686-gcc) or winlibs i686, then add bin\ to PATH.
    pause
    exit /b 1
)

if not exist bin mkdir bin

echo Using compiler: %CXX%
echo.
echo [1/2] Building bin\prostreet_hooks.dll (32-bit)...
%CXX% -m32 -std=c++17 -O2 -shared -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -Icommon hook_dll/prostreet_hooks.cpp ^
      -o bin/prostreet_hooks.dll -static -static-libgcc -static-libstdc++ -luser32 -lkernel32
if errorlevel 1 (
    echo [ERROR] Failed to build prostreet_hooks.dll! Make sure you have 32-bit (i686) MinGW!
    pause
    exit /b 1
)

echo.
echo [2/2] Building bin\NFSProStreetMonitor.exe (32-bit)...
%CXX% -m32 -std=c++17 -O2 -mwindows -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
      -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -Icommon app_exe/main_app.cpp ^
      -o bin/NFSProStreetMonitor.exe -static -static-libgcc -static-libstdc++ ^
      -luser32 -lgdi32 -lcomdlg32 -ladvapi32 -lkernel32
if errorlevel 1 (
    echo [ERROR] Failed to build NFSProStreetMonitor.exe!
    pause
    exit /b 1
)

echo.
echo ============================================================
echo   SUCCESS! Built files:
echo     bin\NFSProStreetMonitor.exe
echo     bin\prostreet_hooks.dll
echo ============================================================
pause
