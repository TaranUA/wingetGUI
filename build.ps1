#Requires -Version 5.1
<#
.SYNOPSIS
    Builds dist\WingetGUI.exe - a single, statically linked, dependency-free exe.

.DESCRIPTION
    Uses the portable MinGW-w64 toolchain in tools\w64devkit (no install, no admin).
    See README.md for how to obtain the toolchain.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build.ps1
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin  = Join-Path $root 'tools\w64devkit\bin'
$src  = Join-Path $root 'src'
$obj  = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

foreach ($tool in @('g++.exe', 'windres.exe')) {
    $path = Join-Path $bin $tool
    if (-not (Test-Path $path)) {
        throw "Toolchain not found: $path - see README.md (section 'Building')."
    }
}

New-Item -ItemType Directory -Force -Path $obj, $dist | Out-Null

# Terminate any running instance so Windows does not lock dist\WingetGUI.exe during link
Get-Process -Name 'WingetGUI' -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host 'Compiling resources...'
& (Join-Path $bin 'windres.exe') -I $src (Join-Path $src 'app.rc') -O coff -o (Join-Path $obj 'app.res.o')
if ($LASTEXITCODE -ne 0) { throw 'windres failed' }

Write-Host 'Compiling and linking...'
$sources = @('main.cpp', 'parser.cpp', 'settings.cpp', 'theme.cpp', 'strings.cpp') |
    ForEach-Object { Join-Path $src $_ }

# Size-oriented flags:
#  -Os                          optimise for size
#  -ffunction-sections/-fdata-sections + --gc-sections
#                                drop unreferenced code and data
#  -fno-exceptions/-fno-rtti    the app uses neither
#  -fno-threadsafe-statics      the only function-local static (the uxtheme
#                                module handle) is initialised on the UI thread
#  -fno-asynchronous-unwind-tables / -fno-unwind-tables
#                                drop .pdata / .xdata SEH unwind tables (~24 KB)
#  -fno-ident                   drop compiler identification strings
$compilerArgs = @(
    '-std=c++17', '-Os', '-s', '-municode', '-mwindows',
    '-Wall', '-Wextra', '-fno-strict-aliasing',
    '-ffunction-sections', '-fdata-sections',
    '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics',
    '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-fno-ident',
    '-o', (Join-Path $dist 'WingetGUI.exe')
) + $sources + @(
    (Join-Path $obj 'app.res.o'),
    '-lcomctl32', '-lshell32', '-luser32', '-lgdi32', '-ladvapi32',
    '-ldwmapi', '-luxtheme',
    '-static', '-static-libgcc', '-static-libstdc++',
    '-Wl,--gc-sections',
    '-Wl,--subsystem,windows', '-Wl,--nxcompat', '-Wl,--dynamicbase', '-Wl,--high-entropy-va'
)
& (Join-Path $bin 'g++.exe') @compilerArgs
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }

$exe = Join-Path $dist 'WingetGUI.exe'
Write-Host ("Built {0}  ({1:N0} bytes)" -f $exe, (Get-Item $exe).Length)