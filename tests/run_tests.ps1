#Requires -Version 5.1
# Builds and runs the parser unit tests (no winget or admin required).
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$bin  = Join-Path $root 'tools\w64devkit\bin'
$obj  = Join-Path $root 'build'

New-Item -ItemType Directory -Force -Path $obj | Out-Null

$exe = Join-Path $obj 'parse_test.exe'
& (Join-Path $bin 'g++.exe') -std=c++17 -O2 -Wall -Wextra -o $exe `
    (Join-Path $root 'tests\parse_test.cpp') (Join-Path $root 'src\parser.cpp')
if ($LASTEXITCODE -ne 0) { throw 'test build failed' }

& $exe
exit $LASTEXITCODE