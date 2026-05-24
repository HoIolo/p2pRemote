$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$cmakeArgs = @('-S', '.', '-B', 'build', '-A', 'x64', '-DCMAKE_BUILD_TYPE=Release')
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
  throw "cmake configure failed with exit code $LASTEXITCODE"
}

& cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
  throw "cmake build failed with exit code $LASTEXITCODE"
}

$outputExe = Join-Path $PSScriptRoot 'build\\Release\\p2p-native-win-client.exe'
if (!(Test-Path $outputExe)) {
  throw "expected native-v2 output missing: $outputExe"
}

New-Item -ItemType Directory -Force -Path ..\dist\win-client | Out-Null
Copy-Item $outputExe ..\dist\win-client\ -Force
Write-Host "built: native-v2\dist\win-client\p2p-native-win-client.exe"
$gstOutputExe = Join-Path $PSScriptRoot 'build\Release\p2p-native-win-client-gst.exe'
if (Test-Path $gstOutputExe) {
  Copy-Item $gstOutputExe ..\dist\win-client\ -Force
  Write-Host "built: native-v2\dist\win-client\p2p-native-win-client-gst.exe"
} else {
  Write-Host "skipped: optional GStreamer client was not built (GStreamer dev files not found)"
}
