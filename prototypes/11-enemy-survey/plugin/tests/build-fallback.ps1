# 이미 빌드한 CommonLib를 읽어 이 플러그인만 빌드한다.
param(
    [Parameter(Mandatory)][string]$CommonLib,
    [Parameter(Mandatory)][string]$SpellsBuild
)
$ErrorActionPreference = 'Stop'

$plugin = Split-Path -Parent $PSScriptRoot
$out = Join-Path $plugin 'build\fallback'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:TEMP = $env:TMP = $out
$libSource = (Resolve-Path -LiteralPath $CommonLib).Path
$spells = (Resolve-Path -LiteralPath $SpellsBuild).Path
$version = (Split-Path -Leaf $spells) -replace '^release-msvc-', ''
if ($version -notmatch '^14\.\d+\.\d+$') { throw 'release-msvc-<도구 버전> 빌드 폴더를 지정해야 함' }
$root = Split-Path -Parent (Split-Path -Parent $spells)
$deps = Join-Path $root "vcpkg_installed\msvc-$version\x64-windows-static-md"

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 C++ 도구를 찾지 못함' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$vcEnv = cmd.exe /d /c ('call "' + $vcvars + '" -vcvars_ver=' + $version + ' >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat 실행 실패' }
foreach ($line in $vcEnv) {
    if ($line -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

Push-Location -LiteralPath $out
try {
    $pluginInfo = Get-Content -Raw -LiteralPath (Join-Path $spells '__PoEProtoSpellsPlugin.cpp')
    Set-Content -LiteralPath '__PoEProtoSurveyPlugin.cpp' -Value ($pluginInfo.Replace('PoEProtoSpells', 'PoEProtoSurvey')) -NoNewline -Encoding utf8
    $flags = @(
        '/nologo', '/c', '/std:c++latest', '/EHsc', '/utf-8', '/Zc:preprocessor', '/MD', '/O2', '/WX',
        '/DNDEBUG', '/DNOMINMAX', '/DENABLE_SKYRIM_AE=1', '/DENABLE_SKYRIM_SE=1', '/DHAS_SKYRIM_MULTI_TARGETING=1',
        '/DSKSE_SUPPORT_PATCH_SAFETY=1', '/DSPDLOG_COMPILED_LIB', '/DSPDLOG_FMT_EXTERNAL',
        '/DUSING_XINPUT', '/DWINVER=0x0601', '/D_WIN32_WINNT=0x0601', '/D__CMAKE_COMMONLIBSSE_PLUGIN=1',
        "/I$plugin\src", "/I$libSource\include", "/I$libSource\extern\openvr\headers",
        "/external:I$deps\include", "/external:I$deps\include\directxtk", '/external:W0',
        "/FI$plugin\src\PCH.h"
    )
    & cl.exe @flags "$plugin\src\main.cpp" "$plugin\src\Survey.cpp" .\__PoEProtoSurveyPlugin.cpp
    if ($LASTEXITCODE -ne 0) { throw 'DLL 컴파일 실패' }
    $libraries = @(
        "$spells\CommonLibSSE-NG\CommonLibSSE.lib", "$deps\lib\spdlog.lib", "$deps\lib\fmt.lib",
        "$deps\lib\DirectXTK.lib", 'advapi32.lib', 'bcrypt.lib', 'd3d11.lib', 'd3dcompiler.lib', 'dbghelp.lib',
        'dxgi.lib', 'ole32.lib', 'version.lib', 'kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib',
        'shell32.lib', 'oleaut32.lib', 'uuid.lib', 'comdlg32.lib'
    )
    & link.exe /NOLOGO /DLL /WX /MACHINE:X64 /INCREMENTAL:NO /MAP /OUT:PoEProtoSurvey.dll main.obj Survey.obj __PoEProtoSurveyPlugin.obj @libraries
    if ($LASTEXITCODE -ne 0) { throw 'DLL 링크 실패' }
    $exports = & dumpbin.exe /NOLOGO /EXPORTS .\PoEProtoSurvey.dll
    if ($LASTEXITCODE -ne 0) { throw 'DLL 내보내기 확인 실패' }
    $exports | Write-Host
    foreach ($symbol in 'SKSEPlugin_Load', 'SKSEPlugin_Query', 'SKSEPlugin_Version') {
        if (-not ($exports -cmatch "\s$symbol\s*$")) { throw "DLL에서 $symbol 내보내기를 찾지 못함" }
    }
} finally {
    Pop-Location
}
