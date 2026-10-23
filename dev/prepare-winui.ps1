$ErrorActionPreference='Stop'
$package=Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windowsappsdk\1.6.250228001'
$webview=Join-Path $env:USERPROFILE '.nuget\packages\microsoft.web.webview2\1.0.2651.64\lib\Microsoft.Web.WebView2.Core.winmd'
if (!(Test-Path -LiteralPath $package)) { throw 'Windows App SDK 1.6.250228001 NuGet package is required in the local NuGet cache.' }
$generated=Join-Path $PSScriptRoot 'build-winui\winrt-generated'
& cppwinrt -input "$package\lib\uap10.0" -input "$package\lib\uap10.0.18362" -input $webview -input "$sdkRoot\UnionMetadata\$($sdk.Name)\Windows.winmd" -output $generated
if($LASTEXITCODE){throw 'WinRT projection generation failed.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$app=Join-Path $PSScriptRoot 'runtime'
if (!(Test-Path -LiteralPath "$app\Microsoft.UI.Xaml.dll")) { [System.IO.Compression.ZipFile]::ExtractToDirectory("$package\tools\MSIX\win10-x64\Microsoft.WindowsAppRuntime.1.6.msix", $app) }
[xml]$manifest=Get-Content -LiteralPath "$PSScriptRoot\src\app.manifest"
foreach($path in Get-ChildItem -LiteralPath "$package\manifests" -Recurse -Filter '*.manifest') {
  [xml]$part=Get-Content -LiteralPath $path.FullName
  foreach($file in $part.assembly.ChildNodes) { if($file.LocalName -eq 'file' -and $file.GetAttribute('name')) { $manifest.assembly.AppendChild($manifest.ImportNode($file,$true)) | Out-Null } }
}
$manifest.Save("$PSScriptRoot\build-winui\winui.manifest")
Copy-Item -LiteralPath "$PSScriptRoot\src\ui\studio.xaml" -Destination "$app\studio.xaml" -Force
Copy-Item -LiteralPath "$package\license.txt" -Destination "$app\WindowsAppSDK-LICENSE.txt" -Force
Copy-Item -LiteralPath (Join-Path $env:USERPROFILE '.nuget\packages\microsoft.web.webview2\1.0.2651.64\runtimes\win-x64\native_uap\Microsoft.Web.WebView2.Core.dll') -Destination $app -Force
