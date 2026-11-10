# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
# Complete Windows registered tests (including embedded backend and ConPTY),
# stock pinned SDK, verified ZIP. Source migration gate always precedes configure.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CkVisionSource,
    [Parameter(Mandatory)][string]$CkCoreFsSource,
    [Parameter(Mandatory)][string]$WorkRoot,
    [Parameter(Mandatory)][string]$ActionScopeRoot,
    [Parameter(Mandatory)][string]$ActionScopeVhd,
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or -not [IO.Directory]::Exists('V:\')) { throw 'Windows workstation requires V:' }
function Extended([string]$Path) {
    if (-not [IO.Path]::IsPathFullyQualified($Path)) { throw 'Absolute path required' }
    $native = $Path.Replace('/', '\')
    if ($native.StartsWith('\\?\', [StringComparison]::Ordinal)) { return $native }
    if ($native.StartsWith('\\', [StringComparison]::Ordinal)) { return '\\?\UNC\' + $native.Substring(2) }
    return '\\?\' + $native
}
function Require-V([string]$Path, [bool]$Temporary) {
    $actual = [IO.Path]::GetFullPath($Path)
    $comparison = $actual.Replace('/', '\')
    if ($comparison.StartsWith('\\?\', [StringComparison]::Ordinal)) { $comparison = $comparison.Substring(4) }
    $prefix = if ($Temporary) { 'V:\tmp\' } else { 'V:\' }
    if (-not $comparison.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Development path is outside $prefix : $actual" }
    return $actual
}
function Assert-ActionScopePath([string]$Path) {
    $prefix = 'V:\tmp\'
    $comparison = $Path.Replace('/', '\')
    if ($comparison.StartsWith('\\?\', [StringComparison]::Ordinal)) {
        if ($Path.Contains('/')) { throw 'Use native separators in an extended action-scope path' }
        $comparison = $comparison.Substring(4)
    }
    if (-not $comparison.StartsWith($prefix, [StringComparison]::Ordinal)) {
        throw 'Action-scope paths must be explicit addresses below V:\tmp'
    }
    foreach ($component in $comparison.Substring($prefix.Length).Split([char]'\')) {
        if ($component -cin @('', '.', '..') -or $component -match '[\x00-\x1f:;*?"<>|]') {
            throw 'Action-scope paths must use unambiguous components'
        }
    }
}
$WorkRoot = Require-V $WorkRoot $true
Assert-ActionScopePath $ActionScopeRoot
Assert-ActionScopePath $ActionScopeVhd
if (-not [IO.Directory]::Exists((Extended $ActionScopeRoot)) -or
    -not [IO.File]::Exists((Extended $ActionScopeVhd))) {
    throw 'The dedicated action-scope VHD and mounted workspace must exist'
}
# The backend verifies the held volume identity and its isolation before test I/O.
$actionScopeConfigure = @(
    "-DCK_CORE_FS_ACTION_SCOPE_ROOT:STRING=$ActionScopeRoot"
    "-DCK_CORE_FS_ACTION_SCOPE_VHD:FILEPATH=$ActionScopeVhd"
)
# Bootstrap already created this unique parent. Refuse all prior outputs; never
# remove/reuse an unknown build or someone's active SDK.
if (-not [IO.Directory]::Exists((Extended $WorkRoot))) { throw 'CI bootstrap must create the unique WorkRoot' }
foreach ($output in 'ckvision-build','ckvision-sdk','build','build-pkg','packages','unpacked','journal-cleanup-build','journal-cleanup-fixtures','journal-cleanup-unused-install') {
    if ([IO.Directory]::Exists((Extended (Join-Path $WorkRoot $output)))) { throw "Output already exists: $output" }
}
$tmp = Join-Path $WorkRoot 'tmp'
[IO.Directory]::CreateDirectory((Extended $tmp)) | Out-Null
foreach ($name in 'TMP','TEMP','TMPDIR') { [Environment]::SetEnvironmentVariable($name, $tmp, 'Process') }
$env:CK_FS_REQUIRE_V_DEV_DRIVE='1'
$env:CMAKE_BUILD_PARALLEL_LEVEL='1';$env:CTEST_PARALLEL_LEVEL='1';$env:MAKEFLAGS='-j1';$env:ARCHIVE_BUILD_JOBS='1'
$env:CPACK_THREADS='1';$env:CPACK_ARCHIVE_THREADS='1';$env:PYTHONDONTWRITEBYTECODE='1'
$source = Require-V ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))) $false
$CkVisionSource = Require-V $CkVisionSource $false
$CkCoreFsSource = Require-V $CkCoreFsSource $false
& python (Join-Path $source 'scripts/ci/verify-inputs.py') --source $source --vision $CkVisionSource --backend $CkCoreFsSource --report (Join-Path $WorkRoot 'ci-inputs.json')
if ($LASTEXITCODE -ne 0) { throw 'Exact source/dependency pin verification failed' }
# This backend-committed scanner retains its 14-source inventory. Never use a
# frontend duplicate, filter CTest, delete fixtures, or treat a hook option as migration.
& python (Join-Path $CkCoreFsSource 'tools/check_windows_runtime_hooks.py') --source $CkCoreFsSource --report (Join-Path $WorkRoot 'windows-hook-preflight.json')
if ($LASTEXITCODE -ne 0) { throw 'BLOCKED before any configure: full Windows fixture migration is incomplete' }
$fixture = Join-Path $WorkRoot 'backend-fixtures'
$frontFixture = Join-Path $WorkRoot 'frontend-fixtures'
[IO.Directory]::CreateDirectory((Extended $fixture)) | Out-Null
[IO.Directory]::CreateDirectory((Extended $frontFixture)) | Out-Null
# Caller expected-provider observation only. No default NTFS and no drive-letter
# guess through mounted/reparse aliases. Each native test independently qualifies its handle.
$provider = & (Join-Path $CkCoreFsSource 'tools/windows_fixture_provider.ps1') -FixtureRoot $fixture -Report (Join-Path $WorkRoot 'windows-provider.json')
if ($provider -cnotin @('NTFS','ReFS')) { throw 'Explicit observed NTFS/ReFS provider required' }
& (Join-Path $CkCoreFsSource 'tools/windows_fixture_provider.ps1') -FixtureRoot $fixture -Report (Join-Path $WorkRoot 'build') -ValidatePathsOnly
# Preprovisioned native toolchain; no machine settings/tool installation changes.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Required Visual Studio C++ toolchain unavailable' }
$hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'amd64' }
$target = if ($Architecture -eq 'arm64') { 'arm64' } else { 'amd64' }
$environment = cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -no_logo -host_arch=$hostArch -arch=$target >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Developer environment failed' }
foreach ($line in $environment) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') } }
foreach ($name in 'TMP','TEMP','TMPDIR') { [Environment]::SetEnvironmentVariable($name, $tmp, 'Process') }
$env:CMAKE_BUILD_PARALLEL_LEVEL='1';$env:CTEST_PARALLEL_LEVEL='1';$env:ARCHIVE_BUILD_JOBS='1'
function Invoke-Step([string]$Name, [scriptblock]$Command) {
    $start=[DateTime]::UtcNow; $clock=[Diagnostics.Stopwatch]::StartNew()
    & $Command
    $exit=$LASTEXITCODE; $clock.Stop()
    [ordered]@{stage=$Name;start=$start.ToString('o');end=[DateTime]::UtcNow.ToString('o');seconds=$clock.Elapsed.TotalSeconds;exit=$exit} | ConvertTo-Json -Compress | Add-Content -LiteralPath (Extended (Join-Path $WorkRoot 'stages.jsonl'))
    if ($exit -ne 0) { throw "$Name failed: $exit" }
}
$sdk=Join-Path $WorkRoot 'ckvision-sdk';$build=Join-Path $WorkRoot 'build'
Invoke-Step 'Configure stock ckVision SDK' { cmake -S $CkVisionSource -B (Join-Path $WorkRoot 'ckvision-build') -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF -DCKVISION_BUILD_EXAMPLES=OFF -DCKVISION_BUILD_TESTING=OFF -DCKVISION_BUILD_FUZZERS=OFF "-DCMAKE_INSTALL_PREFIX=$sdk" }
Invoke-Step 'Build SDK' { cmake --build (Join-Path $WorkRoot 'ckvision-build') --parallel 1 }
Invoke-Step 'Install SDK' { cmake --install (Join-Path $WorkRoot 'ckvision-build') }
Invoke-Step 'Configure complete frontend/backend' { cmake -S $source -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded "-DCMAKE_PREFIX_PATH=$sdk" "-DCKTOOLS_CKVISION_PREFIX=$sdk" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCKTOOLS_VERIFY_CKVISION_PACKAGE=ON "-DCK_CORE_FS_LIB_SOURCE_DIR=$CkCoreFsSource" "-DCK_CORE_FS_NATIVE_TEST_ROOT=$fixture" "-DCKTOOLS_TEST_TEMP_ROOT=$frontFixture" "-DCK_CORE_FS_ARCHIVE_TEST_FS=$provider" "-DCK_CORE_FS_METADATA_TARGET_NATIVE_FS=$provider" -DCK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS=ON -DCK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP=OFF -DCPACK_THREADS=1 -DCPACK_ARCHIVE_THREADS=1 -DBUILD_TESTING=ON @actionScopeConfigure }
Invoke-Step 'Build frontend/backend' { cmake --build $build --parallel 1 }
Invoke-Step 'Package consumer' { cmake --build $build --target verify_ckvision_package --parallel 1 }
Invoke-Step 'All registered tests including ConPTY/backend' { ctest --test-dir $build --output-on-failure --parallel 1 --no-tests=error }
# Separate backend-only cohort: never reuse normal objects, SDK prefix or ZIP.
# This option is mutually exclusive with the first suite's native-close switch.
$journalBuild=Join-Path $WorkRoot 'journal-cleanup-build'
$journalFixture=Join-Path $WorkRoot 'journal-cleanup-fixtures'
[IO.Directory]::CreateDirectory((Extended $journalFixture)) | Out-Null
$journalProvider=& (Join-Path $CkCoreFsSource 'tools/windows_fixture_provider.ps1') -FixtureRoot $journalFixture -Report (Join-Path $WorkRoot 'journal-cleanup-provider.json')
if ($journalProvider -cnotin @('NTFS','ReFS') -or $journalProvider -cne $provider) { throw 'Journal supplemental actual provider must match the admitted fixture provider' }
& (Join-Path $CkCoreFsSource 'tools/windows_fixture_provider.ps1') -FixtureRoot $journalFixture -Report $journalBuild -ValidatePathsOnly
& python (Join-Path $source 'scripts/ci/verify-inputs.py') --source $source --vision $CkVisionSource --backend $CkCoreFsSource --report (Join-Path $WorkRoot 'journal-cleanup-inputs.json')
if ($LASTEXITCODE -ne 0) { throw 'Journal supplemental source/dependency cohort changed' }
Invoke-Step 'Configure separate uniform safe-hook journal SDK' { cmake -S $CkCoreFsSource -B $journalBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF "-DCMAKE_INSTALL_PREFIX=$WorkRoot/journal-cleanup-unused-install" "-DCK_CORE_FS_NATIVE_TEST_ROOT=$journalFixture" "-DCK_CORE_FS_ARCHIVE_TEST_FS=$journalProvider" "-DCK_CORE_FS_METADATA_TARGET_NATIVE_FS=$journalProvider" -DCK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP=ON -DCK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS=OFF -DBUILD_TESTING=ON @actionScopeConfigure }
$cache=[IO.File]::ReadAllText((Extended (Join-Path $journalBuild 'CMakeCache.txt')))
foreach ($setting in @('CK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP:BOOL=ON','CK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS:BOOL=OFF','CMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON')) {
    if (-not ($cache.Split("`n").Trim() -ccontains $setting)) { throw "Journal cache contract missing: $setting" }
}
$commands=@([IO.File]::ReadAllText((Extended (Join-Path $journalBuild 'compile_commands.json'))) | ConvertFrom-Json)
if ($commands.Count -eq 0) { throw 'No journal SDK compile commands exported' }
# BEGIN PURE JOURNAL COMPILER GUARDS
function Get-JournalCompilerArguments([string]$Command) {
    # Decode ordinary Windows CRT argument quoting without invoking any command.
    # Response files remain opaque and are rejected by the definition guard.
    if ($Command.IndexOf([char]0) -ge 0) { throw 'NUL cannot occur in a native compiler command line' }
    $result=[Collections.Generic.List[string]]::new()
    $index=0
    while ($index -lt $Command.Length) {
        while ($index -lt $Command.Length -and ($Command[$index] -eq [char]32 -or $Command[$index] -eq [char]9)) { $index++ }
        if ($index -eq $Command.Length) { break }
        $word=[Text.StringBuilder]::new();$quoted=$false
        while ($index -lt $Command.Length) {
            $slashes=0
            while ($index -lt $Command.Length -and $Command[$index] -eq [char]92) { $slashes++;$index++ }
            if ($index -lt $Command.Length -and $Command[$index] -eq [char]34) {
                [void]$word.Append([char]92,($slashes -shr 1))
                if (($slashes % 2) -ne 0) { [void]$word.Append([char]34) }
                elseif ($quoted -and $index+1 -lt $Command.Length -and $Command[$index+1] -eq [char]34) { [void]$word.Append([char]34);$index++ }
                else { $quoted=-not $quoted }
                $index++;continue
            }
            [void]$word.Append([char]92,$slashes)
            if ($index -eq $Command.Length -or (-not $quoted -and ($Command[$index] -eq [char]32 -or $Command[$index] -eq [char]9))) { break }
            [void]$word.Append($Command[$index]);$index++
        }
        if ($quoted) { throw 'Unterminated compiler argument quote' }
        $result.Add($word.ToString())
    }
    return $result.ToArray()
}
function Assert-JournalHookDefinition([string[]]$Arguments) {
    $macro='CK_FS_NATIVE_CLOSE_TEST_HOOKS';$definitions=0
    foreach ($argument in $Arguments) {
        if ($argument.StartsWith('@',[StringComparison]::Ordinal) -or $argument -ieq '/link' -or $argument -ieq '-link') { throw 'Opaque response/linker arguments cannot prove compiler definitions' }
    }
    for ($index=0;$index -lt $Arguments.Count;$index++) {
        $argument=$Arguments[$index]
        if ($argument -cmatch '^[/-]([DU])(.*)$') {
            $operation=$Matches[1];$payload=$Matches[2]
            if ($payload.Length -eq 0) {
                $index++
                if ($index -eq $Arguments.Count) { throw 'Missing compiler definition/undefinition argument' }
                $payload=$Arguments[$index]
                if ($payload.StartsWith('-',[StringComparison]::Ordinal) -or $payload.StartsWith('/',[StringComparison]::Ordinal)) { throw 'Missing definition before next compiler switch' }
            }
            $name=[regex]::Match($payload,'^\s*([A-Za-z_][A-Za-z0-9_]*)(.*)$',[Text.RegularExpressions.RegexOptions]::Singleline)
            if ($name.Success -and $name.Groups[1].Value -ceq $macro) {
                if ($operation -ceq 'U') { throw 'Journal safe-hook macro is undefined' }
                if ($payload -cne $macro -and $payload -cne ($macro+'=1')) { throw 'Invalid journal safe-hook macro value/form' }
                $definitions++
            } elseif (-not $name.Success -and $payload.Contains($macro)) { throw 'Unsupported journal macro definition payload' }
        } elseif ($argument -cmatch '^[/-][du]' -and $argument.Contains($macro)) { throw 'Unsupported case-sensitive journal definition flag' }
    }
    if ($definitions -ne 1) { throw "Expected exactly one journal safe-hook definition; observed $definitions" }
}
function Assert-JournalReleaseFlags([string[]]$Arguments) {
    foreach ($rule in @(@{group='^[/-]EH';valid='^[/-]EHsc$'},@{group='^[/-]GR';valid='^[/-]GR$'},@{group='^[/-]M[DT]';valid='^[/-]MT$'})) {
        $found=@($Arguments | Where-Object { $_ -cmatch $rule.group })
        if ($found.Count -eq 0 -or @($found | Where-Object { $_ -cnotmatch $rule.valid }).Count -ne 0) { throw 'Missing/contradictory journal C++ unwind/RTTI/CRT arguments' }
    }
}
# END PURE JOURNAL COMPILER GUARDS
$audited=@()
foreach ($entry in $commands) {
    $command=$entry.command
    $compilerArguments=@(Get-JournalCompilerArguments $command)
    Assert-JournalHookDefinition $compilerArguments
    if ($entry.file -match '\.(cpp|cxx|cc)$') { Assert-JournalReleaseFlags $compilerArguments }
    $audited+=,[ordered]@{file=$entry.file;uniform_safe_hook=$true;command=$command}
}
# Check the required Native/Platform/Engine/provider/client and C-wrapper inputs
# are actually represented; all exported SDK/client commands were checked above.
foreach ($required in @('/src/files/native_name.cpp','/src/files/platform/operation_journal_file_windows.cpp','/src/files/reviewed_tree_plan.cpp','/src/c-api/journal_root_c.cpp','/tests/journal_temporary_cleanup_windows_tests.cpp')) {
    if (@($commands | Where-Object { $_.file.Replace('\','/').EndsWith($required,[StringComparison]::Ordinal) }).Count -ne 1) { throw "Required journal cohort input absent/duplicated: $required" }
}
[IO.File]::WriteAllText((Extended (Join-Path $WorkRoot 'journal-cleanup-flags.json')),($audited | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
Invoke-Step 'Build separate journal Native closure and actual C SDK' { cmake --build $journalBuild --target ck_fs_journal_temporary_cleanup_windows_tests ck_file_engine_c --parallel 1 }
$inventoryText=& ctest --test-dir $journalBuild --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'Journal CTest inventory failed' }
$inventoryJson=$inventoryText -join "`n"
[IO.File]::WriteAllText((Extended (Join-Path $WorkRoot 'journal-cleanup-ctest-inventory.json')),$inventoryJson,[Text.UTF8Encoding]::new($false))
$inventory=$inventoryJson | ConvertFrom-Json
$prefix="journal_temporary_cleanup_windows_${journalProvider}_"
$journalTests=@($inventory.tests | Where-Object { $_.name.StartsWith($prefix,[StringComparison]::Ordinal) })
$expected=@()
foreach ($variant in @('ordinary','long')) {
    foreach ($mode in @('control','native','native-oom','consumed','consumed-oom')) { $expected+=$prefix+$variant+'_'+$mode }
}
if ($journalTests.Count -ne 10 -or (($journalTests.name | Sort-Object) -join ',') -cne (($expected | Sort-Object) -join ',')) { throw 'Journal registration must contain exactly all ten ordinary/long and five-mode combinations' }
foreach ($test in $journalTests) {
    foreach ($property in $test.properties) {
        if ($property.name -match '^(DISABLED|SKIP_RETURN_CODE|SKIP_REGULAR_EXPRESSION)$') { throw "Journal test cannot disable/skip native assertions: $($test.name)" }
    }
}
# Selection is exclusive to this NEW supplement. The first full frontend/backend
# CTest above is unchanged and unfiltered; no legacy case or guard is removed.
$journalPattern='^journal_temporary_cleanup_windows_'+$journalProvider+'_(ordinary|long)_(control|native|native-oom|consumed|consumed-oom)$'
Invoke-Step 'All ten registered journal supplemental modes (ordinary and >340 long)' { ctest --test-dir $journalBuild --output-on-failure --parallel 1 --no-tests=error --tests-regex $journalPattern --output-junit (Join-Path $WorkRoot 'journal-cleanup-results.xml') }
$productionBuild=Join-Path $WorkRoot 'build-pkg'
Invoke-Step 'Configure production frontend/backend' {
    cmake -S $source -B $productionBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
        "-DCMAKE_PREFIX_PATH=$sdk" "-DCKTOOLS_CKVISION_PREFIX=$sdk" "-DCK_CORE_FS_LIB_SOURCE_DIR=$CkCoreFsSource" `
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
        -DBUILD_TESTING:BOOL=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON `
        -DCK_CORE_FS_TEST_NATIVE_CLOSE_HOOKS:BOOL=OFF -DCK_CORE_FS_TEST_JOURNAL_TEMPORARY_CLEANUP:BOOL=OFF `
        -DCPACK_THREADS=1 -DCPACK_ARCHIVE_THREADS=1
}
Invoke-Step 'Build production frontend/backend' { cmake --build $productionBuild --parallel 1 }
$packages=Join-Path $WorkRoot 'packages'
$cpack=Join-Path (Split-Path (Get-Command cmake).Source) 'cpack.exe'
Push-Location $productionBuild
try { Invoke-Step 'Package production ZIP' { & $cpack --config (Join-Path $productionBuild 'CPackConfig.cmake') -G ZIP -B $packages -D CPACK_THREADS=1 -D CPACK_ARCHIVE_THREADS=1 } } finally { Pop-Location }
$zips=@([IO.Directory]::GetFiles((Extended $packages),'ck-utilities-*-windows-*.zip'))
if ($zips.Count -ne 1) { throw 'Exactly one qualified ZIP is required' }
$zip=$zips[0];$unpacked=Join-Path $WorkRoot 'unpacked'
$zipBefore=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.Compression.ZipFile]::ExtractToDirectory($zip,(Extended $unpacked))
$roots=@([IO.Directory]::GetDirectories((Extended $unpacked)))
if ($roots.Count -ne 1) { throw 'Exactly one product root required' }
$root=$roots[0];$top=@([IO.Directory]::GetFileSystemEntries($root) | ForEach-Object {[IO.Path]::GetFileName($_)} | Sort-Object)
if (($top -join ',') -ne 'bin,share') { throw "Unexpected ZIP layout: $($top -join ',')" }
$tools='ck-utilities','ck-json-view','ck-find','ck-du','ck-files','ck-edit','ck-chat','ck-todo','ck-sysmon','ck-config'
$docRoot=Join-Path $root 'share/doc/ck-utilities'
$guides=@([IO.Directory]::GetFiles((Join-Path $docRoot 'tools'),'*.md'))
if ($guides.Count -ne $tools.Count) { throw 'Archive guide count mismatch' }
$screenshots=@([IO.Directory]::GetFiles((Join-Path $docRoot 'generated/screenshots'),'*.svg'))
if ($screenshots.Count -lt 1) { throw 'Archive screenshots absent' }
$bin=Extended ([IO.Path]::Combine($root,'bin'))
foreach ($directory in @($root,$bin)) {
    $attributes=[IO.File]::GetAttributes($directory)
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        ($attributes -band [IO.FileAttributes]::Directory) -eq 0) { throw "Unexpected ZIP directory: $directory" }
}
$images=[ordered]@{}
foreach ($tool in $tools) {
    $image=[IO.Path]::Combine($bin,"$tool.exe")
    $attributes=[IO.File]::GetAttributes($image)
    if (($attributes -band ([IO.FileAttributes]::ReparsePoint -bor [IO.FileAttributes]::Directory)) -ne 0) {
        throw "Unexpected ZIP executable: $image"
    }
    $images[$tool]=(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
    & $image --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$tool --help failed from ZIP" }
}
$previousBin=[Environment]::GetEnvironmentVariable('CKTOOLS_EXTRACTED_PACKAGE_BIN','Process')
try {
    [Environment]::SetEnvironmentVariable('CKTOOLS_EXTRACTED_PACKAGE_BIN',$bin,'Process')
    Invoke-Step 'Actual ZIP binaries through ConPTY' {
        ctest --test-dir (Join-Path $build 'tests/package-terminal/Release') --output-on-failure --parallel 1 --no-tests=error --output-junit (Join-Path $WorkRoot 'zip-terminal-results.xml')
    }
} finally {
    [Environment]::SetEnvironmentVariable('CKTOOLS_EXTRACTED_PACKAGE_BIN',$previousBin,'Process')
}
foreach ($tool in $tools) {
    $image=[IO.Path]::Combine($bin,"$tool.exe")
    if ((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant() -cne $images[$tool]) {
        throw "Extracted executable changed during ZIP verification: $image"
    }
}
$digest=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($digest -cne $zipBefore) { throw 'ZIP changed during verification' }
$runtimeReceipt=[ordered]@{package=$zip;sha256=$digest;extracted_bin=$bin;images=$images}
[IO.File]::WriteAllText((Extended (Join-Path $WorkRoot 'zip-terminal-images.json')),($runtimeReceipt | ConvertTo-Json -Depth 3),[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText(($zip+'.sha256'),($digest+'  '+[IO.Path]::GetFileName($zip)),[Text.Encoding]::ASCII)
Write-Host "PASS complete registered tests and verified ZIP: $zip"
