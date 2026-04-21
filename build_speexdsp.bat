@echo off
setlocal EnableDelayedExpansion

echo ============================================
echo  Build SpeexDSP for VS2026 x64 + x86
echo  Output: third_party\speexdsp\
echo ============================================
echo.

if not exist "speexdsp\CMakeLists.txt" (
    echo [ERROR] Cannot find speexdsp\CMakeLists.txt
    pause
    exit /b 1
)
echo [OK] Found speexdsp source

cmake --version > nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found.
    pause
    exit /b 1
)
echo [OK] CMake found

if not exist "third_party\speexdsp\include\speex" mkdir "third_party\speexdsp\include\speex"
copy /y "speexdsp\include\speex\*.h" "third_party\speexdsp\include\speex\" > nul
echo [OK] Base headers copied

for %%A in (x64 Win32) do (
    if /i "%%A"=="x64" (
        set "ARCH_DIR=x64"
    ) else (
        set "ARCH_DIR=x86"
    )

    set "BUILD_DIR=speexdsp-build-%%A"
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
    mkdir "!BUILD_DIR!"

    echo.
    echo [%%A] Step 1/4 Configure...
    cmake speexdsp -B "!BUILD_DIR!" -G "Visual Studio 17 2022" -A %%A
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
    if not exist "third_party\speexdsp\lib\!ARCH_DIR!\Debug" mkdir "third_party\speexdsp\lib\!ARCH_DIR!\Debug"
    if not exist "third_party\speexdsp\lib\!ARCH_DIR!\Release" mkdir "third_party\speexdsp\lib\!ARCH_DIR!\Release"

    if exist "!BUILD_DIR!\include\speex\speexdsp_config_types.h" (
        copy /y "!BUILD_DIR!\include\speex\speexdsp_config_types.h" "third_party\speexdsp\include\speex\" > nul
    )

    set "DEBUG_LIB="
    for /r "!BUILD_DIR!\Debug" %%f in (speexdsp.lib) do set "DEBUG_LIB=%%f"
    if not defined DEBUG_LIB for /r "!BUILD_DIR!" %%f in (speexdsp.lib) do set "DEBUG_LIB=%%f"
    if not defined DEBUG_LIB (
        echo [ERROR] speexdsp.lib not found for %%A
        pause
        exit /b 1
    )
    copy /y "!DEBUG_LIB!" "third_party\speexdsp\lib\!ARCH_DIR!\Debug\speexdsp.lib" > nul

    set "RELEASE_LIB="
    for /r "!BUILD_DIR!\Release" %%f in (speexdsp.lib) do set "RELEASE_LIB=%%f"
    if defined RELEASE_LIB (
        copy /y "!RELEASE_LIB!" "third_party\speexdsp\lib\!ARCH_DIR!\Release\speexdsp.lib" > nul
    ) else (
        copy /y "third_party\speexdsp\lib\!ARCH_DIR!\Debug\speexdsp.lib" "third_party\speexdsp\lib\!ARCH_DIR!\Release\speexdsp.lib" > nul
        echo [WARNING] Release speexdsp.lib not found for %%A, using Debug as fallback
    )

    echo [OK] %%A libs copied to third_party\speexdsp\lib\!ARCH_DIR\
)

echo.
echo ============================================
echo  SpeexDSP Build Complete!
echo ============================================
echo  x64 Debug   : third_party\speexdsp\lib\x64\Debug\speexdsp.lib
echo  x64 Release : third_party\speexdsp\lib\x64\Release\speexdsp.lib
echo  x86 Debug   : third_party\speexdsp\lib\x86\Debug\speexdsp.lib
echo  x86 Release : third_party\speexdsp\lib\x86\Release\speexdsp.lib
echo.
pause
