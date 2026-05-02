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

#include "p2_protocol.h"

using Microsoft::WRL::ComPtr;

static constexpr int kMaxUdp = 1500;
static constexpr int kVideoHeaderBytes = sizeof(P2VideoHeader);
static constexpr int kMaxVideoFragmentPayload = 1200 - kVideoHeaderBytes;

struct Config {
  std::wstring hostIp = L"127.0.0.1";
  uint16_t videoPort = 45000;
  uint16_t inputPort = 45001;
  int width = 1920;
  int height = 1080;
  int fps = 60;
  bool fullscreen = false;
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
    else if (wcscmp(argv[i], L"--video-port") == 0) { if (auto v = next()) c.videoPort = (uint16_t)_wtoi(v); }
    else if (wcscmp(argv[i], L"--input-port") == 0) { if (auto v = next()) c.inputPort = (uint16_t)_wtoi(v); }
    else if (wcscmp(argv[i], L"--width") == 0) { if (auto v = next()) c.width = _wtoi(v); }
    else if (wcscmp(argv[i], L"--height") == 0) { if (auto v = next()) c.height = _wtoi(v); }
    else if (wcscmp(argv[i], L"--fps") == 0) { if (auto v = next()) c.fps = std::max(30, _wtoi(v)); }
    else if (wcscmp(argv[i], L"--fullscreen") == 0) { c.fullscreen = true; }
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

static void PushEncoded(EncodedFrame&& f) {
  {
    std::lock_guard lk(g_encodedMu);
    g_encodedQueue.clear(); // latency policy: keep newest complete frame only
    g_encodedQueue.emplace_back(std::move(f));
  }
  g_encodedCv.notify_one();
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

    std::vector<uint8_t> packet(kMaxUdp);
    uint64_t currentId = 0;
    uint32_t frameBytes = 0;
    uint16_t fragCount = 0;
    uint64_t ptsUs = 0;
    uint16_t flags = 0;
    std::vector<uint8_t> frame;
    std::vector<uint8_t> got;
    uint16_t received = 0;

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

      if (h->frameId != currentId) {
        if (h->frameId < currentId) continue;
        currentId = h->frameId;
        frameBytes = h->frameBytes;
        fragCount = h->fragCount;
        ptsUs = h->ptsUs;
        flags = h->flags;
        frame.assign(frameBytes, 0);
        got.assign(fragCount, 0);
        received = 0;
      }
      if (h->fragCount != fragCount || h->frameBytes != frameBytes) continue;
      if (got[h->fragIndex]) continue;
      size_t off = static_cast<size_t>(h->fragIndex) * kMaxVideoFragmentPayload;
      if (off + h->payloadBytes > frame.size()) continue;
      memcpy(frame.data() + off, packet.data() + h->headerBytes, h->payloadBytes);
      got[h->fragIndex] = 1;
      ++received;
      if (received == fragCount) {
        EncodedFrame out;
        out.bytes = std::move(frame);
        out.frameId = currentId;
        out.ptsUs = ptsUs;
        out.recvQpc = QpcNow();
        out.keyframe = (flags & P2_FLAG_KEYFRAME) != 0;
        PushEncoded(std::move(out));
        currentId = 0;
      }
    }
    closesocket(s);
  }

 private:
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
    return DrainOne(decoded, encoded);
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
  MfDecoder decoder;
  ID3D11Device* renderDevice = g_renderer ? g_renderer->Device() : nullptr;
  if (!decoder.Init(g_cfg.width, g_cfg.height, g_cfg.fps, renderDevice)) {
    MessageBoxW(nullptr, L"Failed to initialize Media Foundation H.264 decoder", L"P2P Native", MB_ICONERROR);
    return;
  }

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
    if (decoder.Decode(encoded, frame)) {
      bool presented = false;
      if (g_renderer) {
        if (frame.gpu) presented = g_renderer->Render(frame.dxgi);
        else presented = g_renderer->Render(frame.nv12);
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
      static uint64_t lastQpc = QpcNow();
      uint64_t now = QpcNow();
      uint64_t frames = g_framesPresented.load(std::memory_order_relaxed);
      double seconds = double(QpcDeltaUs(lastQpc, now)) / 1'000'000.0;
      double fps = seconds > 0.001 ? double(frames - lastFrames) / seconds : 0.0;
      lastFrames = frames;
      lastQpc = now;
      uint64_t gpu = g_gpuFrames.load(std::memory_order_relaxed);
      uint64_t cpu = g_cpuFrames.load(std::memory_order_relaxed);
      double rxMs = double(g_lastRxToPresentUs.load(std::memory_order_relaxed)) / 1000.0;
      wchar_t title[256];
      swprintf_s(title, L"P2P Native v2 -> %s | %.0f fps | rx-present %.2f ms | GPU %llu CPU %llu",
                 g_cfg.hostIp.c_str(), fps, rxMs,
                 static_cast<unsigned long long>(gpu),
                 static_cast<unsigned long long>(cpu));
      SetWindowTextW(hwnd, title);
      return 0;
    }
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
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  timeBeginPeriod(1);
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
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
  g_renderer = std::make_unique<D3DRenderer>();
  if (!g_renderer->Init(g_hwnd, g_cfg.width, g_cfg.height)) {
    g_renderer.reset();
    MessageBoxW(g_hwnd, L"D3D11 flip-model renderer failed; falling back to GDI.", L"P2P Native", MB_ICONWARNING);
  }

  std::thread rx(VideoReceiver(g_cfg.videoPort));
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



