#pragma once

#include "app_state.h"

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

class D3DRenderer {
 public:
  bool Init(HWND hwnd, int width, int height);
  bool Render(const Nv12Frame& frame);
  bool Render(const DxgiFrame& frame);
  ID3D11Device* Device() const;
  bool Reconfigure(int width, int height);
  void WaitForPresentReady();

 private:
  bool CheckTearingSupport();
  bool CompileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob);
  bool CreatePipeline();
  bool CreateBackBufferRtv();
  bool EnsureBackBufferSize();
  bool CreateNv12Textures();
  bool RenderTexture(ID3D11Texture2D* texture, UINT subresource);
  bool GetTextureSrvs(ID3D11Texture2D* texture, UINT subresource,
                      ID3D11ShaderResourceView** ySrv,
                      ID3D11ShaderResourceView** uvSrv);
  void ClearTextureSrvCache();
  bool EnsureCopyNv12Texture();
  bool DrawWithSrvs(ID3D11ShaderResourceView* ySrv, ID3D11ShaderResourceView* uvSrv);
  bool UploadNv12(const Nv12Frame& frame);

  struct TextureSrvCacheEntry {
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    ComPtr<ID3D11ShaderResourceView> ySrv;
    ComPtr<ID3D11ShaderResourceView> uvSrv;
    uint64_t stamp = 0;
  };

  HWND hwnd_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int backBufferWidth_ = 0;
  int backBufferHeight_ = 0;
  bool allowTearing_ = false;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11RenderTargetView> rtv_;
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
  std::array<TextureSrvCacheEntry, 16> srvCache_;
  uint64_t srvCacheClock_ = 0;
  HANDLE frameLatencyWaitable_ = nullptr;
};
