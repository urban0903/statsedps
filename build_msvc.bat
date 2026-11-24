@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion

echo ============================================================
echo   statsEdps v1.0.4 - MSVC x86 (32-bit) Build Script
echo ============================================================

where cl.exe >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            set "VSDIR=%%i"
        )
    )
    if defined VSDIR (
        echo Found Visual Studio: !VSDIR!
        call "!VSDIR!\VC\Auxiliary\Build\vcvars32.bat"
    ) else (
        echo [ERROR] cl.exe not found!
        pause
        exit /b 1
    )
)

if not exist bin mkdir bin

echo [1/2] Building bin\statsEdps_hooks.dll (x86)...
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /LD ^
   /I common hook_dll\statsEdps_hooks.cpp ^
   /Fe:bin\statsEdps_hooks.dll /Fo:bin\ /link /NOLOGO user32.lib kernel32.lib
if errorlevel 1 ( echo [ERROR] DLL build failed! & pause & exit /b 1 )

echo [2/2] Building bin\statsEdps.exe (x86)...
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I common app_exe\main_app.cpp ^
   /Fe:bin\statsEdps.exe /Fo:bin\ /link /NOLOGO /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comdlg32.lib advapi32.lib kernel32.lib
if errorlevel 1 ( echo [ERROR] EXE build failed! & pause & exit /b 1 )

del /q bin\*.obj bin\*.exp bin\*.lib 2>nul
echo SUCCESS: bin\statsEdps.exe + bin\statsEdps_hooks.dll
pause
