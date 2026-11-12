@echo off
chcp 65001 >nul
rem PoE Proto 2 DLL을 구성, 빌드, 테스트하고 모드 폴더 모양으로 묶는다.
rem 사용: build.bat            빌드와 묶기만
rem       build.bat deploy "MO2의 mods 폴더"
rem                            묶은 것을 그 폴더의 "[PoE Proto] 2 Skill Input"에 복사(게임이 꺼져 있을 때만)
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo vcpkg를 찾지 못했습니다: %VCPKG_ROOT%
    exit /b 1
)
cd /d "%~dp0"
set "BUILD_ROOT=G:\skyrim-build\PoEProtoSkillInput"
set "MOD_NAME=[PoE Proto] 2 Skill Input"

cmake --preset vs2022-release || exit /b 1
cmake --build --preset vs2022-release || exit /b 1
ctest --test-dir "%BUILD_ROOT%\build\release-msvc-%VCToolsVersion%" --output-on-failure || exit /b 1
cmake --build --preset vs2022-release --target package_mod || exit /b 1
echo Package: %BUILD_ROOT%\package

if /i not "%~1"=="deploy" exit /b 0
set "MO2_MODS=%~2"
if "%MO2_MODS%"=="" (
    echo 배치할 MO2 mods 폴더를 둘째 인자로 주세요. 예: build.bat deploy "G:\TAKEALOOK\mods"
    exit /b 1
)

tasklist /fi "imagename eq SkyrimSE.exe" 2>nul | find /i "SkyrimSE.exe" >nul
if not errorlevel 1 (
    echo 스카이림이 실행 중이라 배치하지 않았습니다. 게임을 끄고 다시 실행하세요.
    exit /b 1
)
if not exist "%MO2_MODS%" (
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
