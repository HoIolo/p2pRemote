#pragma once

#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "p2_protocol.h"

using Microsoft::WRL::ComPtr;

inline constexpr int kMaxUdp = 1500;
inline constexpr int kVideoHeaderBytes = sizeof(P2VideoHeader);
inline constexpr int kMaxVideoFragmentPayload = 1440 - kVideoHeaderBytes;
inline constexpr size_t kMinEncodedQueueDepth = 1;
inline constexpr size_t kMaxEncodedQueueDepth = 2;
inline constexpr std::array<int, 5> kFpsPresets = {30, 45, 60, 90, 120};
inline constexpr std::array<int, 5> kBitratePresetsMbps = {12, 20, 30, 50, 80};

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
  bool udpVideo = false;
  std::wstring profileFile;
};

struct VideoProfile {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrate = 30'000'000;
};

struct ResolutionPreset {
  int width = 0;
  int height = 0;
};

struct EncodedFrame {
  std::vector<uint8_t> bytes;
  uint64_t frameId = 0;
  uint64_t ptsUs = 0;
  uint64_t recvQpc = 0;
  bool keyframe = false;
};

struct BgraFrame {
  std::vector<uint8_t> bytes;
  int width = 0;
  int height = 0;
};

struct Nv12Frame {
  std::vector<uint8_t> bytes;
  int width = 0;
  int height = 0;
  uint64_t frameId = 0;
  uint64_t recvQpc = 0;
};

struct DxgiFrame {
  ComPtr<ID3D11Texture2D> texture;
  UINT subresource = 0;
  int width = 0;
  int height = 0;
  uint64_t frameId = 0;
  uint64_t recvQpc = 0;
};

struct DecodedFrame {
  bool gpu = false;
  DxgiFrame dxgi;
  Nv12Frame nv12;
};

enum class DecodeStatus {
  Frame,
  NeedMoreInput,
  Error,
};

struct NativeUiStats {
  double presentFps = 0.0;
  double completeFps = 0.0;
  double mbps = 0.0;
  double packetRate = 0.0;
  double rxToPresentMs = 0.0;
  double packetAgeMs = 0.0;
  double frameAgeMs = 0.0;
  double presentJitterMs = 0.0;
  double rxToPresentMaxMs = 0.0;
  double queueDropPct = 0.0;
  double networkDropPct = 0.0;
  uint64_t dropped = 0;
  uint64_t clientDropped = 0;
  uint64_t networkDropped = 0;
  uint64_t decodeFails = 0;
  uint64_t gpuRenderFails = 0;
  uint64_t gpuFrames = 0;
  uint64_t cpuFrames = 0;
  uint32_t queueDepth = 0;
  uint32_t queueTarget = 0;
  uint32_t decodedQueueDepth = 0;
  uint32_t decodedQueueTarget = 0;
  uint64_t renderDropped = 0;
  uint64_t fecRecovered = 0;
  uint64_t keyframeRequests = 0;
  int adaptiveBitrate = 0;
};

class D3DRenderer;

extern Config g_cfg;
extern HWND g_hwnd;
extern HWND g_toolbarHwnd;
extern HWND g_menuHwnd;
extern HWND g_statsHwnd;
extern std::atomic<bool> g_running;
extern std::mutex g_encodedMu;
extern std::condition_variable g_encodedCv;
extern std::deque<EncodedFrame> g_encodedQueue;
extern std::mutex g_decodedMu;
extern std::condition_variable g_decodedCv;
extern std::deque<DecodedFrame> g_decodedQueue;
extern std::mutex g_frameMu;
extern BgraFrame g_latestFrame;
extern SOCKET g_inputSock;
extern sockaddr_in g_inputAddr;
extern std::atomic<uint32_t> g_inputSeq;
extern std::atomic<uint64_t> g_framesPresented;
extern LARGE_INTEGER g_qpcFreq;
extern std::atomic<uint64_t> g_lastPresentQpc;
extern std::atomic<uint64_t> g_lastRxToPresentUs;
extern std::atomic<uint64_t> g_lastPresentIntervalUs;
extern std::atomic<uint64_t> g_presentJitterUs;
extern std::atomic<uint64_t> g_maxRxToPresentUs;
extern std::atomic<uint64_t> g_gpuFrames;
extern std::atomic<uint64_t> g_cpuFrames;
extern std::atomic<uint64_t> g_packetsRx;
extern std::atomic<uint64_t> g_bytesRx;
extern std::atomic<uint64_t> g_framesComplete;
extern std::atomic<uint64_t> g_framesDropped;
extern std::atomic<uint64_t> g_clientFramesDropped;
extern std::atomic<uint64_t> g_networkFramesDropped;
extern std::atomic<uint64_t> g_decodeFails;
extern std::atomic<uint64_t> g_gpuRenderFails;
extern std::atomic<uint64_t> g_lastPacketQpc;
extern std::atomic<uint64_t> g_lastCompleteQpc;
extern std::atomic<bool> g_decoderPrimed;
extern std::atomic<bool> g_decoderHasKeyframe;
extern std::atomic<bool> g_waitingForKeyframe;
extern std::atomic<bool> g_loggedFirstDecodedFrame;
extern std::atomic<bool> g_loggedFirstPresentedFrame;
extern std::atomic<uint64_t> g_lastKeyframeRequestQpc;
extern std::atomic<uint64_t> g_keyframeRequests;
extern std::atomic<int> g_currentBitrate;
extern std::atomic<int> g_activeVideoWidth;
extern std::atomic<int> g_activeVideoHeight;
extern std::atomic<int> g_activeVideoFps;
extern std::atomic<int> g_activeVideoBitrate;
extern std::atomic<uint64_t> g_videoProfileGeneration;
extern std::atomic<uint32_t> g_encodedQueueDepthNow;
extern std::atomic<uint32_t> g_encodedQueueTargetNow;
extern std::atomic<uint32_t> g_decodedQueueDepthNow;
extern std::atomic<uint32_t> g_decodedQueueTargetNow;
extern std::atomic<uint64_t> g_renderFramesDropped;
extern std::atomic<uint64_t> g_fecRecoveredFrames;
extern std::atomic<uint64_t> g_lastClientStatsQpc;
extern std::atomic<uint64_t> g_lastProfileApplyQpc;
extern uint64_t g_startedQpc;
extern std::wstring g_localIp;
extern VideoProfile g_pendingProfile;
extern std::vector<ResolutionPreset> g_resolutionPresets;
extern int g_resolutionIndex;
extern int g_fpsIndex;
extern int g_bitrateIndex;
extern int g_exitCode;
extern bool g_toolbarExpanded;
extern std::mutex g_uiStatsMu;
extern NativeUiStats g_uiStats;
extern std::unique_ptr<D3DRenderer> g_renderer;

uint64_t QpcNow();
uint64_t QpcDeltaUs(uint64_t start, uint64_t end);
void RecordClientFrameDrop(uint64_t count = 1);
void RecordDecodedFrameDrop(uint64_t count = 1);
void RecordNetworkFrameDrop(uint64_t count = 1);
void MaybeSendClientStats(bool force);
void MaybeRequestKeyframeRecovery(const wchar_t* reason);
void Log(const wchar_t* fmt, ...);

Config ParseArgs();
std::string WideToUtf8(const std::wstring& s);
std::wstring DetectLocalIpForHost(const std::wstring& hostIp, uint16_t port);

int ClampEven(int value, int fallback = 2);
int DefaultBitrateForPixels(int width, int height, int fallback);
size_t EncodedQueueDepthTarget();
void PushEncoded(EncodedFrame&& f);
size_t DecodedQueueDepthTarget();
void PushDecoded(DecodedFrame&& frame);

VideoProfile ActiveVideoProfile();
void CommitActiveVideoProfile(const VideoProfile& profile);
VideoProfile CurrentVideoProfile();
bool SameVideoProfile(const VideoProfile& lhs, const VideoProfile& rhs);
std::wstring FormatDouble(double value, const wchar_t* suffix, int decimals = 0);
std::wstring FormatBitrate(int bitrate);
std::wstring FormatResolution(int width, int height);
std::wstring FormatProfileBitrate(int bitrate);
std::wstring FormatCompactProfile(const VideoProfile& profile);
void InitVideoProfileUiState();
void CycleResolution(int delta);
void CycleFps(int delta);
void CycleBitrate(int delta);
void SyncPendingProfileToIndices(const VideoProfile& profile);
bool WriteProfileFile(const std::wstring& profileFile, const VideoProfile& profile);
std::wstring PlatformLabel(const std::wstring& platform);
std::wstring DisplayLabel();
void ClearPendingVideoQueues();
std::wstring FormatElapsed();
NativeUiStats CurrentUiStats();
void StoreUiStats(const NativeUiStats& stats);
