#include "video_pipeline.h"

#include "input.h"
#include "renderer.h"

#include <ws2tcpip.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <codecapi.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

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
    Log(L"UDP video receiver bound on 0.0.0.0:%u fragmentPayload=%d", port_, kMaxVideoFragmentPayload);

    struct PartialFrame {
      uint32_t frameBytes = 0;
      uint16_t fragCount = 0;
      uint64_t ptsUs = 0;
      uint16_t flags = 0;
      uint64_t firstQpc = 0;
      uint16_t received = 0;
      uint16_t dataReceived = 0;
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
    bool reportedFirstPacket = false;
    bool reportedFirstCompleteFrame = false;

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
      const uint16_t dataFragCountFromBytes = static_cast<uint16_t>(
          (h->frameBytes + kMaxVideoFragmentPayload - 1) / kMaxVideoFragmentPayload);
      if (dataFragCountFromBytes == 0 || dataFragCountFromBytes > h->fragCount ||
          h->fragCount > dataFragCountFromBytes + 1) {
        continue;
      }

      const uint64_t now = QpcNow();
      g_packetsRx.fetch_add(1, std::memory_order_relaxed);
      g_bytesRx.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
      g_lastPacketQpc.store(now, std::memory_order_relaxed);
      if (!reportedFirstPacket) {
        reportedFirstPacket = true;
        Log(L"UDP first video packet frame=%llu frag=%u/%u payload=%u frameBytes=%u flags=0x%x",
            static_cast<unsigned long long>(h->frameId),
            static_cast<unsigned>(h->fragIndex + 1),
            static_cast<unsigned>(h->fragCount),
            static_cast<unsigned>(h->payloadBytes),
            static_cast<unsigned>(h->frameBytes),
            static_cast<unsigned>(h->flags));
      }

      if (newestFrameId && h->frameId + 12 < newestFrameId) continue;
      if (h->frameId > newestFrameId) {
        newestFrameId = h->frameId;
        const uint64_t keepFrom = newestFrameId > 3 ? newestFrameId - 3 : 0;
        for (auto it = partials.begin(); it != partials.end();) {
          if (it->first < keepFrom) {
            it = partials.erase(it);
            RecordNetworkFrameDrop();
          } else {
            ++it;
          }
        }
      }

      if (partials.size() > 8) {
        uint64_t keepFrom = newestFrameId > 8 ? newestFrameId - 8 : 0;
        for (auto it = partials.begin(); it != partials.end();) {
          if (it->first < keepFrom || QpcDeltaUs(it->second.firstQpc, now) > 120'000) {
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
        partial.dataFragCount = dataFragCountFromBytes;
        partial.hasFec = h->fragCount > dataFragCountFromBytes;
        partial.fecIndex = partial.dataFragCount;
      } else if (partial.fragCount != h->fragCount || partial.frameBytes != h->frameBytes) {
        partials.erase(it);
        RecordNetworkFrameDrop();
        continue;
      }

      if (partial.got[h->fragIndex]) continue;
      if ((h->flags & P2_FLAG_FEC) != 0) {
        if (h->fragIndex < partial.dataFragCount) continue;
        partial.hasFec = true;
        partial.fecIndex = h->fragIndex;
        partial.fecPayloadBytes = h->payloadBytes;
        partial.fec.assign(packet.data() + h->headerBytes, packet.data() + h->headerBytes + h->payloadBytes);
        partial.got[h->fragIndex] = 1;
        ++partial.received;
      } else {
        if (h->fragIndex >= partial.dataFragCount) continue;
        size_t off = static_cast<size_t>(h->fragIndex) * kMaxVideoFragmentPayload;
        if (off + h->payloadBytes > partial.bytes.size()) continue;
        memcpy(partial.bytes.data() + off, packet.data() + h->headerBytes, h->payloadBytes);
        partial.got[h->fragIndex] = 1;
        ++partial.received;
        ++partial.dataReceived;
      }

      bool frameReady = false;
      if (partial.dataReceived == partial.dataFragCount) {
        frameReady = true;
      } else if (partial.hasFec) {
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
          ++partial.dataReceived;
          frameReady = true;
        }
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
        if (!reportedFirstCompleteFrame) {
          reportedFirstCompleteFrame = true;
          Log(L"UDP first complete frame id=%llu bytes=%u frags=%u dataFrags=%u keyframe=%d",
              static_cast<unsigned long long>(h->frameId),
              static_cast<unsigned>(partial.frameBytes),
              static_cast<unsigned>(partial.fragCount),
              static_cast<unsigned>(partial.dataFragCount),
              (partial.flags & P2_FLAG_KEYFRAME) != 0 ? 1 : 0);
        }
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


void RunUdpVideoReceiver(uint16_t port) {
  VideoReceiver(port)();
}

void RunTcpVideoReceiver(std::wstring hostIp, uint16_t port) {
  TcpVideoReceiver(std::move(hostIp), port)();
}

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

  DecodeStatus Decode(const EncodedFrame& encoded, DecodedFrame& decoded) {
    ComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(encoded.bytes.size()), &buf);
    if (FAILED(hr)) return DecodeStatus::Error;
    BYTE* dst = nullptr; DWORD maxLen = 0;
    if (FAILED(buf->Lock(&dst, &maxLen, nullptr))) return DecodeStatus::Error;
    memcpy(dst, encoded.bytes.data(), encoded.bytes.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(encoded.bytes.size()));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return DecodeStatus::Error;
    if (FAILED(sample->AddBuffer(buf.Get()))) return DecodeStatus::Error;
    sample->SetSampleTime(static_cast<LONGLONG>(encoded.ptsUs * 10));
    sample->SetSampleDuration(10'000'000 / fps_);

    bool gotFrame = false;
    DecodedFrame last;

    hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      DecodedFrame drained;
      const DecodeStatus drainStatus = DrainOne(drained, encoded);
      if (drainStatus == DecodeStatus::Error) return DecodeStatus::Error;
      if (drainStatus == DecodeStatus::Frame) {
        last = std::move(drained);
        gotFrame = true;
      }
      hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return DecodeStatus::Error;

    for (int i = 0; i < 4; ++i) {
      DecodedFrame one;
      const DecodeStatus status = DrainOne(one, encoded);
      if (status == DecodeStatus::NeedMoreInput) break;
      if (status == DecodeStatus::Error) return gotFrame ? DecodeStatus::Frame : DecodeStatus::Error;
      last = std::move(one);
      gotFrame = true;
    }
    if (gotFrame) {
      decoded = std::move(last);
      return DecodeStatus::Frame;
    }
    return DecodeStatus::NeedMoreInput;
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

  DecodeStatus DrainOne(DecodedFrame& decoded, const EncodedFrame& meta) {
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
        if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuf))) return DecodeStatus::Error;
        if (FAILED(MFCreateSample(&outSample))) return DecodeStatus::Error;
        if (FAILED(outSample->AddBuffer(outBuf.Get()))) return DecodeStatus::Error;
      }
      output.pSample = outSample.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return DecodeStatus::NeedMoreInput;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      SetNv12OutputType();
      return DecodeStatus::NeedMoreInput;
    }
    if (FAILED(hr) && usingDxgiOutputSample) {
      // Some decoder configurations reject caller-provided DXGI samples; retry with a CPU sample.
      outSample.Reset();
      output = MFT_OUTPUT_DATA_BUFFER{};
      output.dwStreamID = 0;
      DWORD bufSize = std::max<DWORD>(info.cbSize, static_cast<DWORD>(width_ * height_ * 3 / 2));
      ComPtr<IMFMediaBuffer> outBuf;
      if (FAILED(MFCreateMemoryBuffer(bufSize, &outBuf))) return DecodeStatus::Error;
      if (FAILED(MFCreateSample(&outSample))) return DecodeStatus::Error;
      if (FAILED(outSample->AddBuffer(outBuf.Get()))) return DecodeStatus::Error;
      output.pSample = outSample.Get();
      hr = mft_->ProcessOutput(0, 1, &output, &status);
      if (output.pEvents) output.pEvents->Release();
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return DecodeStatus::NeedMoreInput;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      SetNv12OutputType();
      return DecodeStatus::NeedMoreInput;
    }
    if (FAILED(hr)) return DecodeStatus::Error;

    if (providesSamples && output.pSample) {
      outSample.Attach(output.pSample);
    }
    if (!outSample) return DecodeStatus::Error;

    // Zero-copy path: decoder gave us a D3D11 texture from the same device used by the renderer.
    if (sharedDxDevice_ && ExtractDxgi(outSample.Get(), decoded, meta)) {
      return DecodeStatus::Frame;
    }

    return CopySampleToNv12(outSample.Get(), decoded, meta) ? DecodeStatus::Frame : DecodeStatus::Error;
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

void DecoderThread() {
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
      ClearPendingVideoQueues();
      const VideoProfile activeProfile = ActiveVideoProfile();
      bool reconfigured = !g_renderer || g_renderer->Reconfigure(activeProfile.width, activeProfile.height);
      if (reconfigured) {
        useSharedDevice = sharedDeviceAvailableNow();
        if (auto rebuilt = createDecoder(useSharedDevice)) {
          decoder = std::move(rebuilt);
          appliedProfileGeneration = generation;
          g_decoderPrimed.store(false, std::memory_order_relaxed);
          g_decoderHasKeyframe.store(false, std::memory_order_relaxed);
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
    const DecodeStatus status = decoder->Decode(encoded, frame);
    if (status == DecodeStatus::Frame) {
      g_decoderPrimed.store(true, std::memory_order_relaxed);
      if (!g_loggedFirstDecodedFrame.exchange(true, std::memory_order_relaxed)) {
        Log(L"decoder first frame id=%llu mode=%s",
            static_cast<unsigned long long>(encoded.frameId),
            frame.gpu ? L"gpu" : L"cpu");
      }
      PushDecoded(std::move(frame));
    } else if (status == DecodeStatus::Error) {
      g_decodeFails.fetch_add(1, std::memory_order_relaxed);
      Log(L"decode failed frame=%llu bytes=%zu keyframe=%d",
          static_cast<unsigned long long>(encoded.frameId),
          encoded.bytes.size(),
          encoded.keyframe ? 1 : 0);
      EnterVideoRecovery(L"decode failed");
    }
  }
  MFShutdown();
  CoUninitialize();
}

void RenderThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  SetThreadDescription(GetCurrentThread(), L"P2P video present");
  uint32_t gpuPresentFailStreak = 0;

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
      g_framesPresented.fetch_add(1, std::memory_order_relaxed);
      if (frame.gpu) g_gpuFrames.fetch_add(1, std::memory_order_relaxed);
      else g_cpuFrames.fetch_add(1, std::memory_order_relaxed);
      uint64_t presentQpc = QpcNow();
      uint64_t recvQpc = frame.gpu ? frame.dxgi.recvQpc : frame.nv12.recvQpc;
      g_lastPresentQpc.store(presentQpc, std::memory_order_relaxed);
      g_lastRxToPresentUs.store(QpcDeltaUs(recvQpc, presentQpc), std::memory_order_relaxed);
      if (!g_loggedFirstPresentedFrame.exchange(true, std::memory_order_relaxed)) {
        Log(L"present first frame mode=%s", frame.gpu ? L"gpu" : L"cpu");
      }
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

void EnterVideoRecovery(const wchar_t* reason) {
  // 不再粗暴地切到"等关键帧"模式：之前会把 g_waitingForKeyframe 置为 true，
  // 导致 PushEncoded 丢掉所有 P 帧、等下一个 IDR（最长 keyframeSeconds 秒）才恢复，
  // 表现为视频像 PPT 一样跳一帧。改为：仅丢弃当前在飞的队列、请求一个新的
  // IDR，让 P 帧继续喂给解码器；在新 IDR 到来前画面可能短暂花屏/绿屏，
  // 但帧率立刻恢复，整体观感远好于"卡 1~2 秒再跳一帧"。
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
  SendInputPacket(P2_INPUT_REQUEST_KEYFRAME, 0, 0, 0, 0, 0, 0);
  Log(L"requested keyframe recovery: %s", reason ? reason : L"unknown");
}
