$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
New-Item -ItemType Directory -Force -Path ..\dist\win-client | Out-Null
Copy-Item .\build\Release\p2p-native-win-client.exe ..\dist\win-client\ -Force
Write-Host "built: native-v2\dist\win-client\p2p-native-win-client.exe"
