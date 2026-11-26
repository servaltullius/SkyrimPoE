[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("arm", "disarm", "status", "report")]
    [string]$Action,

    [string]$Save = "",
    [string]$TempSave = "SkyrimPoE_autotest",
    [bool]$Quit = $true,
    [ValidateRange(30, 7200)]
    [int]$TimeoutSec,
    # 조사용 단축 실행. h27이면 기준 세이브를 불러온 뒤 경로 탐침(H26·H27)만 돈다.
    # es이면 적 ES 표시 시험(ES1~ES5, 결정 185)만 돈다.
    # save이면 콘텐츠 검사와 코세이브 왕복(S0~S5)만 돈다.
    # cleanup이면 PoE1 세이브 정리와 출력 슬롯 재로드만 돈다.
    # gems이면 세대별 Spark 손 시전(세대 1은 거부 셋·저장 재로드 포함)만 돈다.
    # firstinstall이면 모드 기록 없는 세이브의 세대 1 최초 설치와 저장 재로드만 돈다.
    # newgame이면 메인 메뉴의 StartNewGame 콜백으로 세대 1 새 게임에 들어가(캐릭터 생성 창은 사람이 마침) 패널을 처음 열 때의 시작 지급·저장 재로드를 돈다(-Save 금지, -OutSave 필수).
    # preserve이면 -PreserveSaves 목록을 차례로 불러와 보존 판정·무정리·무지급·<stem>_out 저장을 돈다(-Save 금지).
    # gen1gate이면 세대 1 게이트(C5)·세대 0 양성 대조를 돈다(-Save 필수, 세대 1 패키지는 -OutSave 필수·세대 0 패키지는 -OutSave 금지).
    # 기존 위치 인자 호출(다섯 번째가 TimeoutSec)이 깨지지 않게 맨 뒤에 둔다.
    [ValidateSet("", "h27", "es", "react", "save", "cleanup", "gems", "firstinstall", "newgame", "preserve", "gen1gate")]
    [string]$Only = "",
    [string]$OutSave = "",
    [string[]]$PreserveSaves = @()
)

$ErrorActionPreference = "Stop"

function Normalize-SaveStem([string]$Name, [string]$ParamName) {
    $trimmed = $Name.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return ""
    }
    if ($trimmed -match '[\x00-\x1F\r\n<>:"/\\|?*]' -or $trimmed.Contains("..")) {
        throw "Parameter -$ParamName must be a valid Windows save filename/stem without path separators, colons, or forbidden characters."
    }
    if ($trimmed.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase)) {
        $trimmed = $trimmed.Substring(0, $trimmed.Length - 4)
    }
    return $trimmed
}

function Assert-AutoTestSaveSlots([string]$Baseline, [string]$Temporary, [string]$Mode, [string]$Out = "") {
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    $checkpointPattern = '^' + [regex]::Escape($Temporary) + '_h_restore(?:_[0-9]+_[0-9]+)?$'
    $usesFinalSlot = $Mode -in "cleanup", "gems", "firstinstall"
    $effectiveOut = if (-not [string]::IsNullOrWhiteSpace($Out)) { $Out } elseif ($usesFinalSlot) { $Temporary + '_final' } else { "" }
    if ($Temporary.Equals($Baseline, $comparison) -or
        (-not [string]::IsNullOrWhiteSpace($effectiveOut) -and $effectiveOut.Equals($Baseline, $comparison)) -or
        $Baseline -match $checkpointPattern) {
        throw "Refusing to arm AutoTest: -TempSave, -OutSave and derived save names must differ from -Save."
    }
}

function Get-AutoTestPaths {
    $skseDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Skyrim Special Edition\SKSE"
    return @{
        SkseDir = $skseDir
        MarkerPath = Join-Path $skseDir "SkyrimPoE.autotest"
        ReportPath = Join-Path $skseDir "SkyrimPoE.autotest.log"
    }
}

switch ($Action) {
    "arm" {
        $cleanSave = Normalize-SaveStem $Save "Save"
        $cleanTemp = Normalize-SaveStem $TempSave "TempSave"
        if ([string]::IsNullOrWhiteSpace($cleanTemp)) {
            throw "Parameter -TempSave cannot be empty."
        }
        $cleanOut = Normalize-SaveStem $OutSave "OutSave"
        $cleanPreserve = @()
        foreach ($rawItem in $PreserveSaves) {
            if ($null -eq $rawItem) { continue }
            foreach ($piece in ($rawItem -split ',')) {
                $stem = Normalize-SaveStem $piece "PreserveSaves"
                if ([string]::IsNullOrWhiteSpace($stem)) {
                    throw "Parameter -PreserveSaves cannot contain empty entries."
                }
                $cleanPreserve += $stem
            }
        }
        if ($Only -in "cleanup", "gems", "firstinstall", "save", "es", "react", "newgame", "preserve", "gen1gate" -and -not $Quit) {
            throw "Refusing to arm AutoTest: -Only $Only requires -Quit:`$true."
        }
        if ($Only -eq "newgame") {
            if (-not [string]::IsNullOrWhiteSpace($cleanSave)) {
                throw "Refusing to arm AutoTest: -Only newgame does not accept -Save."
            }
            if ([string]::IsNullOrWhiteSpace($cleanOut)) {
                throw "Refusing to arm AutoTest: -Only newgame requires -OutSave <save_stem>."
            }
            if ($cleanPreserve.Count -gt 0) {
                throw "Refusing to arm AutoTest: -PreserveSaves is only valid with -Only preserve."
            }
        } elseif ($Only -eq "preserve") {
            if (-not [string]::IsNullOrWhiteSpace($cleanSave)) {
                throw "Refusing to arm AutoTest: -Only preserve does not accept -Save."
            }
            if (-not [string]::IsNullOrWhiteSpace($cleanOut)) {
                throw "Refusing to arm AutoTest: -Only preserve uses fixed <stem>_out outputs and does not accept -OutSave."
            }
            if ($cleanPreserve.Count -lt 1 -or $cleanPreserve.Count -gt 16) {
                throw "Refusing to arm AutoTest: -Only preserve requires 1 to 16 entries in -PreserveSaves."
            }
            $comparison = [System.StringComparison]::OrdinalIgnoreCase
            for ($i = 0; $i -lt $cleanPreserve.Count; $i++) {
                $a = $cleanPreserve[$i]
                $aOut = $a + "_out"
                for ($j = 0; $j -lt $cleanPreserve.Count; $j++) {
                    $b = $cleanPreserve[$j]
                    if ($i -lt $j -and $a.Equals($b, $comparison)) {
                        throw "Refusing to arm AutoTest: duplicate stem '$a' in -PreserveSaves."
                    }
                    if ($b.Equals($aOut, $comparison)) {
                        throw "Refusing to arm AutoTest: preserve output '$aOut' collides with input '$b'."
                    }
                }
            }
        } else {
            if ($cleanPreserve.Count -gt 0) {
                throw "Refusing to arm AutoTest: -PreserveSaves is only valid with -Only preserve."
            }
            if ([string]::IsNullOrWhiteSpace($cleanSave)) {
                throw "Action 'arm' requires -Save <save_stem>."
            }
            Assert-AutoTestSaveSlots $cleanSave $cleanTemp $Only $cleanOut
        }
        $paths = Get-AutoTestPaths
        $skseDir = $paths.SkseDir
        $markerPath = $paths.MarkerPath
        $lines = @(
            "# SkyrimPoE Stage 2 AutoTest marker (consumed on startup)"
        )
        if ($cleanSave) {
            $lines += "save=$cleanSave"
        }
        $lines += @(
            "temp_save=$cleanTemp"
            ("quit=" + ($Quit.ToString().ToLowerInvariant()))
            "consume_marker=true"
        )
        if ($cleanOut) {
            $lines += "out_save=$cleanOut"
        }
        if ($cleanPreserve.Count -gt 0) {
            $lines += ("preserve_saves=" + ($cleanPreserve -join ","))
        }
        if ($Only) {
            $lines += "only=$Only"
        }
        $timeoutDisplay = "DLL default"
        if ($PSBoundParameters.ContainsKey("TimeoutSec")) {
            $lines += "timeout_sec=$TimeoutSec"
            $timeoutDisplay = "$($TimeoutSec)s"
        }
        if ($WhatIfPreference) {
            $lines | Write-Output
        }
        if ($PSCmdlet.ShouldProcess($markerPath, "Write AutoTest marker")) {
            if (-not (Test-Path $skseDir)) {
                New-Item -ItemType Directory -Path $skseDir -Force | Out-Null
            }
            [System.IO.File]::WriteAllLines($markerPath, $lines, [System.Text.UTF8Encoding]::new($false))
            Write-Host "Armed AutoTest marker: $markerPath"
            $onlyDisplay = if ($Only) { $Only } else { "all" }
            $outDisplay = if ($cleanOut) { ", out_save=$cleanOut" } else { "" }
            Write-Host "Target save: $cleanSave (temp_save=$cleanTemp$outDisplay, quit=$Quit, timeout=$timeoutDisplay, only=$onlyDisplay)"
            Write-Host "Note: Skyrim is NOT launched by this script. Launch SKSE via MO2 when ready."
        }
    }
    "disarm" {
        $paths = Get-AutoTestPaths
        $markerPath = $paths.MarkerPath
        if (Test-Path $markerPath) {
            if ($PSCmdlet.ShouldProcess($markerPath, "Remove AutoTest marker")) {
                Remove-Item -Path $markerPath -Force -Confirm:$false
                Write-Host "Removed AutoTest marker: $markerPath"
            }
        } else {
            Write-Host "AutoTest marker is not present."
        }
    }
    "status" {
        $paths = Get-AutoTestPaths
        $markerPath = $paths.MarkerPath
        $reportPath = $paths.ReportPath
        $armed = Test-Path $markerPath
        $hasReport = Test-Path $reportPath
        Write-Host "Armed: $armed"
        if ($armed) {
            Get-Content -Path $markerPath -Encoding UTF8 | ForEach-Object { Write-Host "  $_" }
        }
        Write-Host "Report exists: $hasReport"
        if ($hasReport) {
            $fi = Get-Item $reportPath
            Write-Host ("Report updated: " + $fi.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss"))
        }
    }
    "report" {
        $paths = Get-AutoTestPaths
        $reportPath = $paths.ReportPath
        if (-not (Test-Path $reportPath)) {
            Write-Host "No AutoTest report found at $reportPath"
            exit 1
        }
        Get-Content -Path $reportPath -Encoding UTF8
    }
}
