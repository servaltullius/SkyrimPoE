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
    # 기존 위치 인자 호출(다섯 번째가 TimeoutSec)이 깨지지 않게 맨 뒤에 둔다.
    [ValidateSet("", "h27", "es", "save")]
    [string]$Only = ""
)

$ErrorActionPreference = "Stop"

function Normalize-SaveStem([string]$Name, [string]$ParamName) {
    $trimmed = $Name.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return ""
    }
    if ($trimmed -match '[\r\n\\/]') {
        throw "Parameter -$ParamName must be a save filename/stem without path separators or newlines."
    }
    if ($trimmed.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase)) {
        $trimmed = $trimmed.Substring(0, $trimmed.Length - 4)
    }
    return $trimmed
}

$skseDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Skyrim Special Edition\SKSE"
$markerPath = Join-Path $skseDir "SkyrimPoE.autotest"
$reportPath = Join-Path $skseDir "SkyrimPoE.autotest.log"

switch ($Action) {
    "arm" {
        $cleanSave = Normalize-SaveStem $Save "Save"
        if ([string]::IsNullOrWhiteSpace($cleanSave)) {
            throw "Action 'arm' requires -Save <save_stem>."
        }
        $cleanTemp = Normalize-SaveStem $TempSave "TempSave"
        if ([string]::IsNullOrWhiteSpace($cleanTemp)) {
            throw "Parameter -TempSave cannot be empty."
        }
        # T-5: 직접 쓰는 임시 세이브와 H 체크포인트 이름이 기준 세이브와 겹치는지 확인한다.
        $checkpointPattern = '^' + [regex]::Escape($cleanTemp) + '_h_restore(?:_[0-9]+_[0-9]+)?$'
        if ($cleanTemp.Equals($cleanSave, [System.StringComparison]::OrdinalIgnoreCase) -or
            $cleanSave -match $checkpointPattern) {
            throw "Refusing to arm AutoTest: -TempSave and its derived checkpoint names must differ from -Save."
        }
        $lines = @(
            "# SkyrimPoE Stage 2 AutoTest marker (consumed on startup)"
            "save=$cleanSave"
            "temp_save=$cleanTemp"
            ("quit=" + ($Quit.ToString().ToLowerInvariant()))
            "consume_marker=true"
        )
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
            Write-Host "Target save: $cleanSave (temp_save=$cleanTemp, quit=$Quit, timeout=$timeoutDisplay, only=$onlyDisplay)"
            Write-Host "Note: Skyrim is NOT launched by this script. Launch SKSE via MO2 when ready."
        }
    }
    "disarm" {
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
        if (-not (Test-Path $reportPath)) {
            Write-Host "No AutoTest report found at $reportPath"
            exit 1
        }
        Get-Content -Path $reportPath -Encoding UTF8
    }
}
