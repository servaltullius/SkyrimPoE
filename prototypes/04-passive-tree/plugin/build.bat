@echo off
chcp 65001 >nul
rem PoE Proto 4: 트리 화면을 조립하고, 그것을 Prisma UI 뷰로 띄우는 DLL을 빌드해 모드 폴더 모양으로 묶는다.
rem 사용: build.bat            조립, 빌드, 묶기만
rem       build.bat deploy "MO2의 mods 폴더"
rem                            묶은 것을 그 폴더의 "[PoE Proto] 4 Passive Tree"에 복사(게임이 꺼져 있을 때만)
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo vcpkg를 찾지 못했습니다: %VCPKG_ROOT%
    exit /b 1
)
cd /d "%~dp0"
set "BUILD_ROOT=G:\skyrim-build\PoEProtoTree"
set "MOD_NAME=[PoE Proto] 4 Passive Tree"

rem 화면 파일과 변환한 트리 데이터를 ..\out\site 에 모은다(PoB의 데이터가 필요하다).
python -I "%~dp0..\tools\build_site.py" || exit /b 1

cmake --preset vs2022-release || exit /b 1
cmake --build --preset vs2022-release || exit /b 1
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
rem /mir: 뷰 폴더에서 없어진 파일이 모드 폴더에 남지 않게 한다. 이 모드 폴더는 이 스크립트만 쓴다.
robocopy "%BUILD_ROOT%\package" "%MO2_MODS%\%MOD_NAME%" /mir /xf meta.ini /njh /njs /ndl /nc /ns >nul
if errorlevel 8 (
    echo 배치 중 복사에 실패했습니다.
    exit /b 1
)
echo Deployed: %MO2_MODS%\%MOD_NAME%
exit /b 0
