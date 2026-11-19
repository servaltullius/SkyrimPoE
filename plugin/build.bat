@echo off
chcp 65001 >nul
rem SkyrimPoE 본 모드 DLL 빌드, 단위 테스트(ctest), 패키징, 배치 스크립트
rem 사용: build.bat            빌드 + ctest + 묶기
rem       build.bat deploy "MO2의 mods 폴더"
rem                            게임이 꺼져 있을 때 묶음을 모드 폴더로 복사
setlocal
set "VS_INSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "PATH=%VS_INSTALLER%;%PATH%"
set "VS_ROOT="
if exist "%VS_INSTALLER%\vswhere.exe" (
    for /f "delims=" %%i in ('vswhere.exe -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VS_ROOT=%%i"
)
if not defined VS_ROOT (
    echo Visual Studio 2022 C++ 빌드 도구를 찾지 못했습니다.
    exit /b 1
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VCPKG_VISUAL_STUDIO_PATH=%VS_ROOT%"
if not defined VCPKG_ROOT (
    if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    ) else (
        set "VCPKG_ROOT=%VS_ROOT%\VC\vcpkg"
    )
)
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo vcpkg를 찾지 못했습니다: %VCPKG_ROOT%
    exit /b 1
)
cd /d "%~dp0"
set "BUILD_ROOT=G:\skyrim-build\SkyrimPoE"
set "BUILD_PRESET=vs2022-release"
if defined POE_PLUGIN_BUILD_ROOT (
    set "BUILD_ROOT=%POE_PLUGIN_BUILD_ROOT%"
    set "BUILD_PRESET=vs2022-release-local"
)
set "MOD_NAME=SkyrimPoE"

cmake --preset %BUILD_PRESET% || exit /b 1
cmake --build --preset %BUILD_PRESET% || exit /b 1
ctest --preset %BUILD_PRESET% || exit /b 1
cmake --build --preset %BUILD_PRESET% --target package_mod || exit /b 1
echo Package: %BUILD_ROOT%\package

if /i not "%~1"=="deploy" exit /b 0
set "MO2_MODS=%~2"
if not defined MO2_MODS (
    echo 배치할 MO2 mods 폴더를 둘째 인자로 주세요.
    exit /b 1
)
tasklist /fi "imagename eq SkyrimSE.exe" 2>nul | find /i "SkyrimSE.exe" >nul
if not errorlevel 1 (
    echo 스카이림이 실행 중이라 배치하지 않았습니다. 게임을 끄고 다시 실행하세요.
    exit /b 1
)
if not exist "%MO2_MODS%\." (
    echo MO2 mods 폴더가 없습니다: %MO2_MODS%
    exit /b 1
)
robocopy "%BUILD_ROOT%\package" "%MO2_MODS%\%MOD_NAME%" /e /njh /njs /ndl /nc /ns >nul
if errorlevel 8 (
    echo 배치 중 복사에 실패했습니다.
    exit /b 1
)
echo Deployed: %MO2_MODS%\%MOD_NAME%
exit /b 0
