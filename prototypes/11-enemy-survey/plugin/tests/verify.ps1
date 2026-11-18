# 게임 없이 돌리는 검증: INI, 방문 순서, TSV와 파일 이름을 확인한다.
# 사용: pwsh -NoProfile -File plugin\tests\verify.ps1
# 산출물은 plugin\build\tests(.gitignore의 build/)에 둔다. Visual Studio 2022의 vcvars64.bat이 필요하다.
$ErrorActionPreference = 'Stop'
$plugin = Split-Path -Parent $PSScriptRoot
$out = Join-Path $plugin 'build\tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:TEMP = $env:TMP = $out

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe를 찾지 못함: $vswhere" }
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'C++ 도구가 있는 Visual Studio를 찾지 못함' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$vcEnv = cmd.exe /d /c ('call "' + $vcvars + '" >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat 실행 실패' }
foreach ($line in $vcEnv) {
    if ($line -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

Push-Location -LiteralPath $out
try {
    $flags = @('/nologo', '/std:c++latest', '/EHsc', '/utf-8', '/Zc:preprocessor', '/MD', '/W4', '/WX', "/I$plugin\src")
    $sources = @("$plugin\tests\verify-survey.cpp", "$plugin\src\Survey.cpp")
    & cl.exe @flags $sources /Fe:verify-survey.exe
    if ($LASTEXITCODE -ne 0) { throw '검증 프로그램 빌드 실패' }
    & .\verify-survey.exe "$plugin\dist\SKSE\Plugins\PoEProtoSurvey.ini"
    if ($LASTEXITCODE -ne 0) { throw '검증 실패' }
    Write-Host '검증 통과'
} finally {
    Pop-Location
}
