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
