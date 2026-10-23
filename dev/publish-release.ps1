param([Parameter(Mandatory=$true)][string]$BuildDirectory, [string]$PackageDirectory="", [switch]$SkipStartupCheck)
$ErrorActionPreference='Stop'
$workspaceRoot=(Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path.TrimEnd('\')
$packageRoot=if($PackageDirectory){[IO.Path]::GetFullPath($PackageDirectory)}else{Join-Path $workspaceRoot 'release\R500'}
$releaseRoot=Join-Path $packageRoot 'app'
$stageRoot=Join-Path $PSScriptRoot ('.release-stage-'+[guid]::NewGuid().ToString('N'))
$backupRoot=Join-Path $PSScriptRoot ('.release-backup-'+[guid]::NewGuid().ToString('N'))
$runtimeRoot=Join-Path $PSScriptRoot 'runtime'
function File-Sha256([string]$path) {
  $stream=[IO.File]::OpenRead($path)
  $hash=[Security.Cryptography.SHA256]::Create()
  try { return [BitConverter]::ToString($hash.ComputeHash($stream)) }
  finally { $hash.Dispose(); $stream.Dispose() }
}
function Assert-WorkspaceTree([string]$path) {
  $absolute=[IO.Path]::GetFullPath($path)
  if (!$absolute.StartsWith($workspaceRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Release path is outside the workspace.' }
  if (Test-Path -LiteralPath $absolute) {
    $item=Get-Item -LiteralPath $absolute
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Release paths cannot be links.' }
    if ($item.PSIsContainer -and (Get-ChildItem -LiteralPath $absolute -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })) { throw 'Release tree contains a link.' }
  }
}
foreach($path in @($releaseRoot,$stageRoot,$backupRoot)) { Assert-WorkspaceTree $path }
$running=Get-Process -Name FajkStudio,fajk_cli -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($releaseRoot+'\',[StringComparison]::OrdinalIgnoreCase) }
if ($running) { throw 'Close Fajk Studio and its release CLI before publishing. The existing release has not been replaced.' }
foreach($name in @('FajkStudio.exe','fajk_cli.exe','fajk_decode.dll','fajk_decode.lib')) {
  if (!(Test-Path -LiteralPath (Join-Path $BuildDirectory $name))) { throw "Missing release executable: $name. Build all targets before publishing." }
}
$cache=Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
if ($cache -notcontains 'CMAKE_BUILD_TYPE:STRING=Release' -or $cache -match '^FAJK_SANITIZE:BOOL=(ON|TRUE|1)$') { throw 'Only an ordinary Release build can be published.' }
New-Item -ItemType Directory -Force -Path $packageRoot,$stageRoot | Out-Null
# Carry user preferences into the new release folder once, never overwrite edits.
$priorSettings=Join-Path $workspaceRoot 'release\R480\settings.ini'
if($packageRoot -eq (Join-Path $workspaceRoot 'release\R500')){$priorSettings=Join-Path $workspaceRoot 'release\R490\settings.ini'}
if(!(Test-Path -LiteralPath $priorSettings)){$priorSettings=Join-Path $workspaceRoot 'release\R470\settings.ini'}
if(!(Test-Path -LiteralPath $priorSettings)){$priorSettings=Join-Path $workspaceRoot 'release\R465\settings.ini'}
$newSettings=Join-Path $packageRoot 'settings.ini'
if($packageRoot -in @((Join-Path $workspaceRoot 'release\R490'),(Join-Path $workspaceRoot 'release\R500')) -and !(Test-Path -LiteralPath $newSettings) -and (Test-Path -LiteralPath $priorSettings)){Copy-Item -LiteralPath $priorSettings -Destination $newSettings}
if($packageRoot -eq (Join-Path $workspaceRoot 'release\R500') -and (Test-Path -LiteralPath $newSettings)){
  $r500Settings=Get-Content -LiteralPath $newSettings -Raw
  if($r500Settings -notmatch '(?m)^defaultsVersion=500\s*$'){
    foreach($key in @('adaptiveQuantizationEnabled','r441TextProtection')){
      if($r500Settings -match "(?m)^$key=.*$"){$r500Settings=$r500Settings -replace "(?m)^$key=.*$","$key=0"}
      else{$r500Settings+="`r`n$key=0"}
    }
    $r500Settings+="`r`ndefaultsVersion=500`r`n"
    Set-Content -LiteralPath $newSettings -Value $r500Settings -Encoding Unicode
  }
}

try {
foreach($file in Get-ChildItem -LiteralPath $runtimeRoot -File -Recurse) {
  $relative=$file.FullName.Substring($runtimeRoot.Length+1)
  if ($file.Extension -notin @('.dll','.pri','.winmd','.mui') -and !$relative.StartsWith('Microsoft.UI.Xaml\Assets\')) { continue }
  $destination=Join-Path $stageRoot $relative
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
  Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}
foreach($name in @('FajkStudio.exe','fajk_cli.exe')) {
  $source=Join-Path $BuildDirectory $name
  $destination=Join-Path $stageRoot $name
  Copy-Item -LiteralPath $source -Destination $destination
  if ((File-Sha256 $source) -ne (File-Sha256 $destination)) { throw "Release executable verification failed: $name" }
}
Copy-Item -LiteralPath "$PSScriptRoot\src\ui\studio.xaml" -Destination $stageRoot
New-Item -ItemType Directory -Force -Path (Join-Path $stageRoot 'SDK') | Out-Null
foreach($name in @('fajk_decode.dll','fajk_decode.lib')) {
  $sdkSource=Join-Path $BuildDirectory $name
  $sdkDestination=Join-Path (Join-Path $stageRoot 'SDK') $name
  Copy-Item -LiteralPath $sdkSource -Destination $sdkDestination
  if ((File-Sha256 $sdkSource) -ne (File-Sha256 $sdkDestination)) { throw "SDK verification failed: $name" }
}
Copy-Item -LiteralPath "$PSScriptRoot\include\fajk\decode.h","$PSScriptRoot\SDK\README.md" -Destination (Join-Path $stageRoot 'SDK')
Copy-Item -LiteralPath "$runtimeRoot\WindowsAppSDK-LICENSE.txt" -Destination $stageRoot
if (!$SkipStartupCheck) {
& "$stageRoot\fajk_cli.exe" hardware | Out-Null
if ($LASTEXITCODE) { throw 'Prepared release could not start its CLI.' }
}
Copy-Item -LiteralPath "$PSScriptRoot\release-files\Spustit Fajk Studio.cmd","$PSScriptRoot\release-files\Fajk CLI.cmd","$PSScriptRoot\release-files\README.txt" -Destination $packageRoot -Force
# Migrate older layouts once. Current settings live outside the replaced app folder.
$settings=Join-Path $packageRoot 'settings.ini'
$oldSettings=Join-Path $releaseRoot 'settings.ini'
if ((Test-Path -LiteralPath $oldSettings) -and (!(Test-Path -LiteralPath $settings) -or (Get-Item -LiteralPath $oldSettings).LastWriteTimeUtc -gt (Get-Item -LiteralPath $settings).LastWriteTimeUtc)) { Copy-Item -LiteralPath $oldSettings -Destination $settings -Force }
if (Test-Path -LiteralPath $releaseRoot) { Move-Item -LiteralPath $releaseRoot -Destination $backupRoot }
try { Move-Item -LiteralPath $stageRoot -Destination $releaseRoot }
catch {
  if (!(Test-Path -LiteralPath $releaseRoot) -and (Test-Path -LiteralPath $backupRoot)) { Move-Item -LiteralPath $backupRoot -Destination $releaseRoot }
  throw
}
if (Test-Path -LiteralPath $backupRoot) { Assert-WorkspaceTree $backupRoot; Remove-Item -LiteralPath $backupRoot -Recurse -Force }
}
finally {
  if (Test-Path -LiteralPath $stageRoot) { Assert-WorkspaceTree $stageRoot; Remove-Item -LiteralPath $stageRoot -Recurse -Force }
}
if (Test-Path -LiteralPath "$BuildDirectory\FajkStudio.pdb") {
  New-Item -ItemType Directory -Force -Path "$PSScriptRoot\symbols" | Out-Null
  Copy-Item -LiteralPath "$BuildDirectory\FajkStudio.pdb" -Destination "$PSScriptRoot\symbols" -Force
}
Write-Host "Development candidate prepared: $releaseRoot"
