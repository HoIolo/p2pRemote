cmake_minimum_required(VERSION 3.20)
project(P2PNativeWinClient LANGUAGES CXX)

option(P2P_ALLOW_LEGACY_VIDEO_FALLBACK "Allow building the old non-GStreamer video client when GStreamer is missing" OFF)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-video-1.0
  )
endif()

if (GSTREAMER_FOUND)
  set(P2P_WIN_CLIENT_SOURCE src/gst_receiver.cpp)
  set(P2P_WIN_CLIENT_GST 1)
  message(STATUS "Building GStreamer native-v2 Windows client")
else()
  if (NOT P2P_ALLOW_LEGACY_VIDEO_FALLBACK)
    message(FATAL_ERROR
      "GStreamer development files not found. Install the MSVC x86_64 runtime + development MSI, "
      "or set GSTREAMER_1_0_ROOT_MSVC_X86_64 to C:/gstreamer/1.0/msvc_x86_64. "
      "Set -DP2P_ALLOW_LEGACY_VIDEO_FALLBACK=ON only if you intentionally want the old client.")
  endif()
  set(P2P_WIN_CLIENT_SOURCE src/main.cpp)
  set(P2P_WIN_CLIENT_GST 0)
  message(WARNING "GStreamer development files not found; falling back to legacy native-v2 Windows client")
endif()

add_executable(p2p-native-win-client WIN32
  ${P2P_WIN_CLIENT_SOURCE}
)

target_compile_features(p2p-native-win-client PRIVATE cxx_std_20)
target_compile_definitions(p2p-native-win-client PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_include_directories(p2p-native-win-client PRIVATE ../shared)
if (P2P_WIN_CLIENT_GST)
  target_link_libraries(p2p-native-win-client PRIVATE PkgConfig::GSTREAMER)
endif()
if (MSVC)
  target_compile_options(p2p-native-win-client PRIVATE /O2 /Ob3 /Ot /fp:fast /EHsc)
  target_link_options(p2p-native-win-client PRIVATE /OPT:REF /OPT:ICF)
endif()

if (P2P_WIN_CLIENT_GST)
  target_link_libraries(p2p-native-win-client PRIVATE
    ws2_32
    shell32
    user32
    gdi32
    winmm
  )
else()
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
endif()
