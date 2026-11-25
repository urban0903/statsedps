@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion

echo ============================================================
echo   statsEdps - MSVC x86 (32-bit) Build Script
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

echo [1/4] Compiling resources (version.rc)...
rc /nologo /I common /DSTATSEDPS_IS_DLL /fo bin\ver_dll.res common\version.rc
if errorlevel 1 ( echo [ERROR] DLL rc failed! & pause & exit /b 1 )
rc /nologo /I common /fo bin\ver_exe.res common\version.rc
if errorlevel 1 ( echo [ERROR] EXE rc failed! & pause & exit /b 1 )

echo [2/4] Building bin\statsEdps_hooks.dll (x86)...
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /LD ^
   /I common hook_dll\statsEdps_hooks.cpp bin\ver_dll.res ^
   /Fe:bin\statsEdps_hooks.dll /Fo:bin\ /link /NOLOGO user32.lib kernel32.lib
if errorlevel 1 ( echo [ERROR] DLL build failed! & pause & exit /b 1 )

echo [3/4] Building bin\statsEdps.exe (x86)...
cl /nologo /std:c++17 /O2 /MT /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /I common app_exe\main_app.cpp bin\ver_exe.res ^
   /Fe:bin\statsEdps.exe /Fo:bin\ /link /NOLOGO /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib comdlg32.lib advapi32.lib kernel32.lib
if errorlevel 1 ( echo [ERROR] EXE build failed! & pause & exit /b 1 )

del /q bin\*.obj bin\*.exp bin\*.lib bin\*.res 2>nul
echo [4/4] SUCCESS: bin\statsEdps.exe + bin\statsEdps_hooks.dll
pause
