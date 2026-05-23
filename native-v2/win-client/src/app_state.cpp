#include "app_state.h"

#include <ws2tcpip.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

Config g_cfg;
HWND g_hwnd = nullptr;
HWND g_toolbarHwnd = nullptr;
HWND g_menuHwnd = nullptr;
HWND g_statsHwnd = nullptr;
std::atomic<bool> g_running{true};
std::mutex g_encodedMu;
std::condition_variable g_encodedCv;
std::deque<EncodedFrame> g_encodedQueue;
std::mutex g_decodedMu;
std::condition_variable g_decodedCv;
std::deque<DecodedFrame> g_decodedQueue;
std::mutex g_frameMu;
BgraFrame g_latestFrame;
SOCKET g_inputSock = INVALID_SOCKET;
sockaddr_in g_inputAddr{};
std::atomic<uint32_t> g_inputSeq{1};
std::atomic<uint64_t> g_framesPresented{0};
LARGE_INTEGER g_qpcFreq{};
std::atomic<uint64_t> g_lastPresentQpc{0};
std::atomic<uint64_t> g_lastRxToPresentUs{0};
std::atomic<uint64_t> g_gpuFrames{0};
std::atomic<uint64_t> g_cpuFrames{0};
std::atomic<uint64_t> g_packetsRx{0};
std::atomic<uint64_t> g_bytesRx{0};
std::atomic<uint64_t> g_framesComplete{0};
std::atomic<uint64_t> g_framesDropped{0};
std::atomic<uint64_t> g_clientFramesDropped{0};
std::atomic<uint64_t> g_networkFramesDropped{0};
std::atomic<uint64_t> g_decodeFails{0};
std::atomic<uint64_t> g_gpuRenderFails{0};
std::atomic<uint64_t> g_lastPacketQpc{0};
std::atomic<uint64_t> g_lastCompleteQpc{0};
std::atomic<bool> g_decoderPrimed{false};
std::atomic<bool> g_decoderHasKeyframe{false};
std::atomic<bool> g_waitingForKeyframe{false};
std::atomic<bool> g_loggedFirstDecodedFrame{false};
std::atomic<bool> g_loggedFirstPresentedFrame{false};
std::atomic<uint64_t> g_lastKeyframeRequestQpc{0};
std::atomic<uint64_t> g_keyframeRequests{0};
std::atomic<int> g_currentBitrate{12'000'000};
std::atomic<int> g_activeVideoWidth{1920};
std::atomic<int> g_activeVideoHeight{1080};
std::atomic<int> g_activeVideoFps{60};
std::atomic<int> g_activeVideoBitrate{14'000'000};
std::atomic<uint64_t> g_videoProfileGeneration{0};
std::atomic<uint32_t> g_encodedQueueDepthNow{0};
std::atomic<uint32_t> g_encodedQueueTargetNow{0};
std::atomic<uint32_t> g_decodedQueueDepthNow{0};
std::atomic<uint32_t> g_decodedQueueTargetNow{4};
std::atomic<uint64_t> g_renderFramesDropped{0};
std::atomic<uint64_t> g_lastProfileApplyQpc{0};
uint64_t g_startedQpc = 0;
std::wstring g_localIp = L"-";
VideoProfile g_pendingProfile;
std::vector<ResolutionPreset> g_resolutionPresets;
int g_resolutionIndex = 0;
int g_fpsIndex = 0;
int g_bitrateIndex = 0;
int g_exitCode = 0;
bool g_toolbarExpanded = false;
std::mutex g_uiStatsMu;
NativeUiStats g_uiStats;

uint64_t QpcNow() {
  LARGE_INTEGER q{};
  QueryPerformanceCounter(&q);
  return static_cast<uint64_t>(q.QuadPart);
}

uint64_t QpcDeltaUs(uint64_t start, uint64_t end) {
  if (!g_qpcFreq.QuadPart || end <= start) return 0;
  return (end - start) * 1'000'000ull / static_cast<uint64_t>(g_qpcFreq.QuadPart);
}

void RecordClientFrameDrop(uint64_t count) {
  g_framesDropped.fetch_add(count, std::memory_order_relaxed);
  g_clientFramesDropped.fetch_add(count, std::memory_order_relaxed);
}

void RecordDecodedFrameDrop(uint64_t count) {
  g_framesDropped.fetch_add(count, std::memory_order_relaxed);
  g_clientFramesDropped.fetch_add(count, std::memory_order_relaxed);
  g_renderFramesDropped.fetch_add(count, std::memory_order_relaxed);
}

void RecordNetworkFrameDrop(uint64_t count) {
  g_framesDropped.fetch_add(count, std::memory_order_relaxed);
  g_networkFramesDropped.fetch_add(count, std::memory_order_relaxed);
}

void Log(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
  va_end(args);
  OutputDebugStringW(buf);
  OutputDebugStringW(L"\n");
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
      std::vector<char> utf8(static_cast<size_t>(needed));
      int writtenChars = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), needed, nullptr, nullptr);
      if (writtenChars > 1) {
        DWORD written = 0;
        WriteFile(out, utf8.data(), static_cast<DWORD>(writtenChars - 1), &written, nullptr);
        WriteFile(out, "\n", 1, &written, nullptr);
      }
    }
  }
}
Config ParseArgs() {
  Config c;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    auto next = [&]() -> LPWSTR { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (wcscmp(argv[i], L"--host-ip") == 0) { if (auto v = next()) c.hostIp = v; }
    else if (wcscmp(argv[i], L"--host-name") == 0) { if (auto v = next()) c.hostName = v; }
    else if (wcscmp(argv[i], L"--host-platform") == 0) { if (auto v = next()) c.hostPlatform = v; }
    else if (wcscmp(argv[i], L"--video-port") == 0) { if (auto v = next()) c.videoPort = (uint16_t)_wtoi(v); }
    else if (wcscmp(argv[i], L"--input-port") == 0) { if (auto v = next()) c.inputPort = (uint16_t)_wtoi(v); }
    else if (wcscmp(argv[i], L"--width") == 0) { if (auto v = next()) c.width = _wtoi(v); }
    else if (wcscmp(argv[i], L"--height") == 0) { if (auto v = next()) c.height = _wtoi(v); }
    else if (wcscmp(argv[i], L"--fps") == 0) { if (auto v = next()) c.fps = std::max(30, _wtoi(v)); }
    else if (wcscmp(argv[i], L"--bitrate") == 0) { if (auto v = next()) c.bitrate = std::max(0, _wtoi(v)); }
    else if (wcscmp(argv[i], L"--profile-file") == 0) { if (auto v = next()) c.profileFile = v; }
    else if (wcscmp(argv[i], L"--fullscreen") == 0) { c.fullscreen = true; }
    else if (wcscmp(argv[i], L"--udp-video") == 0) { c.udpVideo = true; }
    else if (wcscmp(argv[i], L"--transport") == 0) { if (auto v = next()) c.udpVideo = (_wcsicmp(v, L"udp") == 0); }
  }
  if (argv) LocalFree(argv);
  return c;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring DetectLocalIpForHost(const std::wstring& hostIp, uint16_t port) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return L"-";

  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(port);
  std::string ip = WideToUtf8(hostIp);
  if (inet_pton(AF_INET, ip.c_str(), &remote.sin_addr) != 1) {
    closesocket(s);
    return L"-";
  }

  connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
  sockaddr_in local{};
  int len = sizeof(local);
  std::wstring out = L"-";
  if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
      std::string value(buf);
      out.assign(value.begin(), value.end());
    }
  }
  closesocket(s);
  return out;
}

void PushEncoded(EncodedFrame&& f) {
  bool queued = false;
  {
    std::lock_guard lk(g_encodedMu);
    size_t queueDepthTarget = EncodedQueueDepthTarget();
    if (g_waitingForKeyframe.load(std::memory_order_relaxed)) {
      if (!f.keyframe) {
        RecordClientFrameDrop();
        return;
      }
      g_waitingForKeyframe.store(false, std::memory_order_relaxed);
    }

    if (f.keyframe) {
      g_decoderHasKeyframe.store(true, std::memory_order_relaxed);
    }

    if (!g_decoderPrimed.load(std::memory_order_relaxed) &&
        g_decoderHasKeyframe.load(std::memory_order_relaxed)) {
      queueDepthTarget = kMaxEncodedQueueDepth;
      g_encodedQueueTargetNow.store(static_cast<uint32_t>(queueDepthTarget), std::memory_order_relaxed);
    }

    // Until a keyframe has entered the decode pipeline, delta frames cannot
    // establish decoder state. Once the first keyframe is queued, keep feeding
    // subsequent frames because Media Foundation may need more than one sample
    // before it produces output.
    if (!g_decoderPrimed.load(std::memory_order_relaxed) &&
        !g_decoderHasKeyframe.load(std::memory_order_relaxed) &&
        !f.keyframe) {
      RecordClientFrameDrop();
      return;
    }

    while (g_encodedQueue.size() >= queueDepthTarget) {
      if (!g_decoderPrimed.load(std::memory_order_relaxed) && !f.keyframe) {
        RecordClientFrameDrop();
        return;
      }
      g_encodedQueue.pop_front();
      RecordClientFrameDrop();
    }
    g_encodedQueue.emplace_back(std::move(f));
    g_encodedQueueDepthNow.store(static_cast<uint32_t>(g_encodedQueue.size()), std::memory_order_relaxed);
    queued = true;
  }
  if (queued) g_encodedCv.notify_one();
}
std::wstring FormatDouble(double value, const wchar_t* suffix, int decimals) {
  wchar_t buf[64];
  if (decimals <= 0) swprintf_s(buf, L"%.0f%s", value, suffix);
  else swprintf_s(buf, L"%.*f%s", decimals, value, suffix);
  return buf;
}

std::wstring FormatBitrate(int bitrate) {
  if (bitrate <= 0) return L"-";
  return FormatDouble(double(bitrate) / 1'000'000.0, L" Mbps", 1);
}

int ClampEven(int value, int fallback) {
  int number = std::max(2, static_cast<int>(std::lround(value)));
  return (number % 2) == 0 ? number : number - 1;
}

int DefaultBitrateForPixels(int width, int height, int fallback) {
  const int64_t pixels = static_cast<int64_t>(width) * height;
  int bitrate = std::max(8'000'000, fallback);
  if (pixels <= 1280ll * 720ll) bitrate = std::max(bitrate, 8'000'000);
  else if (pixels <= 1600ll * 900ll) bitrate = std::max(bitrate, 10'000'000);
  else if (pixels <= 1920ll * 1080ll) bitrate = std::max(bitrate, 12'000'000);
  else if (pixels <= 1920ll * 1200ll) bitrate = std::max(bitrate, 14'000'000);
  else if (pixels <= 2560ll * 1440ll) bitrate = std::max(bitrate, 18'000'000);
  else bitrate = std::max(bitrate, 24'000'000);
  return bitrate;
}

size_t EncodedQueueDepthTarget() {
  size_t target = kMinEncodedQueueDepth;
  target = std::max(kMinEncodedQueueDepth, target);
  target = std::min(kMaxEncodedQueueDepth, target);
  g_encodedQueueTargetNow.store(static_cast<uint32_t>(target), std::memory_order_relaxed);
  return target;
}

size_t DecodedQueueDepthTarget() {
  const int fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
  size_t target = 1;
  g_decodedQueueTargetNow.store(static_cast<uint32_t>(target), std::memory_order_relaxed);
  return target;
}

void PushDecoded(DecodedFrame&& frame) {
  bool queued = false;
  {
    std::lock_guard lk(g_decodedMu);
    const size_t queueTarget = DecodedQueueDepthTarget();
    while (g_decodedQueue.size() >= queueTarget) {
      g_decodedQueue.pop_front();
      RecordDecodedFrameDrop();
    }
    g_decodedQueue.emplace_back(std::move(frame));
    g_decodedQueueDepthNow.store(static_cast<uint32_t>(g_decodedQueue.size()), std::memory_order_relaxed);
    queued = true;
  }
  if (queued) g_decodedCv.notify_one();
}

VideoProfile ActiveVideoProfile() {
  VideoProfile profile;
  profile.width = std::max(640, g_activeVideoWidth.load(std::memory_order_relaxed));
  profile.height = std::max(360, g_activeVideoHeight.load(std::memory_order_relaxed));
  profile.fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
  const int bitrate = g_activeVideoBitrate.load(std::memory_order_relaxed);
  profile.bitrate = bitrate > 0 ? bitrate : DefaultBitrateForPixels(profile.width, profile.height, 14'000'000);
  return profile;
}

void CommitActiveVideoProfile(const VideoProfile& profile) {
  const int width = std::max(640, ClampEven(profile.width, g_cfg.width));
  const int height = std::max(360, ClampEven(profile.height, g_cfg.height));
  const int fps = std::clamp(profile.fps, 30, 240);
  const int fallbackBitrate = DefaultBitrateForPixels(width, height, 14'000'000);
  const int bitrate = std::clamp(profile.bitrate > 0 ? profile.bitrate : fallbackBitrate, 2'000'000, 80'000'000);
  g_activeVideoWidth.store(width, std::memory_order_relaxed);
  g_activeVideoHeight.store(height, std::memory_order_relaxed);
  g_activeVideoFps.store(fps, std::memory_order_relaxed);
  g_activeVideoBitrate.store(bitrate, std::memory_order_relaxed);
  g_currentBitrate.store(bitrate, std::memory_order_relaxed);
}

VideoProfile CurrentVideoProfile() {
  return ActiveVideoProfile();
}

bool SameVideoProfile(const VideoProfile& lhs, const VideoProfile& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fps == rhs.fps && lhs.bitrate == rhs.bitrate;
}

std::wstring FormatResolution(int width, int height) {
  wchar_t buf[64];
  swprintf_s(buf, L"%dx%d", width, height);
  return buf;
}

std::wstring FormatProfileBitrate(int bitrate) {
  if (bitrate <= 0) return L"自动";
  wchar_t buf[32];
  swprintf_s(buf, L"%d Mbps", std::max(1, (bitrate + 500'000) / 1'000'000));
  return buf;
}

std::wstring FormatCompactProfile(const VideoProfile& profile) {
  return FormatResolution(profile.width, profile.height) + L" / "
       + std::to_wstring(profile.fps) + L" fps / "
       + FormatProfileBitrate(profile.bitrate);
}

void UpdatePendingProfileFromIndices() {
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

void InitVideoProfileUiState() {
  g_pendingProfile = CurrentVideoProfile();
  g_resolutionPresets.clear();

  const int width = std::max(640, g_pendingProfile.width);
  const int height = std::max(360, g_pendingProfile.height);
  const bool landscape = width >= height;
  const double longEdge = static_cast<double>(std::max(width, height));
  const double shortEdge = static_cast<double>(std::max(1, std::min(width, height)));
  const double aspect = longEdge / shortEdge;
  const int longEdges[] = {1280, 1600, 1920, 2560, 3840};
  for (int targetLong : longEdges) {
    ResolutionPreset preset{};
    if (landscape) {
      preset.width = ClampEven(targetLong, width);
      preset.height = ClampEven(static_cast<int>(std::lround(targetLong / aspect)), height);
    } else {
      preset.height = ClampEven(targetLong, height);
      preset.width = ClampEven(static_cast<int>(std::lround(targetLong / aspect)), width);
    }
    if (preset.width < 640 || preset.height < 360) continue;
    auto duplicate = std::find_if(g_resolutionPresets.begin(), g_resolutionPresets.end(), [&](const ResolutionPreset& item) {
      return item.width == preset.width && item.height == preset.height;
    });
    if (duplicate == g_resolutionPresets.end()) {
      g_resolutionPresets.push_back(preset);
    }
  }

  auto currentIt = std::find_if(g_resolutionPresets.begin(), g_resolutionPresets.end(), [&](const ResolutionPreset& item) {
    return item.width == g_pendingProfile.width && item.height == g_pendingProfile.height;
  });
  if (currentIt == g_resolutionPresets.end()) {
    g_resolutionPresets.push_back({g_pendingProfile.width, g_pendingProfile.height});
  }

  std::sort(g_resolutionPresets.begin(), g_resolutionPresets.end(), [](const ResolutionPreset& lhs, const ResolutionPreset& rhs) {
    const int64_t lhsPixels = static_cast<int64_t>(lhs.width) * lhs.height;
    const int64_t rhsPixels = static_cast<int64_t>(rhs.width) * rhs.height;
    if (lhsPixels != rhsPixels) return lhsPixels < rhsPixels;
    return lhs.width < rhs.width;
  });

  for (size_t i = 0; i < g_resolutionPresets.size(); ++i) {
    if (g_resolutionPresets[i].width == g_pendingProfile.width && g_resolutionPresets[i].height == g_pendingProfile.height) {
      g_resolutionIndex = static_cast<int>(i);
      break;
    }
  }

  int bestFpsDistance = INT_MAX;
  for (size_t i = 0; i < kFpsPresets.size(); ++i) {
    int distance = std::abs(kFpsPresets[i] - g_pendingProfile.fps);
    if (distance < bestFpsDistance) {
      bestFpsDistance = distance;
      g_fpsIndex = static_cast<int>(i);
    }
  }

  int currentMbps = std::max(1, (g_pendingProfile.bitrate + 500'000) / 1'000'000);
  int bestBitrateDistance = INT_MAX;
  for (size_t i = 0; i < kBitratePresetsMbps.size(); ++i) {
    int distance = std::abs(kBitratePresetsMbps[i] - currentMbps);
    if (distance < bestBitrateDistance) {
      bestBitrateDistance = distance;
      g_bitrateIndex = static_cast<int>(i);
    }
  }

  UpdatePendingProfileFromIndices();
}

void CycleResolution(int delta) {
  if (g_resolutionPresets.empty()) return;
  const int count = static_cast<int>(g_resolutionPresets.size());
  g_resolutionIndex = (g_resolutionIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

void CycleFps(int delta) {
  const int count = static_cast<int>(kFpsPresets.size());
  g_fpsIndex = (g_fpsIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

void CycleBitrate(int delta) {
  const int count = static_cast<int>(kBitratePresetsMbps.size());
  g_bitrateIndex = (g_bitrateIndex + delta + count) % count;
  UpdatePendingProfileFromIndices();
}

int FindResolutionPresetIndex(int width, int height) {
  if (g_resolutionPresets.empty()) return -1;
  for (size_t i = 0; i < g_resolutionPresets.size(); ++i) {
    if (g_resolutionPresets[i].width == width && g_resolutionPresets[i].height == height) {
      return static_cast<int>(i);
    }
  }
  int bestIndex = 0;
  int64_t bestDiff = LLONG_MAX;
  const int64_t targetPixels = static_cast<int64_t>(width) * height;
  for (size_t i = 0; i < g_resolutionPresets.size(); ++i) {
    const int64_t pixels = static_cast<int64_t>(g_resolutionPresets[i].width) * g_resolutionPresets[i].height;
    const int64_t diff = pixels > targetPixels ? (pixels - targetPixels) : (targetPixels - pixels);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

int FindClosestFpsIndex(int fps) {
  int bestIndex = 0;
  int bestDiff = INT_MAX;
  for (size_t i = 0; i < kFpsPresets.size(); ++i) {
    const int diff = std::abs(kFpsPresets[i] - fps);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

int FindClosestBitrateIndex(int bitrate) {
  const int currentMbps = std::max(1, (bitrate + 500'000) / 1'000'000);
  int bestIndex = 0;
  int bestDiff = INT_MAX;
  for (size_t i = 0; i < kBitratePresetsMbps.size(); ++i) {
    const int diff = std::abs(kBitratePresetsMbps[i] - currentMbps);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIndex = static_cast<int>(i);
    }
  }
  return bestIndex;
}

void SyncPendingProfileToIndices(const VideoProfile& profile) {
  g_pendingProfile = profile;
  const int resIndex = FindResolutionPresetIndex(profile.width, profile.height);
  if (resIndex >= 0) g_resolutionIndex = resIndex;
  g_fpsIndex = FindClosestFpsIndex(profile.fps);
  g_bitrateIndex = FindClosestBitrateIndex(profile.bitrate);
}

bool WriteProfileFile(const std::wstring& profileFile, const VideoProfile& profile) {
  if (profileFile.empty()) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, profileFile.c_str(), L"wb") != 0 || !file) return false;
  std::string json = "{\n"
                     "  \"profileVersion\": 2,\n"
                     "  \"width\": " + std::to_string(profile.width) + ",\n"
                     "  \"height\": " + std::to_string(profile.height) + ",\n"
                     "  \"fps\": " + std::to_string(profile.fps) + ",\n"
                     "  \"bitrate\": " + std::to_string(profile.bitrate) + "\n"
                     "}\n";
  const size_t written = fwrite(json.data(), 1, json.size(), file);
  fclose(file);
  return written == json.size();
}

std::wstring PlatformLabel(const std::wstring& platform) {
  if (platform == L"darwin") return L"macOS";
  if (platform == L"win32") return L"Windows";
  if (platform == L"linux") return L"Linux";
  if (!platform.empty() && platform != L"unknown") return platform;
  return L"-";
}

std::wstring DisplayLabel() {
  std::wstring label = L"显示屏 1";
  if (!g_cfg.hostName.empty() && g_cfg.hostName != L"Remote Device") {
    label += L" (" + g_cfg.hostName + L")";
  }
  return label;
}

void ClearPendingVideoQueues() {
  {
    std::lock_guard lk(g_encodedMu);
    if (!g_encodedQueue.empty()) {
      RecordClientFrameDrop(static_cast<uint64_t>(g_encodedQueue.size()));
      g_encodedQueue.clear();
      g_encodedQueueDepthNow.store(0, std::memory_order_relaxed);
    }
  }
  {
    std::lock_guard lk(g_decodedMu);
    if (!g_decodedQueue.empty()) {
      RecordDecodedFrameDrop(static_cast<uint64_t>(g_decodedQueue.size()));
      g_decodedQueue.clear();
      g_decodedQueueDepthNow.store(0, std::memory_order_relaxed);
    }
  }
}

std::wstring FormatElapsed() {
  uint64_t now = QpcNow();
  uint64_t seconds = QpcDeltaUs(g_startedQpc, now) / 1'000'000ull;
  uint64_t h = seconds / 3600;
  uint64_t m = (seconds / 60) % 60;
  uint64_t s = seconds % 60;
  wchar_t buf[32];
  swprintf_s(buf, L"%02llu:%02llu:%02llu",
             static_cast<unsigned long long>(h),
             static_cast<unsigned long long>(m),
             static_cast<unsigned long long>(s));
  return buf;
}

NativeUiStats CurrentUiStats() {
  std::lock_guard lk(g_uiStatsMu);
  return g_uiStats;
}

void StoreUiStats(const NativeUiStats& stats) {
  std::lock_guard lk(g_uiStatsMu);
  g_uiStats = stats;
}
