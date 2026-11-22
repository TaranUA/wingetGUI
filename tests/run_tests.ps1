#Requires -Version 5.1
# Builds and runs the parser and Win32 runtime tests (no winget or admin required).
param([switch]$LiveWinget)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$bin  = Join-Path $root 'tools\w64devkit\bin'
$obj  = Join-Path $root 'build'

New-Item -ItemType Directory -Force -Path $obj | Out-Null

$exe = Join-Path $obj 'parse_test.exe'
& (Join-Path $bin 'g++.exe') -std=c++17 -O2 -Wall -Wextra -o $exe `
    (Join-Path $root 'tests\parse_test.cpp') (Join-Path $root 'src\parser.cpp') -lshell32
if ($LASTEXITCODE -ne 0) { throw 'test build failed' }

& $exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$testDir = Join-Path $obj ('runtime-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDir | Out-Null
$runtime = Join-Path $testDir 'runtime_test.exe'
try {
    & (Join-Path $bin 'g++.exe') -std=c++17 -O2 -Wall -Wextra -municode -static -o $runtime `
        (Join-Path $root 'tests\runtime_test.cpp') (Join-Path $root 'src\parser.cpp') `
        (Join-Path $root 'src\settings.cpp') (Join-Path $root 'src\theme.cpp') `
        -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 -ldwmapi -luxtheme
    if ($LASTEXITCODE -ne 0) { throw 'runtime test build failed' }

    & $runtime
    $result = $LASTEXITCODE
    if ($result -eq 0 -and $LiveWinget) {
        & $runtime --live-winget
        $result = $LASTEXITCODE
    }
} finally {
    Remove-Item -LiteralPath (Join-Path $testDir 'WingetGUI\settings.ini'), $runtime, `
        (Join-Path $testDir 'WingetGUI.ini'), (Join-Path $testDir 'WingetGUI'), $testDir `
        -ErrorAction SilentlyContinue
}
exit $result
