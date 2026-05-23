#include "renderer.h"

#include <algorithm>
#include <cstring>

bool D3DRenderer::Init(HWND hwnd, int width, int height) {
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

bool D3DRenderer::Render(const Nv12Frame& frame) {
    if (!swap_ || !ctx_ || frame.bytes.empty()) return false;
    if (frame.width != width_ || frame.height != height_) return false;
    if (frame.bytes.size() < static_cast<size_t>(width_) * height_ * 3 / 2) return false;

    if (!UploadNv12(frame)) return false;

    UINT presentFlags = allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(0, presentFlags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
    return SUCCEEDED(hr);
}

bool D3DRenderer::Render(const DxgiFrame& frame) {
    if (!swap_ || !ctx_ || !frame.texture) return false;
    if (!RenderTexture(frame.texture.Get(), frame.subresource)) return false;
    UINT presentFlags = allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(0, presentFlags);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
    return SUCCEEDED(hr);
}

ID3D11Device* D3DRenderer::Device() const { return device_.Get(); }

bool D3DRenderer::Reconfigure(int width, int height) {
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

void D3DRenderer::WaitForPresentReady() {
    if (frameLatencyWaitable_) {
      WaitForSingleObject(frameLatencyWaitable_, 8);
  }
}

bool D3DRenderer::CheckTearingSupport() {
    ComPtr<IDXGIFactory5> factory5;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory5)))) return false;
    BOOL allowTearing = FALSE;
    if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                             &allowTearing, sizeof(allowTearing)))) {
      return false;
  }
    return allowTearing == TRUE;
}

bool D3DRenderer::CompileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
      if (errors) OutputDebugStringA(reinterpret_cast<const char*>(errors->GetBufferPointer()));
      return false;
  }
    return true;
}

bool D3DRenderer::CreatePipeline() {
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

bool D3DRenderer::CreateNv12Textures() {
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

bool D3DRenderer::RenderTexture(ID3D11Texture2D* texture, UINT subresource) {
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

bool D3DRenderer::EnsureCopyNv12Texture() {
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

bool D3DRenderer::DrawWithSrvs(ID3D11ShaderResourceView* ySrv, ID3D11ShaderResourceView* uvSrv) {
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

bool D3DRenderer::UploadNv12(const Nv12Frame& frame) {
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

std::unique_ptr<D3DRenderer> g_renderer;
