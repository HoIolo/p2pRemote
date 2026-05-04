#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <atomic>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

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
static HWND g_toolbarHwnd = nullptr;
static HWND g_menuHwnd = nullptr;
static HWND g_statsHwnd = nullptr;
static GstElement* g_pipeline = nullptr;
static SOCKET g_inputSock = INVALID_SOCKET;
static sockaddr_in g_inputAddr{};
static std::atomic<uint32_t> g_inputSeq{1};
static std::atomic<bool> g_running{true};
static LARGE_INTEGER g_qpcFreq{};
static uint64_t g_startedQpc = 0;
static std::atomic<uint64_t> g_framesPresented{0};
static std::atomic<uint64_t> g_lastBufferQpc{0};
static std::atomic<double> g_presentFps{0.0};
static DWORD g_prevWindowStyle = 0;
static DWORD g_prevWindowExStyle = 0;
static RECT g_prevWindowRect{};
static bool g_fullscreenActive = false;

struct VideoRect {
  int left = 0;
  int top = 0;
  int width = 1;
  int height = 1;
};

struct VideoProfile {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrate = 14'000'000;
};

struct ResolutionPreset {
  int width = 0;
  int height = 0;
};

static VideoProfile g_activeProfile;
static VideoProfile g_pendingProfile;
static std::vector<ResolutionPreset> g_resolutionPresets;
static int g_resolutionIndex = 0;
static int g_fpsIndex = 2;
static int g_bitrateIndex = 1;
static constexpr std::array<int, 5> kFpsPresets = {30, 45, 60, 90, 120};
static constexpr std::array<int, 6> kBitratePresetsMbps = {6, 10, 14, 20, 28, 40};
static constexpr int kToolbarWidth = 560;
static constexpr int kToolbarHeight = 60;
static constexpr int kMenuWidth = 440;
static constexpr int kMenuHeight = 486;
static constexpr int kStatsWidth = 560;
static constexpr int kStatsHeight = 430;

static uint64_t QpcNow() {
  LARGE_INTEGER q{};
  QueryPerformanceCounter(&q);
  return static_cast<uint64_t>(q.QuadPart);
}

static uint64_t QpcDeltaUs(uint64_t start, uint64_t end) {
  if (!g_qpcFreq.QuadPart || end <= start) return 0;
  return (end - start) * 1'000'000ull / static_cast<uint64_t>(g_qpcFreq.QuadPart);
}

static void SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode);
static void ApplyVideoRenderRectangle(HWND hwnd);
static bool StartGStreamer(HWND hwnd);

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

static int ClampEven(int value, int fallback = 2) {
  int number = std::max(2, static_cast<int>(std::lround(value > 0 ? value : fallback)));
  return (number % 2) == 0 ? number : number - 1;
}

static int AutoBitrateForPixels(int width, int height, int fallback) {
  const int64_t pixels = static_cast<int64_t>(width) * height;
  int bitrate = std::max(6'000'000, fallback);
  if (pixels <= 1280ll * 720ll) bitrate = std::max(bitrate, 6'000'000);
  else if (pixels <= 1600ll * 900ll) bitrate = std::max(bitrate, 10'000'000);
  else if (pixels <= 1920ll * 1080ll) bitrate = std::max(bitrate, 14'000'000);
  else if (pixels <= 2560ll * 1440ll) bitrate = std::max(bitrate, 22'000'000);
  else bitrate = std::max(bitrate, 32'000'000);
  return bitrate;
}

static void CommitActiveProfile(VideoProfile profile) {
  profile.width = std::max(640, ClampEven(profile.width, g_cfg.width));
  profile.height = std::max(360, ClampEven(profile.height, g_cfg.height));
  profile.fps = std::clamp(profile.fps, 30, 240);
  const int fallbackBitrate = AutoBitrateForPixels(profile.width, profile.height, 14'000'000);
  profile.bitrate = std::clamp(profile.bitrate > 0 ? profile.bitrate : fallbackBitrate, 2'000'000, 200'000'000);
  g_activeProfile = profile;
  g_cfg.width = profile.width;
  g_cfg.height = profile.height;
  g_cfg.fps = profile.fps;
  g_cfg.bitrate = profile.bitrate;
}

static std::wstring FormatResolution(int width, int height) {
  wchar_t buf[64];
  swprintf_s(buf, L"%dx%d", width, height);
  return buf;
}

static std::wstring FormatBitrate(int bitrate) {
  if (bitrate <= 0) return L"Auto";
  wchar_t buf[32];
  swprintf_s(buf, L"%d Mbps", std::max(1, (bitrate + 500'000) / 1'000'000));
  return buf;
}

static std::wstring FormatCompactProfile(const VideoProfile& profile) {
  return FormatResolution(profile.width, profile.height) + L" / "
       + std::to_wstring(profile.fps) + L" fps / "
       + FormatBitrate(profile.bitrate);
}

static bool SameProfile(const VideoProfile& a, const VideoProfile& b) {
  return a.width == b.width && a.height == b.height && a.fps == b.fps && a.bitrate == b.bitrate;
}

static int FindClosestFpsIndex(int fps) {
  int best = 0;
  int diff = INT_MAX;
  for (int i = 0; i < static_cast<int>(kFpsPresets.size()); ++i) {
    const int d = std::abs(kFpsPresets[i] - fps);
    if (d < diff) { diff = d; best = i; }
  }
  return best;
}

static int FindClosestBitrateIndex(int bitrate) {
  const int mbps = std::max(1, (bitrate + 500'000) / 1'000'000);
  int best = 0;
  int diff = INT_MAX;
  for (int i = 0; i < static_cast<int>(kBitratePresetsMbps.size()); ++i) {
    const int d = std::abs(kBitratePresetsMbps[i] - mbps);
    if (d < diff) { diff = d; best = i; }
  }
  return best;
}

static int FindResolutionPresetIndex(int width, int height) {
  if (g_resolutionPresets.empty()) return -1;
  int best = 0;
  int64_t bestDiff = LLONG_MAX;
  const int64_t target = static_cast<int64_t>(width) * height;
  for (int i = 0; i < static_cast<int>(g_resolutionPresets.size()); ++i) {
    if (g_resolutionPresets[i].width == width && g_resolutionPresets[i].height == height) return i;
    const int64_t pixels = static_cast<int64_t>(g_resolutionPresets[i].width) * g_resolutionPresets[i].height;
    const int64_t diff = pixels > target ? pixels - target : target - pixels;
    if (diff < bestDiff) { bestDiff = diff; best = i; }
  }
  return best;
}

static void UpdatePendingProfileFromIndices() {
  if (!g_resolutionPresets.empty()) {
    g_resolutionIndex = std::clamp(g_resolutionIndex, 0, static_cast<int>(g_resolutionPresets.size()) - 1);
    g_pendingProfile.width = g_resolutionPresets[g_resolutionIndex].width;
    g_pendingProfile.height = g_resolutionPresets[g_resolutionIndex].height;
  }
  g_fpsIndex = std::clamp(g_fpsIndex, 0, static_cast<int>(kFpsPresets.size()) - 1);
  g_bitrateIndex = std::clamp(g_bitrateIndex, 0, static_cast<int>(kBitratePresetsMbps.size()) - 1);
  g_pendingProfile.fps = kFpsPresets[g_fpsIndex];
  g_pendingProfile.bitrate = kBitratePresetsMbps[g_bitrateIndex] * 1'000'000;
}

static void SyncPendingProfileToIndices(const VideoProfile& profile) {
  g_pendingProfile = profile;
  const int res = FindResolutionPresetIndex(profile.width, profile.height);
  if (res >= 0) g_resolutionIndex = res;
  g_fpsIndex = FindClosestFpsIndex(profile.fps);
  g_bitrateIndex = FindClosestBitrateIndex(profile.bitrate);
  UpdatePendingProfileFromIndices();
}

static void InitVideoProfileUiState() {
  g_resolutionPresets.clear();
  g_pendingProfile = g_activeProfile;
  const int width = std::max(640, g_activeProfile.width);
  const int height = std::max(360, g_activeProfile.height);
  const bool landscape = width >= height;
  const double longEdge = static_cast<double>(std::max(width, height));
  const double shortEdge = static_cast<double>(std::max(1, std::min(width, height)));
  const double aspect = longEdge / shortEdge;
  const int longEdges[] = {1280, 1600, 1920, 2560, 3840};
  for (const int targetLong : longEdges) {
    ResolutionPreset preset{};
    if (landscape) {
      preset.width = ClampEven(targetLong, width);
      preset.height = ClampEven(static_cast<int>(std::lround(targetLong / aspect)), height);
    } else {
      preset.height = ClampEven(targetLong, height);
      preset.width = ClampEven(static_cast<int>(std::lround(targetLong / aspect)), width);
    }
    if (preset.width < 640 || preset.height < 360) continue;
    const auto dup = std::find_if(g_resolutionPresets.begin(), g_resolutionPresets.end(), [&](const ResolutionPreset& item) {
      return item.width == preset.width && item.height == preset.height;
    });
    if (dup == g_resolutionPresets.end()) g_resolutionPresets.push_back(preset);
  }
  if (FindResolutionPresetIndex(g_activeProfile.width, g_activeProfile.height) < 0) {
    g_resolutionPresets.push_back({g_activeProfile.width, g_activeProfile.height});
  }
  std::sort(g_resolutionPresets.begin(), g_resolutionPresets.end(), [](const ResolutionPreset& a, const ResolutionPreset& b) {
    const int64_t ap = static_cast<int64_t>(a.width) * a.height;
    const int64_t bp = static_cast<int64_t>(b.width) * b.height;
    return ap != bp ? ap < bp : a.width < b.width;
  });
  SyncPendingProfileToIndices(g_activeProfile);
}

static bool WriteProfileFile(const VideoProfile& profile) {
  if (g_cfg.profileFile.empty()) return true;
  FILE* file = nullptr;
  if (_wfopen_s(&file, g_cfg.profileFile.c_str(), L"wb") != 0 || !file) return false;
  std::string json = "{\n"
                     "  \"width\": " + std::to_string(profile.width) + ",\n"
                     "  \"height\": " + std::to_string(profile.height) + ",\n"
                     "  \"fps\": " + std::to_string(profile.fps) + ",\n"
                     "  \"bitrate\": " + std::to_string(profile.bitrate) + "\n"
                     "}\n";
  const size_t n = fwrite(json.data(), 1, json.size(), file);
  fclose(file);
  return n == json.size();
}

static uint16_t WinToMacKey(WPARAM vk, LPARAM lp) {
  if (vk == VK_SHIFT) {
    vk = MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
  } else if (vk == VK_CONTROL) {
    vk = (lp & 0x01000000) ? VK_RCONTROL : VK_LCONTROL;
  } else if (vk == VK_MENU) {
    vk = (lp & 0x01000000) ? VK_RMENU : VK_LMENU;
  }

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
    case VK_CAPITAL: return 57;
    case VK_LSHIFT: return 56;
    case VK_RSHIFT: return 60;
    case VK_LCONTROL: return 59;
    case VK_RCONTROL: return 62;
    case VK_LMENU: return 58;
    case VK_RMENU: return 61;
    case VK_LWIN: return 55;
    case VK_RWIN: return 54;
    case VK_OEM_MINUS: return 27;
    case VK_OEM_PLUS: return 24;
    case VK_OEM_4: return 33;
    case VK_OEM_6: return 30;
    case VK_OEM_5: return 42;
    case VK_OEM_1: return 41;
    case VK_OEM_7: return 39;
    case VK_OEM_COMMA: return 43;
    case VK_OEM_PERIOD: return 47;
    case VK_OEM_2: return 44;
    case VK_OEM_3: return 50;
    case VK_NUMPAD0: return 82;
    case VK_NUMPAD1: return 83;
    case VK_NUMPAD2: return 84;
    case VK_NUMPAD3: return 85;
    case VK_NUMPAD4: return 86;
    case VK_NUMPAD5: return 87;
    case VK_NUMPAD6: return 88;
    case VK_NUMPAD7: return 89;
    case VK_NUMPAD8: return 91;
    case VK_NUMPAD9: return 92;
    case VK_DECIMAL: return 65;
    case VK_MULTIPLY: return 67;
    case VK_ADD: return 69;
    case VK_DIVIDE: return 75;
    case VK_SUBTRACT: return 78;
    default: break;
  }
  static const uint16_t digits[] = {29,18,19,20,21,23,22,26,28,25};
  if (vk >= '0' && vk <= '9') return digits[vk - '0'];
  static const uint16_t letters[26] = {
    0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6
  };
  if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
  if (vk >= VK_F1 && vk <= VK_F12) {
    static const uint16_t f[12] = {122,120,99,118,96,97,98,100,101,109,103,111};
    return f[vk - VK_F1];
  }
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

static uint16_t MacModifierMaskForCurrentWinKeys() {
  uint16_t mask = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) mask |= P2_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) mask |= P2_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000) mask |= P2_MOD_OPTION;
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mask |= P2_MOD_COMMAND;
  return mask;
}

static bool HasNonTextModifierDown() {
  return (GetKeyState(VK_CONTROL) & 0x8000) ||
         (GetKeyState(VK_MENU) & 0x8000) ||
         (GetKeyState(VK_LWIN) & 0x8000) ||
         (GetKeyState(VK_RWIN) & 0x8000);
}

static bool IsModifierVirtualKey(WPARAM vk) {
  switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
      return true;
    default:
      return false;
  }
}

static bool IsTextVirtualKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return true;
  if (vk >= '0' && vk <= '9') return true;
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return true;
  switch (vk) {
    case VK_SPACE:
    case VK_OEM_MINUS:
    case VK_OEM_PLUS:
    case VK_OEM_4:
    case VK_OEM_6:
    case VK_OEM_5:
    case VK_OEM_1:
    case VK_OEM_7:
    case VK_OEM_COMMA:
    case VK_OEM_PERIOD:
    case VK_OEM_2:
    case VK_OEM_3:
    case VK_DECIMAL:
    case VK_MULTIPLY:
    case VK_ADD:
    case VK_DIVIDE:
    case VK_SUBTRACT:
      return true;
    default:
      return false;
  }
}

static HFONT CreateUiFont(int px, int weight = FW_NORMAL) {
  return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static void DrawTextRect(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, HFONT font, UINT format) {
  HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
  SetTextColor(hdc, color);
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, text.c_str(), -1, &rc, format);
  SelectObject(hdc, oldFont);
}

static std::wstring FormatElapsed() {
  const uint64_t now = QpcNow();
  const uint64_t seconds = QpcDeltaUs(g_startedQpc, now) / 1'000'000ull;
  wchar_t buf[32];
  swprintf_s(buf, L"%02llu:%02llu:%02llu",
             static_cast<unsigned long long>(seconds / 3600),
             static_cast<unsigned long long>((seconds / 60) % 60),
             static_cast<unsigned long long>(seconds % 60));
  return buf;
}

static void ApplyRoundedRegion(HWND hwnd, int radius) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HRGN region = CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, rc.bottom - rc.top + 1, radius, radius);
  SetWindowRgn(hwnd, region, TRUE);
}

static void ClampToMonitor(int& x, int& y, int width, int height) {
  HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  const RECT r = mi.rcMonitor;
  x = std::max<int>(r.left + 12, std::min<int>(x, r.right - width - 12));
  y = std::max<int>(r.top + 12, std::min<int>(y, r.bottom - height - 12));
}

static void HideNativePopups() {
  if (g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static void UpdateOverlayLayout() {
  if (!g_hwnd || !g_toolbarHwnd) return;
  RECT owner{};
  GetWindowRect(g_hwnd, &owner);
  const int ownerW = std::max(1L, owner.right - owner.left);
  int toolbarX = owner.left + std::max(0, (ownerW - kToolbarWidth) / 2);
  int toolbarY = owner.top + 10;
  ClampToMonitor(toolbarX, toolbarY, kToolbarWidth, kToolbarHeight);
  SetWindowPos(g_toolbarHwnd, HWND_TOPMOST, toolbarX, toolbarY, kToolbarWidth, kToolbarHeight,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ApplyRoundedRegion(g_toolbarHwnd, 28);
  if (g_menuHwnd && IsWindowVisible(g_menuHwnd)) {
    int x = toolbarX + 28;
    int y = toolbarY + kToolbarHeight + 8;
    ClampToMonitor(x, y, kMenuWidth, kMenuHeight);
    SetWindowPos(g_menuHwnd, HWND_TOPMOST, x, y, kMenuWidth, kMenuHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRoundedRegion(g_menuHwnd, 18);
  }
  if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) {
    int x = toolbarX + std::max(0, kToolbarWidth - kStatsWidth);
    int y = toolbarY + kToolbarHeight + 8;
    ClampToMonitor(x, y, kStatsWidth, kStatsHeight);
    SetWindowPos(g_statsHwnd, HWND_TOPMOST, x, y, kStatsWidth, kStatsHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRoundedRegion(g_statsHwnd, 18);
  }
}

static void ShowOnlyPopup(HWND popup) {
  if (!popup) return;
  if (g_menuHwnd && popup != g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd && popup != g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  ShowWindow(popup, SW_SHOWNOACTIVATE);
  UpdateOverlayLayout();
  InvalidateRect(popup, nullptr, FALSE);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static void TogglePopup(HWND popup) {
  if (!popup) return;
  if (IsWindowVisible(popup)) {
    ShowWindow(popup, SW_HIDE);
    if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
  } else {
    ShowOnlyPopup(popup);
  }
}

static void ToggleNativeFullscreen() {
  if (!g_hwnd) return;
  if (!g_fullscreenActive) {
    g_prevWindowStyle = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_STYLE));
    g_prevWindowExStyle = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE));
    GetWindowRect(g_hwnd, &g_prevWindowRect);
    HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, (g_prevWindowStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, g_prevWindowExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_fullscreenActive = true;
  } else {
    SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_prevWindowStyle | WS_VISIBLE);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, g_prevWindowExStyle);
    SetWindowPos(g_hwnd, HWND_TOP, g_prevWindowRect.left, g_prevWindowRect.top,
                 g_prevWindowRect.right - g_prevWindowRect.left,
                 g_prevWindowRect.bottom - g_prevWindowRect.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_fullscreenActive = false;
  }
  g_cfg.fullscreen = g_fullscreenActive;
  ApplyVideoRenderRectangle(g_hwnd);
  UpdateOverlayLayout();
}

static void DrawSignalBars(HDC hdc, int x, int y, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, brush));
  HPEN pen = CreatePen(PS_SOLID, 1, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  const int heights[] = {14, 23, 32};
  for (int i = 0; i < 3; ++i) {
    RoundRect(hdc, x + i * 12, y + 34 - heights[i], x + i * 12 + 5, y + 34, 5, 5);
  }
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(pen);
  DeleteObject(brush);
}

static void DrawSelectorButton(HDC hdc, RECT rc, bool left) {
  HBRUSH bg = CreateSolidBrush(RGB(238, 242, 246));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(210, 218, 228));
  HBRUSH oldBg = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldBorder = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
  SelectObject(hdc, oldBorder);
  SelectObject(hdc, oldBg);
  DeleteObject(border);
  DeleteObject(bg);
  HPEN pen = CreatePen(PS_SOLID, 3, RGB(34, 45, 60));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  if (left) {
    MoveToEx(hdc, cx + 4, cy - 7, nullptr); LineTo(hdc, cx - 4, cy); LineTo(hdc, cx + 4, cy + 7);
  } else {
    MoveToEx(hdc, cx - 4, cy - 7, nullptr); LineTo(hdc, cx + 4, cy); LineTo(hdc, cx - 4, cy + 7);
  }
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawToolbar(HDC hdc, RECT rc) {
  HBRUSH bg = CreateSolidBrush(RGB(250, 252, 254));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(214, 221, 228));
  HBRUSH oldBg = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldBorder = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 28, 28);
  SelectObject(hdc, oldBorder);
  SelectObject(hdc, oldBg);
  DeleteObject(border);
  DeleteObject(bg);

  HBRUSH closeBg = CreateSolidBrush(RGB(255, 96, 86));
  HBRUSH old = reinterpret_cast<HBRUSH>(SelectObject(hdc, closeBg));
  HPEN closePen = CreatePen(PS_SOLID, 1, RGB(235, 74, 66));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, closePen));
  Ellipse(hdc, 20, 22, 34, 36);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, old);
  DeleteObject(closePen);
  DeleteObject(closeBg);

  HBRUSH profileFill = CreateSolidBrush(IsWindowVisible(g_menuHwnd) ? RGB(227, 241, 255) : RGB(243, 246, 250));
  HPEN profileBorder = CreatePen(PS_SOLID, 1, RGB(210, 222, 235));
  old = reinterpret_cast<HBRUSH>(SelectObject(hdc, profileFill));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, profileBorder));
  RoundRect(hdc, 64, 8, 274, 52, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, old);
  DeleteObject(profileBorder);
  DeleteObject(profileFill);

  HFONT titleFont = CreateUiFont(14, FW_SEMIBOLD);
  HFONT subFont = CreateUiFont(11, FW_NORMAL);
  RECT profileTitle{78, 11, 260, 28};
  RECT profileSub{78, 28, 260, 45};
  DrawTextRect(hdc, L"Video", profileTitle, RGB(18, 28, 42), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, FormatCompactProfile(g_activeProfile), profileSub, RGB(92, 104, 118), subFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  HBRUSH statsFill = CreateSolidBrush(IsWindowVisible(g_statsHwnd) ? RGB(225, 248, 238) : RGB(239, 248, 244));
  HPEN statsBorder = CreatePen(PS_SOLID, 1, RGB(210, 228, 217));
  old = reinterpret_cast<HBRUSH>(SelectObject(hdc, statsFill));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, statsBorder));
  RoundRect(hdc, 286, 8, 430, 52, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, old);
  DeleteObject(statsBorder);
  DeleteObject(statsFill);
  DrawSignalBars(hdc, 304, 14, RGB(17, 190, 122));
  const double fps = g_presentFps.load(std::memory_order_relaxed);
  const uint64_t lastBuffer = g_lastBufferQpc.load(std::memory_order_relaxed);
  const double ageMs = lastBuffer ? double(QpcDeltaUs(lastBuffer, QpcNow())) / 1000.0 : 0.0;
  wchar_t statsText[96];
  if (fps > 0.1) swprintf_s(statsText, L"%.0f fps / %.0f ms", fps, ageMs);
  else swprintf_s(statsText, L"UDP/GStreamer");
  RECT statsTitle{346, 11, 418, 28};
  RECT statsSub{346, 28, 418, 45};
  DrawTextRect(hdc, L"Link", statsTitle, RGB(24, 92, 52), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, statsText, statsSub, RGB(72, 118, 90), subFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  HBRUSH timeFill = CreateSolidBrush(RGB(243, 246, 250));
  HPEN timeBorder = CreatePen(PS_SOLID, 1, RGB(222, 228, 235));
  old = reinterpret_cast<HBRUSH>(SelectObject(hdc, timeFill));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, timeBorder));
  RoundRect(hdc, 442, 8, 546, 52, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, old);
  DeleteObject(timeBorder);
  DeleteObject(timeFill);
  RECT timeTitle{454, 14, 534, 34};
  RECT timeSub{454, 30, 534, 46};
  HFONT timeFont = CreateUiFont(18, FW_SEMIBOLD);
  DrawTextRect(hdc, FormatElapsed(), timeTitle, RGB(48, 58, 72), timeFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, L"Session", timeSub, RGB(123, 133, 144), subFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(timeFont);
  DeleteObject(titleFont);
  DeleteObject(subFont);
}

static void DrawSeparator(HDC hdc, int y, int width) {
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(216, 222, 226));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 28, y, nullptr);
  LineTo(hdc, width - 28, y);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawMenuRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value, COLORREF color = RGB(28, 37, 48)) {
  HFONT labelFont = CreateUiFont(17, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(13, FW_NORMAL);
  RECT labelRc{28, y + 6, 180, y + 34};
  RECT valueRc{182, y + 8, kMenuWidth - 28, y + 34};
  DrawTextRect(hdc, label, labelRc, color, labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DrawTextRect(hdc, value, valueRc, color == RGB(28, 37, 48) ? RGB(110, 120, 132) : color, valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawMenuSelectorRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value) {
  HFONT labelFont = CreateUiFont(17, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(13, FW_NORMAL);
  RECT labelRc{28, y + 6, 180, y + 34};
  DrawTextRect(hdc, label, labelRc, RGB(28, 37, 48), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  RECT leftRc{238, y + 4, 270, y + 34};
  RECT rightRc{376, y + 4, 408, y + 34};
  DrawSelectorButton(hdc, leftRc, true);
  DrawSelectorButton(hdc, rightRc, false);
  RECT valueRc{280, y + 6, 366, y + 34};
  DrawTextRect(hdc, value, valueRc, RGB(110, 120, 132), valueFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawMenu(HDC hdc, RECT rc) {
  HBRUSH bg = CreateSolidBrush(RGB(250, 252, 254));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(214, 221, 228));
  HBRUSH oldBg = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldBorder = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldBorder);
  SelectObject(hdc, oldBg);
  DeleteObject(border);
  DeleteObject(bg);
  const bool changed = !SameProfile(g_activeProfile, g_pendingProfile);
  DrawMenuRow(hdc, 18, L"Current", FormatCompactProfile(g_activeProfile));
  DrawSeparator(hdc, 76, kMenuWidth);
  DrawMenuSelectorRow(hdc, 94, L"Resolution", FormatResolution(g_pendingProfile.width, g_pendingProfile.height));
  DrawMenuSelectorRow(hdc, 148, L"FPS", std::to_wstring(g_pendingProfile.fps) + L" fps");
  DrawMenuSelectorRow(hdc, 202, L"Bitrate", FormatBitrate(g_pendingProfile.bitrate));
  DrawSeparator(hdc, 258, kMenuWidth);
  DrawMenuRow(hdc, 276, L"Apply", changed ? FormatCompactProfile(g_pendingProfile) : L"Active");
  DrawMenuRow(hdc, 330, L"Stats", L"fps / buffer / backend");
  DrawMenuRow(hdc, 384, g_cfg.fullscreen ? L"Exit fullscreen" : L"Fullscreen", L"F11");
  DrawSeparator(hdc, 438, kMenuWidth);
  DrawMenuRow(hdc, 448, L"Disconnect", L"", RGB(232, 62, 52));
}

static void DrawStatsRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value) {
  HFONT labelFont = CreateUiFont(15, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(16, FW_NORMAL);
  RECT labelRc{30, y, 210, y + 30};
  RECT valueRc{220, y, kStatsWidth - 30, y + 30};
  DrawTextRect(hdc, label, labelRc, RGB(132, 142, 154), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, value, valueRc, RGB(18, 24, 36), valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawStats(HDC hdc, RECT rc) {
  HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(220, 224, 228));
  HBRUSH oldBg = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldBorder = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldBorder);
  SelectObject(hdc, oldBg);
  DeleteObject(border);
  DeleteObject(bg);
  DrawSignalBars(hdc, 36, 28, RGB(17, 190, 122));
  HFONT titleFont = CreateUiFont(20, FW_BOLD);
  RECT titleRc{92, 28, kStatsWidth - 30, 70};
  DrawTextRect(hdc, L"GStreamer low-latency link", titleRc, RGB(15, 22, 36), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(titleFont);
  DrawSeparator(hdc, 92, kStatsWidth);

  const double fps = g_presentFps.load(std::memory_order_relaxed);
  const uint64_t frames = g_framesPresented.load(std::memory_order_relaxed);
  const uint64_t lastBuffer = g_lastBufferQpc.load(std::memory_order_relaxed);
  const double ageMs = lastBuffer ? double(QpcDeltaUs(lastBuffer, QpcNow())) / 1000.0 : 0.0;
  wchar_t fpsText[64];
  swprintf_s(fpsText, L"%.1f fps", fps);
  wchar_t ageText[64];
  swprintf_s(ageText, L"%.0f ms", ageMs);
  DrawStatsRow(hdc, 118, L"Present FPS:", fps > 0.1 ? fpsText : L"-- fps");
  DrawStatsRow(hdc, 156, L"Last frame age:", frames ? ageText : L"-- ms");
  DrawStatsRow(hdc, 194, L"Frames:", std::to_wstring(frames));
  DrawStatsRow(hdc, 232, L"Profile:", FormatCompactProfile(g_activeProfile));
  DrawSeparator(hdc, 274, kStatsWidth);
  DrawStatsRow(hdc, 300, L"Video backend:", L"GStreamer RTP/H.264");
  DrawStatsRow(hdc, 338, L"Decode/display:", L"d3d11h264dec / d3d11videosink");
  DrawStatsRow(hdc, 376, L"Input channel:", L"UDP low-latency control");
}

static void CycleResolution(int delta) {
  if (g_resolutionPresets.empty()) return;
  const int count = static_cast<int>(g_resolutionPresets.size());
  g_resolutionIndex = (g_resolutionIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

static void CycleFps(int delta) {
  const int count = static_cast<int>(kFpsPresets.size());
  g_fpsIndex = (g_fpsIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

static void CycleBitrate(int delta) {
  const int count = static_cast<int>(kBitratePresetsMbps.size());
  g_bitrateIndex = (g_bitrateIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

static void SendVideoProfileCommand(const VideoProfile& profile) {
  const uint16_t bitrateMbps = static_cast<uint16_t>(std::clamp((profile.bitrate + 500'000) / 1'000'000, 1, 200));
  for (int i = 0; i < 3; ++i) {
    SendInputPacket(P2_INPUT_SET_VIDEO_PROFILE, 0, 0, profile.width, profile.height,
                    static_cast<uint16_t>(std::clamp(profile.fps, 30, 240)), bitrateMbps);
    if (i < 2) Sleep(15);
  }
}

static void RequestKeyframe() {
  for (int i = 0; i < 2; ++i) {
    SendInputPacket(P2_INPUT_REQUEST_KEYFRAME, 0, 0, 0, 0, 0, 0);
    if (i == 0) Sleep(10);
  }
}

static void RestartGStreamer() {
  if (!g_hwnd) return;
  if (g_pipeline) {
    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);
    g_pipeline = nullptr;
  }
  StartGStreamer(g_hwnd);
  RequestKeyframe();
}

static void ApplyPendingVideoProfile() {
  VideoProfile profile = g_pendingProfile;
  profile.width = std::max(640, ClampEven(profile.width, g_cfg.width));
  profile.height = std::max(360, ClampEven(profile.height, g_cfg.height));
  profile.fps = std::clamp(profile.fps, 30, 240);
  profile.bitrate = std::clamp(profile.bitrate, 2'000'000, 200'000'000);
  if (!WriteProfileFile(profile)) {
    MessageBoxW(g_hwnd, L"Failed to save the updated native-v2 profile.", L"P2P Native GStreamer", MB_ICONERROR);
    return;
  }
  SendVideoProfileCommand(profile);
  CommitActiveProfile(profile);
  SyncPendingProfileToIndices(profile);
  RestartGStreamer();
  HideNativePopups();
  ApplyVideoRenderRectangle(g_hwnd);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static bool HandleSelectorClick(int x, int y, int rowY, void (*cycleFn)(int)) {
  if (y < rowY || y >= rowY + 40) return false;
  if (x >= 238 && x <= 270) {
    cycleFn(-1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  if (x >= 376 && x <= 408) {
    cycleFn(1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  if (x >= 280 && x <= 366) {
    cycleFn(1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  return false;
}

static void HandleToolbarClick(int x, int y) {
  if (x >= 14 && x <= 48 && y >= 12 && y <= 46) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  } else if (x >= 64 && x <= 274 && y >= 8 && y <= 52) {
    TogglePopup(g_menuHwnd);
  } else if (x >= 286 && x <= 430 && y >= 8 && y <= 52) {
    TogglePopup(g_statsHwnd);
  }
}

static void HandleMenuClick(int x, int y) {
  if (HandleSelectorClick(x, y, 94, CycleResolution)) return;
  if (HandleSelectorClick(x, y, 148, CycleFps)) return;
  if (HandleSelectorClick(x, y, 202, CycleBitrate)) return;
  if (y >= 276 && y < 316) {
    ApplyPendingVideoProfile();
  } else if (y >= 330 && y < 370) {
    ShowOnlyPopup(g_statsHwnd);
  } else if (y >= 384 && y < 424) {
    ToggleNativeFullscreen();
  } else if (y >= 448 && y < 480) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  }
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const LONG_PTR kind = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursor(nullptr, IDC_ARROW));
      return TRUE;
    case WM_LBUTTONDOWN: {
      const int x = GET_X_LPARAM(lp);
      const int y = GET_Y_LPARAM(lp);
      if (kind == 1) HandleToolbarClick(x, y);
      else if (kind == 2) HandleMenuClick(x, y);
      if (g_hwnd) SetFocus(g_hwnd);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      if (kind == 1) DrawToolbar(hdc, rc);
      else if (kind == 2) DrawMenu(hdc, rc);
      else if (kind == 3) DrawStats(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateOverlayWindows(HINSTANCE hInst) {
  WNDCLASSW overlay{};
  overlay.lpfnWndProc = OverlayWndProc;
  overlay.hInstance = hInst;
  overlay.hCursor = LoadCursor(nullptr, IDC_ARROW);
  overlay.lpszClassName = L"P2PNativeGstOverlay";
  RegisterClassW(&overlay);
  const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
  g_toolbarHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                  0, 0, kToolbarWidth, kToolbarHeight, g_hwnd, nullptr, hInst, nullptr);
  g_menuHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                               0, 0, kMenuWidth, kMenuHeight, g_hwnd, nullptr, hInst, nullptr);
  g_statsHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                0, 0, kStatsWidth, kStatsHeight, g_hwnd, nullptr, hInst, nullptr);
  if (g_toolbarHwnd) SetWindowLongPtrW(g_toolbarHwnd, GWLP_USERDATA, 1);
  if (g_menuHwnd) SetWindowLongPtrW(g_menuHwnd, GWLP_USERDATA, 2);
  if (g_statsHwnd) SetWindowLongPtrW(g_statsHwnd, GWLP_USERDATA, 3);
  UpdateOverlayLayout();
  if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
}

static VideoRect CurrentVideoRect(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int cw = static_cast<int>(std::max(1L, rc.right - rc.left));
  const int ch = static_cast<int>(std::max(1L, rc.bottom - rc.top));
  const double videoAspect = static_cast<double>(std::max(1, g_cfg.width)) / static_cast<double>(std::max(1, g_cfg.height));
  const double clientAspect = static_cast<double>(cw) / static_cast<double>(ch);
  VideoRect out{};
  if (clientAspect > videoAspect) {
    out.height = ch;
    out.width = std::max(1, static_cast<int>(static_cast<double>(ch) * videoAspect + 0.5));
    out.left = (cw - out.width) / 2;
    out.top = 0;
  } else {
    out.width = cw;
    out.height = std::max(1, static_cast<int>(static_cast<double>(cw) / videoAspect + 0.5));
    out.left = 0;
    out.top = (ch - out.height) / 2;
  }
  return out;
}

static bool NormalizedPoint(HWND hwnd, LPARAM lp, float& x, float& y) {
  const VideoRect vr = CurrentVideoRect(hwnd);
  const int px = GET_X_LPARAM(lp);
  const int py = GET_Y_LPARAM(lp);
  const bool inside = px >= vr.left && px < vr.left + vr.width && py >= vr.top && py < vr.top + vr.height;
  x = static_cast<float>(px - vr.left) / static_cast<float>(std::max(1, vr.width));
  y = static_cast<float>(py - vr.top) / static_cast<float>(std::max(1, vr.height));
  x = std::max(0.0f, std::min(1.0f, x));
  y = std::max(0.0f, std::min(1.0f, y));
  return inside;
}

static void ApplyVideoRenderRectangle(HWND hwnd) {
  if (!g_pipeline) return;
  GstElement* sink = gst_bin_get_by_name(GST_BIN(g_pipeline), "videosink");
  if (sink && GST_IS_VIDEO_OVERLAY(sink)) {
    const VideoRect vr = CurrentVideoRect(hwnd);
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(sink), vr.left, vr.top, vr.width, vr.height);
    gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
  }
  if (sink) gst_object_unref(sink);
}

static GstPadProbeReturn OnVideoBuffer(GstPad*, GstPadProbeInfo* info, gpointer) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    g_framesPresented.fetch_add(1, std::memory_order_relaxed);
    g_lastBufferQpc.store(QpcNow(), std::memory_order_relaxed);
  }
  return GST_PAD_PROBE_OK;
}

static std::string GstPipelineDescription(HWND hwnd) {
  char overlay[64];
  snprintf(overlay, sizeof(overlay), "%llu", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
  std::string desc;
  desc += "udpsrc port=" + std::to_string(g_cfg.videoPort);
  desc += " buffer-size=8388608 ";
  desc += " caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000\" ";
  desc += "! rtpjitterbuffer latency=0 faststart-min-packets=1 drop-on-latency=true do-lost=true mode=0 max-dropout-time=40 max-misorder-time=20 ";
  desc += "! rtph264depay request-keyframe=true wait-for-keyframe=true ";
  desc += "! h264parse config-interval=-1 disable-passthrough=true ";
  desc += "! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ";
  desc += "! d3d11h264dec discard-corrupted-frames=true automatic-request-sync-points=true qos=true ";
  desc += "! queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ";
  desc += "! d3d11videosink name=videosink sync=false async=false qos=true max-lateness=0 force-aspect-ratio=false";
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
    const VideoRect vr = CurrentVideoRect(hwnd);
    gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(sink), vr.left, vr.top, vr.width, vr.height);
    gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), FALSE);
  }
  if (sink) {
    GstPad* sinkPad = gst_element_get_static_pad(sink, "sink");
    if (sinkPad) {
      gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, OnVideoBuffer, nullptr, nullptr);
      gst_object_unref(sinkPad);
    }
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
      SetFocus(hwnd);
      SetTimer(hwnd, 1, 500, nullptr);
      return 0;
    case WM_TIMER: {
      static uint64_t lastFrames = 0;
      static uint64_t lastQpc = QpcNow();
      const uint64_t now = QpcNow();
      const uint64_t frames = g_framesPresented.load(std::memory_order_relaxed);
      const double seconds = double(QpcDeltaUs(lastQpc, now)) / 1'000'000.0;
      const double fps = seconds > 0.001 ? double(frames - lastFrames) / seconds : 0.0;
      g_presentFps.store(fps, std::memory_order_relaxed);
      lastFrames = frames;
      lastQpc = now;
      wchar_t title[512];
      const uint64_t lastBuffer = g_lastBufferQpc.load(std::memory_order_relaxed);
      const double ageMs = lastBuffer ? double(QpcDeltaUs(lastBuffer, now)) / 1000.0 : 0.0;
      swprintf_s(title, L"P2P Native v2 GStreamer -> %s | %.0f fps | last frame %.0f ms | %dx%d@%d %d Mbps",
                 g_cfg.hostIp.c_str(), fps, ageMs, g_activeProfile.width, g_activeProfile.height,
                 g_activeProfile.fps, std::max(1, g_activeProfile.bitrate / 1'000'000));
      SetWindowTextW(hwnd, title);
      if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
      if (g_menuHwnd && IsWindowVisible(g_menuHwnd)) InvalidateRect(g_menuHwnd, nullptr, FALSE);
      if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) InvalidateRect(g_statsHwnd, nullptr, FALSE);
      return 0;
    }
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {
        HideNativePopups();
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_HIDE);
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        ApplyVideoRenderRectangle(hwnd);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_MOVE:
      UpdateOverlayLayout();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) {
        HideNativePopups();
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_MOUSEMOVE: {
      float x, y; const bool inside = NormalizedPoint(hwnd, lp, x, y);
      if (!inside && !(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) return 0;
      uint16_t b = (wp & MK_LBUTTON) ? 0 : ((wp & MK_RBUTTON) ? 2 : ((wp & MK_MBUTTON) ? 1 : 0));
      SendInputPacket(P2_INPUT_MOVE, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
      SetCapture(hwnd);
      SetFocus(hwnd);
      float x, y; if (!NormalizedPoint(hwnd, lp, x, y)) return 0;
      uint16_t b = msg == WM_RBUTTONDOWN ? 2 : (msg == WM_MBUTTONDOWN ? 1 : 0);
      SendInputPacket(P2_INPUT_DOWN, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
      ReleaseCapture();
      float x, y; if (!NormalizedPoint(hwnd, lp, x, y)) return 0;
      uint16_t b = msg == WM_RBUTTONUP ? 2 : (msg == WM_MBUTTONUP ? 1 : 0);
      SendInputPacket(P2_INPUT_UP, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      LPARAM clientLp = MAKELPARAM(pt.x, pt.y);
      float x, y; if (!NormalizedPoint(hwnd, clientLp, x, y)) return 0;
      SendInputPacket(P2_INPUT_WHEEL, x, y, 0, -GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
      return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      if (wp == VK_F11) {
        ToggleNativeFullscreen();
        return 0;
      }
      if (wp == VK_ESCAPE && ((g_menuHwnd && IsWindowVisible(g_menuHwnd)) || (g_statsHwnd && IsWindowVisible(g_statsHwnd)))) {
        HideNativePopups();
        return 0;
      }
      if (!IsModifierVirtualKey(wp) && !HasNonTextModifierDown() && IsTextVirtualKey(wp)) return 0;
      uint16_t mac = WinToMacKey(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_DOWN, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      if (!IsModifierVirtualKey(wp) && !HasNonTextModifierDown() && IsTextVirtualKey(wp)) return 0;
      uint16_t mac = WinToMacKey(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_UP, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_CHAR: {
      if (wp >= 0x20 && wp != 0x7f) {
        SendInputPacket(P2_INPUT_TEXT, 0, 0, 0, 0, MacModifierMaskForCurrentWinKeys(), static_cast<uint16_t>(wp & 0xffff));
      }
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
      KillTimer(hwnd, 1);
      g_running.store(false, std::memory_order_relaxed);
      StopGStreamer();
      if (g_toolbarHwnd) { DestroyWindow(g_toolbarHwnd); g_toolbarHwnd = nullptr; }
      if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
      if (g_statsHwnd) { DestroyWindow(g_statsHwnd); g_statsHwnd = nullptr; }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
  g_cfg = ParseArgs();
  QueryPerformanceFrequency(&g_qpcFreq);
  g_startedQpc = QpcNow();
  const int initialBitrate = std::max(2'000'000, g_cfg.bitrate > 0 ? g_cfg.bitrate : AutoBitrateForPixels(g_cfg.width, g_cfg.height, 14'000'000));
  CommitActiveProfile(VideoProfile{g_cfg.width, g_cfg.height, g_cfg.fps, initialBitrate});
  InitVideoProfileUiState();
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
  if (g_cfg.fullscreen) {
    g_fullscreenActive = true;
  }
  ShowWindow(g_hwnd, nCmdShow);
  UpdateWindow(g_hwnd);
  CreateOverlayWindows(hInst);
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
