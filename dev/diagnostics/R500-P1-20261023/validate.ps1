$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path '.').Path
$diag=Join-Path $taskRoot 'dev/diagnostics/R500-P1-20261023'
$old=Join-Path $taskRoot 'dev/diagnostics/Temporal-20261023'
$python='C:/Users/fajkp/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$env:TEMP=Join-Path $taskRoot 'dev/test-tmp';$env:TMP=$env:TEMP
& './dev/build.ps1' -Quick -Sanitize -Target @('fajk_cli','fajk_temporal_tests') -NoPublish *> (Join-Path $diag 'asan.log')
if($LASTEXITCODE){throw 'ASAN build/tests failed.'}
Write-Output 'PASS 8/8 ASAN, including native DLL calls'
foreach($build in @('build-r4','build-asan')){
    & "./dev/$build/fajk_temporal_tests.exe" (Join-Path $old 'exact-screen.avi') *> (Join-Path $diag "pipeline-$build.log")
    if($LASTEXITCODE){throw "Native pipeline failed: $build"}
}
foreach($test in @(@{input='r500-native/local-gpu-temporal.fajkv';output='sdk-exact'},@{input='r500-production/screen-gpu-temporal.fajkv';output='sdk-multiple-keys'},@{input='vector-check/local-t3.fajkv';output='sdk-corrections'})){
    & $python dev/tools/verify_temporal_sdk.py --library dev/build-r4/fajk_decode.dll --cli dev/build-r4/fajk_cli.exe --input (Join-Path $old $test.input) --output (Join-Path $diag $test.output) *> (Join-Path $diag ($test.output+'.log'))
    if($LASTEXITCODE){throw "SDK failed: $($test.output)"}
    Write-Output "PASS $($test.output)"
}
& $python dev/tools/verify_padded_decode.py --before (Join-Path $old 'final-checkpoint/fajk_decode.dll') --after dev/build-r4/fajk_decode.dll --inputs (Join-Path $old 'r500-similar/local-t3.fajkv') (Join-Path $old 'r500-native/motion-gpu-intra.fajkv') --output (Join-Path $diag 'padded') *> (Join-Path $diag 'padded.log')
if($LASTEXITCODE){throw 'Paired padded SDK benchmark failed.'}
Write-Output 'PASS 16 paired SDK benchmark cases, exact pixels and guards'
& $python dev/tools/verify_r500_cli.py --cli dev/build-r4/fajk_cli.exe --input dev/samples/references/22950-reference.png --output (Join-Path $diag 'cli') *> (Join-Path $diag 'cli.log')
if($LASTEXITCODE){throw 'CLI checks failed.'}
$ui=Join-Path $diag 'ui'
& './dev/publish-release.ps1' -BuildDirectory (Join-Path $taskRoot 'dev/build-r4') -PackageDirectory $ui
Copy-Item -LiteralPath (Join-Path $taskRoot 'dev/samples/references/22950-reference.png') -Destination (Join-Path $ui 'smoke.png')
$app=Start-Process -FilePath (Join-Path $ui 'app/FajkStudio.exe') -WorkingDirectory (Join-Path $ui 'app') -ArgumentList @('--smoke-fast-gpu',('"{0}"' -f (Join-Path $ui 'smoke.png'))) -WindowStyle Hidden -PassThru
if(!$app.WaitForExit(60000)){throw "Owned UI process $($app.Id) did not close."}
if($app.ExitCode){throw "UI exit $($app.ExitCode)."}
$log=Get-Content -LiteralPath (Join-Path $ui 'app/ui-smoke.txt');$passes=@($log|Where-Object{$_ -match 'PASS'}).Count
if($passes -lt 46 -or $log -cmatch '\bERROR\b' -or !($log -match 'live sharpening change same packet PASS') -or !($log -match 'similar-region tolerance / hold persistence PASS')){throw 'UI smoke failed.'}
@{exit_code=$app.ExitCode;pass_records=$passes;unified_backend=($log -match 'unified CPU/GPU.*PASS').Count -gt 0} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $diag 'ui-results.json') -Encoding UTF8
Write-Output "PASS UI $passes records"
