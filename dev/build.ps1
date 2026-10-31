param([ValidateSet('Release','Debug')][string]$Configuration='Release', [switch]$Sanitize, [switch]$Quick, [switch]$LinkOptimization, [string[]]$Target=@(), [string]$PackageDirectory="", [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$NativeBuildDirectory="", [ValidateSet('Full','Iteration')][string]$TestMode='Full', [switch]$NoPublish, [switch]$SkipTests)
$ErrorActionPreference='Stop'
$previousBuildTemp=$env:TEMP; $previousBuildTmp=$env:TMP
$taskBuildTemp=Join-Path $PSScriptRoot $(if($Sanitize){'test-tmp-asan'}else{'test-tmp'})
New-Item -ItemType Directory -Force -Path $taskBuildTemp | Out-Null
$env:TEMP=$taskBuildTemp; $env:TMP=$taskBuildTemp
try {
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio C++ Desktop tools first.' }
$msvc = Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk = Get-ChildItem -LiteralPath "$sdkRoot\Lib" -Directory | Where-Object { (Test-Path -LiteralPath (Join-Path $_.FullName 'um\x64\mfplat.lib')) -and (Test-Path -LiteralPath (Join-Path $_.FullName 'um\x64\gdi32.lib')) } | Sort-Object Name -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK with Media Foundation is required.' }
$cmakeRoot = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$env:PATH = "$($msvc.FullName)\bin\Hostx64\x64;$sdkRoot\bin\$($sdk.Name)\x64;$cmakeRoot\CMake\bin;$cmakeRoot\Ninja;$env:PATH"
$env:INCLUDE = "$($msvc.FullName)\include;$sdkRoot\Include\$($sdk.Name)\ucrt;$sdkRoot\Include\$($sdk.Name)\shared;$sdkRoot\Include\$($sdk.Name)\um;$sdkRoot\Include\$($sdk.Name)\winrt"
$env:LIB = "$($msvc.FullName)\lib\x64;$sdkRoot\Lib\$($sdk.Name)\ucrt\x64;$sdkRoot\Lib\$($sdk.Name)\um\x64"
function Invoke-CMake([string[]]$Arguments) {
  # Windows PowerShell 5 turns redirected native stderr into error records.
  # A CMake warning must not abort a logged build; the exit code remains decisive.
  $previousPreference=$ErrorActionPreference
  try {
    $ErrorActionPreference='Continue'
    & cmake @Arguments 2>&1 | ForEach-Object { $_.ToString() }
    $exitCode=$LASTEXITCODE
  } finally { $ErrorActionPreference=$previousPreference }
  if($exitCode){throw "CMake failed with exit code $exitCode."}
}
function Get-SourceSha256([string]$Path) {
  # Child Windows PowerShell processes may inherit a PSModulePath containing
  # only PowerShell 7 modules. Use .NET directly instead of module autoloading.
  $stream=[IO.File]::OpenRead($Path)
  $hash=[Security.Cryptography.SHA256]::Create()
  try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
  finally { $hash.Dispose(); $stream.Dispose() }
}
$out = if($Sanitize){'build-asan'}else{'build-r4'}
if ($NativeBuildDirectory) { $out=$NativeBuildDirectory }
$cachePath=Join-Path "$PSScriptRoot\$out" 'CMakeCache.txt'
$metricRequested=$Target -contains 'fajk_metric'
if ($Quick -and (Test-Path -LiteralPath $cachePath)) {
  $cache=Get-Content -LiteralPath $cachePath
  $cachedSource=($cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' }) -replace '^CMAKE_HOME_DIRECTORY:INTERNAL=',''
  if ([IO.Path]::GetFullPath($cachedSource) -ne [IO.Path]::GetFullPath($PSScriptRoot)) { throw 'Build cache belongs to another checkout. Use -NativeBuildDirectory with a fresh directory.' }
  if ($cache -notcontains "CMAKE_BUILD_TYPE:STRING=$Configuration") { throw 'Quick build configuration differs. Run without -Quick first.' }
  $cachedSanitize=[bool]($cache -match '^FAJK_SANITIZE:BOOL=(ON|TRUE|1)$')
  if ($cachedSanitize -ne $Sanitize.IsPresent) { throw 'Quick build sanitizer differs. Run without -Quick first.' }
  $cachedLinkOptimization=[bool]($cache -match '^FAJK_LINK_OPTIMIZATION:BOOL=(ON|TRUE|1)$')
  if ($cachedLinkOptimization -ne $LinkOptimization.IsPresent) { throw 'Quick build link optimization differs. Run without -Quick first.' }
  if($metricRequested -and $cache -notcontains 'FAJK_REFERENCE_METRIC:BOOL=ON') {
    Invoke-CMake @('-S',$PSScriptRoot,'-B',"$PSScriptRoot\$out",'-DFAJK_REFERENCE_METRIC=ON')
  }
  # Ninja regenerates CMake when project files change. Source-only iterations
  # need neither WinUI preparation nor repeated compiler capability probes.
} else {
  if (!$Target.Count -or $Target -contains 'FajkStudio') { . "$PSScriptRoot\prepare-winui.ps1" }
  $metricArgs=if($metricRequested){@('-DFAJK_REFERENCE_METRIC=ON')}else{@()}
  Invoke-CMake (@('-S',$PSScriptRoot,'-B',"$PSScriptRoot\$out",'-G','Ninja',"-DCMAKE_BUILD_TYPE=$Configuration","-DFAJK_SANITIZE=$($Sanitize.IsPresent)","-DFAJK_LINK_OPTIMIZATION=$($LinkOptimization.IsPresent)",'-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded','-DCMAKE_C_FLAGS_DEBUG=/Ob0 /Od /RTC1 /Z7','-DCMAKE_CXX_FLAGS_DEBUG=/Ob0 /Od /RTC1 /Z7')+$metricArgs)
}
$testTarget=if($TestMode -eq 'Iteration'){'fajk_decode_robustness_tests'}else{'fajk_tests'}
if (!$SkipTests -and $Target.Count -and $Target -notcontains $testTarget) { $Target += $testTarget }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_decode_robustness_tests') { $Target += 'fajk_decode_robustness_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_r441_tests') { $Target += 'fajk_r441_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_debug_tests') { $Target += 'fajk_debug_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_temporal_tests') { $Target += 'fajk_temporal_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_playback_tests') { $Target += 'fajk_playback_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_coefficient_temporal_tests') { $Target += 'fajk_coefficient_temporal_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_motion_temporal_tests') { $Target += 'fajk_motion_temporal_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_postfilter_tests') { $Target += 'fajk_postfilter_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_gpu_tests') { $Target += 'fajk_gpu_tests' }
if (!$SkipTests -and $TestMode -eq 'Full' -and $Target.Count -and $Target -notcontains 'fajk_sdk_tests') { $Target += 'fajk_sdk_tests' }
if ($Target -contains 'FajkStudio' -and $Target -notcontains 'fajk_cli') { $Target += 'fajk_cli' }
if ($Target -contains 'FajkStudio' -and $Target -notcontains 'fajk_decode') { $Target += 'fajk_decode' }
# A restored experiment can have an older timestamp than its compiled object.
# Hash the small source tree so Ninja always sees content changes, too.
$sourceManifest=Join-Path "$PSScriptRoot\$out" 'source-hashes.json'
$previousHashes=@{}
if(Test-Path -LiteralPath $sourceManifest) {
  $savedHashes=Get-Content -LiteralPath $sourceManifest -Raw | ConvertFrom-Json
  foreach($property in $savedHashes.PSObject.Properties) { $previousHashes[$property.Name]=$property.Value }
}
$currentHashes=[ordered]@{}
$sourceFiles=@(Get-ChildItem -LiteralPath "$PSScriptRoot\src","$PSScriptRoot\include","$PSScriptRoot\tests" -Recurse -File | Where-Object { $_.Extension -in @('.cpp','.c','.h','.hpp','.inl','.rc','.xaml') })+@(Get-Item -LiteralPath "$PSScriptRoot\CMakeLists.txt")
$changedSources=0
foreach($sourceFile in $sourceFiles) {
  $sourceKey=$sourceFile.FullName.Substring($PSScriptRoot.Length+1)
  $sourceHash=Get-SourceSha256 $sourceFile.FullName
  $currentHashes[$sourceKey]=$sourceHash
  if($previousHashes[$sourceKey] -ne $sourceHash) { $sourceFile.LastWriteTimeUtc=[DateTime]::UtcNow; ++$changedSources }
}
Write-Host "Source content check: $changedSources changed files."
if ($Sanitize) {
  $taskAsanRuntime=Join-Path $msvc.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
  if (!(Test-Path -LiteralPath $taskAsanRuntime)) { throw 'AddressSanitizer runtime is missing.' }
  Copy-Item -LiteralPath $taskAsanRuntime -Destination "$PSScriptRoot\$out" -Force
}
$buildArgs=@('--build',"$PSScriptRoot\$out",'--parallel','8')
if($Target.Count){$buildArgs+=@('--target')+$Target}
Invoke-CMake $buildArgs
$currentHashes | ConvertTo-Json | Set-Content -LiteralPath $sourceManifest -Encoding UTF8
if ($Sanitize) {
  $asanRuntime = Join-Path $msvc.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
  if (!(Test-Path -LiteralPath $asanRuntime)) { throw 'AddressSanitizer runtime is missing from the selected MSVC installation.' }
  Copy-Item -LiteralPath $asanRuntime -Destination "$PSScriptRoot\$out" -Force
}
# Keep test transactions in the workspace; redirected sandbox TEMP can reject
# the atomic rename used by container integration tests.
$taskTestTemp=$taskBuildTemp
New-Item -ItemType Directory -Force -Path $taskTestTemp | Out-Null
$previousTestTemp=$env:TEMP; $previousTestTmp=$env:TMP
$env:TEMP=$taskTestTemp; $env:TMP=$taskTestTemp
try {
  if ($SkipTests) { Write-Host 'Tests skipped by this build invocation.'; $testExit=0 } elseif ($TestMode -eq 'Iteration') {
    & "$PSScriptRoot\$out\fajk_decode_robustness_tests.exe"
    $testExit=$LASTEXITCODE
  } else {
    & ctest --test-dir "$PSScriptRoot\$out" --output-on-failure
    $testExit=$LASTEXITCODE
  }
}
finally { $env:TEMP=$previousTestTemp; $env:TMP=$previousTestTmp }
if ($testExit) { throw 'Tests failed.' }

if (!$NoPublish -and !$Sanitize -and $Configuration -eq 'Release' -and (!$Target.Count -or $Target -contains 'FajkStudio')) {
  if (!$SkipTests -and $TestMode -ne 'Full') { throw 'Publishing requires full tests or explicit -SkipTests.' }
  & "$PSScriptRoot\publish-release.ps1" -BuildDirectory "$PSScriptRoot\$out" -PackageDirectory $PackageDirectory -SkipStartupCheck:$SkipTests
}
if (!$NoPublish -and !$Sanitize -and $Configuration -eq 'Release' -and (!$Target.Count -or $Target -contains 'fajk_decode')) {
  New-Item -ItemType Directory -Force -Path "$PSScriptRoot\SDK" | Out-Null
  Copy-Item -LiteralPath "$PSScriptRoot\$out\fajk_decode.dll", "$PSScriptRoot\$out\fajk_decode.lib" -Destination "$PSScriptRoot\SDK" -Force
  Copy-Item -LiteralPath "$PSScriptRoot\include\fajk\decode.h" -Destination "$PSScriptRoot\SDK\decode.h" -Force
}
} finally { $env:TEMP=$previousBuildTemp; $env:TMP=$previousBuildTmp }
