@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  Build Opus 1.6.1 for VS2026 x64 + x86
echo  Output: third_party\opus\
echo ============================================
echo.

if not exist "opus-1.6.1\CMakeLists.txt" (
    echo [ERROR] Cannot find opus-1.6.1\CMakeLists.txt
    pause
    exit /b 1
)
echo [OK] Found opus-1.6.1 source

cmake --version > nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found.
    pause
    exit /b 1
)
for /f "tokens=3" %%v in ('cmake --version 2^>nul ^| findstr /i "version"') do (
    echo [OK] CMake version: %%v
)

if not exist "third_party\opus\include\opus" mkdir "third_party\opus\include\opus"
copy /y "opus-1.6.1\include\*.h" "third_party\opus\include\opus\" > nul
echo [OK] Headers copied to third_party\opus\include\opus\

for %%A in (x64 Win32) do (
    if /i "%%A"=="x64" (
        set "ARCH_DIR=x64"
    ) else (
        set "ARCH_DIR=x86"
    )

    set "BUILD_DIR=opus-1.6.1-build-%%A"
    if exist "!BUILD_DIR!" (
        echo [INFO] Removing old build directory !BUILD_DIR!...
        rmdir /s /q "!BUILD_DIR!"
    )
    mkdir "!BUILD_DIR!"

    echo.
    echo [%%A] Step 1/4 Configure...
    cmake opus-1.6.1 ^
        -B "!BUILD_DIR!" ^
        -G "Visual Studio 17 2022" ^
        -A %%A ^
        -DOPUS_BUILD_SHARED_LIBRARY=OFF ^
        -DOPUS_BUILD_TESTING=OFF ^
        -DOPUS_BUILD_PROGRAMS=OFF
    if errorlevel 1 (
        echo [ERROR] CMake configure failed for %%A
        pause
        exit /b 1
    )

    echo [%%A] Step 2/4 Build Debug...
    cmake --build "!BUILD_DIR!" --config Debug --parallel
    if errorlevel 1 (
        echo [ERROR] Debug build failed for %%A
        pause
        exit /b 1
    )

    echo [%%A] Step 3/4 Build Release...
    cmake --build "!BUILD_DIR!" --config Release --parallel
    if errorlevel 1 (
        echo [ERROR] Release build failed for %%A
        pause
        exit /b 1
    )

    echo [%%A] Step 4/4 Copy libs...
    if not exist "third_party\opus\lib\!ARCH_DIR!\Debug" mkdir "third_party\opus\lib\!ARCH_DIR!\Debug"
    if not exist "third_party\opus\lib\!ARCH_DIR!\Release" mkdir "third_party\opus\lib\!ARCH_DIR!\Release"

    set "DEBUG_LIB="
    for /r "!BUILD_DIR!\Debug" %%f in (opus.lib) do set "DEBUG_LIB=%%f"
    if not defined DEBUG_LIB (
        for /r "!BUILD_DIR!" %%f in (opus.lib) do (
            echo %%f | findstr /i "debug" > nul
            if not errorlevel 1 set "DEBUG_LIB=%%f"
        )
    )
    if not defined DEBUG_LIB (
        echo [ERROR] Debug opus.lib not found for %%A
        pause
        exit /b 1
    )
    copy /y "!DEBUG_LIB!" "third_party\opus\lib\!ARCH_DIR!\Debug\opus.lib" > nul

    set "RELEASE_LIB="
    for /r "!BUILD_DIR!\Release" %%f in (opus.lib) do set "RELEASE_LIB=%%f"
    if defined RELEASE_LIB (
        copy /y "!RELEASE_LIB!" "third_party\opus\lib\!ARCH_DIR!\Release\opus.lib" > nul
    ) else (
        copy /y "third_party\opus\lib\!ARCH_DIR!\Debug\opus.lib" "third_party\opus\lib\!ARCH_DIR!\Release\opus.lib" > nul
        echo [WARNING] Release opus.lib not found for %%A, using Debug as fallback
    )

    echo [OK] %%A libs copied to third_party\opus\lib\!ARCH_DIR\
)

echo.
echo ============================================
echo  Build Complete!
echo ============================================
echo  x64 Debug   : third_party\opus\lib\x64\Debug\opus.lib
echo  x64 Release : third_party\opus\lib\x64\Release\opus.lib
echo  x86 Debug   : third_party\opus\lib\x86\Debug\opus.lib
echo  x86 Release : third_party\opus\lib\x86\Release\opus.lib
echo.
pause
