#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d3d11.h>
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
static constexpr int kMaxVideoFragmentPayload = 1200 - kVideoHeaderBytes;

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
static std::atomic<uint64_t> g_decodeFails{0};
static std::atomic<uint64_t> g_gpuRenderFails{0};
static std::atomic<uint64_t> g_lastPacketQpc{0};
static std::atomic<uint64_t> g_lastCompleteQpc{0};
static std::atomic<bool> g_decoderPrimed{false};
static uint64_t g_startedQpc = 0;
static std::wstring g_localIp = L"-";

struct NativeUiStats {
  double presentFps = 0.0;
  double completeFps = 0.0;
  double mbps = 0.0;
  double packetRate = 0.0;
  double rxToPresentMs = 0.0;
  double packetAgeMs = 0.0;
  double frameAgeMs = 0.0;
  double lossPct = 0.0;
  uint64_t dropped = 0;
  uint64_t decodeFails = 0;
  uint64_t gpuRenderFails = 0;
  uint64_t gpuFrames = 0;
  uint64_t cpuFrames = 0;
};

static std::mutex g_uiStatsMu;
static NativeUiStats g_uiStats;

static uint64_t QpcNow() {
  LARGE_INTEGER q{};
  QueryPerformanceCounter(&q);
  return static_cast<uint64_t>(q.QuadPart);
}

static uint64_t QpcDeltaUs(uint64_t start, uint64_t end) {
  if (!g_qpcFreq.QuadPart || end <= start) return 0;
  return (end - start) * 1'000'000ull / static_cast<uint64_t>(g_qpcFreq.QuadPart);
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
    // Until the decoder has successfully produced a frame, keep the newest
    // keyframe around and drop delta frames that would otherwise starve sync.
    if (!g_decoderPrimed.load(std::memory_order_relaxed) && !f.keyframe) {
      g_framesDropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (!g_encodedQueue.empty()) {
      const EncodedFrame& pending = g_encodedQueue.back();
      if (pending.keyframe && !f.keyframe) {
        g_framesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }

    g_encodedQueue.clear(); // latency policy: keep newest decodable frame only
    g_encodedQueue.emplace_back(std::move(f));
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
    desc.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swap_);
    if (FAILED(hr)) return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

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
};

static std::unique_ptr<D3DRenderer> g_renderer;

static constexpr int kToolbarWidth = 500;
static constexpr int kToolbarHeight = 64;
static constexpr int kMenuWidth = 430;
static constexpr int kMenuHeight = 518;
static constexpr int kStatsWidth = 560;
static constexpr int kStatsHeight = 720;
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
  x = std::max(r.left + 12, std::min(x, r.right - width - 12));
  y = std::max(r.top + 12, std::min(y, r.bottom - height - 12));
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
    int x = toolbarX + 35;
    int y = toolbarY + kToolbarHeight + 8;
    ClampToMonitor(x, y, kMenuWidth, kMenuHeight);
    SetWindowPos(g_menuHwnd, HWND_TOPMOST, x, y, kMenuWidth, kMenuHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRoundedRegion(g_menuHwnd, 18);
  }

  if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) {
    int x = toolbarX - 30;
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

static void DrawToolbar(HDC hdc, RECT rc) {
  HBRUSH bg = CreateSolidBrush(RGB(247, 249, 250));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(218, 224, 230));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 28, 28);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  HPEN ink = CreatePen(PS_SOLID, 2, RGB(22, 30, 40));
  HPEN oldInk = reinterpret_cast<HPEN>(SelectObject(hdc, ink));
  MoveToEx(hdc, 26, 23, nullptr); LineTo(hdc, 42, 39);
  MoveToEx(hdc, 42, 23, nullptr); LineTo(hdc, 26, 39);
  MoveToEx(hdc, 92, 31, nullptr); LineTo(hdc, 112, 31);
  MoveToEx(hdc, 102, 21, nullptr); LineTo(hdc, 102, 41);
  SelectObject(hdc, oldInk);
  DeleteObject(ink);

  HBRUSH controlPill = CreateSolidBrush(RGB(221, 226, 231));
  HBRUSH oldControlPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, controlPill));
  HPEN controlPen = CreatePen(PS_SOLID, 1, RGB(221, 226, 231));
  HPEN oldControlPen = reinterpret_cast<HPEN>(SelectObject(hdc, controlPen));
  RoundRect(hdc, 138, 8, 304, 56, 18, 18);
  SelectObject(hdc, oldControlPen);
  SelectObject(hdc, oldControlPill);
  DeleteObject(controlPen);
  DeleteObject(controlPill);

  HPEN controlInk = CreatePen(PS_SOLID, 2, RGB(24, 33, 44));
  HPEN oldControlInk = reinterpret_cast<HPEN>(SelectObject(hdc, controlInk));
  RoundRect(hdc, 158, 19, 184, 28, 5, 5);
  RoundRect(hdc, 158, 35, 184, 44, 5, 5);
  MoveToEx(hdc, 164, 23, nullptr); LineTo(hdc, 178, 23);
  MoveToEx(hdc, 164, 39, nullptr); LineTo(hdc, 178, 39);
  SelectObject(hdc, oldControlInk);
  DeleteObject(controlInk);

  HFONT controlFont = CreateUiFont(21, FW_SEMIBOLD);
  RECT controlRc{194, 14, 294, 50};
  DrawTextRect(hdc, L"控制中心", controlRc, RGB(18, 24, 34), controlFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(controlFont);

  HBRUSH pill = CreateSolidBrush(RGB(221, 226, 231));
  HBRUSH oldPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, pill));
  HPEN noPen = CreatePen(PS_SOLID, 1, RGB(221, 226, 231));
  HPEN oldNoPen = reinterpret_cast<HPEN>(SelectObject(hdc, noPen));
  RoundRect(hdc, 318, 8, 384, 56, 18, 18);
  SelectObject(hdc, oldNoPen);
  SelectObject(hdc, oldPill);
  DeleteObject(noPen);
  DeleteObject(pill);

  DrawSignalBars(hdc, 336, 17, RGB(17, 190, 122));

  HFONT font = CreateUiFont(24, FW_SEMIBOLD);
  RECT timeRc{396, 15, 490, 52};
  DrawTextRect(hdc, FormatElapsed(), timeRc, RGB(82, 87, 94), font, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(font);
}

static void DrawMenuRow(HDC hdc, int y, int icon, const std::wstring& label, const std::wstring& value, bool chevron, bool danger = false) {
  COLORREF ink = danger ? RGB(232, 62, 52) : RGB(28, 37, 48);
  COLORREF subtle = danger ? RGB(232, 62, 52) : RGB(120, 130, 142);
  DrawMenuIcon(hdc, icon, 24, y + 10, ink);
  HFONT labelFont = CreateUiFont(20, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(14, FW_NORMAL);
  RECT labelRc{62, y + 7, 274, y + 38};
  DrawTextRect(hdc, label, labelRc, ink, labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (!value.empty()) {
    RECT valueRc{246, y + 10, chevron ? 374 : 400, y + 37};
    DrawTextRect(hdc, value, valueRc, subtle, valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  if (chevron) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(26, 34, 44));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 384, y + 17, nullptr); LineTo(hdc, 392, y + 24); LineTo(hdc, 384, y + 31);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
  }
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
  HBRUSH bg = CreateSolidBrush(RGB(237, 248, 249));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(196, 210, 216));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  wchar_t displayValue[64];
  swprintf_s(displayValue, L"%dx%d", g_cfg.width, g_cfg.height);
  DrawMenuRow(hdc, 18, 0, DisplayLabel(), displayValue, true);
  DrawSeparator(hdc, 72);
  DrawMenuRow(hdc, 86, 1, L"画质", FormatDouble(g_cfg.fps, L" fps", 0), true);
  DrawMenuRow(hdc, 134, 2, L"窗口", g_cfg.fullscreen ? L"全屏" : L"窗口", true);
  DrawMenuRow(hdc, 182, 3, L"声音", L"未启用", true);
  DrawMenuRow(hdc, 230, 4, L"安全", g_cfg.udpVideo ? L"UDP 直连" : L"TCP 直连", true);
  DrawMenuRow(hdc, 278, 5, L"外设", L"键鼠", true);
  DrawMenuRow(hdc, 326, 6, L"更多", L"Native v2", true);
  DrawSeparator(hdc, 382);
  DrawMenuRow(hdc, 396, 7, g_cfg.fullscreen ? L"退出全屏幕" : L"进入全屏幕", L"F11", false);
  DrawSeparator(hdc, 452);
  DrawMenuRow(hdc, 466, 8, L"退出远控", L"", false, true);
}

static void DrawStatsRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value) {
  HFONT labelFont = CreateUiFont(19, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(21, FW_NORMAL);
  RECT labelRc{32, y, 205, y + 32};
  RECT valueRc{230, y, kStatsWidth - 32, y + 32};
  DrawTextRect(hdc, label, labelRc, RGB(146, 151, 160), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, value, valueRc, RGB(18, 24, 36), valueFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
  HFONT titleFont = CreateUiFont(24, FW_BOLD);
  std::wstring latency = stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms";
  RECT titleRc{92, 30, kStatsWidth - 30, 72};
  DrawTextRect(hdc, L"网络延时: " + latency, titleRc, RGB(15, 22, 36), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(titleFont);

  HPEN line = CreatePen(PS_SOLID, 1, RGB(232, 235, 238));
  HPEN oldLine = reinterpret_cast<HPEN>(SelectObject(hdc, line));
  MoveToEx(hdc, 0, 100, nullptr); LineTo(hdc, kStatsWidth, 100);
  SelectObject(hdc, oldLine);
  DeleteObject(line);

  DrawStatsRow(hdc, 126, L"帧延时:", stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms");
  DrawStatsRow(hdc, 166, L"帧率:", stats.presentFps > 0.1 ? FormatDouble(stats.presentFps, L"", 0) : FormatDouble(g_cfg.fps, L"", 0));
  DrawStatsRow(hdc, 206, L"带宽占用:", FormatDouble(stats.mbps, L" Mbps", 1));
  DrawStatsRow(hdc, 246, L"丢包率:", FormatDouble(stats.lossPct, L"%", 1));

  DrawStatsSeparator(hdc, 292);
  DrawStatsRow(hdc, 316, L"传输通道:", g_cfg.udpVideo ? L"UDP 局域网直连" : L"TCP 局域网直连");
  DrawStatsRow(hdc, 356, L"被控端 IP:", g_cfg.hostIp);
  DrawStatsRow(hdc, 396, L"控制端 IP:", g_localIp);
  DrawStatsRow(hdc, 436, L"客户端版本:", kNativeClientVersion);
  DrawStatsRow(hdc, 476, L"被控端系统:", PlatformLabel(g_cfg.hostPlatform));

  DrawStatsSeparator(hdc, 522);
  DrawStatsRow(hdc, 542, L"编解码器:", L"H.264 / Media Foundation");
  DrawStatsRow(hdc, 576, L"编码模式:", stats.gpuFrames > 0 ? L"硬编 / 硬解" : L"硬编 / 硬解优先");
  DrawStatsRow(hdc, 610, L"采集方式:", g_cfg.hostPlatform == L"win32" ? L"DXGI" : L"ScreenCaptureKit");
  wchar_t target[96];
  swprintf_s(target, L"%dx%d @ %d fps", g_cfg.width, g_cfg.height, g_cfg.fps);
  DrawStatsRow(hdc, 644, L"目标配置:", target);
  DrawStatsRow(hdc, 678, L"目标码率:", FormatBitrate(g_cfg.bitrate));
}

static void HandleToolbarClick(int x, int y) {
  if (x >= 18 && x <= 54 && y >= 14 && y <= 50) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  } else if (x >= 76 && x <= 124 && y >= 10 && y <= 54) {
    TogglePopup(g_menuHwnd);
  } else if (x >= 138 && x <= 304 && y >= 6 && y <= 58) {
    TogglePopup(g_menuHwnd);
  } else if (x >= 318 && x <= 384 && y >= 6 && y <= 58) {
    TogglePopup(g_statsHwnd);
  }
}

static void HandleMenuClick(int x, int y) {
  if (y >= 396 && y < 444) {
    ToggleNativeFullscreen();
  } else if (y >= 466 && y < 512) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  } else if (y >= 80 && y < 374) {
    ShowOnlyPopup(g_statsHwnd);
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
      std::vector<uint8_t> bytes;
      std::vector<uint8_t> got;
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
          if (it->first < keepFrom || QpcDeltaUs(it->second.firstQpc, now) > 250'000) {
            it = partials.erase(it);
            g_framesDropped.fetch_add(1, std::memory_order_relaxed);
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
      } else if (partial.fragCount != h->fragCount || partial.frameBytes != h->frameBytes) {
        partials.erase(it);
        g_framesDropped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      if (partial.got[h->fragIndex]) continue;
      size_t off = static_cast<size_t>(h->fragIndex) * kMaxVideoFragmentPayload;
      if (off + h->payloadBytes > partial.bytes.size()) continue;
      memcpy(partial.bytes.data() + off, packet.data() + h->headerBytes, h->payloadBytes);
      partial.got[h->fragIndex] = 1;
      ++partial.received;
      if (partial.received == partial.fragCount) {
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
  SetThreadDescription(GetCurrentThread(), L"P2P H264 decode + present");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION, MFSTARTUP_LITE);
  const bool sharedDeviceAvailable = g_renderer && g_renderer->Device();
  bool useSharedDevice = sharedDeviceAvailable;
  auto createDecoder = [&](bool preferSharedDevice) -> std::unique_ptr<MfDecoder> {
    auto decoder = std::make_unique<MfDecoder>();
    ID3D11Device* renderDevice = (preferSharedDevice && g_renderer) ? g_renderer->Device() : nullptr;
    if (!decoder->Init(g_cfg.width, g_cfg.height, g_cfg.fps, renderDevice)) return nullptr;
    return decoder;
  };

  auto decoder = createDecoder(useSharedDevice);
  if (!decoder) {
    MessageBoxW(nullptr, L"Failed to initialize Media Foundation H.264 decoder", L"P2P Native", MB_ICONERROR);
    MFShutdown();
    CoUninitialize();
    return;
  }
  uint32_t gpuPresentFailStreak = 0;

  while (g_running.load()) {
    EncodedFrame encoded;
    {
      std::unique_lock lk(g_encodedMu);
      g_encodedCv.wait(lk, [] { return !g_running.load() || !g_encodedQueue.empty(); });
      if (!g_running.load()) break;
      encoded = std::move(g_encodedQueue.back());
      g_encodedQueue.clear();
    }
    DecodedFrame frame;
    if (decoder->Decode(encoded, frame)) {
      g_decoderPrimed.store(true, std::memory_order_relaxed);
      bool presented = false;
      if (g_renderer) {
        if (frame.gpu) {
          presented = g_renderer->Render(frame.dxgi);
          if (!presented) {
            g_gpuRenderFails.fetch_add(1, std::memory_order_relaxed);
            ++gpuPresentFailStreak;
            if (useSharedDevice && sharedDeviceAvailable && gpuPresentFailStreak >= 4) {
              Log(L"GPU present failed %u times, falling back to CPU-copy decode", gpuPresentFailStreak);
              if (auto fallback = createDecoder(false)) {
                decoder = std::move(fallback);
                useSharedDevice = false;
                gpuPresentFailStreak = 0;
                g_decoderPrimed.store(false, std::memory_order_relaxed);
              }
            }
          } else {
            gpuPresentFailStreak = 0;
          }
        } else {
          presented = g_renderer->Render(frame.nv12);
          if (presented) gpuPresentFailStreak = 0;
        }
        if (presented) {
          g_framesPresented.fetch_add(1, std::memory_order_relaxed);
          if (frame.gpu) g_gpuFrames.fetch_add(1, std::memory_order_relaxed);
          else g_cpuFrames.fetch_add(1, std::memory_order_relaxed);
          uint64_t presentQpc = QpcNow();
          uint64_t recvQpc = frame.gpu ? frame.dxgi.recvQpc : frame.nv12.recvQpc;
          g_lastPresentQpc.store(presentQpc, std::memory_order_relaxed);
          g_lastRxToPresentUs.store(QpcDeltaUs(recvQpc, presentQpc), std::memory_order_relaxed);
        }
      }
      if (!presented && !frame.gpu) {
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
    } else {
      g_decodeFails.fetch_add(1, std::memory_order_relaxed);
    }
  }
  MFShutdown();
  CoUninitialize();
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

static void SendInput(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode) {
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
      stats.decodeFails = decodeFails;
      stats.gpuRenderFails = gpuRenderFails;
      stats.gpuFrames = gpu;
      stats.cpuFrames = cpu;
      const uint64_t knownFrames = complete + dropped;
      stats.lossPct = knownFrames ? (double(dropped) * 100.0 / double(knownFrames)) : 0.0;
      StoreUiStats(stats);

      wchar_t title[512];
      swprintf_s(title, L"P2P Native v2 %s -> %s | present %.0f fps complete %.0f fps | %.1f Mbps %.0f pkt/s | last pkt %.0f ms frame %.0f ms | rx-present %.2f ms | drop %llu decfail %llu gpuerr %llu | GPU %llu CPU %llu",
                 g_cfg.udpVideo ? L"UDP" : L"TCP", g_cfg.hostIp.c_str(), fps, cfps, mbps, pps, packetAgeMs, frameAgeMs, rxMs,
                 static_cast<unsigned long long>(dropped),
                 static_cast<unsigned long long>(decodeFails),
                 static_cast<unsigned long long>(gpuRenderFails),
                 static_cast<unsigned long long>(gpu),
                 static_cast<unsigned long long>(cpu));
      SetWindowTextW(hwnd, title);
      if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
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
        SetCursor(nullptr); // remote cursor is included in the captured stream; avoid double cursor
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = (wp & MK_RBUTTON) ? 2 : ((wp & MK_MBUTTON) ? 1 : 0);
      SendInput(P2_INPUT_MOVE, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
      SetCapture(hwnd); SetFocus(hwnd);
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONDOWN ? 2 : (msg == WM_MBUTTONDOWN ? 1 : 0);
      SendInput(P2_INPUT_DOWN, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
      ReleaseCapture();
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONUP ? 2 : (msg == WM_MBUTTONUP ? 1 : 0);
      SendInput(P2_INPUT_UP, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &pt);
      LPARAM clp = MAKELPARAM(pt.x, pt.y);
      float x, y; NormalizedPoint(hwnd, clp, x, y);
      SendInput(P2_INPUT_WHEEL, x, y, 0, -GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
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
      if (mac != 0xffff) SendInput(P2_INPUT_KEY_DOWN, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      uint16_t mac = VkToMacKeyCode(wp);
      if (mac != 0xffff) SendInput(P2_INPUT_KEY_UP, 0, 0, 0, 0, 0, mac);
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
      if (g_toolbarHwnd) { DestroyWindow(g_toolbarHwnd); g_toolbarHwnd = nullptr; }
      if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
      if (g_statsHwnd) { DestroyWindow(g_statsHwnd); g_statsHwnd = nullptr; }
      PostQuitMessage(0);
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
  if (!g_renderer->Init(g_hwnd, g_cfg.width, g_cfg.height)) {
    g_renderer.reset();
    MessageBoxW(g_hwnd, L"D3D11 flip-model renderer failed; falling back to GDI.", L"P2P Native", MB_ICONWARNING);
  }

  std::thread rx = g_cfg.udpVideo
    ? std::thread(VideoReceiver(g_cfg.videoPort))
    : std::thread(TcpVideoReceiver(g_cfg.hostIp, g_cfg.videoPort));
  std::thread dec(DecoderThread);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g_running.store(false);
  g_encodedCv.notify_all();
  if (rx.joinable()) rx.detach(); // recvfrom may block; process is exiting
  if (dec.joinable()) dec.join();
  g_renderer.reset();
  if (g_inputSock != INVALID_SOCKET) closesocket(g_inputSock);
  WSACleanup();
  timeEndPeriod(1);
  return 0;
}
