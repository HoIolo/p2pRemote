param(
  [Parameter(Mandatory=$true)][string]$HostIp,
  [string]$HostName = 'Remote Device',
  [ValidateSet('darwin','win32','linux','unknown')][string]$HostPlatform = 'unknown',
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Fps = 60,
  [int]$Bitrate = 18000000,
  [int]$VideoPort = 45000,
  [int]$InputPort = 45001,
  [ValidateSet('tcp','udp')][string]$Transport = 'udp',
  [switch]$NoFullscreen
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$exe = '.\build\Release\p2p-native-win-client.exe'
if (!(Test-Path $exe)) {
  .\build.ps1
}
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
  $env:Path = "$(Join-Path $gstRoots[0] 'bin');$env:Path"
} else {
  throw "GStreamer runtime not found; set GSTREAMER_1_0_ROOT_MSVC_X86_64 first."
}
$args = @(
  '--host-ip', $HostIp,
  '--host-name', $HostName,
  '--host-platform', $HostPlatform,
  '--video-port', $VideoPort,
  '--input-port', $InputPort,
  '--width', $Width,
  '--height', $Height,
  '--fps', $Fps,
  '--bitrate', $Bitrate,
  '--transport', $Transport
)
if (!$NoFullscreen) { $args += '--fullscreen' }
& $exe @args
