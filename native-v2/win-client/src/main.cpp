#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <mmsystem.h>

#include <atomic>
#include <array>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

#include "p2_protocol.h"

using Microsoft::WRL::ComPtr;

static constexpr int kMaxUdp = 1500;
static constexpr int kVideoHeaderBytes = sizeof(P2VideoHeader);
static constexpr int kMaxVideoFragmentPayload = 1440 - kVideoHeaderBytes;
static constexpr size_t kMinEncodedQueueDepth = 18;
static constexpr size_t kMaxEncodedQueueDepth = 120;

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
  int bitrate = 28'000'000;
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

static Config g_cfg;
static HWND g_hwnd = nullptr;
static HWND g_toolbarHwnd = nullptr;
static HWND g_menuHwnd = nullptr;
static HWND g_statsHwnd = nullptr;
static std::atomic<bool> g_running{true};
static std::mutex g_encodedMu;
static std::condition_variable g_encodedCv;
static std::deque<EncodedFrame> g_encodedQueue;
static std::mutex g_decodedMu;
static std::condition_variable g_decodedCv;
static std::deque<DecodedFrame> g_decodedQueue;
static std::mutex g_frameMu;
static BgraFrame g_latestFrame;
static SOCKET g_inputSock = INVALID_SOCKET;
static sockaddr_in g_inputAddr{};
static std::atomic<uint32_t> g_inputSeq{1};
static std::atomic<uint64_t> g_framesPresented{0};
static LARGE_INTEGER g_qpcFreq{};
static std::atomic<uint64_t> g_lastPresentQpc{0};
static std::atomic<uint64_t> g_lastRxToPresentUs{0};
static std::atomic<uint64_t> g_gpuFrames{0};
static std::atomic<uint64_t> g_cpuFrames{0};
static std::atomic<uint64_t> g_packetsRx{0};
static std::atomic<uint64_t> g_bytesRx{0};
static std::atomic<uint64_t> g_framesComplete{0};
static std::atomic<uint64_t> g_framesDropped{0};
static std::atomic<uint64_t> g_clientFramesDropped{0};
static std::atomic<uint64_t> g_networkFramesDropped{0};
static std::atomic<uint64_t> g_decodeFails{0};
static std::atomic<uint64_t> g_gpuRenderFails{0};
static std::atomic<uint64_t> g_lastPacketQpc{0};
static std::atomic<uint64_t> g_lastCompleteQpc{0};
static std::atomic<bool> g_decoderPrimed{false};
static std::atomic<bool> g_waitingForKeyframe{false};
static std::atomic<uint64_t> g_lastKeyframeRequestQpc{0};
static std::atomic<uint64_t> g_keyframeRequests{0};
static std::atomic<int> g_currentBitrate{12'000'000};
static std::atomic<int> g_minAdaptiveBitrate{4'000'000};
static std::atomic<int> g_maxAdaptiveBitrate{20'000'000};
static std::atomic<uint64_t> g_lastBitrateControlQpc{0};
static std::atomic<uint64_t> g_lastBitrateIncreaseQpc{0};
static std::atomic<double> g_recentDropScore{0.0};
static std::atomic<uint64_t> g_lastAutoProfileChangeQpc{0};
static std::atomic<int> g_activeVideoWidth{1920};
static std::atomic<int> g_activeVideoHeight{1080};
static std::atomic<int> g_activeVideoFps{60};
static std::atomic<int> g_activeVideoBitrate{14'000'000};
static std::atomic<uint64_t> g_videoProfileGeneration{0};
static std::atomic<uint32_t> g_encodedQueueDepthNow{0};
static std::atomic<uint32_t> g_encodedQueueTargetNow{0};
static std::atomic<uint32_t> g_decodedQueueDepthNow{0};
static std::atomic<uint32_t> g_decodedQueueTargetNow{4};
static std::atomic<uint64_t> g_renderFramesDropped{0};
static std::atomic<uint64_t> g_lastProfileApplyQpc{0};
static uint64_t g_startedQpc = 0;
static std::wstring g_localIp = L"-";
static VideoProfile g_pendingProfile;
static std::vector<ResolutionPreset> g_resolutionPresets;
static int g_resolutionIndex = 0;
static int g_fpsIndex = 0;
static int g_bitrateIndex = 0;
static int g_exitCode = 0;
static constexpr std::array<int, 5> kFpsPresets = {30, 45, 60, 90, 120};
static constexpr std::array<int, 5> kBitratePresetsMbps = {8, 12, 16, 24, 32};

struct NativeUiStats {
  double presentFps = 0.0;
  double completeFps = 0.0;
  double mbps = 0.0;
  double packetRate = 0.0;
  double rxToPresentMs = 0.0;
  double packetAgeMs = 0.0;
  double frameAgeMs = 0.0;
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
};

static std::mutex g_uiStatsMu;
static NativeUiStats g_uiStats;

static size_t EncodedQueueDepthTarget();
static void EnterVideoRecovery(const wchar_t* reason, bool reduceBitrate = true);
static void SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode);
static void SendVideoBitrateControl(int bitrate, const wchar_t* reason);

static uint64_t QpcNow() {
  LARGE_INTEGER q{};
  QueryPerformanceCounter(&q);
  return static_cast<uint64_t>(q.QuadPart);
}

static uint64_t QpcDeltaUs(uint64_t start, uint64_t end) {
  if (!g_qpcFreq.QuadPart || end <= start) return 0;
  return (end - start) * 1'000'000ull / static_cast<uint64_t>(g_qpcFreq.QuadPart);
}

static void RecordClientFrameDrop(uint64_t count = 1) {
  g_framesDropped.fetch_add(count, std::memory_order_relaxed);
  g_clientFramesDropped.fetch_add(count, std::memory_order_relaxed);
}

static void RecordNetworkFrameDrop(uint64_t count = 1) {
  g_framesDropped.fetch_add(count, std::memory_order_relaxed);
  g_networkFramesDropped.fetch_add(count, std::memory_order_relaxed);
}

static void Log(const wchar_t* fmt, ...) {
  wchar_t buf[1024];
  va_list args;
  va_start(args, fmt);
  _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
  va_end(args);
  OutputDebugStringW(buf);
  OutputDebugStringW(L"\n");
}

static bool StartsWith(const wchar_t* s, const wchar_t* p) {
  return wcsncmp(s, p, wcslen(p)) == 0;
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

static std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
}

static std::wstring DetectLocalIpForHost(const std::wstring& hostIp, uint16_t port) {
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

static void PushEncoded(EncodedFrame&& f) {
  bool queued = false;
  {
    std::lock_guard lk(g_encodedMu);
    const size_t queueDepthTarget = EncodedQueueDepthTarget();
    const size_t protectionThreshold = std::max<size_t>(queueDepthTarget * 9 / 10, queueDepthTarget > 2 ? queueDepthTarget - 2 : queueDepthTarget);
    if (g_waitingForKeyframe.load(std::memory_order_relaxed)) {
      if (!f.keyframe) {
        RecordClientFrameDrop();
        return;
      }
      g_waitingForKeyframe.store(false, std::memory_order_relaxed);
    }

    // Until the decoder has successfully produced a frame, keep the newest
    // keyframe around and drop delta frames that would otherwise starve sync.
    if (!g_decoderPrimed.load(std::memory_order_relaxed) && !f.keyframe) {
      RecordClientFrameDrop();
      return;
    }

    if (!g_encodedQueue.empty()) {
      const EncodedFrame& pending = g_encodedQueue.back();
      if (pending.keyframe && !f.keyframe && g_encodedQueue.size() >= protectionThreshold) {
        RecordClientFrameDrop();
        return;
      }
    }

    while (g_encodedQueue.size() >= queueDepthTarget) {
      g_encodedQueue.pop_front();
      RecordClientFrameDrop();
    }
    g_encodedQueue.emplace_back(std::move(f));
    g_encodedQueueDepthNow.store(static_cast<uint32_t>(g_encodedQueue.size()), std::memory_order_relaxed);
    queued = true;
  }
  if (queued) g_encodedCv.notify_one();
}

class D3DRenderer {
 public:
  bool Init(HWND hwnd, int width, int height) {
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;
    allowTearing_ = CheckTearingSupport();

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device_, &actual, &ctx_);
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(ctx_.As(&multithread))) {
      multithread->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgiDevice))) return false;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allowTearing_) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swap_);
    if (FAILED(hr)) return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap_.As(&swap2))) {
      swap2->SetMaximumFrameLatency(1);
      frameLatencyWaitable_ = swap2->GetFrameLatencyWaitableObject();
    }

    return CreatePipeline() && CreateNv12Textures();
  }

  bool Render(const Nv12Frame& frame) {
    if (!swap_ || !ctx_ || frame.bytes.empty()) return false;
    if (frame.width != width_ || frame.height != height_) return false;
    if (frame.bytes.size() < static_cast<size_t>(width_) * height_ * 3 / 2) return false;

    if (!UploadNv12(frame)) return false;

    UINT presentFlags = allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(0, presentFlags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
    return SUCCEEDED(hr);
  }

  bool Render(const DxgiFrame& frame) {
    if (!swap_ || !ctx_ || !frame.texture) return false;
    if (!RenderTexture(frame.texture.Get(), frame.subresource)) return false;
    UINT presentFlags = allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(0, presentFlags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
    return SUCCEEDED(hr);
  }

  ID3D11Device* Device() const { return device_.Get(); }

  bool Reconfigure(int width, int height) {
    width = std::max(2, width);
    height = std::max(2, height);
    if (width == width_ && height == height_) return true;
    width_ = width;
    height_ = height;
    yTex_.Reset();
    uvTex_.Reset();
    ySrv_.Reset();
    uvSrv_.Reset();
    copyNv12Tex_.Reset();
    copyYSrv_.Reset();
    copyUvSrv_.Reset();
    if (!swap_) return CreateNv12Textures();
    HRESULT hr = swap_->ResizeBuffers(0, static_cast<UINT>(width_), static_cast<UINT>(height_),
                                      DXGI_FORMAT_UNKNOWN,
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                                      (allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    if (FAILED(hr)) return false;
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap_.As(&swap2))) {
      swap2->SetMaximumFrameLatency(1);
      frameLatencyWaitable_ = swap2->GetFrameLatencyWaitableObject();
    }
    return CreateNv12Textures();
  }

  void WaitForPresentReady() {
    if (frameLatencyWaitable_) {
      WaitForSingleObject(frameLatencyWaitable_, 8);
    }
  }

 private:
  bool CheckTearingSupport() {
    ComPtr<IDXGIFactory5> factory5;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) return false;
    BOOL allowTearing = FALSE;
    if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                             &allowTearing, sizeof(allowTearing)))) {
      return false;
    }
    return allowTearing == TRUE;
  }

  bool CompileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
      if (errors) OutputDebugStringA(reinterpret_cast<const char*>(errors->GetBufferPointer()));
      return false;
    }
    return true;
  }

  bool CreatePipeline() {
    static const char* kVs = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
  float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
  float2 uv[3]  = { float2(0.0, 1.0),  float2(0.0, -1.0), float2(2.0, 1.0) };
  VSOut o; o.pos = float4(pos[id], 0.0, 1.0); o.uv = uv[id]; return o;
}
)HLSL";
    static const char* kPs = R"HLSL(
Texture2D<float> yTex : register(t0);
Texture2D<float2> uvTex : register(t1);
SamplerState samp0 : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(VSOut i) : SV_Target {
  float y = yTex.Sample(samp0, i.uv).r;
  float2 uv = uvTex.Sample(samp0, i.uv).rg;
  y = saturate((y - 0.0627451) * 1.1643836);
  float u = uv.x - 0.5;
  float v = uv.y - 0.5;
  float r = y + 1.7927411 * v;
  float g = y - 0.2132486 * u - 0.5329093 * v;
  float b = y + 2.1124018 * u;
  return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)HLSL";
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader(kVs, "main", "vs_5_0", vsBlob)) return false;
    if (!CompileShader(kPs, "main", "ps_5_0", psBlob)) return false;
    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_))) return false;
    if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_))) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MinLOD = 0;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device_->CreateSamplerState(&samp, &sampler_));
  }

  bool CreateNv12Textures() {
    D3D11_TEXTURE2D_DESC yDesc{};
    yDesc.Width = static_cast<UINT>(width_);
    yDesc.Height = static_cast<UINT>(height_);
    yDesc.MipLevels = 1;
    yDesc.ArraySize = 1;
    yDesc.Format = DXGI_FORMAT_R8_UNORM;
    yDesc.SampleDesc.Count = 1;
    yDesc.Usage = D3D11_USAGE_DYNAMIC;
    yDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    yDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&yDesc, nullptr, &yTex_))) return false;

    D3D11_TEXTURE2D_DESC uvDesc = yDesc;
    uvDesc.Width = static_cast<UINT>(width_ / 2);
    uvDesc.Height = static_cast<UINT>(height_ / 2);
    uvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device_->CreateTexture2D(&uvDesc, nullptr, &uvTex_))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC ySrvDesc{};
    ySrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    ySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(yTex_.Get(), &ySrvDesc, &ySrv_))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC uvSrvDesc{};
    uvSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvSrvDesc.Texture2D.MipLevels = 1;
    return SUCCEEDED(device_->CreateShaderResourceView(uvTex_.Get(), &uvSrvDesc, &uvSrv_));
  }

  bool RenderTexture(ID3D11Texture2D* texture, UINT subresource) {
    ComPtr<ID3D11ShaderResourceView> ySrv;
    ComPtr<ID3D11ShaderResourceView> uvSrv;
    D3D11_SHADER_RESOURCE_VIEW_DESC ySrvDesc{};
    ySrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    ySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrvDesc.Texture2D.MipLevels = 1;
    HRESULT hrY = subresource == 0 ? device_->CreateShaderResourceView(texture, &ySrvDesc, &ySrv) : E_FAIL;

    D3D11_SHADER_RESOURCE_VIEW_DESC uvSrvDesc{};
    uvSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvSrvDesc.Texture2D.MipLevels = 1;
    HRESULT hrUv = subresource == 0 ? device_->CreateShaderResourceView(texture, &uvSrvDesc, &uvSrv) : E_FAIL;

    if (FAILED(hrY) || FAILED(hrUv)) {
      if (!EnsureCopyNv12Texture()) return false;
      ctx_->CopySubresourceRegion(copyNv12Tex_.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
      return DrawWithSrvs(copyYSrv_.Get(), copyUvSrv_.Get());
    }

    return DrawWithSrvs(ySrv.Get(), uvSrv.Get());
  }

  bool EnsureCopyNv12Texture() {
    if (copyNv12Tex_ && copyYSrv_ && copyUvSrv_) return true;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &copyNv12Tex_))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC ySrvDesc{};
    ySrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    ySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrvDesc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(copyNv12Tex_.Get(), &ySrvDesc, &copyYSrv_))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC uvSrvDesc{};
    uvSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    uvSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    uvSrvDesc.Texture2D.MipLevels = 1;
    return SUCCEEDED(device_->CreateShaderResourceView(copyNv12Tex_.Get(), &uvSrvDesc, &copyUvSrv_));
  }

  bool DrawWithSrvs(ID3D11ShaderResourceView* ySrv, ID3D11ShaderResourceView* uvSrv) {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
    if (FAILED(hr)) return false;

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { ySrv, uvSrv };
    ctx_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[1] = { sampler_.Get() };
    ctx_->PSSetSamplers(0, 1, samplers);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ctx_->PSSetShaderResources(0, 2, nullSrvs);
    return true;
  }

  bool UploadNv12(const Nv12Frame& frame) {
    const uint8_t* ySrc = frame.bytes.data();
    const uint8_t* uvSrc = frame.bytes.data() + static_cast<size_t>(width_) * height_;

    D3D11_MAPPED_SUBRESOURCE yMap{};
    if (FAILED(ctx_->Map(yTex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &yMap))) return false;
    for (int row = 0; row < height_; ++row) {
      memcpy(static_cast<uint8_t*>(yMap.pData) + static_cast<size_t>(row) * yMap.RowPitch,
             ySrc + static_cast<size_t>(row) * width_, width_);
    }
    ctx_->Unmap(yTex_.Get(), 0);

    D3D11_MAPPED_SUBRESOURCE uvMap{};
    if (FAILED(ctx_->Map(uvTex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &uvMap))) return false;
    for (int row = 0; row < height_ / 2; ++row) {
      memcpy(static_cast<uint8_t*>(uvMap.pData) + static_cast<size_t>(row) * uvMap.RowPitch,
             uvSrc + static_cast<size_t>(row) * width_, width_);
    }
    ctx_->Unmap(uvTex_.Get(), 0);
    return DrawWithSrvs(ySrv_.Get(), uvSrv_.Get());
  }

  HWND hwnd_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool allowTearing_ = false;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> ps_;
  ComPtr<ID3D11SamplerState> sampler_;
  ComPtr<ID3D11Texture2D> yTex_;
  ComPtr<ID3D11Texture2D> uvTex_;
  ComPtr<ID3D11ShaderResourceView> ySrv_;
  ComPtr<ID3D11ShaderResourceView> uvSrv_;
  ComPtr<ID3D11Texture2D> copyNv12Tex_;
  ComPtr<ID3D11ShaderResourceView> copyYSrv_;
  ComPtr<ID3D11ShaderResourceView> copyUvSrv_;
  HANDLE frameLatencyWaitable_ = nullptr;
};

static std::unique_ptr<D3DRenderer> g_renderer;

static constexpr int kToolbarWidth = 560;
static constexpr int kToolbarHeight = 60;
static constexpr int kMenuWidth = 440;
static constexpr int kMenuHeight = 486;
static constexpr int kStatsWidth = 580;
static constexpr int kStatsHeight = 862;
static constexpr wchar_t kNativeClientVersion[] = L"native v2 0.1.0";

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static HFONT CreateUiFont(int px, int weight = FW_NORMAL) {
  return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static std::wstring FormatDouble(double value, const wchar_t* suffix, int decimals = 0) {
  wchar_t buf[64];
  if (decimals <= 0) swprintf_s(buf, L"%.0f%s", value, suffix);
  else swprintf_s(buf, L"%.*f%s", decimals, value, suffix);
  return buf;
}

static std::wstring FormatBitrate(int bitrate) {
  if (bitrate <= 0) return L"-";
  return FormatDouble(double(bitrate) / 1'000'000.0, L" Mbps", 1);
}

static int ClampEven(int value, int fallback = 2) {
  int number = std::max(2, static_cast<int>(std::lround(value)));
  return (number % 2) == 0 ? number : number - 1;
}

static int AutoBitrateForPixels(int width, int height, int fallback) {
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

static size_t EncodedQueueDepthTarget() {
  const int fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
  size_t target = static_cast<size_t>(std::clamp((fps + 3) / 4, 8, 26));
  const uint64_t lastProfileApply = g_lastProfileApplyQpc.load(std::memory_order_relaxed);
  const uint64_t now = QpcNow();
  if (lastProfileApply && QpcDeltaUs(lastProfileApply, now) < 2'500'000) {
    target += 12;
  }
  target = std::max(kMinEncodedQueueDepth, target);
  target = std::min(kMaxEncodedQueueDepth, target);
  g_encodedQueueTargetNow.store(static_cast<uint32_t>(target), std::memory_order_relaxed);
  return target;
}

static size_t DecodedQueueDepthTarget() {
  const int fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
  size_t target = static_cast<size_t>(std::clamp((fps + 14) / 15, 2, 6));
  g_decodedQueueTargetNow.store(static_cast<uint32_t>(target), std::memory_order_relaxed);
  return target;
}

static void PushDecoded(DecodedFrame&& frame) {
  bool queued = false;
  {
    std::lock_guard lk(g_decodedMu);
    const size_t queueTarget = DecodedQueueDepthTarget();
    while (g_decodedQueue.size() >= queueTarget) {
      g_decodedQueue.pop_front();
      g_renderFramesDropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_decodedQueue.emplace_back(std::move(frame));
    g_decodedQueueDepthNow.store(static_cast<uint32_t>(g_decodedQueue.size()), std::memory_order_relaxed);
    queued = true;
  }
  if (queued) g_decodedCv.notify_one();
}

static VideoProfile ActiveVideoProfile() {
  VideoProfile profile;
  profile.width = std::max(640, g_activeVideoWidth.load(std::memory_order_relaxed));
  profile.height = std::max(360, g_activeVideoHeight.load(std::memory_order_relaxed));
  profile.fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
  const int bitrate = g_activeVideoBitrate.load(std::memory_order_relaxed);
  profile.bitrate = bitrate > 0 ? bitrate : AutoBitrateForPixels(profile.width, profile.height, 14'000'000);
  return profile;
}

static void CommitActiveVideoProfile(const VideoProfile& profile) {
  const int width = std::max(640, ClampEven(profile.width, g_cfg.width));
  const int height = std::max(360, ClampEven(profile.height, g_cfg.height));
  const int fps = std::clamp(profile.fps, 30, 240);
  const int fallbackBitrate = AutoBitrateForPixels(width, height, 14'000'000);
  const int bitrate = std::clamp(profile.bitrate > 0 ? profile.bitrate : fallbackBitrate, 2'000'000, 80'000'000);
  g_activeVideoWidth.store(width, std::memory_order_relaxed);
  g_activeVideoHeight.store(height, std::memory_order_relaxed);
  g_activeVideoFps.store(fps, std::memory_order_relaxed);
  g_activeVideoBitrate.store(bitrate, std::memory_order_relaxed);
  g_currentBitrate.store(bitrate, std::memory_order_relaxed);
  const int adaptiveCeiling = std::max({bitrate, AutoBitrateForPixels(width, height, bitrate), 20'000'000});
  g_maxAdaptiveBitrate.store(std::min(80'000'000, adaptiveCeiling), std::memory_order_relaxed);
  g_minAdaptiveBitrate.store(std::max(2'000'000, bitrate / 3), std::memory_order_relaxed);
}

static VideoProfile CurrentVideoProfile() {
  return ActiveVideoProfile();
}

static bool SameVideoProfile(const VideoProfile& lhs, const VideoProfile& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fps == rhs.fps && lhs.bitrate == rhs.bitrate;
}

static std::wstring FormatResolution(int width, int height) {
  wchar_t buf[64];
  swprintf_s(buf, L"%dx%d", width, height);
  return buf;
}

static std::wstring FormatProfileBitrate(int bitrate) {
  if (bitrate <= 0) return L"自动";
  wchar_t buf[32];
  swprintf_s(buf, L"%d Mbps", std::max(1, (bitrate + 500'000) / 1'000'000));
  return buf;
}

static std::wstring FormatCompactProfile(const VideoProfile& profile) {
  return FormatResolution(profile.width, profile.height) + L" / "
       + std::to_wstring(profile.fps) + L" fps / "
       + FormatProfileBitrate(profile.bitrate);
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

static void InitVideoProfileUiState() {
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

static int FindResolutionPresetIndex(int width, int height) {
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

static int FindClosestFpsIndex(int fps) {
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

static int FindClosestBitrateIndex(int bitrate) {
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

static void SyncPendingProfileToIndices(const VideoProfile& profile) {
  g_pendingProfile = profile;
  const int resIndex = FindResolutionPresetIndex(profile.width, profile.height);
  if (resIndex >= 0) g_resolutionIndex = resIndex;
  g_fpsIndex = FindClosestFpsIndex(profile.fps);
  g_bitrateIndex = FindClosestBitrateIndex(profile.bitrate);
}

static bool WriteProfileFile(const std::wstring& profileFile, const VideoProfile& profile) {
  if (profileFile.empty()) return false;
  FILE* file = nullptr;
  if (_wfopen_s(&file, profileFile.c_str(), L"wb") != 0 || !file) return false;
  std::string json = "{\n"
                     "  \"width\": " + std::to_string(profile.width) + ",\n"
                     "  \"height\": " + std::to_string(profile.height) + ",\n"
                     "  \"fps\": " + std::to_string(profile.fps) + ",\n"
                     "  \"bitrate\": " + std::to_string(profile.bitrate) + "\n"
                     "}\n";
  const size_t written = fwrite(json.data(), 1, json.size(), file);
  fclose(file);
  return written == json.size();
}

static std::wstring PlatformLabel(const std::wstring& platform) {
  if (platform == L"darwin") return L"macOS";
  if (platform == L"win32") return L"Windows";
  if (platform == L"linux") return L"Linux";
  if (!platform.empty() && platform != L"unknown") return platform;
  return L"-";
}

static std::wstring DisplayLabel() {
  std::wstring label = L"显示屏 1";
  if (!g_cfg.hostName.empty() && g_cfg.hostName != L"Remote Device") {
    label += L" (" + g_cfg.hostName + L")";
  }
  return label;
}

static std::wstring FormatElapsed() {
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

static NativeUiStats CurrentUiStats() {
  std::lock_guard lk(g_uiStatsMu);
  return g_uiStats;
}

static void StoreUiStats(const NativeUiStats& stats) {
  std::lock_guard lk(g_uiStatsMu);
  g_uiStats = stats;
}

static void DrawTextRect(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, HFONT font, UINT format) {
  HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
  SetTextColor(hdc, color);
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, text.c_str(), -1, &rc, format);
  SelectObject(hdc, oldFont);
}

static void DrawSignalBars(HDC hdc, int x, int y, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, brush));
  HPEN pen = CreatePen(PS_SOLID, 1, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  const int widths[] = {5, 5, 5};
  const int heights[] = {14, 23, 32};
  for (int i = 0; i < 3; ++i) {
    int left = x + i * 12;
    RoundRect(hdc, left, y + 34 - heights[i], left + widths[i], y + 34, 5, 5);
  }
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(pen);
  DeleteObject(brush);
}

static void DrawMenuIcon(HDC hdc, int kind, int x, int y, COLORREF color) {
  HPEN pen = CreatePen(PS_SOLID, 2, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));

  switch (kind) {
    case 0:
      RoundRect(hdc, x + 2, y + 3, x + 24, y + 20, 4, 4);
      MoveToEx(hdc, x + 13, y + 20, nullptr); LineTo(hdc, x + 13, y + 25);
      MoveToEx(hdc, x + 8, y + 25, nullptr); LineTo(hdc, x + 18, y + 25);
      break;
    case 1:
      Rectangle(hdc, x + 3, y + 4, x + 24, y + 23);
      MoveToEx(hdc, x + 6, y + 17, nullptr); LineTo(hdc, x + 12, y + 11); LineTo(hdc, x + 16, y + 15); LineTo(hdc, x + 22, y + 8);
      Ellipse(hdc, x + 7, y + 6, x + 12, y + 11);
      break;
    case 2:
      RoundRect(hdc, x + 3, y + 3, x + 24, y + 24, 4, 4);
      MoveToEx(hdc, x + 9, y + 3, nullptr); LineTo(hdc, x + 9, y + 10); LineTo(hdc, x + 3, y + 10);
      MoveToEx(hdc, x + 18, y + 24, nullptr); LineTo(hdc, x + 18, y + 17); LineTo(hdc, x + 24, y + 17);
      break;
    case 3:
      MoveToEx(hdc, x + 4, y + 14, nullptr); LineTo(hdc, x + 10, y + 14); LineTo(hdc, x + 17, y + 7); LineTo(hdc, x + 17, y + 21); LineTo(hdc, x + 10, y + 14);
      Arc(hdc, x + 12, y + 7, x + 28, y + 21, x + 22, y + 8, x + 22, y + 20);
      break;
    case 4: {
      POINT pts[] = {{x + 14, y + 2}, {x + 24, y + 6}, {x + 21, y + 20}, {x + 14, y + 26}, {x + 7, y + 20}, {x + 4, y + 6}};
      Polygon(hdc, pts, ARRAYSIZE(pts));
      MoveToEx(hdc, x + 10, y + 14, nullptr); LineTo(hdc, x + 13, y + 17); LineTo(hdc, x + 19, y + 10);
      break;
    }
    case 5:
      MoveToEx(hdc, x + 14, y + 3, nullptr); LineTo(hdc, x + 14, y + 23);
      MoveToEx(hdc, x + 8, y + 10, nullptr); LineTo(hdc, x + 14, y + 16); LineTo(hdc, x + 21, y + 9);
      Ellipse(hdc, x + 11, y + 21, x + 17, y + 27);
      Rectangle(hdc, x + 18, y + 6, x + 24, y + 12);
      break;
    case 6:
      for (int yy = 0; yy < 2; ++yy) for (int xx = 0; xx < 2; ++xx) {
        RoundRect(hdc, x + 3 + xx * 12, y + 4 + yy * 12, x + 11 + xx * 12, y + 12 + yy * 12, 3, 3);
      }
      break;
    case 7:
      MoveToEx(hdc, x + 4, y + 11, nullptr); LineTo(hdc, x + 4, y + 4); LineTo(hdc, x + 11, y + 4);
      MoveToEx(hdc, x + 17, y + 4, nullptr); LineTo(hdc, x + 24, y + 4); LineTo(hdc, x + 24, y + 11);
      MoveToEx(hdc, x + 24, y + 17, nullptr); LineTo(hdc, x + 24, y + 24); LineTo(hdc, x + 17, y + 24);
      MoveToEx(hdc, x + 11, y + 24, nullptr); LineTo(hdc, x + 4, y + 24); LineTo(hdc, x + 4, y + 17);
      break;
    case 8:
      MoveToEx(hdc, x + 3, y + 5, nullptr); LineTo(hdc, x + 15, y + 5); MoveToEx(hdc, x + 3, y + 23, nullptr); LineTo(hdc, x + 15, y + 23);
      MoveToEx(hdc, x + 3, y + 5, nullptr); LineTo(hdc, x + 3, y + 23);
      MoveToEx(hdc, x + 11, y + 14, nullptr); LineTo(hdc, x + 25, y + 14);
      MoveToEx(hdc, x + 20, y + 9, nullptr); LineTo(hdc, x + 25, y + 14); LineTo(hdc, x + 20, y + 19);
      break;
  }

  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
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
  RECT r = mi.rcMonitor;
  const int minX = static_cast<int>(r.left + 12);
  const int maxX = static_cast<int>(r.right - width - 12);
  const int minY = static_cast<int>(r.top + 12);
  const int maxY = static_cast<int>(r.bottom - height - 12);
  x = std::max(minX, std::min(x, maxX));
  y = std::max(minY, std::min(y, maxY));
}

static void UpdateOverlayLayout() {
  if (!g_hwnd || !g_toolbarHwnd) return;
  RECT owner{};
  GetWindowRect(g_hwnd, &owner);
  int ownerW = owner.right - owner.left;
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

static void HideNativePopups() {
  if (g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static void ShowOnlyPopup(HWND hwnd) {
  if (g_menuHwnd && hwnd != g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd && hwnd != g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  if (hwnd) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateOverlayLayout();
    InvalidateRect(hwnd, nullptr, TRUE);
  }
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static void TogglePopup(HWND hwnd) {
  if (!hwnd) return;
  if (IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_HIDE);
  } else {
    ShowOnlyPopup(hwnd);
  }
  if (g_hwnd) SetFocus(g_hwnd);
}

static void SetNativeFullscreen(bool fullScreen) {
  if (!g_hwnd || g_cfg.fullscreen == fullScreen) return;
  g_cfg.fullscreen = fullScreen;
  HideNativePopups();

  if (fullScreen) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int x = std::max(40, GetSystemMetrics(SM_CXSCREEN) / 2 - 640);
    int y = std::max(40, GetSystemMetrics(SM_CYSCREEN) / 2 - 380);
    SetWindowPos(g_hwnd, HWND_TOP, x, y, 1280, 760,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  UpdateOverlayLayout();
}

static void ToggleNativeFullscreen() {
  SetNativeFullscreen(!g_cfg.fullscreen);
}

static void DrawMiniChevron(HDC hdc, int centerX, int centerY, bool left, COLORREF color) {
  HPEN pen = CreatePen(PS_SOLID, 2, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  if (left) {
    MoveToEx(hdc, centerX + 3, centerY - 6, nullptr); LineTo(hdc, centerX - 3, centerY); LineTo(hdc, centerX + 3, centerY + 6);
  } else {
    MoveToEx(hdc, centerX - 3, centerY - 6, nullptr); LineTo(hdc, centerX + 3, centerY); LineTo(hdc, centerX - 3, centerY + 6);
  }
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawSelectorButton(HDC hdc, RECT rc, bool left) {
  HBRUSH fill = CreateSolidBrush(RGB(226, 232, 236));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(210, 217, 222));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, fill));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(fill);
  DrawMiniChevron(hdc, (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, left, RGB(28, 37, 48));
}

static void DrawToolbar(HDC hdc, RECT rc) {
  const NativeUiStats stats = CurrentUiStats();
  HBRUSH bg = CreateSolidBrush(RGB(249, 251, 253));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(219, 225, 232));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 24, 24);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  HBRUSH closeFill = CreateSolidBrush(RGB(242, 245, 248));
  HPEN closeBorder = CreatePen(PS_SOLID, 1, RGB(224, 229, 235));
  HBRUSH oldCloseFill = reinterpret_cast<HBRUSH>(SelectObject(hdc, closeFill));
  HPEN oldCloseBorder = reinterpret_cast<HPEN>(SelectObject(hdc, closeBorder));
  Ellipse(hdc, 14, 12, 48, 46);
  SelectObject(hdc, oldCloseBorder);
  SelectObject(hdc, oldCloseFill);
  DeleteObject(closeBorder);
  DeleteObject(closeFill);

  HPEN ink = CreatePen(PS_SOLID, 2, RGB(22, 30, 40));
  HPEN oldInk = reinterpret_cast<HPEN>(SelectObject(hdc, ink));
  MoveToEx(hdc, 25, 22, nullptr); LineTo(hdc, 37, 34);
  MoveToEx(hdc, 37, 22, nullptr); LineTo(hdc, 25, 34);
  SelectObject(hdc, oldInk);
  DeleteObject(ink);

  HBRUSH controlPill = CreateSolidBrush(RGB(236, 241, 246));
  HBRUSH oldControlPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, controlPill));
  HPEN controlPen = CreatePen(PS_SOLID, 1, RGB(218, 225, 233));
  HPEN oldControlPen = reinterpret_cast<HPEN>(SelectObject(hdc, controlPen));
  RoundRect(hdc, 64, 8, 274, 52, 18, 18);
  SelectObject(hdc, oldControlPen);
  SelectObject(hdc, oldControlPill);
  DeleteObject(controlPen);
  DeleteObject(controlPill);

  HPEN controlInk = CreatePen(PS_SOLID, 2, RGB(24, 33, 44));
  HPEN oldControlInk = reinterpret_cast<HPEN>(SelectObject(hdc, controlInk));
  RoundRect(hdc, 82, 16, 104, 24, 4, 4);
  RoundRect(hdc, 82, 31, 104, 39, 4, 4);
  MoveToEx(hdc, 87, 20, nullptr); LineTo(hdc, 99, 20);
  MoveToEx(hdc, 87, 35, nullptr); LineTo(hdc, 99, 35);
  SelectObject(hdc, oldControlInk);
  DeleteObject(controlInk);

  HFONT controlFont = CreateUiFont(16, FW_SEMIBOLD);
  HFONT controlSubFont = CreateUiFont(12, FW_NORMAL);
  RECT controlRc{114, 11, 262, 30};
  RECT controlSubRc{114, 28, 262, 46};
  const VideoProfile activeProfile = ActiveVideoProfile();
  std::wstring controlSummary = FormatResolution(activeProfile.width, activeProfile.height)
                              + L" / " + std::to_wstring(activeProfile.fps) + L" fps / "
                              + FormatBitrate(g_currentBitrate.load(std::memory_order_relaxed));
  DrawTextRect(hdc, L"显示控制", controlRc, RGB(18, 24, 34), controlFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, controlSummary, controlSubRc, RGB(104, 114, 126), controlSubFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(controlFont);
  DeleteObject(controlSubFont);

  HBRUSH pill = CreateSolidBrush(RGB(236, 245, 240));
  HBRUSH oldPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, pill));
  HPEN noPen = CreatePen(PS_SOLID, 1, RGB(210, 228, 217));
  HPEN oldNoPen = reinterpret_cast<HPEN>(SelectObject(hdc, noPen));
  RoundRect(hdc, 286, 8, 430, 52, 18, 18);
  SelectObject(hdc, oldNoPen);
  SelectObject(hdc, oldPill);
  DeleteObject(noPen);
  DeleteObject(pill);

  DrawSignalBars(hdc, 304, 14, RGB(17, 190, 122));

  wchar_t statsText[96];
  if (stats.rxToPresentMs > 0.0 || stats.mbps > 0.0) {
    swprintf_s(statsText, L"%s · %.0f ms · %.1f Mbps", g_cfg.udpVideo ? L"UDP" : L"TCP", stats.rxToPresentMs, stats.mbps);
  } else {
    swprintf_s(statsText, L"%s 直连", g_cfg.udpVideo ? L"UDP" : L"TCP");
  }
  HFONT statsFont = CreateUiFont(14, FW_SEMIBOLD);
  HFONT statsSubFont = CreateUiFont(11, FW_NORMAL);
  RECT statsRc{346, 11, 418, 28};
  RECT statsSubRc{346, 28, 418, 45};
  DrawTextRect(hdc, L"链路概览", statsRc, RGB(24, 92, 52), statsFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, statsText, statsSubRc, RGB(72, 118, 90), statsSubFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(statsFont);
  DeleteObject(statsSubFont);

  HBRUSH timerFill = CreateSolidBrush(RGB(243, 246, 250));
  HPEN timerBorder = CreatePen(PS_SOLID, 1, RGB(222, 228, 235));
  HBRUSH oldTimerFill = reinterpret_cast<HBRUSH>(SelectObject(hdc, timerFill));
  HPEN oldTimerBorder = reinterpret_cast<HPEN>(SelectObject(hdc, timerBorder));
  RoundRect(hdc, 442, 8, 546, 52, 18, 18);
  SelectObject(hdc, oldTimerBorder);
  SelectObject(hdc, oldTimerFill);
  DeleteObject(timerBorder);
  DeleteObject(timerFill);

  HFONT timeFont = CreateUiFont(18, FW_SEMIBOLD);
  HFONT timeSubFont = CreateUiFont(11, FW_NORMAL);
  RECT timeRc{454, 14, 534, 34};
  RECT timeSubRc{454, 30, 534, 46};
  DrawTextRect(hdc, FormatElapsed(), timeRc, RGB(48, 58, 72), timeFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, L"本次会话", timeSubRc, RGB(123, 133, 144), timeSubFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(timeFont);
  DeleteObject(timeSubFont);
}

static void DrawMenuRow(HDC hdc, int y, int icon, const std::wstring& label, const std::wstring& value, bool chevron, bool danger = false) {
  COLORREF ink = danger ? RGB(232, 62, 52) : RGB(28, 37, 48);
  COLORREF subtle = danger ? RGB(232, 62, 52) : RGB(120, 130, 142);
  DrawMenuIcon(hdc, icon, 24, y + 9, ink);
  HFONT labelFont = CreateUiFont(17, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(13, FW_NORMAL);
  RECT labelRc{62, y + 6, 250, y + 34};
  DrawTextRect(hdc, label, labelRc, ink, labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (!value.empty()) {
    RECT valueRc{214, y + 9, chevron ? 390 : 412, y + 34};
    DrawTextRect(hdc, value, valueRc, subtle, valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  if (chevron) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(26, 34, 44));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 402, y + 14, nullptr); LineTo(hdc, 410, y + 21); LineTo(hdc, 402, y + 28);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
  }
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawMenuSelectorRow(HDC hdc, int y, int icon, const std::wstring& label, const std::wstring& value) {
  DrawMenuIcon(hdc, icon, 24, y + 9, RGB(28, 37, 48));
  HFONT labelFont = CreateUiFont(17, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(13, FW_NORMAL);
  RECT labelRc{62, y + 6, 180, y + 34};
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

static void DrawSeparator(HDC hdc, int y) {
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(216, 222, 226));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 28, y, nullptr);
  LineTo(hdc, kMenuWidth - 28, y);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawStatsSeparator(HDC hdc, int y) {
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(232, 235, 238));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 32, y, nullptr);
  LineTo(hdc, kStatsWidth - 32, y);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawMenu(HDC hdc, RECT rc) {
  const NativeUiStats stats = CurrentUiStats();
  const VideoProfile activeProfile = CurrentVideoProfile();
  const bool pendingChanges = !SameVideoProfile(activeProfile, g_pendingProfile);

  HBRUSH bg = CreateSolidBrush(RGB(250, 252, 254));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(214, 221, 228));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  DrawMenuRow(hdc, 18, 0, DisplayLabel(), FormatCompactProfile(activeProfile), false);
  DrawSeparator(hdc, 76);
  DrawMenuSelectorRow(hdc, 94, 0, L"分辨率", FormatResolution(g_pendingProfile.width, g_pendingProfile.height));
  DrawMenuSelectorRow(hdc, 148, 1, L"帧率", std::to_wstring(g_pendingProfile.fps) + L" fps");
  DrawMenuSelectorRow(hdc, 202, 4, L"码率", FormatProfileBitrate(g_pendingProfile.bitrate));
  DrawSeparator(hdc, 258);
  DrawMenuRow(hdc, 276, 6, L"立即应用", pendingChanges ? FormatCompactProfile(g_pendingProfile) : L"当前已生效", false);
  wchar_t statsValue[64];
  if (stats.presentFps > 0.1 || stats.rxToPresentMs > 0.0) {
    swprintf_s(statsValue, L"%.0f fps / %.0f ms", stats.presentFps, stats.rxToPresentMs);
  } else {
    swprintf_s(statsValue, L"%s", g_cfg.udpVideo ? L"UDP 直连" : L"TCP 直连");
  }
  DrawMenuRow(hdc, 330, 5, L"链路统计", statsValue, true);
  DrawMenuRow(hdc, 384, 7, g_cfg.fullscreen ? L"退出全屏幕" : L"进入全屏幕", L"F11", false);
  DrawSeparator(hdc, 438);
  DrawMenuRow(hdc, 448, 8, L"退出远控", L"", false, true);
}

static void DrawStatsRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value) {
  HFONT labelFont = CreateUiFont(16, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(18, FW_NORMAL);
  RECT labelRc{32, y, 205, y + 32};
  RECT valueRc{232, y, kStatsWidth - 32, y + 32};
  DrawTextRect(hdc, label, labelRc, RGB(146, 151, 160), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, value, valueRc, RGB(18, 24, 36), valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawStats(HDC hdc, RECT rc) {
  NativeUiStats stats = CurrentUiStats();
  HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(220, 224, 228));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  DrawSignalBars(hdc, 38, 34, RGB(17, 190, 122));
  HFONT titleFont = CreateUiFont(20, FW_BOLD);
  std::wstring latency = stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms";
  RECT titleRc{92, 30, kStatsWidth - 30, 72};
  DrawTextRect(hdc, L"显示尾延时: " + latency, titleRc, RGB(15, 22, 36), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(titleFont);

  HPEN line = CreatePen(PS_SOLID, 1, RGB(232, 235, 238));
  HPEN oldLine = reinterpret_cast<HPEN>(SelectObject(hdc, line));
  MoveToEx(hdc, 0, 100, nullptr); LineTo(hdc, kStatsWidth, 100);
  SelectObject(hdc, oldLine);
  DeleteObject(line);

  DrawStatsRow(hdc, 126, L"收帧后延时:", stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms");
  const VideoProfile activeProfile = ActiveVideoProfile();
  DrawStatsRow(hdc, 166, L"显示帧率:", stats.presentFps > 0.1 ? FormatDouble(stats.presentFps, L"", 0) : FormatDouble(activeProfile.fps, L"", 0));
  DrawStatsRow(hdc, 206, L"接收完整帧率:", stats.completeFps > 0.1 ? FormatDouble(stats.completeFps, L"", 0) : L"--");
  DrawStatsRow(hdc, 246, L"带宽占用:", FormatDouble(stats.mbps, L" Mbps", 1));
  DrawStatsRow(hdc, 286, L"客户端丢旧帧(当前):", FormatDouble(stats.queueDropPct, L"%", 1));

  DrawStatsSeparator(hdc, 332);
  DrawStatsRow(hdc, 356, L"传输通道:", g_cfg.udpVideo ? L"UDP 局域网直连" : L"TCP 局域网直连");
  DrawStatsRow(hdc, 396, L"被控端 IP:", g_cfg.hostIp);
  DrawStatsRow(hdc, 436, L"控制端 IP:", g_localIp);
  DrawStatsRow(hdc, 476, L"网络拼帧废弃(当前):", FormatDouble(stats.networkDropPct, L"%", 1));
  DrawStatsRow(hdc, 516, L"被控端系统:", PlatformLabel(g_cfg.hostPlatform));

  DrawStatsSeparator(hdc, 562);
  DrawStatsRow(hdc, 586, L"当前发送码率:", FormatBitrate(g_currentBitrate.load(std::memory_order_relaxed)));
  DrawStatsRow(hdc, 620, L"编码队列:", std::to_wstring(stats.queueDepth) + L" / " + std::to_wstring(stats.queueTarget) + L" 帧");
  DrawStatsRow(hdc, 654, L"显示队列:", std::to_wstring(stats.decodedQueueDepth) + L" / " + std::to_wstring(stats.decodedQueueTarget) + L" 帧");
  DrawStatsRow(hdc, 688, L"显示丢旧帧:", std::to_wstring(stats.renderDropped));
  DrawStatsRow(hdc, 722, L"编解码器:", L"H.264 / Media Foundation");
  DrawStatsRow(hdc, 756, L"编码模式:", stats.gpuFrames > 0 ? L"硬编 / 硬解" : L"硬编 / 硬解优先");
  DrawStatsRow(hdc, 790, L"采集方式:", g_cfg.hostPlatform == L"win32" ? L"DXGI" : L"ScreenCaptureKit");
  wchar_t target[128];
  swprintf_s(target, L"%dx%d @ %d fps / %d Mbps",
             activeProfile.width,
             activeProfile.height,
             activeProfile.fps,
             std::max(1, (activeProfile.bitrate + 500'000) / 1'000'000));
  DrawStatsRow(hdc, 824, L"目标档位:", target);
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

static void SendVideoProfileCommand(const VideoProfile& profile) {
  const uint16_t bitrateMbps = static_cast<uint16_t>(std::clamp((profile.bitrate + 500'000) / 1'000'000, 1, 200));
  for (int i = 0; i < 3; ++i) {
    SendInputPacket(P2_INPUT_SET_VIDEO_PROFILE, 0, 0, profile.width, profile.height,
                    static_cast<uint16_t>(std::clamp(profile.fps, 30, 240)), bitrateMbps);
    if (i < 2) Sleep(15);
  }
}

static bool RequestProfileApply(const VideoProfile& requestedProfile, const wchar_t* reason, bool autoTriggered = false) {
  VideoProfile profile = requestedProfile;
  profile.width = ClampEven(profile.width, g_cfg.width);
  profile.height = ClampEven(profile.height, g_cfg.height);
  profile.fps = std::clamp(profile.fps, 30, 240);
  profile.bitrate = std::clamp(profile.bitrate, 2'000'000, 80'000'000);
  const VideoProfile activeProfile = CurrentVideoProfile();
  if (SameVideoProfile(activeProfile, profile)) {
    if (!autoTriggered) HideNativePopups();
    return false;
  }

  SyncPendingProfileToIndices(profile);
  if (!autoTriggered && !WriteProfileFile(g_cfg.profileFile, profile)) {
    if (autoTriggered) {
      Log(L"auto profile apply save failed: %dx%d@%d bitrate=%d (%s)",
          profile.width, profile.height, profile.fps, profile.bitrate, reason ? reason : L"auto");
    } else {
      MessageBoxW(g_hwnd, L"Failed to save the updated native-v2 profile.", L"P2P Native", MB_ICONERROR);
    }
    return false;
  }

  if (autoTriggered) {
    g_lastAutoProfileChangeQpc.store(QpcNow(), std::memory_order_relaxed);
  }
  g_lastProfileApplyQpc.store(QpcNow(), std::memory_order_relaxed);
  SendVideoProfileCommand(profile);
  CommitActiveVideoProfile(profile);
  g_videoProfileGeneration.fetch_add(1, std::memory_order_relaxed);
  Log(L"profile apply requested: %dx%d@%d bitrate=%d (%s)",
      profile.width, profile.height, profile.fps, profile.bitrate, reason ? reason : L"manual");
  EnterVideoRecovery(L"profile changed", false);
  HideNativePopups();
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
  if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
  if (g_statsHwnd) InvalidateRect(g_statsHwnd, nullptr, FALSE);
  return true;
}

static bool BuildLowerAutoProfile(const VideoProfile& activeProfile, VideoProfile& nextProfile) {
  nextProfile = activeProfile;
  const int liveBitrate = std::max(g_minAdaptiveBitrate.load(std::memory_order_relaxed),
                                   g_currentBitrate.load(std::memory_order_relaxed));
  nextProfile.bitrate = liveBitrate;

  const int fpsIndex = FindClosestFpsIndex(activeProfile.fps);
  if (fpsIndex > 0) {
    nextProfile.fps = kFpsPresets[fpsIndex - 1];
    return true;
  }
  return false;
}

static void ApplyPendingVideoProfile() {
  RequestProfileApply(g_pendingProfile, L"manual");
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

static void CreateOverlayWindows(HINSTANCE hInst) {
  WNDCLASSW overlay{};
  overlay.lpfnWndProc = OverlayWndProc;
  overlay.hInstance = hInst;
  overlay.hCursor = LoadCursor(nullptr, IDC_ARROW);
  overlay.lpszClassName = L"P2PNativeOverlay";
  RegisterClassW(&overlay);

  DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
  g_toolbarHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                  0, 0, kToolbarWidth, kToolbarHeight, g_hwnd, nullptr, hInst, nullptr);
  g_menuHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                               0, 0, kMenuWidth, kMenuHeight, g_hwnd, nullptr, hInst, nullptr);
  g_statsHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                0, 0, kStatsWidth, kStatsHeight, g_hwnd, nullptr, hInst, nullptr);
  SetWindowLongPtrW(g_toolbarHwnd, GWLP_USERDATA, 1);
  SetWindowLongPtrW(g_menuHwnd, GWLP_USERDATA, 2);
  SetWindowLongPtrW(g_statsHwnd, GWLP_USERDATA, 3);
  UpdateOverlayLayout();
  ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
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
      int x = GET_X_LPARAM(lp);
      int y = GET_Y_LPARAM(lp);
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
      SetBkMode(hdc, TRANSPARENT);
      if (kind == 1) DrawToolbar(hdc, rc);
      else if (kind == 2) DrawMenu(hdc, rc);
      else if (kind == 3) DrawStats(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

class VideoReceiver {
 public:
  explicit VideoReceiver(uint16_t port) : port_(port) {}

  void operator()() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"P2P UDP video receiver");

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    DWORD timeoutMs = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      closesocket(s);
      MessageBoxW(nullptr, L"Failed to bind video UDP port", L"P2P Native", MB_ICONERROR);
      return;
    }

    struct PartialFrame {
      uint32_t frameBytes = 0;
      uint16_t fragCount = 0;
      uint64_t ptsUs = 0;
      uint16_t flags = 0;
      uint64_t firstQpc = 0;
      uint16_t received = 0;
      bool hasFec = false;
      uint16_t dataFragCount = 0;
      uint16_t fecIndex = 0;
      uint16_t fecPayloadBytes = 0;
      std::vector<uint8_t> bytes;
      std::vector<uint8_t> got;
      std::vector<uint8_t> fec;
    };

    std::vector<uint8_t> packet(kMaxUdp);
    std::unordered_map<uint64_t, PartialFrame> partials;
    partials.reserve(16);
    uint64_t newestFrameId = 0;

    while (g_running.load()) {
      int n = recv(s, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()), 0);
      if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT) continue;
        break;
      }
      if (n < kVideoHeaderBytes) continue;
      auto* h = reinterpret_cast<P2VideoHeader*>(packet.data());
      if (memcmp(h->magic, "P2V2", 4) != 0 || h->version != P2_VERSION) continue;
      if (h->headerBytes != sizeof(P2VideoHeader) || h->payloadBytes + h->headerBytes > n) continue;
      if (h->fragCount == 0 || h->fragIndex >= h->fragCount || h->frameBytes > 8 * 1024 * 1024) continue;

      const uint64_t now = QpcNow();
      g_packetsRx.fetch_add(1, std::memory_order_relaxed);
      g_bytesRx.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
      g_lastPacketQpc.store(now, std::memory_order_relaxed);

      if (newestFrameId && h->frameId + 120 < newestFrameId) continue;
      if (h->frameId > newestFrameId) newestFrameId = h->frameId;

      if (partials.size() > 24) {
        uint64_t keepFrom = newestFrameId > 24 ? newestFrameId - 24 : 0;
        for (auto it = partials.begin(); it != partials.end();) {
          if (it->first < keepFrom || QpcDeltaUs(it->second.firstQpc, now) > 750'000) {
            it = partials.erase(it);
            RecordNetworkFrameDrop();
          } else {
            ++it;
          }
        }
      }

      auto [it, inserted] = partials.try_emplace(h->frameId);
      PartialFrame& partial = it->second;
      if (inserted) {
        partial.frameBytes = h->frameBytes;
        partial.fragCount = h->fragCount;
        partial.ptsUs = h->ptsUs;
        partial.flags = h->flags;
        partial.firstQpc = now;
        partial.bytes.assign(h->frameBytes, 0);
        partial.got.assign(h->fragCount, 0);
        partial.hasFec = (h->flags & P2_FLAG_FEC) != 0;
        partial.dataFragCount = partial.hasFec && h->fragCount > 1 ? uint16_t(h->fragCount - 1) : h->fragCount;
        partial.fecIndex = partial.dataFragCount;
      } else if (partial.fragCount != h->fragCount || partial.frameBytes != h->frameBytes) {
        partials.erase(it);
        RecordNetworkFrameDrop();
        continue;
      }

      if (partial.got[h->fragIndex]) continue;
      if ((h->flags & P2_FLAG_FEC) != 0) {
        partial.hasFec = true;
        partial.dataFragCount = partial.fragCount > 1 ? uint16_t(partial.fragCount - 1) : partial.fragCount;
        partial.fecIndex = h->fragIndex;
        partial.fecPayloadBytes = h->payloadBytes;
        partial.fec.assign(packet.data() + h->headerBytes, packet.data() + h->headerBytes + h->payloadBytes);
        partial.got[h->fragIndex] = 1;
        ++partial.received;
      } else {
        size_t off = static_cast<size_t>(h->fragIndex) * kMaxVideoFragmentPayload;
        if (off + h->payloadBytes > partial.bytes.size()) continue;
        memcpy(partial.bytes.data() + off, packet.data() + h->headerBytes, h->payloadBytes);
        partial.got[h->fragIndex] = 1;
        ++partial.received;
      }

      bool frameReady = false;
      if (partial.hasFec) {
        uint16_t missingIndex = 0xffff;
        uint16_t missingCount = 0;
        for (uint16_t idx = 0; idx < partial.dataFragCount; ++idx) {
          if (!partial.got[idx]) {
            missingIndex = idx;
            ++missingCount;
            if (missingCount > 1) break;
          }
        }
        if (missingCount == 0) {
          frameReady = true;
        } else if (missingCount == 1 && !partial.fec.empty() && partial.got[partial.fecIndex]) {
          const size_t off = static_cast<size_t>(missingIndex) * kMaxVideoFragmentPayload;
          const size_t expectedLen = std::min<size_t>(kMaxVideoFragmentPayload, partial.bytes.size() - off);
          std::vector<uint8_t> recovered(partial.fec.begin(), partial.fec.end());
          recovered.resize(expectedLen, 0);
          for (uint16_t idx = 0; idx < partial.dataFragCount; ++idx) {
            if (idx == missingIndex || !partial.got[idx]) continue;
            const size_t srcOff = static_cast<size_t>(idx) * kMaxVideoFragmentPayload;
            const size_t srcLen = std::min<size_t>(kMaxVideoFragmentPayload, partial.bytes.size() - srcOff);
            for (size_t j = 0; j < srcLen && j < recovered.size(); ++j) {
              recovered[j] ^= partial.bytes[srcOff + j];
            }
          }
          memcpy(partial.bytes.data() + off, recovered.data(), recovered.size());
          partial.got[missingIndex] = 1;
          frameReady = true;
        }
      } else if (partial.received == partial.fragCount) {
        frameReady = true;
      }

      if (frameReady) {
        EncodedFrame out;
        out.bytes = std::move(partial.bytes);
        out.frameId = h->frameId;
        out.ptsUs = partial.ptsUs;
        out.recvQpc = QpcNow();
        out.keyframe = (partial.flags & P2_FLAG_KEYFRAME) != 0;
        PushEncoded(std::move(out));
        g_framesComplete.fetch_add(1, std::memory_order_relaxed);
        g_lastCompleteQpc.store(QpcNow(), std::memory_order_relaxed);
        partials.erase(h->frameId);
      }
    }
    closesocket(s);
  }

 private:
  uint16_t port_;
};

class TcpVideoReceiver {
 public:
  TcpVideoReceiver(std::wstring hostIp, uint16_t port) : hostIp_(std::move(hostIp)), port_(port) {}

  void operator()() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"P2P TCP video receiver");

    while (g_running.load()) {
      SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (s == INVALID_SOCKET) return;

      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
      int rcvbuf = 8 * 1024 * 1024;
      setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port_);
      std::string ip = WideToUtf8(hostIp_);
      if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(s);
        return;
      }

      if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        Sleep(200);
        continue;
      }

      while (g_running.load()) {
        P2TcpVideoHeader h{};
        if (!RecvAll(s, reinterpret_cast<uint8_t*>(&h), sizeof(h))) break;
        if (memcmp(h.magic, "P2T2", 4) != 0 || h.version != P2_VERSION || h.headerBytes != sizeof(P2TcpVideoHeader)) break;
        if (h.frameBytes == 0 || h.frameBytes > 16 * 1024 * 1024) break;

        EncodedFrame out;
        out.bytes.resize(h.frameBytes);
        if (!RecvAll(s, out.bytes.data(), h.frameBytes)) break;
        out.frameId = h.frameId;
        out.ptsUs = h.ptsUs;
        out.recvQpc = QpcNow();
        out.keyframe = (h.flags & P2_FLAG_KEYFRAME) != 0;

        g_packetsRx.fetch_add(1, std::memory_order_relaxed);
        g_bytesRx.fetch_add(static_cast<uint64_t>(sizeof(h)) + h.frameBytes, std::memory_order_relaxed);
        g_framesComplete.fetch_add(1, std::memory_order_relaxed);
        g_lastPacketQpc.store(out.recvQpc, std::memory_order_relaxed);
        g_lastCompleteQpc.store(out.recvQpc, std::memory_order_relaxed);
        PushEncoded(std::move(out));
      }

      closesocket(s);
      if (g_running.load()) Sleep(200);
    }
  }

 private:
  bool RecvAll(SOCKET s, uint8_t* dst, size_t bytes) {
    size_t off = 0;
    while (off < bytes && g_running.load()) {
      int n = recv(s, reinterpret_cast<char*>(dst + off), static_cast<int>(std::min<size_t>(bytes - off, 64 * 1024)), 0);
      if (n <= 0) return false;
      off += static_cast<size_t>(n);
    }
    return off == bytes;
  }

  std::wstring hostIp_;
  uint16_t port_;
};

static void NV12ToBGRA(const uint8_t* src, DWORD srcLen, int width, int height, std::vector<uint8_t>& out) {
  const size_t ySize = static_cast<size_t>(width) * height;
  if (srcLen < ySize + ySize / 2) return;
  const uint8_t* yPlane = src;
  const uint8_t* uvPlane = src + ySize;
  out.resize(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int Y = int(yPlane[y * width + x]);
      int uvIndex = (y / 2) * width + (x & ~1);
      int U = int(uvPlane[uvIndex]) - 128;
      int V = int(uvPlane[uvIndex + 1]) - 128;
      int C = Y - 16;
      int R = (298 * C + 409 * V + 128) >> 8;
      int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
      int B = (298 * C + 516 * U + 128) >> 8;
      R = std::clamp(R, 0, 255); G = std::clamp(G, 0, 255); B = std::clamp(B, 0, 255);
      size_t o = (static_cast<size_t>(y) * width + x) * 4;
      out[o + 0] = static_cast<uint8_t>(B);
      out[o + 1] = static_cast<uint8_t>(G);
      out[o + 2] = static_cast<uint8_t>(R);
      out[o + 3] = 255;
    }
  }
}

class MfDecoder {
 public:
  bool Init(int width, int height, int fps, ID3D11Device* sharedDevice) {
    width_ = width;
    height_ = height;
    fps_ = std::max(30, fps);
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) return false;
    InitDxvaDeviceManager(sharedDevice);

    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(mft_.As(&codec))) {
      VARIANT v; VariantInit(&v);
      v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
      HRESULT lowLatencyHr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
      if (FAILED(lowLatencyHr)) {
        VariantClear(&v);
        VariantInit(&v);
        v.vt = VT_UI4; v.ulVal = 1;
        codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
      }
      VariantClear(&v);
    }

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
      attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps_, 1);
    hr = mft_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) return false;

    return SetNv12OutputType();
  }

  bool Decode(const EncodedFrame& encoded, DecodedFrame& decoded) {
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(encoded.bytes.size()), &buf);
    if (FAILED(hr)) return false;
    BYTE* dst = nullptr; DWORD maxLen = 0;
    buf->Lock(&dst, &maxLen, nullptr);
    memcpy(dst, encoded.bytes.data(), encoded.bytes.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(encoded.bytes.size()));

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(encoded.ptsUs * 10));
    sample->SetSampleDuration(10'000'000 / fps_);

    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      DrainOne(decoded, encoded);
      hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return false;

    bool gotFrame = false;
    DecodedFrame last;
    for (int i = 0; i < 4; ++i) {
      DecodedFrame one;
      if (!DrainOne(one, encoded)) break;
      last = std::move(one);
      gotFrame = true;
    }
    if (gotFrame) {
      decoded = std::move(last);
      return true;
    }
    return false;
  }

 private:
  bool SetNv12OutputType() {
    ComPtr<IMFMediaType> out;
    HRESULT hr = MFCreateMediaType(&out);
    if (FAILED(hr)) return false;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps_, 1);
    hr = mft_->SetOutputType(0, out.Get(), 0);
    return SUCCEEDED(hr);
  }

  void InitDxvaDeviceManager(ID3D11Device* sharedDevice) {
    if (sharedDevice) {
      dxDevice_ = sharedDevice;
      sharedDxDevice_ = true;
      sharedDevice->GetImmediateContext(dxCtx_.GetAddressOf());
    } else {
      UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
      D3D_FEATURE_LEVEL levels[] = {
          D3D_FEATURE_LEVEL_11_1,
          D3D_FEATURE_LEVEL_11_0,
          D3D_FEATURE_LEVEL_10_1,
      };
      D3D_FEATURE_LEVEL actual{};
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &dxDevice_, &actual, &dxCtx_))) {
        return;
      }
    }
    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, &dxgiManager_))) return;
    if (FAILED(dxgiManager_->ResetDevice(dxDevice_.Get(), resetToken))) return;
    mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiManager_.Get()));
  }

  bool DrainOne(DecodedFrame& decoded, const EncodedFrame& meta) {
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(0, &info);

    ComPtr<IMFSample> outSample;
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;

    const bool providesSamples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    bool usingDxgiOutputSample = false;
    if (!providesSamples) {
      if (sharedDxDevice_ && CreateDxgiOutputSample(&outSample)) {
        usingDxgiOutputSample = true;
      } else {
        DWORD bufSize = std::max<DWORD>(info.cbSize, static_cast<DWORD>(width_ * height_ * 3 / 2));
        ComPtr<IMFMediaBuffer> outBuf;
        if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuf))) return false;
        MFCreateSample(&outSample);
        outSample->AddBuffer(outBuf.Get());
      }
      output.pSample = outSample.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      SetNv12OutputType();
      return false;
    }
    if (FAILED(hr) && usingDxgiOutputSample) {
      // Some decoder configurations reject caller-provided DXGI samples; retry with a CPU sample.
      outSample.Reset();
      output = MFT_OUTPUT_DATA_BUFFER{};
      output.dwStreamID = 0;
      DWORD bufSize = std::max<DWORD>(info.cbSize, static_cast<DWORD>(width_ * height_ * 3 / 2));
      ComPtr<IMFMediaBuffer> outBuf;
      if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuf))) return false;
      MFCreateSample(&outSample);
      outSample->AddBuffer(outBuf.Get());
      output.pSample = outSample.Get();
      hr = mft_->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents) output.pEvents->Release();
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      SetNv12OutputType();
      return false;
    }
    if (FAILED(hr)) return false;

    if (providesSamples && output.pSample) {
      outSample.Attach(output.pSample);
    }
    if (!outSample) return false;

    // Zero-copy path: decoder gave us a D3D11 texture from the same device used by the renderer.
    if (sharedDxDevice_ && ExtractDxgi(outSample.Get(), decoded, meta)) {
      return true;
    }

    return CopySampleToNv12(outSample.Get(), decoded, meta);
  }

  bool CreateDxgiOutputSample(ComPtr<IMFSample>* sampleOut) {
    if (!dxDevice_) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dxDevice_->CreateTexture2D(&desc, nullptr, &texture))) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer))) return false;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    if (FAILED(sample->AddBuffer(buffer.Get()))) return false;
    *sampleOut = sample;
    return true;
  }

  bool ExtractDxgi(IMFSample* sample, DecodedFrame& decoded, const EncodedFrame& meta) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return false;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    if (FAILED(buffer.As(&dxgiBuffer))) return false;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture)))) return false;
    UINT subresource = 0;
    dxgiBuffer->GetSubresourceIndex(&subresource);

    decoded = DecodedFrame{};
    decoded.gpu = true;
    decoded.dxgi.texture = texture;
    decoded.dxgi.subresource = subresource;
    decoded.dxgi.width = width_;
    decoded.dxgi.height = height_;
    decoded.dxgi.frameId = meta.frameId;
    decoded.dxgi.recvQpc = meta.recvQpc;
    return true;
  }

  bool CopySampleToNv12(IMFSample* sample, DecodedFrame& decoded, const EncodedFrame& meta) {
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) return false;
    BYTE* src = nullptr; DWORD maxLen = 0, curLen = 0;
    if (FAILED(contiguous->Lock(&src, &maxLen, &curLen))) return false;
    const DWORD needed = static_cast<DWORD>(width_ * height_ * 3 / 2);
    Nv12Frame nv12;
    if (curLen >= needed) {
      nv12.width = width_;
      nv12.height = height_;
      nv12.frameId = meta.frameId;
      nv12.recvQpc = meta.recvQpc;
      nv12.bytes.assign(src, src + needed);
    }
    contiguous->Unlock();
    if (nv12.bytes.empty()) return false;

    decoded = DecodedFrame{};
    decoded.gpu = false;
    decoded.nv12 = std::move(nv12);
    return true;
  }

  ComPtr<IMFTransform> mft_;
  ComPtr<ID3D11Device> dxDevice_;
  ComPtr<ID3D11DeviceContext> dxCtx_;
  ComPtr<IMFDXGIDeviceManager> dxgiManager_;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 60;
  bool sharedDxDevice_ = false;
};

static void DecoderThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  SetThreadDescription(GetCurrentThread(), L"P2P H264 decode");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_LITE);
  auto sharedDeviceAvailableNow = [&]() -> bool {
    return g_renderer && g_renderer->Device();
  };
  bool useSharedDevice = sharedDeviceAvailableNow();
  auto createDecoder = [&](bool preferSharedDevice) -> std::unique_ptr<MfDecoder> {
    auto decoder = std::make_unique<MfDecoder>();
    ID3D11Device* renderDevice = (preferSharedDevice && g_renderer) ? g_renderer->Device() : nullptr;
    const VideoProfile activeProfile = ActiveVideoProfile();
    if (!decoder->Init(activeProfile.width, activeProfile.height, activeProfile.fps, renderDevice)) return nullptr;
    return decoder;
  };

  auto decoder = createDecoder(useSharedDevice);
  if (!decoder) {
    MessageBoxW(nullptr, L"Failed to initialize Media Foundation H.264 decoder", L"P2P Native", MB_ICONERROR);
    MFShutdown();
    CoUninitialize();
    return;
  }
  uint64_t appliedProfileGeneration = g_videoProfileGeneration.load(std::memory_order_relaxed);

  while (g_running.load()) {
    const uint64_t generation = g_videoProfileGeneration.load(std::memory_order_relaxed);
    if (generation != appliedProfileGeneration) {
      const VideoProfile activeProfile = ActiveVideoProfile();
      bool reconfigured = !g_renderer || g_renderer->Reconfigure(activeProfile.width, activeProfile.height);
      if (reconfigured) {
        useSharedDevice = sharedDeviceAvailableNow();
        if (auto rebuilt = createDecoder(useSharedDevice)) {
          decoder = std::move(rebuilt);
          appliedProfileGeneration = generation;
          g_decoderPrimed.store(false, std::memory_order_relaxed);
          Log(L"decoder/render pipeline reconfigured: %dx%d@%d bitrate=%d",
              activeProfile.width, activeProfile.height, activeProfile.fps, activeProfile.bitrate);
        } else {
          Log(L"decoder rebuild failed after profile change: %dx%d@%d",
              activeProfile.width, activeProfile.height, activeProfile.fps);
        }
      } else {
        Log(L"renderer reconfigure failed after profile change: %dx%d",
            activeProfile.width, activeProfile.height);
      }
    }

    EncodedFrame encoded;
    {
      std::unique_lock lk(g_encodedMu);
      g_encodedCv.wait(lk, [] { return !g_running.load() || !g_encodedQueue.empty(); });
      if (!g_running.load()) break;
      encoded = std::move(g_encodedQueue.front());
      g_encodedQueue.pop_front();
      g_encodedQueueDepthNow.store(static_cast<uint32_t>(g_encodedQueue.size()), std::memory_order_relaxed);
    }
    DecodedFrame frame;
    if (decoder->Decode(encoded, frame)) {
      g_decoderPrimed.store(true, std::memory_order_relaxed);
      PushDecoded(std::move(frame));
    } else {
      g_decodeFails.fetch_add(1, std::memory_order_relaxed);
      EnterVideoRecovery(L"decode failed");
    }
  }
  MFShutdown();
  CoUninitialize();
}

static void RenderThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"P2P video present");
  uint32_t gpuPresentFailStreak = 0;
  uint64_t lastPresentQpcLocal = 0;

  while (g_running.load()) {
    DecodedFrame frame;
    {
      std::unique_lock lk(g_decodedMu);
      g_decodedCv.wait(lk, [] { return !g_running.load() || !g_decodedQueue.empty(); });
      if (!g_running.load()) break;
      frame = std::move(g_decodedQueue.back());
      if (g_decodedQueue.size() > 1) {
        g_renderFramesDropped.fetch_add(static_cast<uint64_t>(g_decodedQueue.size() - 1), std::memory_order_relaxed);
      }
      g_decodedQueue.clear();
      g_decodedQueueDepthNow.store(0, std::memory_order_relaxed);
    }

    bool presented = false;
    if (g_renderer) {
      const int fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
      const uint64_t frameIntervalUs = 1'000'000ull / static_cast<uint64_t>(fps);
      const uint64_t nowBeforePresent = QpcNow();
      if (lastPresentQpcLocal) {
        const uint64_t sinceLastPresentUs = QpcDeltaUs(lastPresentQpcLocal, nowBeforePresent);
        if (sinceLastPresentUs + 2'000 < frameIntervalUs) {
          Sleep(static_cast<DWORD>(std::max<uint64_t>(0, (frameIntervalUs - sinceLastPresentUs - 1'000) / 1000)));
        }
      }
      g_renderer->WaitForPresentReady();
      if (frame.gpu) {
        presented = g_renderer->Render(frame.dxgi);
        if (!presented) {
          g_gpuRenderFails.fetch_add(1, std::memory_order_relaxed);
          ++gpuPresentFailStreak;
        } else {
          gpuPresentFailStreak = 0;
        }
      } else {
        presented = g_renderer->Render(frame.nv12);
        if (presented) gpuPresentFailStreak = 0;
      }
    }

    if (presented) {
      lastPresentQpcLocal = QpcNow();
      g_framesPresented.fetch_add(1, std::memory_order_relaxed);
      if (frame.gpu) g_gpuFrames.fetch_add(1, std::memory_order_relaxed);
      else g_cpuFrames.fetch_add(1, std::memory_order_relaxed);
      uint64_t presentQpc = QpcNow();
      uint64_t recvQpc = frame.gpu ? frame.dxgi.recvQpc : frame.nv12.recvQpc;
      g_lastPresentQpc.store(presentQpc, std::memory_order_relaxed);
      g_lastRxToPresentUs.store(QpcDeltaUs(recvQpc, presentQpc), std::memory_order_relaxed);
      continue;
    }

    if (!frame.gpu) {
      BgraFrame bgra;
      NV12ToBGRA(frame.nv12.bytes.data(), static_cast<DWORD>(frame.nv12.bytes.size()), frame.nv12.width, frame.nv12.height, bgra.bytes);
      bgra.width = frame.nv12.width;
      bgra.height = frame.nv12.height;
      {
        std::lock_guard lk(g_frameMu);
        g_latestFrame = std::move(bgra);
      }
      InvalidateRect(g_hwnd, nullptr, FALSE);
    }
  }
}

static uint16_t VkToMacKeyCode(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') {
    static const uint16_t map[26] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    return map[vk - 'A'];
  }
  if (vk >= '0' && vk <= '9') {
    static const uint16_t map[10] = {29,18,19,20,21,23,22,26,28,25};
    return map[vk - '0'];
  }
  switch (vk) {
    case VK_RETURN: return 36; case VK_TAB: return 48; case VK_SPACE: return 49; case VK_BACK: return 51; case VK_ESCAPE: return 53;
    case VK_LEFT: return 123; case VK_RIGHT: return 124; case VK_DOWN: return 125; case VK_UP: return 126;
    case VK_DELETE: return 117; case VK_HOME: return 115; case VK_END: return 119; case VK_PRIOR: return 116; case VK_NEXT: return 121;
    case VK_LSHIFT: case VK_SHIFT: return 56; case VK_RSHIFT: return 60;
    case VK_LCONTROL: case VK_CONTROL: return 59; case VK_RCONTROL: return 62;
    case VK_LMENU: case VK_MENU: return 58; case VK_RMENU: return 61;
    case VK_LWIN: return 55; case VK_RWIN: return 54;
    case VK_OEM_MINUS: return 27; case VK_OEM_PLUS: return 24; case VK_OEM_4: return 33; case VK_OEM_6: return 30;
    case VK_OEM_5: return 42; case VK_OEM_1: return 41; case VK_OEM_7: return 39; case VK_OEM_COMMA: return 43;
    case VK_OEM_PERIOD: return 47; case VK_OEM_2: return 44; case VK_OEM_3: return 50;
    default:
      if (vk >= VK_F1 && vk <= VK_F12) {
        static const uint16_t f[12] = {122,120,99,118,96,97,98,100,101,109,103,111};
        return f[vk - VK_F1];
      }
      return 0xffff;
  }
}

static void SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode) {
  if (g_inputSock == INVALID_SOCKET) return;
  P2InputPacket p{};
  memcpy(p.magic, "P2I2", 4);
  p.version = P2_VERSION;
  p.kind = kind;
  p.bytes = sizeof(P2InputPacket);
  p.seq = g_inputSeq.fetch_add(1);
  p.x = x; p.y = y; p.dx = dx; p.dy = dy; p.button = button; p.keyCode = keyCode;
  sendto(g_inputSock, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&g_inputAddr), sizeof(g_inputAddr));
}

static void SendVideoBitrateControl(int bitrate, const wchar_t* reason) {
  if (g_inputSock == INVALID_SOCKET) return;
  const int clamped = std::max(g_minAdaptiveBitrate.load(std::memory_order_relaxed),
                               std::min(g_maxAdaptiveBitrate.load(std::memory_order_relaxed), bitrate));
  const int current = g_currentBitrate.load(std::memory_order_relaxed);
  if (clamped == current) return;
  g_currentBitrate.store(clamped, std::memory_order_relaxed);
  const uint64_t now = QpcNow();
  g_lastBitrateControlQpc.store(now, std::memory_order_relaxed);
  if (clamped > current) {
    g_lastBitrateIncreaseQpc.store(now, std::memory_order_relaxed);
  }
  SendInputPacket(P2_INPUT_SET_VIDEO_BITRATE, 0, 0, static_cast<int32_t>(clamped), 0, 0, 0);
  Log(L"adaptive bitrate request=%d (%s)", clamped, reason ? reason : L"adaptive");
}

static void EnterVideoRecovery(const wchar_t* reason, bool reduceBitrate) {
  g_waitingForKeyframe.store(true, std::memory_order_relaxed);
  g_decoderPrimed.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lk(g_encodedMu);
    if (!g_encodedQueue.empty()) {
      RecordClientFrameDrop(static_cast<uint64_t>(g_encodedQueue.size()));
      g_encodedQueue.clear();
    }
  }
  {
    std::lock_guard lk(g_decodedMu);
    if (!g_decodedQueue.empty()) {
      g_renderFramesDropped.fetch_add(static_cast<uint64_t>(g_decodedQueue.size()), std::memory_order_relaxed);
      g_decodedQueue.clear();
      g_decodedQueueDepthNow.store(0, std::memory_order_relaxed);
    }
  }

  if (!g_cfg.udpVideo) return;
  const uint64_t now = QpcNow();
  const uint64_t last = g_lastKeyframeRequestQpc.load(std::memory_order_relaxed);
  if (last && QpcDeltaUs(last, now) < 120'000) return;
  g_lastKeyframeRequestQpc.store(now, std::memory_order_relaxed);
  g_keyframeRequests.fetch_add(1, std::memory_order_relaxed);
  if (reduceBitrate) {
    const int current = g_currentBitrate.load(std::memory_order_relaxed);
    const int reduced = std::max(g_minAdaptiveBitrate.load(std::memory_order_relaxed), current * 85 / 100);
    if (reduced < current) {
      SendVideoBitrateControl(reduced, L"recovery");
    }
  }
  SendInputPacket(P2_INPUT_REQUEST_KEYFRAME, 0, 0, 0, 0, 0, 0);
  Log(L"requested keyframe recovery: %s", reason ? reason : L"unknown");
}

static void NormalizedPoint(HWND hwnd, LPARAM lp, float& x, float& y) {
  RECT rc{}; GetClientRect(hwnd, &rc);
  int w = std::max(1L, rc.right - rc.left);
  int h = std::max(1L, rc.bottom - rc.top);
  x = std::clamp(float(GET_X_LPARAM(lp)) / float(w), 0.0f, 1.0f);
  y = std::clamp(float(GET_Y_LPARAM(lp)) / float(h), 0.0f, 1.0f);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      SetFocus(hwnd);
      SetTimer(hwnd, 1, 500, nullptr);
      return 0;
    case WM_TIMER: {
      static uint64_t lastFrames = 0;
      static uint64_t lastComplete = 0;
      static uint64_t lastPackets = 0;
      static uint64_t lastBytes = 0;
      static uint64_t lastClientDropped = 0;
      static uint64_t lastNetworkDropped = 0;
      static uint64_t lastQpc = QpcNow();
      uint64_t now = QpcNow();
      uint64_t frames = g_framesPresented.load(std::memory_order_relaxed);
      uint64_t complete = g_framesComplete.load(std::memory_order_relaxed);
      uint64_t packets = g_packetsRx.load(std::memory_order_relaxed);
      uint64_t bytes = g_bytesRx.load(std::memory_order_relaxed);
      double seconds = double(QpcDeltaUs(lastQpc, now)) / 1'000'000.0;
      double fps = seconds > 0.001 ? double(frames - lastFrames) / seconds : 0.0;
      double cfps = seconds > 0.001 ? double(complete - lastComplete) / seconds : 0.0;
      double pps = seconds > 0.001 ? double(packets - lastPackets) / seconds : 0.0;
      double mbps = seconds > 0.001 ? double(bytes - lastBytes) * 8.0 / seconds / 1'000'000.0 : 0.0;
      lastFrames = frames;
      lastComplete = complete;
      lastPackets = packets;
      lastBytes = bytes;
      lastQpc = now;
      uint64_t gpu = g_gpuFrames.load(std::memory_order_relaxed);
      uint64_t cpu = g_cpuFrames.load(std::memory_order_relaxed);
      double rxMs = double(g_lastRxToPresentUs.load(std::memory_order_relaxed)) / 1000.0;
      double packetAgeMs = double(QpcDeltaUs(g_lastPacketQpc.load(std::memory_order_relaxed), now)) / 1000.0;
      double frameAgeMs = double(QpcDeltaUs(g_lastCompleteQpc.load(std::memory_order_relaxed), now)) / 1000.0;
      uint64_t dropped = g_framesDropped.load(std::memory_order_relaxed);
      uint64_t clientDropped = g_clientFramesDropped.load(std::memory_order_relaxed);
      uint64_t networkDropped = g_networkFramesDropped.load(std::memory_order_relaxed);
      uint64_t decodeFails = g_decodeFails.load(std::memory_order_relaxed);
      uint64_t gpuRenderFails = g_gpuRenderFails.load(std::memory_order_relaxed);
      NativeUiStats stats{};
      stats.presentFps = fps;
      stats.completeFps = cfps;
      stats.mbps = mbps;
      stats.packetRate = pps;
      stats.rxToPresentMs = rxMs;
      stats.packetAgeMs = packetAgeMs;
      stats.frameAgeMs = frameAgeMs;
      stats.dropped = dropped;
      stats.clientDropped = clientDropped;
      stats.networkDropped = networkDropped;
      stats.decodeFails = decodeFails;
      stats.gpuRenderFails = gpuRenderFails;
      stats.gpuFrames = gpu;
      stats.cpuFrames = cpu;
      stats.queueDepth = g_encodedQueueDepthNow.load(std::memory_order_relaxed);
      stats.queueTarget = g_encodedQueueTargetNow.load(std::memory_order_relaxed);
      stats.decodedQueueDepth = g_decodedQueueDepthNow.load(std::memory_order_relaxed);
      stats.decodedQueueTarget = g_decodedQueueTargetNow.load(std::memory_order_relaxed);
      stats.renderDropped = g_renderFramesDropped.load(std::memory_order_relaxed);
      const uint64_t clientDroppedDelta = clientDropped >= lastClientDropped ? (clientDropped - lastClientDropped) : clientDropped;
      const uint64_t networkDroppedDelta = networkDropped >= lastNetworkDropped ? (networkDropped - lastNetworkDropped) : networkDropped;
      const uint64_t completeDelta = complete >= lastComplete ? (complete - lastComplete) : complete;
      const uint64_t clientKnownFrames = completeDelta + clientDroppedDelta;
      const uint64_t networkKnownFrames = completeDelta + networkDroppedDelta;
      stats.networkDropPct = networkKnownFrames ? (double(networkDroppedDelta) * 100.0 / double(networkKnownFrames)) : 0.0;
      stats.queueDropPct = clientKnownFrames ? (double(clientDroppedDelta) * 100.0 / double(clientKnownFrames)) : 0.0;
      lastClientDropped = clientDropped;
      lastNetworkDropped = networkDropped;
      StoreUiStats(stats);

      if (g_cfg.udpVideo) {
        const VideoProfile activeProfile = ActiveVideoProfile();
        double prevScore = g_recentDropScore.load(std::memory_order_relaxed);
        double dropScore = prevScore * 0.72 + stats.queueDropPct * 0.28;
        g_recentDropScore.store(dropScore, std::memory_order_relaxed);
        const uint64_t lastCtlQpc = g_lastBitrateControlQpc.load(std::memory_order_relaxed);
        const uint64_t lastUpQpc = g_lastBitrateIncreaseQpc.load(std::memory_order_relaxed);
        const uint64_t lastProfileQpc = g_lastAutoProfileChangeQpc.load(std::memory_order_relaxed);
        const uint64_t sinceCtl = lastCtlQpc ? QpcDeltaUs(lastCtlQpc, now) : UINT64_MAX;
        const uint64_t sinceUp = lastUpQpc ? QpcDeltaUs(lastUpQpc, now) : UINT64_MAX;
        const uint64_t sinceProfile = lastProfileQpc ? QpcDeltaUs(lastProfileQpc, now) : UINT64_MAX;
        const bool canAdjust = sinceCtl >= 700'000;
        const int current = g_currentBitrate.load(std::memory_order_relaxed);
        const int bitrateFloor = g_minAdaptiveBitrate.load(std::memory_order_relaxed);
        const double queueFill = stats.queueTarget > 0 ? (double(stats.queueDepth) / double(stats.queueTarget)) : 0.0;
        const bool overloaded = dropScore >= 2.0 || queueFill >= 0.72 || stats.frameAgeMs > 32.0 || stats.packetAgeMs > 24.0 || (stats.presentFps > 0.1 && stats.presentFps + 4.0 < double(activeProfile.fps));
        const bool severeOverload = dropScore >= 6.0 || queueFill >= 0.9 || stats.frameAgeMs > 60.0 || stats.packetAgeMs > 45.0 || (stats.presentFps > 0.1 && stats.presentFps + 8.0 < double(activeProfile.fps));
        const bool stable = dropScore <= 1.0 && stats.frameAgeMs < 16.0 && stats.packetAgeMs < 12.0 && stats.presentFps >= double(activeProfile.fps) - 1.0;
        if (canAdjust) {
          if (overloaded) {
            const int reduced = std::max(bitrateFloor, current * (queueFill >= 0.9 ? 68 : 74) / 100);
            if (reduced < current) {
              SendVideoBitrateControl(reduced, L"overload");
            }
          } else if (stable && sinceUp >= 4'000'000) {
            const int increased = std::min(g_maxAdaptiveBitrate.load(std::memory_order_relaxed), current + std::max(200'000, current / 18));
            if (increased > current) {
              SendVideoBitrateControl(increased, L"stable");
            }
          }
        }
        if (severeOverload && sinceProfile >= 15'000'000 && (current <= bitrateFloor + 500'000 || stats.presentFps + 20.0 < double(activeProfile.fps))) {
          VideoProfile downgraded{};
          if (BuildLowerAutoProfile(activeProfile, downgraded)) {
            if (RequestProfileApply(downgraded, L"auto-overload", true)) {
              return 0;
            }
          }
        }
      }

      wchar_t title[512];
      swprintf_s(title, L"P2P Native v2 %s -> %s | present %.0f fps complete %.0f fps | %.1f Mbps %.0f pkt/s | last pkt %.0f ms frame %.0f ms | rx-present %.2f ms | drop local %llu net %llu | GPU %llu CPU %llu | bitrate %d Mbps",
                 g_cfg.udpVideo ? L"UDP" : L"TCP", g_cfg.hostIp.c_str(), fps, cfps, mbps, pps, packetAgeMs, frameAgeMs, rxMs,
                 static_cast<unsigned long long>(clientDropped),
                 static_cast<unsigned long long>(networkDropped),
                 static_cast<unsigned long long>(gpu),
                 static_cast<unsigned long long>(cpu),
                 std::max(1, g_currentBitrate.load(std::memory_order_relaxed) / 1'000'000));
      SetWindowTextW(hwnd, title);
      if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
      if (g_menuHwnd && IsWindowVisible(g_menuHwnd)) InvalidateRect(g_menuHwnd, nullptr, FALSE);
      if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) InvalidateRect(g_statsHwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOVE:
      UpdateOverlayLayout();
      return 0;
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {
        HideNativePopups();
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_HIDE);
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) {
        HideNativePopups();
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_HIDE);
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && g_framesPresented.load(std::memory_order_relaxed) > 0) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = (wp & MK_RBUTTON) ? 2 : ((wp & MK_MBUTTON) ? 1 : 0);
      SendInputPacket(P2_INPUT_MOVE, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
      SetCapture(hwnd); SetFocus(hwnd);
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONDOWN ? 2 : (msg == WM_MBUTTONDOWN ? 1 : 0);
      SendInputPacket(P2_INPUT_DOWN, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
      ReleaseCapture();
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONUP ? 2 : (msg == WM_MBUTTONUP ? 1 : 0);
      SendInputPacket(P2_INPUT_UP, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &pt);
      LPARAM clp = MAKELPARAM(pt.x, pt.y);
      float x, y; NormalizedPoint(hwnd, clp, x, y);
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
      uint16_t mac = VkToMacKeyCode(wp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_DOWN, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      uint16_t mac = VkToMacKeyCode(wp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_UP, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      if (g_renderer && g_framesPresented.load(std::memory_order_relaxed) > 0) {
        EndPaint(hwnd, &ps);
        return 0;
      }
      BgraFrame frame;
      {
        std::lock_guard lk(g_frameMu);
        frame = g_latestFrame;
      }
      if (!frame.bytes.empty()) {
        RECT rc{}; GetClientRect(hwnd, &rc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = frame.width;
        bmi.bmiHeader.biHeight = -frame.height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, frame.width, frame.height, frame.bytes.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
      } else {
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(hdc, RGB(220, 230, 255)); SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Waiting for native v2 video stream...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, 1);
  g_running.store(false);
  g_encodedCv.notify_all();
  g_decodedCv.notify_all();
  if (g_toolbarHwnd) { DestroyWindow(g_toolbarHwnd); g_toolbarHwnd = nullptr; }
  if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
  if (g_statsHwnd) { DestroyWindow(g_statsHwnd); g_statsHwnd = nullptr; }
  PostQuitMessage(g_exitCode);
  return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

static bool InitInputSocket() {
  g_inputSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_inputSock == INVALID_SOCKET) return false;
  int sndbuf = 64 * 1024;
  setsockopt(g_inputSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
  g_inputAddr.sin_family = AF_INET;
  g_inputAddr.sin_port = htons(g_cfg.inputPort);
  std::string ip = WideToUtf8(g_cfg.hostIp);
  return inet_pton(AF_INET, ip.c_str(), &g_inputAddr.sin_addr) == 1;
}

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
  g_recentDropScore.store(0.0, std::memory_order_relaxed);
  g_lastBitrateControlQpc.store(0, std::memory_order_relaxed);
  g_lastBitrateIncreaseQpc.store(0, std::memory_order_relaxed);

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
    ? std::thread(VideoReceiver(g_cfg.videoPort))
    : std::thread(TcpVideoReceiver(g_cfg.hostIp, g_cfg.videoPort));
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
