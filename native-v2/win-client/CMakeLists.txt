cmake_minimum_required(VERSION 3.20)
project(P2PNativeWinClient LANGUAGES CXX)

add_executable(p2p-native-win-client WIN32
  src/main.cpp
)

target_compile_features(p2p-native-win-client PRIVATE cxx_std_20)
target_compile_definitions(p2p-native-win-client PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_include_directories(p2p-native-win-client PRIVATE ../shared)
if (MSVC)
  target_compile_options(p2p-native-win-client PRIVATE /O2 /Ob3 /Ot /fp:fast /EHsc)
  target_link_options(p2p-native-win-client PRIVATE /OPT:REF /OPT:ICF)
endif()

target_link_libraries(p2p-native-win-client PRIVATE
  ws2_32
  mfplat
  mf
  mfuuid
  wmcodecdspuuid
  d3d11
  dxgi
  d3dcompiler
  ole32
  oleaut32
  shell32
  user32
  gdi32
  winmm
)
