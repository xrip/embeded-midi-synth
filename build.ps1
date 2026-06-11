param(
    [string]$Target = 'dls_pack',
    [switch]$Debug,
    [string[]]$Define = @(),   # extra preprocessor defines, e.g. -Define WT_BLOCK=8
    [string]$OutName            # override host output exe name (defaults to $Target)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$StandaloneBuildDir = Join-Path $RepoRoot 'build'

$GccExe = $env:CC
if (-not $GccExe) {
    $GccCmd = Get-Command gcc -ErrorAction SilentlyContinue
    if ($GccCmd) {
        $GccExe = $GccCmd.Source
    } else {
        $ClionGcc = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin\mingw\bin\gcc.exe'
        if (Test-Path $ClionGcc) {
            $GccExe = $ClionGcc
        } else {
            throw "Compiler not found. Put gcc in PATH or set CC to a C compiler."
        }
    }
}
$GccDir = Split-Path -Parent $GccExe
if ($GccDir -and (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $GccDir)) {
    $env:PATH = "$GccDir$([IO.Path]::PathSeparator)$env:PATH"
}

$HostTargets = @{
    'dls_pack'       = 'tools/dls_pack.c'
    'gus_pack'       = 'tools/gus_pack.c'
    'wt_render'      = 'examples/wt_render.c'
    'midi_selfcheck' = 'tools/midi_selfcheck.c'
    'mulaw_probe'    = 'tools/mulaw_probe.c'
}

if ($HostTargets.ContainsKey($Target)) {
    New-Item -ItemType Directory -Force -Path $StandaloneBuildDir | Out-Null
    $ExeName = if ($OutName) { $OutName } else { $Target }
    $ExeSuffix = if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' }
    $OutputExe = Join-Path $StandaloneBuildDir ($ExeName + $ExeSuffix)
    $Source = Join-Path $RepoRoot $HostTargets[$Target]

    $CommonArgs = @(
        '-std=c2x',
        '-Wall',
        '-Wextra',
        '-Wpedantic',
        '-Wno-unused-function',
        '-I', $RepoRoot,
        '-o', $OutputExe,
        $Source,
        '-lm'
    )
    foreach ($d in $Define) { $CommonArgs += "-D$d" }

    if ($Debug) {
        $CompilerArgs = @('-O0', '-g3') + $CommonArgs
    } else {
        $CompilerArgs = @('-Ofast', '-ffunction-sections', '-fdata-sections', '-DNDEBUG') + $CommonArgs
    }

    & $GccExe @CompilerArgs
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Write-Host "Built $OutputExe"
    return
}

throw "Unknown target '$Target'. Known targets: $($HostTargets.Keys -join ', ')"
