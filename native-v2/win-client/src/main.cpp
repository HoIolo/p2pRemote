#include "app_state.h"
#include "input.h"
#include "overlay.h"
#include "renderer.h"
#include "video_pipeline.h"
#include "window.h"

#include <mmsystem.h>

#include <algorithm>
#include <memory>
#include <thread>

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  g_cfg = ParseArgs();
  InitVideoProfileUiState();
  QueryPerformanceFrequency(&g_qpcFreq);
  g_startedQpc = QpcNow();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  timeBeginPeriod(1);
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
  g_localIp = DetectLocalIpForHost(g_cfg.hostIp, g_cfg.videoPort);
  if (!InitInputSocket()) {
    MessageBoxW(nullptr, L"Bad --host-ip or input socket init failed", L"P2P Native", MB_ICONERROR);
    return 1;
  }
  const int initialBitrate = std::max(2'000'000, g_cfg.bitrate > 0 ? g_cfg.bitrate : 14'000'000);
  CommitActiveVideoProfile(VideoProfile{g_cfg.width, g_cfg.height, g_cfg.fps, initialBitrate});

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"P2PNativeWinClient";
  RegisterClassW(&wc);

  std::wstring title = L"P2P Native v2 Client -> " + g_cfg.hostIp;
  DWORD style = g_cfg.fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 760;
  if (g_cfg.fullscreen) {
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
  }
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style,
                           x, y, w, h, nullptr, nullptr, hInst, nullptr);
  CreateOverlayWindows(hInst);
  g_renderer = std::make_unique<D3DRenderer>();
  const VideoProfile initialProfile = ActiveVideoProfile();
  if (!g_renderer->Init(g_hwnd, initialProfile.width, initialProfile.height)) {
    g_renderer.reset();
    MessageBoxW(g_hwnd, L"D3D11 flip-model renderer failed; falling back to GDI.", L"P2P Native", MB_ICONWARNING);
  }

  std::thread rx = g_cfg.udpVideo
    ? std::thread(RunUdpVideoReceiver, g_cfg.videoPort)
    : std::thread(RunTcpVideoReceiver, g_cfg.hostIp, g_cfg.videoPort);
  std::thread dec(DecoderThread);
  std::thread ren(RenderThread);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g_running.store(false);
  g_encodedCv.notify_all();
  g_decodedCv.notify_all();
  if (rx.joinable()) rx.detach(); // recvfrom may block; process is exiting
  if (dec.joinable()) dec.join();
  if (ren.joinable()) ren.join();
  g_renderer.reset();
  if (g_inputSock != INVALID_SOCKET) closesocket(g_inputSock);
  WSACleanup();
  timeEndPeriod(1);
  return static_cast<int>(msg.wParam);
}
