# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds, tests and packages CK Utilities on Windows with MSVC and Ninja, the
# way CI and releases do: the pinned ckVision SDK and the suite in Release
# with the static C runtime, every CTest suite (including the ConPTY contract
# that starts each tool and quits it), then the ZIP archive, extracted and
# checked so every packaged tool starts.
param(
    [Parameter(Mandatory=$true)][string]$CkVisionSource,
    [Parameter(Mandatory=$true)][string]$WorkRoot,
    [string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
$WorkRoot = (Resolve-Path $WorkRoot).Path
foreach ($name in 'TMP','TEMP') { [Environment]::SetEnvironmentVariable($name, (Join-Path $WorkRoot 'tmp'), 'Process') }
New-Item -ItemType Directory -Force -Path (Join-Path $WorkRoot 'tmp') | Out-Null

# The MSVC developer environment of the newest Visual Studio with C++ tools,
# including its bundled CMake and Ninja.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ toolset was found' }
$hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
$target = if ($Architecture -eq 'arm64') { 'arm64' } else { 'amd64' }
$environment = cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -no_logo -host_arch=$hostArch -arch=$target >nul && set"
foreach ($line in $environment) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') } }

function Invoke-Step([string]$Name, [scriptblock]$Command) {
    Write-Host "== $Name"
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

$sdk = Join-Path $WorkRoot 'ckvision-sdk'
Invoke-Step 'Configure ckVision SDK' { cmake -S $CkVisionSource -B (Join-Path $WorkRoot 'ckvision-build') -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCKVISION_BUILD_EXAMPLES=OFF -DCKVISION_BUILD_TESTING=OFF -DCKVISION_BUILD_FUZZERS=OFF "-DCMAKE_INSTALL_PREFIX=$sdk" }
Invoke-Step 'Build ckVision SDK' { cmake --build (Join-Path $WorkRoot 'ckvision-build') --parallel }
Invoke-Step 'Install ckVision SDK' { cmake --install (Join-Path $WorkRoot 'ckvision-build') }

$build = Join-Path $WorkRoot 'build'
Invoke-Step 'Configure CK Utilities' { cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded "-DCMAKE_PREFIX_PATH=$sdk" "-DCKTOOLS_CKVISION_PREFIX=$sdk" -DBUILD_TESTING=ON }
Invoke-Step 'Build CK Utilities' { cmake --build $build --parallel }
Invoke-Step 'Test CK Utilities' { ctest --test-dir $build --output-on-failure --parallel 4 }

$packages = Join-Path $WorkRoot 'packages'
# CMake's cpack, beside cmake: Chocolatey installs an unrelated cpack.exe
# that can come first on PATH.
$cpack = Join-Path (Split-Path (Get-Command cmake).Source) 'cpack.exe'
Push-Location $build
try { Invoke-Step 'Package' { & $cpack -G ZIP -B $packages } } finally { Pop-Location }
$zip = Get-ChildItem $packages -Filter 'ck-utilities-*-windows-*.zip' | Select-Object -First 1
if (-not $zip) { throw 'The ZIP archive was not produced' }
$unpacked = Join-Path $WorkRoot 'unpacked'
if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
Expand-Archive -LiteralPath $zip.FullName -DestinationPath $unpacked
$root = Get-ChildItem $unpacked -Directory | Select-Object -First 1
$top = @(Get-ChildItem $root.FullName | ForEach-Object Name | Sort-Object)
if (($top -join ',') -ne 'bin,share') { throw "The archive holds more than the product: $($top -join ', ')" }
$tools = 'ck-utilities','ck-json-view','ck-find','ck-du','ck-edit','ck-chat','ck-todo','ck-sysmon','ck-config'
foreach ($tool in $tools) {
    & (Join-Path $root.FullName "bin/$tool.exe") --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$tool --help failed from the archive" }
}
(Get-FileHash $zip.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $zip.Name |
    Set-Content -Encoding ascii -NoNewline (Join-Path $packages ($zip.Name + '.sha256'))
Write-Host "PASS $($zip.Name): $($tools.Count) tools start from the archive"
