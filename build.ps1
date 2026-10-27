# Builds bin\window-ring.exe (64-bit) and copies window-ring.ini beside it.
# Uses the clang that ships with Windhawk when it is installed, otherwise any
# MinGW-w64 clang++ on PATH. Pass -Compiler to pick another one.
param([string]$Compiler)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here 'src\window-ring.cpp'
$bin = Join-Path $here 'bin'
$out = Join-Path $bin 'window-ring.exe'

$windhawkClang = 'C:\Program Files\Windhawk\Compiler\bin\clang++.exe'
if (-not $Compiler) {
    $Compiler = if (Test-Path $windhawkClang) { $windhawkClang } else { 'clang++' }
}
New-Item -ItemType Directory -Force $bin | Out-Null

$clangArgs = @(
    '-std=c++23', '-O2', '-target', 'x86_64-w64-mingw32',
    '-municode', '-mwindows', '-static',
    '-DUNICODE', '-D_UNICODE',
    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00',
    '-DNTDDI_VERSION=0x0A000008', '-D__USE_MINGW_ANSI_STDIO=0'
)
$include = 'C:\Program Files\Windhawk\Compiler\include'
if ($Compiler -eq $windhawkClang) { $clangArgs += @('-I', $include) }
$clangArgs += @($src, '-ldwmapi', '-luser32', '-lgdi32', '-lshcore', '-o', $out)

& $Compiler @clangArgs
if ($LASTEXITCODE -ne 0) { throw "Compile failed, exit $LASTEXITCODE" }

Copy-Item (Join-Path $here 'src\window-ring.ini') $bin -Force
"built: $out ($((Get-Item $out).Length) bytes)"
