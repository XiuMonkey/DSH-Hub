# ---------------------------------------------------------------------------
# rebuild-host.ps1 - rebuild the HOST (DSH Hub) baseline build. ASCII only.
#
#   powershell -ExecutionPolicy Bypass -File rebuild-host.ps1
#   powershell -ExecutionPolicy Bypass -File rebuild-host.ps1 -ErrorsOnly
#
# Mirrors the toolchain setup of the repo's own scripts (build.ps1 in the
# extension uses the same paths). Builds <repo>/build/windows-ninja.
# ---------------------------------------------------------------------------
param(
    [switch]$ErrorsOnly,
    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'

$repo  = 'C:\Users\Playe\Documents\DSH hub\DSH Hub'
$msvc  = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$kits  = 'C:\Program Files (x86)\Windows Kits\10'
$sdk   = '10.0.26100.0'
$qt    = 'D:\Qt\6.11.2\msvc2022_64'
$ninja = 'D:\Qt\Tools\Ninja\ninja.exe'
$cmake = 'D:\Qt\Tools\CMake_64\bin\cmake.exe'

$env:INCLUDE = "$msvc\include;$kits\Include\$sdk\ucrt;$kits\Include\$sdk\shared;$kits\Include\$sdk\um;$kits\Include\$sdk\winrt;$kits\Include\$sdk\cppwinrt"
$env:LIB     = "$msvc\lib\x64;$kits\Lib\$sdk\ucrt\x64;$kits\Lib\$sdk\um\x64"
$env:PATH    = "$msvc\bin\Hostx64\x64;$kits\bin\$sdk\x64;$(Split-Path $ninja);$(Split-Path $cmake);$qt\bin;$env:PATH"
$env:VSLANG  = '1033'

$buildDir = Join-Path $repo 'build\windows-ninja'
$cmakeArgs = @('--build', $buildDir)
if ($Target) { $cmakeArgs += @('--target', $Target) }

$output = & $cmake @cmakeArgs 2>&1
$code = $LASTEXITCODE
if ($ErrorsOnly) {
    $output | Select-String -Pattern 'error' | Select-Object -First 40
} else {
    $output | Select-Object -Last 30
}
Write-Host "[rebuild-host] exit=$code"
exit $code
