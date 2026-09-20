@echo off
rem ===================================================================
rem  Plants vs Zombies (C / Win32 GDI) - build script
rem
rem  Tries: gcc in PATH  ->  winget-installed MinGW  ->  tcc
rem  To use a custom toolchain, uncomment and edit the next line:
rem  set PATH=C:\your\mingw64\bin;%PATH%
rem ===================================================================
setlocal enabledelayedexpansion

where gcc >nul 2>nul
if not errorlevel 1 goto build_gcc

for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%D\mingw64\bin\gcc.exe" (
        set "PATH=%%D\mingw64\bin;!PATH!"
        echo [i] found MinGW from winget: %%D
    )
)

where gcc >nul 2>nul
if not errorlevel 1 goto build_gcc
goto try_tcc

:build_gcc
echo [1/2] compiling with gcc (64-bit) ...
gcc -O2 -mwindows -static-libgcc -o pvz.exe pvz.c -lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm -Wall -Wextra
if errorlevel 1 goto fail
goto ok

:try_tcc
where tcc >nul 2>nul
if errorlevel 1 goto nogcc
echo [1/2] no gcc found, compiling with tcc (32-bit) ...
tcc -m32 -o pvz.exe -Wl,-subsystem=windows pvz.c -lgdi32 -luser32
if errorlevel 1 goto fail
goto ok

:nogcc
echo [ERROR] no C compiler found.
echo.
echo   Install MinGW-w64 (~260MB):
echo       winget install BrechtSanders.WinLibs.POSIX.UCRT
echo.
echo   Or the tiny TinyCC (~1.5MB, remember to pass -m32):
echo       https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip
echo.
pause
exit /b 1

:fail
echo [FAILED] compile error, see messages above.
pause
exit /b 1

:ok
echo [2/2] OK -^> pvz.exe
echo.
echo To build the headless balance simulator as well:
echo       gcc -O2 -o sim.exe sim_test.c -lgdi32 -luser32 -lm
echo.
echo launching ...
start "" pvz.exe
endlocal

rem  sprites need AlphaBlend (msimg32); TinyCC ships no msimg32 import lib,
rem  so if only tcc is available the sprite layer is unavailable - install MinGW.
