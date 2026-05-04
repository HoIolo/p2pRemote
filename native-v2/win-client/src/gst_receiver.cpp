#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>

#include "p2_protocol.h"

struct Config {
  std::wstring hostIp = L"127.0.0.1";
  std::wstring hostName = L"Remote Device";
  std::wstring hostPlatform = L"unknown";
  uint16_t videoPort = 45000;
  uint16_t inputPort = 45001;
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrate = 0;
  bool fullscreen = false;
  std::wstring profileFile;
};

static Config g_cfg;
static HWND g_hwnd = nullptr;
static GstElement* g_pipeline = nullptr;
static SOCKET g_inputSock = INVALID_SOCKET;
static sockaddr_in g_inputAddr{};
static std::atomic<uint32_t> g_inputSeq{1};
static std::atomic<bool> g_running{true};

static std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
}

static Config ParseArgs() {
  Config c;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    auto next = [&]() -> LPWSTR { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (wcscmp(argv[i], L"--host-ip") == 0) { if (auto v = next()) c.hostIp = v; }
    else if (wcscmp(argv[i], L"--host-name") == 0) { if (auto v = next()) c.hostName = v; }
    else if (wcscmp(argv[i], L"--host-platform") == 0) { if (auto v = next()) c.hostPlatform = v; }
    else if (wcscmp(argv[i], L"--video-port") == 0) { if (auto v = next()) c.videoPort = static_cast<uint16_t>(_wtoi(v)); }
    else if (wcscmp(argv[i], L"--input-port") == 0) { if (auto v = next()) c.inputPort = static_cast<uint16_t>(_wtoi(v)); }
    else if (wcscmp(argv[i], L"--width") == 0) { if (auto v = next()) c.width = _wtoi(v); }
    else if (wcscmp(argv[i], L"--height") == 0) { if (auto v = next()) c.height = _wtoi(v); }
    else if (wcscmp(argv[i], L"--fps") == 0) { if (auto v = next()) c.fps = std::max(1, _wtoi(v)); }
    else if (wcscmp(argv[i], L"--bitrate") == 0) { if (auto v = next()) c.bitrate = _wtoi(v); }
    else if (wcscmp(argv[i], L"--profile-file") == 0) { if (auto v = next()) c.profileFile = v; }
    else if (wcscmp(argv[i], L"--fullscreen") == 0) { c.fullscreen = true; }
    else if (wcscmp(argv[i], L"--transport") == 0) { (void)next(); }
    else if (wcscmp(argv[i], L"--udp-video") == 0) { }
  }
  if (argv) LocalFree(argv);
  return c;
}

static uint16_t WinToMacKey(WPARAM vk, LPARAM lp) {
  switch (vk) {
    case VK_BACK: return 51;
    case VK_TAB: return 48;
    case VK_RETURN: return 36;
    case VK_ESCAPE: return 53;
    case VK_SPACE: return 49;
    case VK_DELETE: return 117;
    case VK_LEFT: return 123;
    case VK_RIGHT: return 124;
    case VK_DOWN: return 125;
    case VK_UP: return 126;
    case VK_HOME: return 115;
    case VK_END: return 119;
    case VK_PRIOR: return 116;
    case VK_NEXT: return 121;
    case VK_SHIFT: return (MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? 60 : 56;
    case VK_CONTROL: return (lp & 0x01000000) ? 62 : 59;
    case VK_MENU: return (lp & 0x01000000) ? 61 : 58;
    case VK_LWIN: case VK_RWIN: return 55;
    default: break;
  }
  static const uint16_t digits[] = {29,18,19,20,21,23,22,26,28,25};
  if (vk >= '0' && vk <= '9') return digits[vk - '0'];
  static const uint16_t letters[26] = {
    0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6
  };
  if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
  return 0xffff;
}

static bool InitInputSocket() {
  g_inputSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_inputSock == INVALID_SOCKET) return false;
  int sndbuf = 64 * 1024;
  setsockopt(g_inputSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
  g_inputAddr.sin_family = AF_INET;
  g_inputAddr.sin_port = htons(g_cfg.inputPort);
  const std::string ip = WideToUtf8(g_cfg.hostIp);
  return inet_pton(AF_INET, ip.c_str(), &g_inputAddr.sin_addr) == 1;
}

static void SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode) {
  if (g_inputSock == INVALID_SOCKET) return;
  P2InputPacket p{};
  std::memcpy(p.magic, "P2I2", sizeof(p.magic));
  p.version = 1;
  p.kind = kind;
  p.bytes = sizeof(P2InputPacket);
  p.seq = g_inputSeq.fetch_add(1, std::memory_order_relaxed);
  p.x = x; p.y = y; p.dx = dx; p.dy = dy; p.button = button; p.keyCode = keyCode;
  sendto(g_inputSock, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&g_inputAddr), sizeof(g_inputAddr));
}

static void NormalizedPoint(HWND hwnd, LPARAM lp, float& x, float& y) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int w = std::max(1L, rc.right - rc.left);
  const int h = std::max(1L, rc.bottom - rc.top);
  x = static_cast<float>(GET_X_LPARAM(lp)) / static_cast<float>(w);
  y = static_cast<float>(GET_Y_LPARAM(lp)) / static_cast<float>(h);
  x = std::max(0.0f, std::min(1.0f, x));
  y = std::max(0.0f, std::min(1.0f, y));
}

static std::string GstPipelineDescription(HWND hwnd) {
  char overlay[64];
  snprintf(overlay, sizeof(overlay), "%llu", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
  std::string desc;
  desc += "udpsrc port=" + std::to_string(g_cfg.videoPort);
  desc += " caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000\" ";
  desc += "! rtpjitterbuffer latency=10 drop-on-latency=true do-lost=true ";
  desc += "! rtph264depay ";
  desc += "! h264parse ";
  desc += "! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ";
  desc += "! decodebin ";
  desc += "! videoconvert ";
  desc += "! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ";
  desc += "! d3d11videosink name=videosink sync=false async=false force-aspect-ratio=true";
  return desc;
}

static bool StartGStreamer(HWND hwnd) {
  GError* error = nullptr;
  const std::string desc = GstPipelineDescription(hwnd);
  g_pipeline = gst_parse_launch(desc.c_str(), &error);
  if (!g_pipeline) {
    std::wstring msg = L"Failed to create GStreamer pipeline";
    if (error && error->message) {
      const int n = MultiByteToWideChar(CP_UTF8, 0, error->message, -1, nullptr, 0);
      std::wstring detail(n ? n - 1 : 0, L'\0');
      if (n) MultiByteToWideChar(CP_UTF8, 0, error->message, -1, detail.data(), n);
      msg += L":\n" + detail;
    }
    if (error) g_error_free(error);
    MessageBoxW(hwnd, msg.c_str(), L"P2P Native GStreamer", MB_ICONERROR);
    return false;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(g_pipeline), "videosink");
  if (sink && GST_IS_VIDEO_OVERLAY(sink)) {
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), reinterpret_cast<guintptr>(hwnd));
    gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), TRUE);
  }
  if (sink) gst_object_unref(sink);

  const GstStateChangeReturn ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    MessageBoxW(hwnd, L"Failed to start GStreamer pipeline", L"P2P Native GStreamer", MB_ICONERROR);
    return false;
  }
  return true;
}

static void StopGStreamer() {
  if (!g_pipeline) return;
  gst_element_set_state(g_pipeline, GST_STATE_NULL);
  gst_object_unref(g_pipeline);
  g_pipeline = nullptr;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      DragAcceptFiles(hwnd, FALSE);
      return 0;
    case WM_SIZE:
      if (g_pipeline) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(g_pipeline), "videosink");
        if (sink && GST_IS_VIDEO_OVERLAY(sink)) gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
        if (sink) gst_object_unref(sink);
      }
      return 0;
    case WM_MOUSEMOVE: {
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = (wp & MK_LBUTTON) ? 0 : ((wp & MK_RBUTTON) ? 2 : ((wp & MK_MBUTTON) ? 1 : 0));
      SendInputPacket(P2_INPUT_MOVE, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
      SetCapture(hwnd);
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONDOWN ? 2 : (msg == WM_MBUTTONDOWN ? 1 : 0);
      SendInputPacket(P2_INPUT_DOWN, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
      ReleaseCapture();
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONUP ? 2 : (msg == WM_MBUTTONUP ? 1 : 0);
      SendInputPacket(P2_INPUT_UP, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      LPARAM clientLp = MAKELPARAM(pt.x, pt.y);
      float x, y; NormalizedPoint(hwnd, clientLp, x, y);
      SendInputPacket(P2_INPUT_WHEEL, x, y, 0, -GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
      return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      uint16_t mac = WinToMacKey(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_DOWN, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      uint16_t mac = WinToMacKey(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_UP, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{}; GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      SetTextColor(hdc, RGB(220, 230, 255)); SetBkMode(hdc, TRANSPARENT);
      DrawTextW(hdc, L"Waiting for GStreamer RTP/H.264 video...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      g_running.store(false, std::memory_order_relaxed);
      StopGStreamer();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  g_cfg = ParseArgs();
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  timeBeginPeriod(1);

  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
  if (!InitInputSocket()) {
    MessageBoxW(nullptr, L"Bad --host-ip or input socket init failed", L"P2P Native GStreamer", MB_ICONERROR);
    return 1;
  }

  int gstArgc = 0;
  char** gstArgv = nullptr;
  gst_init(&gstArgc, &gstArgv);

  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"P2PNativeGstWinClient";
  RegisterClassW(&wc);

  DWORD style = g_cfg.fullscreen ? (WS_POPUP | WS_VISIBLE) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 760;
  if (g_cfg.fullscreen) {
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
  }
  std::wstring title = L"P2P Native v2 GStreamer -> " + g_cfg.hostIp;
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, x, y, w, h, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;
  ShowWindow(g_hwnd, nCmdShow);
  UpdateWindow(g_hwnd);
  StartGStreamer(g_hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  StopGStreamer();
  if (g_inputSock != INVALID_SOCKET) closesocket(g_inputSock);
  WSACleanup();
  timeEndPeriod(1);
  return static_cast<int>(msg.wParam);
}
