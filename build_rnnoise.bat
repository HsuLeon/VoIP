@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  Build RNNoise for VS2026 x64 + x86
echo  Output: third_party\rnnoise\
echo ============================================
echo.

if not exist "rnnoise\CMakeLists.txt" (
    echo [ERROR] Cannot find rnnoise\CMakeLists.txt
    pause
    exit /b 1
)
echo [OK] Found rnnoise source

cmake --version > nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found.
    pause
    exit /b 1
)
echo [OK] CMake found

echo.
echo [Step 0/4] Applying MSVC compatibility patches...
python patch_rnnoise.py
if errorlevel 1 (
    echo [ERROR] Patch script failed. Make sure Python is installed.
    pause
    exit /b 1
)

if not exist "third_party\rnnoise\include" mkdir "third_party\rnnoise\include"
copy /y "rnnoise\include\rnnoise.h" "third_party\rnnoise\include\" > nul
echo [OK] Header copied

for %%A in (x64 Win32) do (
    if /i "%%A"=="x64" (
        set "ARCH_DIR=x64"
    ) else (
        set "ARCH_DIR=x86"
    )

    set "BUILD_DIR=rnnoise-build-%%A"
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
    mkdir "!BUILD_DIR!"

    echo.
    echo [%%A] Step 1/4 Configure...
    cmake rnnoise -B "!BUILD_DIR!" -G "Visual Studio 17 2022" -A %%A
    if errorlevel 1 (
        echo [ERROR] Configure failed for %%A
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
    if not exist "third_party\rnnoise\lib\!ARCH_DIR!\Debug" mkdir "third_party\rnnoise\lib\!ARCH_DIR!\Debug"
    if not exist "third_party\rnnoise\lib\!ARCH_DIR!\Release" mkdir "third_party\rnnoise\lib\!ARCH_DIR!\Release"

    set "DEBUG_LIB="
    for /r "!BUILD_DIR!\Debug" %%f in (rnnoise.lib) do set "DEBUG_LIB=%%f"
    if not defined DEBUG_LIB for /r "!BUILD_DIR!" %%f in (rnnoise.lib) do set "DEBUG_LIB=%%f"
    if not defined DEBUG_LIB (
        echo [ERROR] rnnoise.lib not found for %%A
        pause
        exit /b 1
    )
    copy /y "!DEBUG_LIB!" "third_party\rnnoise\lib\!ARCH_DIR!\Debug\rnnoise.lib" > nul

    set "RELEASE_LIB="
    for /r "!BUILD_DIR!\Release" %%f in (rnnoise.lib) do set "RELEASE_LIB=%%f"
    if defined RELEASE_LIB (
        copy /y "!RELEASE_LIB!" "third_party\rnnoise\lib\!ARCH_DIR!\Release\rnnoise.lib" > nul
    ) else (
        copy /y "third_party\rnnoise\lib\!ARCH_DIR!\Debug\rnnoise.lib" "third_party\rnnoise\lib\!ARCH_DIR!\Release\rnnoise.lib" > nul
        echo [WARNING] Release rnnoise.lib not found for %%A, using Debug as fallback
    )

    echo [OK] %%A libs copied to third_party\rnnoise\lib\!ARCH_DIR\
)

echo.
echo ============================================
echo  RNNoise Build Complete!
echo ============================================
echo  x64 Debug   : third_party\rnnoise\lib\x64\Debug\rnnoise.lib
echo  x64 Release : third_party\rnnoise\lib\x64\Release\rnnoise.lib
echo  x86 Debug   : third_party\rnnoise\lib\x86\Debug\rnnoise.lib
echo  x86 Release : third_party\rnnoise\lib\x86\Release\rnnoise.lib
echo.
pause
