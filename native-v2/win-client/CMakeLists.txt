cmake_minimum_required(VERSION 3.20)
project(P2PNativeWinClient LANGUAGES CXX)

message(STATUS "Building native VideoToolbox/Media Foundation v2 client")

function(configure_win_client_target target_name)
  target_compile_features(${target_name} PRIVATE cxx_std_20)
  target_compile_definitions(${target_name} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_include_directories(${target_name} PRIVATE ../shared)
  if (MSVC)
    target_compile_options(${target_name} PRIVATE /O2 /Ob3 /Ot /fp:fast /utf-8 /EHsc)
    target_link_options(${target_name} PRIVATE /OPT:REF /OPT:ICF)
  endif()
endfunction()

add_executable(p2p-native-win-client WIN32
  src/app_state.cpp
  src/input.cpp
  src/main.cpp
  src/overlay.cpp
  src/renderer.cpp
  src/video_pipeline.cpp
  src/window.cpp
)
configure_win_client_target(p2p-native-win-client)


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
  dwmapi
  imm32
  avrt
)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET gstreamer-1.0 gstreamer-video-1.0)
endif()

if (GSTREAMER_FOUND)
  message(STATUS "Building optional GStreamer RTP/H.264 client")
  add_executable(p2p-native-win-client-gst WIN32
    src/gst_receiver.cpp
  )
  configure_win_client_target(p2p-native-win-client-gst)
  target_link_libraries(p2p-native-win-client-gst PRIVATE
    PkgConfig::GSTREAMER
    ws2_32
    shell32
    user32
    gdi32
    winmm
    imm32
  )
else()
  message(STATUS "GStreamer development files not found; skipping optional p2p-native-win-client-gst")
endif()
