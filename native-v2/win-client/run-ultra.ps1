param(
  [Parameter(Mandatory=$true)][string]$HostIp,
  [string]$HostName = 'Remote Device',
  [ValidateSet('darwin','win32','linux','unknown')][string]$HostPlatform = 'unknown',
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Fps = 60,
  [int]$Bitrate = 30000000,
  [int]$VideoPort = 45000,
  [int]$InputPort = 45001,
  [ValidateSet('tcp','udp','gst')][string]$Transport = 'udp',
  [switch]$NoFullscreen
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$exe = if ($Transport -eq 'gst') { '.\build\Release\p2p-native-win-client-gst.exe' } else { '.\build\Release\p2p-native-win-client.exe' }
if (!(Test-Path $exe)) {
  .\build.ps1
}
if ($Transport -eq 'gst' -and !(Test-Path $exe)) {
  throw "GStreamer client was not built. Install GStreamer MSVC x86_64 development files and set GSTREAMER_1_0_ROOT_MSVC_X86_64, or use -Transport udp."
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
