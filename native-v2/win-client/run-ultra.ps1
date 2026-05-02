param(
  [Parameter(Mandatory=$true)][string]$HostIp,
  [int]$Width = 1920,
  [int]$Height = 1080,
  [int]$Fps = 120,
  [int]$VideoPort = 45000,
  [int]$InputPort = 45001,
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
  '--video-port', $VideoPort,
  '--input-port', $InputPort,
  '--width', $Width,
  '--height', $Height,
  '--fps', $Fps
)
if (!$NoFullscreen) { $args += '--fullscreen' }
& $exe @args
