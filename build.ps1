param(
    [string]$Target = 'gm_dls_player',
    [int]$Jobs = 14,
    [switch]$Debug,
    [string[]]$Define = @(),   # extra preprocessor defines, e.g. -Define WT_BLOCK=8
    [string]$OutName            # override host output exe name (defaults to $Target)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $RepoRoot 'cmake-build-rp2040'
$StandaloneBuildDir = Join-Path $RepoRoot 'build'

$CMakeExe = 'C:\Users\xr1p\AppData\Local\Programs\CLion\bin\cmake\win\x64\bin\cmake.exe'
$ClionMingwBin = 'C:\Users\xr1p\AppData\Local\Programs\CLion\bin\mingw\bin'
$GccExe = Join-Path $ClionMingwBin 'gcc.exe'

$env:PICO_SDK_PATH = 'F:/pico-sdk'
$env:PICOTOOL_FETCH_FROM_GIT_PATH = 'c:\temp\'

if (($env:PATH -split ';') -notcontains $ClionMingwBin) {
    $env:PATH = "$ClionMingwBin;$env:PATH"
}

$HostTargets = @{
    'gm_dls_player' = 'gm_dls_player.c'
    'dls_pack'      = 'dls_pack.c'
    'wt_render'     = 'wt_render.c'
}

if ($HostTargets.ContainsKey($Target)) {
    if (-not (Test-Path $GccExe)) {
        throw "Compiler not found: $GccExe"
    }

    New-Item -ItemType Directory -Force -Path $StandaloneBuildDir | Out-Null
    $ExeName = if ($OutName) { $OutName } else { $Target }
    $OutputExe = Join-Path $StandaloneBuildDir ($ExeName + '.exe')
    $Source = Join-Path $RepoRoot $HostTargets[$Target]

    $CommonArgs = @(
        '-std=c2x',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-o', $OutputExe,
        $Source,
        '-lm'
    )
    foreach ($d in $Define) { $CommonArgs += "-D$d" }

    if ($Debug) {
        $CompilerArgs = @('-O0', '-g3') + $CommonArgs
    } else {
        $CompilerArgs = @('-O3', '-DNDEBUG') + $CommonArgs
    }

    & $GccExe @CompilerArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Host "Built $OutputExe"
    return
}

& $CMakeExe --build $BuildDir --target $Target -j $Jobs
