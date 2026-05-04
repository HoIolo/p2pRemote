$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$gstRoots = @(
  @(
    $env:GSTREAMER_1_0_ROOT_MSVC_X86_64,
    $env:GSTREAMER_1_0_ROOT_MINGW_X86_64,
    "$env:USERPROFILE\gstreamer-sdk\1.0\msvc_x86_64",
    'C:\gstreamer\1.0\msvc_x86_64',
    'C:\gstreamer\1.0\mingw_x86_64'
  ) | Where-Object { $_ -and (Test-Path $_) }
)

if ($gstRoots.Count -gt 0) {
  $gstRoot = $gstRoots[0]
  $pkgConfig = Join-Path $gstRoot 'bin\pkg-config.exe'
  if (Test-Path $pkgConfig) {
    $env:PKG_CONFIG = $pkgConfig
    $env:PKG_CONFIG_PATH = Join-Path $gstRoot 'lib\pkgconfig'
  }
  $env:Path = "$(Join-Path $gstRoot 'bin');$env:Path"
  Write-Host "using GStreamer: $gstRoot"
} else {
  Write-Error "GStreamer SDK not found; install runtime + development SDK or set GSTREAMER_1_0_ROOT_MSVC_X86_64."
}

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
