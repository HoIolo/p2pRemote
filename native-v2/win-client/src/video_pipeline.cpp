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
#include <array>
#include <cstring>
#include <cwchar>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

static const wchar_t* HrName(HRESULT hr) {
  switch (hr) {
    case S_OK: return L"S_OK";
    case MF_E_TRANSFORM_NEED_MORE_INPUT: return L"MF_E_TRANSFORM_NEED_MORE_INPUT";
    case MF_E_TRANSFORM_STREAM_CHANGE: return L"MF_E_TRANSFORM_STREAM_CHANGE";
    case MF_E_NOTACCEPTING: return L"MF_E_NOTACCEPTING";
    case MF_E_INVALIDMEDIATYPE: return L"MF_E_INVALIDMEDIATYPE";
    case MF_E_TRANSFORM_TYPE_NOT_SET: return L"MF_E_TRANSFORM_TYPE_NOT_SET";
    default: return L"";
  }
}

static void FormatHr(HRESULT hr, wchar_t* out, size_t outCount) {
  const wchar_t* name = HrName(hr);
  if (name && name[0]) swprintf_s(out, outCount, L"%s (0x%08x)", name, static_cast<unsigned>(hr));
  else swprintf_s(out, outCount, L"0x%08x", static_cast<unsigned>(hr));
}

static size_t FindAnnexBStartCode(const uint8_t* data, size_t size, size_t offset, size_t* codeLen) {
  for (size_t i = offset; i + 3 <= size; ++i) {
    if (data[i] != 0 || data[i + 1] != 0) continue;
    if (data[i + 2] == 1) {
      if (codeLen) *codeLen = 3;
      return i;
    }
    if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
      if (codeLen) *codeLen = 4;
      return i;
    }
  }
  return size;
}

static void H264NalSummary(const std::vector<uint8_t>& bytes,
                           uint32_t& sps,
                           uint32_t& pps,
                           uint32_t& idr,
                           wchar_t* firstTypes,
                           size_t firstTypesCount) {
  sps = 0;
  pps = 0;
  idr = 0;
  if (firstTypes && firstTypesCount) firstTypes[0] = 0;
  const uint8_t* data = bytes.data();
  const size_t size = bytes.size();
  size_t len = 0;
  size_t sc = FindAnnexBStartCode(data, size, 0, &len);
  uint32_t listed = 0;
  while (sc < size) {
    const size_t nalStart = sc + len;
    if (nalStart >= size) break;
    size_t nextLen = 0;
    size_t next = FindAnnexBStartCode(data, size, nalStart, &nextLen);
    size_t nalEnd = next;
    while (nalEnd > nalStart && data[nalEnd - 1] == 0) --nalEnd;
    if (nalEnd > nalStart) {
      const uint8_t type = data[nalStart] & 0x1f;
      if (type == 7) ++sps;
      else if (type == 8) ++pps;
      else if (type == 5) ++idr;
      if (firstTypes && firstTypesCount && listed < 12) {
        wchar_t item[16];
        swprintf_s(item, L"%s%u", listed == 0 ? L"" : L",", static_cast<unsigned>(type));
        wcscat_s(firstTypes, firstTypesCount, item);
        ++listed;
      }
    }
    sc = next;
    len = nextLen;
  }
}

static std::vector<uint8_t> ExtractH264SequenceHeader(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> out;
  bool haveSps = false;
  bool havePps = false;
  const uint8_t* data = bytes.data();
  const size_t size = bytes.size();
  size_t len = 0;
  size_t sc = FindAnnexBStartCode(data, size, 0, &len);
  while (sc < size) {
    const size_t nalStart = sc + len;
    if (nalStart >= size) break;
    size_t nextLen = 0;
    size_t next = FindAnnexBStartCode(data, size, nalStart, &nextLen);
    size_t nalEnd = next;
    while (nalEnd > nalStart && data[nalEnd - 1] == 0) --nalEnd;
    if (nalEnd > nalStart) {
      const uint8_t type = data[nalStart] & 0x1f;
      if (type == 7 || type == 8) {
        static const uint8_t kStartCode[] = {0, 0, 0, 1};
        out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
        out.insert(out.end(), data + nalStart, data + nalEnd);
        if (type == 7) haveSps = true;
        if (type == 8) havePps = true;
      }
    }
    if (haveSps && havePps) break;
    sc = next;
    len = nextLen;
  }
  if (!haveSps || !havePps) out.clear();
  return out;
}

static bool EnvFlagEnabled(const wchar_t* name) {
  wchar_t value[16]{};
  DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
  return n > 0 && (_wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0 || _wcsicmp(value, L"yes") == 0);
}

static const wchar_t* VideoSubtypeName(const GUID& subtype) {
  if (IsEqualGUID(subtype, MFVideoFormat_NV12)) return L"NV12";
  if (IsEqualGUID(subtype, MFVideoFormat_YV12)) return L"YV12";
  if (IsEqualGUID(subtype, MFVideoFormat_IYUV)) return L"IYUV";
  if (IsEqualGUID(subtype, MFVideoFormat_I420)) return L"I420";
  if (IsEqualGUID(subtype, MFVideoFormat_YUY2)) return L"YUY2";
  if (IsEqualGUID(subtype, MFVideoFormat_RGB32)) return L"RGB32";
  if (IsEqualGUID(subtype, MFVideoFormat_ARGB32)) return L"ARGB32";
  return L"unknown";
}

static bool IsSupportedDecoderOutputSubtype(const GUID& subtype) {
  return IsEqualGUID(subtype, MFVideoFormat_NV12) ||
         IsEqualGUID(subtype, MFVideoFormat_IYUV) ||
         IsEqualGUID(subtype, MFVideoFormat_I420) ||
         IsEqualGUID(subtype, MFVideoFormat_YV12) ||
         IsEqualGUID(subtype, MFVideoFormat_YUY2);
}

static bool IsPreferredDecoderOutputSubtype(const GUID& subtype, int rank, bool preferNv12) {
  if (preferNv12) {
    switch (rank) {
      case 0: return IsEqualGUID(subtype, MFVideoFormat_NV12);
      case 1: return IsEqualGUID(subtype, MFVideoFormat_IYUV);
      case 2: return IsEqualGUID(subtype, MFVideoFormat_I420);
      case 3: return IsEqualGUID(subtype, MFVideoFormat_YV12);
      case 4: return IsEqualGUID(subtype, MFVideoFormat_YUY2);
      default: return false;
    }
  }
  switch (rank) {
    case 0: return IsEqualGUID(subtype, MFVideoFormat_IYUV);
    case 1: return IsEqualGUID(subtype, MFVideoFormat_I420);
    case 2: return IsEqualGUID(subtype, MFVideoFormat_YV12);
    case 3: return IsEqualGUID(subtype, MFVideoFormat_NV12);
    case 4: return IsEqualGUID(subtype, MFVideoFormat_YUY2);
    default: return false;
  }
}

static bool Planar420ToNv12(const uint8_t* src,
                            DWORD srcLen,
                            int width,
                            int height,
                            bool yv12,
                            std::vector<uint8_t>& out) {
  const int yStride = width;
  const int chromaWidth = width / 2;
  const int chromaHeight = height / 2;
  const int chromaStride = chromaWidth;
  int surfaceHeight = static_cast<int>((static_cast<uint64_t>(srcLen) * 2) / (3ull * yStride));
  surfaceHeight &= ~1;
  if (surfaceHeight < height) surfaceHeight = height;
  const size_t needed = static_cast<size_t>(yStride) * surfaceHeight +
                        static_cast<size_t>(chromaStride) * (surfaceHeight / 2) * 2;
  if (srcLen < needed) return false;
  const size_t ySize = static_cast<size_t>(width) * height;
  const uint8_t* yPlane = src;
  const uint8_t* firstChroma = src + static_cast<size_t>(yStride) * surfaceHeight;
  const uint8_t* secondChroma = firstChroma + static_cast<size_t>(chromaStride) * (surfaceHeight / 2);
  const uint8_t* uPlane = yv12 ? secondChroma : firstChroma;
  const uint8_t* vPlane = yv12 ? firstChroma : secondChroma;
  out.resize(ySize + ySize / 2);
  memcpy(out.data(), yPlane, ySize);
  uint8_t* uvOut = out.data() + ySize;
  for (int y = 0; y < chromaHeight; ++y) {
    for (int x = 0; x < chromaWidth; ++x) {
      const size_t srcIndex = static_cast<size_t>(y) * chromaStride + x;
      const size_t dstIndex = static_cast<size_t>(y) * width + x * 2;
      uvOut[dstIndex + 0] = uPlane[srcIndex];
      uvOut[dstIndex + 1] = vPlane[srcIndex];
    }
  }
  return true;
}

static bool Yuy2ToNv12(const uint8_t* src,
                       DWORD srcLen,
                       int width,
                       int height,
                       std::vector<uint8_t>& out) {
  const size_t needed = static_cast<size_t>(width) * height * 2;
  if (srcLen < needed) return false;
  const size_t ySize = static_cast<size_t>(width) * height;
  out.assign(ySize + ySize / 2, 128);
  uint8_t* yOut = out.data();
  uint8_t* uvOut = out.data() + ySize;

  for (int row = 0; row < height; ++row) {
    const uint8_t* line = src + static_cast<size_t>(row) * width * 2;
    uint8_t* yLine = yOut + static_cast<size_t>(row) * width;
    for (int x = 0; x < width; x += 2) {
      const size_t p = static_cast<size_t>(x) * 2;
      yLine[x] = line[p + 0];
      if (x + 1 < width) yLine[x + 1] = line[p + 2];
    }
  }

  for (int row = 0; row < height; row += 2) {
    const uint8_t* line0 = src + static_cast<size_t>(row) * width * 2;
    const uint8_t* line1 = (row + 1 < height) ? (src + static_cast<size_t>(row + 1) * width * 2) : line0;
    uint8_t* uvLine = uvOut + static_cast<size_t>(row / 2) * width;
    for (int x = 0; x < width; x += 2) {
      const size_t p = static_cast<size_t>(x) * 2;
      uvLine[x] = static_cast<uint8_t>((static_cast<int>(line0[p + 1]) + static_cast<int>(line1[p + 1]) + 1) / 2);
      if (x + 1 < width) {
        uvLine[x + 1] = static_cast<uint8_t>((static_cast<int>(line0[p + 3]) + static_cast<int>(line1[p + 3]) + 1) / 2);
      }
    }
  }
  return true;
}

static void CopyPlaneRows(const uint8_t* src,
                          LONG stride,
                          int rowBytes,
                          int rows,
                          uint8_t* dst,
                          int dstStride) {
  if (stride < 0) {
    src += static_cast<size_t>(rows - 1) * static_cast<size_t>(-stride);
  }
  for (int y = 0; y < rows; ++y) {
    memcpy(dst + static_cast<size_t>(y) * dstStride, src, rowBytes);
    src += stride;
  }
}

static int AbsStride(LONG stride) {
  return stride < 0 ? static_cast<int>(-stride) : static_cast<int>(stride);
}

static int Derive420SurfaceHeight(DWORD srcLen, int strideAbs, int visibleHeight) {
  if (strideAbs <= 0) return visibleHeight;
  int surfaceHeight = static_cast<int>((static_cast<uint64_t>(srcLen) * 2) / (3ull * strideAbs));
  surfaceHeight &= ~1;
  if (surfaceHeight < visibleHeight) surfaceHeight = visibleHeight;
  return surfaceHeight;
}

static bool Nv12StridedToNv12(const uint8_t* src,
                              DWORD srcLen,
                              LONG yStride,
                              int width,
                              int height,
                              std::vector<uint8_t>& out) {
  const int yStrideAbs = std::max(width, AbsStride(yStride));
  const int surfaceHeight = Derive420SurfaceHeight(srcLen, yStrideAbs, height);
  const size_t yPlaneBytes = static_cast<size_t>(yStrideAbs) * surfaceHeight;
  if (srcLen < yPlaneBytes + static_cast<size_t>(yStrideAbs) * (surfaceHeight / 2)) return false;
  const size_t ySize = static_cast<size_t>(width) * height;
  out.resize(ySize + ySize / 2);
  uint8_t* yOut = out.data();
  uint8_t* uvOut = out.data() + ySize;
  CopyPlaneRows(src, yStride, width, height, yOut, width);
  const uint8_t* uvPlane = src + yPlaneBytes;
  CopyPlaneRows(uvPlane, yStrideAbs, width, height / 2, uvOut, width);
  return true;
}

static bool Planar420StridedToNv12(const uint8_t* src,
                                   DWORD srcLen,
                                   LONG yStride,
                                   int width,
                                   int height,
                                   bool yv12,
                                   std::vector<uint8_t>& out) {
  const int yStrideAbs = std::max(width, AbsStride(yStride));
  const int surfaceHeight = Derive420SurfaceHeight(srcLen, yStrideAbs, height);
  const int chromaWidth = width / 2;
  const int chromaHeight = height / 2;
  const int chromaStride = std::max(chromaWidth, yStrideAbs / 2);
  const size_t yPlaneBytes = static_cast<size_t>(yStrideAbs) * surfaceHeight;
  const size_t chromaPlaneBytes = static_cast<size_t>(chromaStride) * (surfaceHeight / 2);
  if (srcLen < yPlaneBytes + chromaPlaneBytes * 2) return false;
  const size_t ySize = static_cast<size_t>(width) * height;
  out.resize(ySize + ySize / 2);

  uint8_t* yOut = out.data();
  uint8_t* uvOut = out.data() + ySize;
  const uint8_t* yPlane = src;
  CopyPlaneRows(yPlane, yStride, width, height, yOut, width);

  const uint8_t* firstChroma = src + yPlaneBytes;
  const uint8_t* secondChroma = firstChroma + chromaPlaneBytes;
  const uint8_t* uPlane = yv12 ? secondChroma : firstChroma;
  const uint8_t* vPlane = yv12 ? firstChroma : secondChroma;

  for (int y = 0; y < chromaHeight; ++y) {
    const uint8_t* uLine = uPlane + static_cast<size_t>(y) * chromaStride;
    const uint8_t* vLine = vPlane + static_cast<size_t>(y) * chromaStride;
    uint8_t* uvLine = uvOut + static_cast<size_t>(y) * width;
    for (int x = 0; x < chromaWidth; ++x) {
      uvLine[x * 2 + 0] = uLine[x];
      uvLine[x * 2 + 1] = vLine[x];
    }
  }
  return true;
}

static bool Yuy2StridedToNv12(const uint8_t* src,
                              LONG stride,
                              int width,
                              int height,
                              std::vector<uint8_t>& out) {
  if (stride < 0) {
    src += static_cast<size_t>(height - 1) * static_cast<size_t>(-stride);
  }
  const size_t ySize = static_cast<size_t>(width) * height;
  out.assign(ySize + ySize / 2, 128);
  uint8_t* yOut = out.data();
  uint8_t* uvOut = out.data() + ySize;

  for (int row = 0; row < height; ++row) {
    const uint8_t* line = src + static_cast<size_t>(row) * stride;
    uint8_t* yLine = yOut + static_cast<size_t>(row) * width;
    for (int x = 0; x < width; x += 2) {
      const size_t p = static_cast<size_t>(x) * 2;
      yLine[x] = line[p + 0];
      if (x + 1 < width) yLine[x + 1] = line[p + 2];
    }
  }

  for (int row = 0; row < height; row += 2) {
    const uint8_t* line0 = src + static_cast<size_t>(row) * stride;
    const uint8_t* line1 = (row + 1 < height) ? (src + static_cast<size_t>(row + 1) * stride) : line0;
    uint8_t* uvLine = uvOut + static_cast<size_t>(row / 2) * width;
    for (int x = 0; x < width; x += 2) {
      const size_t p = static_cast<size_t>(x) * 2;
      uvLine[x] = static_cast<uint8_t>((static_cast<int>(line0[p + 1]) + static_cast<int>(line1[p + 1]) + 1) / 2);
      if (x + 1 < width) {
        uvLine[x + 1] = static_cast<uint8_t>((static_cast<int>(line0[p + 3]) + static_cast<int>(line1[p + 3]) + 1) / 2);
      }
    }
  }
  return true;
}

class VideoReceiver {
 public:
  explicit VideoReceiver(uint16_t port) : port_(port) {}

  void operator()() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"P2P UDP video receiver");

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    int rcvbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    DWORD timeoutMs = 10;
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
    auto partialTtlUs = []() -> uint64_t {
      const int fps = std::max(30, g_activeVideoFps.load(std::memory_order_relaxed));
      return std::max<uint64_t>(16'000, 2'000'000ull / static_cast<uint64_t>(fps));
    };

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

      if (newestFrameId && h->frameId + 2 < newestFrameId) continue;
      if (h->frameId > newestFrameId) {
        newestFrameId = h->frameId;
        const uint64_t keepFrom = newestFrameId > 1 ? newestFrameId - 1 : 0;
        for (auto it = partials.begin(); it != partials.end();) {
          if (it->first < keepFrom || QpcDeltaUs(it->second.firstQpc, now) > partialTtlUs()) {
            it = partials.erase(it);
            RecordNetworkFrameDrop();
          } else {
            ++it;
          }
        }
      }

      if (partials.size() > 4) {
        uint64_t keepFrom = newestFrameId > 1 ? newestFrameId - 1 : 0;
        for (auto it = partials.begin(); it != partials.end();) {
          if (it->first < keepFrom || QpcDeltaUs(it->second.firstQpc, now) > partialTtlUs()) {
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
      } else if (newestFrameId > h->frameId + 1 || QpcDeltaUs(partial.firstQpc, now) > partialTtlUs()) {
        partials.erase(h->frameId);
        RecordNetworkFrameDrop();
        continue;
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
  VideoReceiver{port}();
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
  bool Init(int width, int height, int fps, ID3D11Device* sharedDevice, bool forceCpu) {
    width_ = width;
    height_ = height;
    fps_ = std::max(30, fps);
    forceCpuOutput_ = forceCpu ||
                      EnvFlagEnabled(L"P2P_NATIVE_V2_FORCE_CPU_DECODE") ||
                      EnvFlagEnabled(L"P2P_NATIVE_V2_DISABLE_DXVA");
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) {
      wchar_t hrText[96];
      FormatHr(hr, hrText, std::size(hrText));
      Log(L"decoder init failed: CoCreateInstance CMSH264DecoderMFT %s", hrText);
      return false;
    }
    if (!forceCpuOutput_) {
      InitDxvaDeviceManager(sharedDevice);
    } else {
      Log(L"decoder init: CPU/system-memory output (DXVA disabled/forced off)");
    }

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

    return true;
  }

  bool UsingDxva() const { return dxvaEnabled_; }

  DecodeStatus Decode(const EncodedFrame& encoded, DecodedFrame& decoded) {
    if (!inputTypeSet_) {
      if (!ConfigureInputType(encoded)) return DecodeStatus::Error;
    }
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
      for (;;) {
        const DecodeStatus drainStatus = DrainOne(drained, encoded);
        if (drainStatus == DecodeStatus::NeedMoreInput) break;
        if (drainStatus == DecodeStatus::Error) return DecodeStatus::Error;
        last = std::move(drained);
        gotFrame = true;
      }
      hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
      if (!reportedProcessInputError_) {
        reportedProcessInputError_ = true;
        wchar_t hrText[96];
        FormatHr(hr, hrText, std::size(hrText));
        Log(L"decoder ProcessInput failed frame=%llu bytes=%zu keyframe=%d hr=%s",
            static_cast<unsigned long long>(encoded.frameId),
            encoded.bytes.size(),
            encoded.keyframe ? 1 : 0,
            hrText);
      }
      return DecodeStatus::Error;
    }

    for (;;) {
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
  bool ConfigureInputType(const EncodedFrame& encoded) {
    uint32_t sps = 0, pps = 0, idr = 0;
    wchar_t firstTypes[96]{};
    H264NalSummary(encoded.bytes, sps, pps, idr, firstTypes, std::size(firstTypes));
    std::vector<uint8_t> sequenceHeader = ExtractH264SequenceHeader(encoded.bytes);

    auto trySubtype = [&](const GUID& subtype, const wchar_t* name, bool attachSequenceHeader) -> HRESULT {
      ComPtr<IMFMediaType> in;
      HRESULT hr = MFCreateMediaType(&in);
      if (FAILED(hr)) return hr;
      in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      in->SetGUID(MF_MT_SUBTYPE, subtype);
      MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width_, height_);
      MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps_, 1);
      in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
      if (attachSequenceHeader && !sequenceHeader.empty()) {
        in->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                    sequenceHeader.data(),
                    static_cast<UINT32>(sequenceHeader.size()));
      }
      hr = mft_->SetInputType(0, in.Get(), 0);
      if (SUCCEEDED(hr)) {
        if (!SetDecoderOutputType(false)) {
          Log(L"decoder input accepted but output setup failed: subtype=%s", name);
          return E_FAIL;
        }
        StartStreaming();
        inputTypeSet_ = true;
        inputSubtypeName_ = name;
        Log(L"decoder input configured: subtype=%s size=%dx%d@%d seq=%zu firstFrame=%llu bytes=%zu keyframe=%d sps=%u pps=%u idr=%u nals=[%s]",
            name,
            width_, height_, fps_,
            sequenceHeader.size(),
            static_cast<unsigned long long>(encoded.frameId),
            encoded.bytes.size(),
            encoded.keyframe ? 1 : 0,
            sps, pps, idr,
            firstTypes[0] ? firstTypes : L"-");
      } else {
        wchar_t hrText[96];
        FormatHr(hr, hrText, std::size(hrText));
        Log(L"decoder input rejected: subtype=%s seq=%zu hr=%s sps=%u pps=%u idr=%u nals=[%s]",
            name,
            sequenceHeader.size(),
            hrText,
            sps, pps, idr,
            firstTypes[0] ? firstTypes : L"-");
      }
      return hr;
    };

    // The Mac VideoToolbox sender gives us complete Annex-B access units.  Some
    // Windows H.264 MFT builds wait forever when Annex-B is advertised as the
    // generic frame-aligned H264 subtype without a sequence header; H264_ES is
    // the tolerant elementary-stream subtype.  Keep H264 as a fallback for older
    // systems that do not accept H264_ES.
    HRESULT hr = trySubtype(MFVideoFormat_H264_ES, L"H264_ES", true);
    if (FAILED(hr)) hr = trySubtype(MFVideoFormat_H264_ES, L"H264_ES", false);
    if (FAILED(hr)) hr = trySubtype(MFVideoFormat_H264, L"H264", true);
    if (FAILED(hr)) hr = trySubtype(MFVideoFormat_H264, L"H264", false);
    return SUCCEEDED(hr);
  }

  bool SetDecoderOutputType(bool clearFirst) {
    HRESULT lastHr = E_FAIL;
    if (clearFirst) {
      // Do not clear with SetOutputType(nullptr) during MF_E_TRANSFORM_STREAM_CHANGE.
      // Some Windows H.264 decoder MFT builds reject every subsequent candidate
      // after an explicit clear.  Just mark local state stale and apply one of
      // the freshly advertised types as-is.
      outputTypeSet_ = false;
    }

    bool loggedCandidates = false;
    for (int rank = 0; rank <= 4; ++rank) {
      for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = mft_->GetOutputAvailableType(0, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) {
          lastHr = hr;
          break;
        }

        GUID subtype{};
        if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        if (!loggedCandidates && i < 8) {
          Log(L"decoder output candidate[%u]: %s", i, VideoSubtypeName(subtype));
        }
        if (!IsPreferredDecoderOutputSubtype(subtype, rank, dxvaEnabled_)) continue;

        // Use the advertised type exactly as returned.  It can contain private
        // decoder attributes; rewriting frame-size/rate/interlace here makes
        // some MFT implementations reject the otherwise valid type.
        hr = mft_->SetOutputType(0, candidate.Get(), 0);
        if (SUCCEEDED(hr)) {
          outputSubtype_ = subtype;
          outputTypeSet_ = true;
          Log(L"decoder output configured: advertised %s candidate=%u dxva=%d",
              VideoSubtypeName(subtype),
              i,
              dxvaEnabled_ ? 1 : 0);
          return true;
        }
        if (i < 8) {
          wchar_t hrText[96];
          FormatHr(hr, hrText, std::size(hrText));
          Log(L"decoder output set failed: advertised %s candidate=%u hr=%s",
              VideoSubtypeName(subtype),
              i,
              hrText);
        }
        lastHr = hr;
      }
      loggedCandidates = true;
    }

    for (int rank = 0; rank <= 4; ++rank) {
      GUID subtype = MFVideoFormat_IYUV;
      if (dxvaEnabled_) {
        if (rank == 0) subtype = MFVideoFormat_NV12;
        else if (rank == 1) subtype = MFVideoFormat_IYUV;
        else if (rank == 2) subtype = MFVideoFormat_I420;
        else if (rank == 3) subtype = MFVideoFormat_YV12;
        else if (rank == 4) subtype = MFVideoFormat_YUY2;
      } else {
        if (rank == 1) subtype = MFVideoFormat_I420;
        else if (rank == 2) subtype = MFVideoFormat_YV12;
        else if (rank == 3) subtype = MFVideoFormat_NV12;
        else if (rank == 4) subtype = MFVideoFormat_YUY2;
      }

      ComPtr<IMFMediaType> out;
      HRESULT hr = MFCreateMediaType(&out);
      if (FAILED(hr)) return false;
      out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      out->SetGUID(MF_MT_SUBTYPE, subtype);
      MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width_, height_);
      MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps_, 1);
      out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      hr = mft_->SetOutputType(0, out.Get(), 0);
      if (SUCCEEDED(hr)) {
        outputSubtype_ = subtype;
        outputTypeSet_ = true;
        Log(L"decoder output configured: manual %s dxva=%d",
            VideoSubtypeName(subtype),
            dxvaEnabled_ ? 1 : 0);
        return true;
      }
      lastHr = hr;
    }

    if (FAILED(lastHr)) {
      wchar_t hrText[96];
      FormatHr(lastHr, hrText, std::size(hrText));
      Log(L"decoder output type rejected: %s", hrText);
    }
    return false;
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
        Log(L"decoder init: DXVA device create failed; falling back to CPU/system-memory output");
        forceCpuOutput_ = true;
        return;
      }
    }
    if (dxDevice_) {
      ComPtr<ID3D11Multithread> multithread;
      if (SUCCEEDED(dxDevice_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
      }
    }
    UINT resetToken = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiManager_);
    if (FAILED(hr)) {
      wchar_t hrText[96];
      FormatHr(hr, hrText, std::size(hrText));
      Log(L"decoder init: MFCreateDXGIDeviceManager failed %s; falling back to CPU/system-memory output", hrText);
      dxgiManager_.Reset();
      forceCpuOutput_ = true;
      return;
    }
    hr = dxgiManager_->ResetDevice(dxDevice_.Get(), resetToken);
    if (FAILED(hr)) {
      wchar_t hrText[96];
      FormatHr(hr, hrText, std::size(hrText));
      Log(L"decoder init: DXGI ResetDevice failed %s; falling back to CPU/system-memory output", hrText);
      dxgiManager_.Reset();
      forceCpuOutput_ = true;
      return;
    }
    hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(dxgiManager_.Get()));
    if (FAILED(hr)) {
      wchar_t hrText[96];
      FormatHr(hr, hrText, std::size(hrText));
      Log(L"decoder init: SET_D3D_MANAGER failed %s; falling back to CPU/system-memory output", hrText);
      dxgiManager_.Reset();
      forceCpuOutput_ = true;
      return;
    }
    dxvaEnabled_ = true;
    Log(L"decoder init: DXVA enabled (%s renderer device)",
        sharedDxDevice_ ? L"shared" : L"private");
  }

  void StartStreaming() {
    if (streamingStarted_) return;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    streamingStarted_ = true;
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
      if (dxvaEnabled_ && sharedDxDevice_ && IsEqualGUID(outputSubtype_, MFVideoFormat_NV12) &&
          CreateDxgiOutputSample(&outSample)) {
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
      Log(L"decoder stream change; resetting output type");
      SetDecoderOutputType(true);
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
      Log(L"decoder stream change after retry; resetting output type");
      SetDecoderOutputType(true);
      return DecodeStatus::NeedMoreInput;
    }
    if (FAILED(hr)) {
      if (!reportedProcessOutputError_) {
        reportedProcessOutputError_ = true;
        wchar_t hrText[96];
        FormatHr(hr, hrText, std::size(hrText));
        Log(L"decoder ProcessOutput failed subtype=%s gpuSample=%d hr=%s",
            inputSubtypeName_,
            usingDxgiOutputSample ? 1 : 0,
            hrText);
      }
      return DecodeStatus::Error;
    }

    if (providesSamples && output.pSample) {
      outSample.Attach(output.pSample);
    }
    if (!outSample) return DecodeStatus::Error;

    // Zero-copy path: decoder gave us a D3D11 texture from the same device used by the renderer.
    if (dxvaEnabled_ && sharedDxDevice_ && ExtractDxgi(outSample.Get(), decoded, meta)) {
      return DecodeStatus::Frame;
    }

    return CopySampleToNv12(outSample.Get(), decoded, meta) ? DecodeStatus::Frame : DecodeStatus::Error;
  }

  bool CreateDxgiOutputSample(ComPtr<IMFSample>* sampleOut) {
    if (!sampleOut || !EnsureDxgiOutputPool()) return false;
    *sampleOut = dxgiOutputPool_[dxgiOutputPoolIndex_].sample;
    dxgiOutputPoolIndex_ = (dxgiOutputPoolIndex_ + 1) % dxgiOutputPool_.size();
    return true;
  }

  bool EnsureDxgiOutputPool() {
    if (!dxDevice_) return false;
    if (!dxgiOutputPool_.empty()) return true;

    constexpr size_t kPoolSize = 8;
    dxgiOutputPool_.reserve(kPoolSize);
    for (size_t i = 0; i < kPoolSize; ++i) {
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
      if (FAILED(dxDevice_->CreateTexture2D(&desc, nullptr, &texture))) {
        dxgiOutputPool_.clear();
        return false;
      }
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &buffer))) {
        dxgiOutputPool_.clear();
        return false;
      }
      ComPtr<IMFSample> sample;
      if (FAILED(MFCreateSample(&sample))) {
        dxgiOutputPool_.clear();
        return false;
      }
      if (FAILED(sample->AddBuffer(buffer.Get()))) {
        dxgiOutputPool_.clear();
        return false;
      }
      dxgiOutputPool_.push_back(DxgiOutputPoolEntry{sample});
    }
    dxgiOutputPoolIndex_ = 0;
    Log(L"decoder DXGI output sample pool created: %zu textures", dxgiOutputPool_.size());
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
    ComPtr<IMFMediaBuffer> original;
    if (FAILED(sample->GetBufferByIndex(0, &original))) return false;

    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(original.As(&buffer2d))) {
      BYTE* scanline0 = nullptr;
      LONG stride = 0;
      HRESULT hr = buffer2d->Lock2D(&scanline0, &stride);
      if (SUCCEEDED(hr)) {
        Nv12Frame nv12;
        nv12.width = width_;
        nv12.height = height_;
        nv12.frameId = meta.frameId;
        nv12.recvQpc = meta.recvQpc;
        bool converted = false;
        DWORD contiguousLen = 0;
        original->GetCurrentLength(&contiguousLen);
        if (IsEqualGUID(outputSubtype_, MFVideoFormat_NV12)) {
          converted = Nv12StridedToNv12(scanline0, contiguousLen, stride, width_, height_, nv12.bytes);
        } else if (IsEqualGUID(outputSubtype_, MFVideoFormat_IYUV) ||
                   IsEqualGUID(outputSubtype_, MFVideoFormat_I420)) {
          converted = Planar420StridedToNv12(scanline0, contiguousLen, stride, width_, height_, false, nv12.bytes);
        } else if (IsEqualGUID(outputSubtype_, MFVideoFormat_YV12)) {
          converted = Planar420StridedToNv12(scanline0, contiguousLen, stride, width_, height_, true, nv12.bytes);
        } else if (IsEqualGUID(outputSubtype_, MFVideoFormat_YUY2)) {
          converted = Yuy2StridedToNv12(scanline0, stride, width_, height_, nv12.bytes);
        }
        buffer2d->Unlock2D();
        if (converted && !nv12.bytes.empty()) {
          decoded = DecodedFrame{};
          decoded.gpu = false;
          decoded.nv12 = std::move(nv12);
          if (!reportedCopyMode_) {
            reportedCopyMode_ = true;
            Log(L"decoder output copy mode: IMF2DBuffer subtype=%s stride=%ld",
                VideoSubtypeName(outputSubtype_),
                static_cast<long>(stride));
          }
          return true;
        }
      } else if (!reportedCopyError_) {
        reportedCopyError_ = true;
        wchar_t hrText[96];
        FormatHr(hr, hrText, std::size(hrText));
        Log(L"decoder IMF2DBuffer Lock2D failed: %s", hrText);
      }
    }

    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous))) return false;
    BYTE* src = nullptr; DWORD maxLen = 0, curLen = 0;
    HRESULT hr = contiguous->Lock(&src, &maxLen, &curLen);
    if (FAILED(hr)) {
      if (!reportedCopyError_) {
        reportedCopyError_ = true;
        wchar_t hrText[96];
        FormatHr(hr, hrText, std::size(hrText));
        Log(L"decoder NV12 copy lock failed: %s", hrText);
      }
      return false;
    }
    const DWORD needed = static_cast<DWORD>(width_ * height_ * 3 / 2);
    Nv12Frame nv12;
    bool converted = false;
    if (IsEqualGUID(outputSubtype_, MFVideoFormat_NV12) && curLen >= needed) {
      nv12.width = width_;
      nv12.height = height_;
      nv12.frameId = meta.frameId;
      nv12.recvQpc = meta.recvQpc;
      nv12.bytes.assign(src, src + needed);
      converted = true;
    } else if ((IsEqualGUID(outputSubtype_, MFVideoFormat_IYUV) ||
                IsEqualGUID(outputSubtype_, MFVideoFormat_I420) ||
                IsEqualGUID(outputSubtype_, MFVideoFormat_YV12))) {
      nv12.width = width_;
      nv12.height = height_;
      nv12.frameId = meta.frameId;
      nv12.recvQpc = meta.recvQpc;
      converted = Planar420ToNv12(src,
                                  curLen,
                                  width_,
                                  height_,
                                  IsEqualGUID(outputSubtype_, MFVideoFormat_YV12),
                                  nv12.bytes);
    } else if (IsEqualGUID(outputSubtype_, MFVideoFormat_YUY2)) {
      nv12.width = width_;
      nv12.height = height_;
      nv12.frameId = meta.frameId;
      nv12.recvQpc = meta.recvQpc;
      converted = Yuy2ToNv12(src, curLen, width_, height_, nv12.bytes);
    }
    contiguous->Unlock();
    if (!converted || nv12.bytes.empty()) {
      if (!reportedCopyError_) {
        reportedCopyError_ = true;
        Log(L"decoder output copy/convert failed: subtype=%s curLen=%u neededNv12=%u",
            VideoSubtypeName(outputSubtype_),
            curLen,
            needed);
      }
      return false;
    }

    decoded = DecodedFrame{};
    decoded.gpu = false;
    decoded.nv12 = std::move(nv12);
    return true;
  }

  ComPtr<IMFTransform> mft_;
  ComPtr<ID3D11Device> dxDevice_;
  ComPtr<ID3D11DeviceContext> dxCtx_;
  ComPtr<IMFDXGIDeviceManager> dxgiManager_;
  struct DxgiOutputPoolEntry {
    ComPtr<IMFSample> sample;
  };
  std::vector<DxgiOutputPoolEntry> dxgiOutputPool_;
  size_t dxgiOutputPoolIndex_ = 0;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 60;
  bool sharedDxDevice_ = false;
  bool forceCpuOutput_ = false;
  bool dxvaEnabled_ = false;
  bool inputTypeSet_ = false;
  bool outputTypeSet_ = false;
  bool streamingStarted_ = false;
  const wchar_t* inputSubtypeName_ = L"-";
  GUID outputSubtype_ = MFVideoFormat_NV12;
  bool reportedProcessInputError_ = false;
  bool reportedProcessOutputError_ = false;
  bool reportedCopyError_ = false;
  bool reportedCopyMode_ = false;
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
  bool forceCpuAfterDxvaFailure = false;
  auto createDecoder = [&](bool preferSharedDevice, bool forceCpu) -> std::unique_ptr<MfDecoder> {
    auto decoder = std::make_unique<MfDecoder>();
    ID3D11Device* renderDevice = (preferSharedDevice && g_renderer) ? g_renderer->Device() : nullptr;
    const VideoProfile activeProfile = ActiveVideoProfile();
    if (!decoder->Init(activeProfile.width, activeProfile.height, activeProfile.fps, renderDevice, forceCpu)) return nullptr;
    return decoder;
  };

  auto decoder = createDecoder(useSharedDevice, false);
  if (decoder && !decoder->UsingDxva() && !EnvFlagEnabled(L"P2P_NATIVE_V2_FORCE_CPU_DECODE") &&
      !EnvFlagEnabled(L"P2P_NATIVE_V2_DISABLE_DXVA")) {
    forceCpuAfterDxvaFailure = true;
  }
  if (!decoder) {
    decoder = createDecoder(false, true);
    forceCpuAfterDxvaFailure = true;
    if (!decoder) {
      MessageBoxW(nullptr, L"Failed to initialize Media Foundation H.264 decoder", L"P2P Native", MB_ICONERROR);
      MFShutdown();
      CoUninitialize();
      return;
    }
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
        if (auto rebuilt = createDecoder(useSharedDevice, forceCpuAfterDxvaFailure)) {
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
      encoded = std::move(g_encodedQueue.back());
      if (g_encodedQueue.size() > 1) {
        RecordClientFrameDrop(static_cast<uint64_t>(g_encodedQueue.size() - 1));
      }
      g_encodedQueue.clear();
      g_encodedQueueDepthNow.store(0, std::memory_order_relaxed);
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
      if (decoder && decoder->UsingDxva()) {
        Log(L"DXVA decode failed; falling back to CPU decoder");
        forceCpuAfterDxvaFailure = true;
        ClearPendingVideoQueues();
        if (auto rebuilt = createDecoder(false, true)) {
          decoder = std::move(rebuilt);
          g_decoderPrimed.store(false, std::memory_order_relaxed);
          g_decoderHasKeyframe.store(false, std::memory_order_relaxed);
        }
      }
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

      // Present can be throttled by DWM / swap-chain frame latency.  Do the
      // wait before selecting a frame; otherwise we pop a decoded frame, block
      // inside Present(), and show an already-stale frame while newer decoded
      // frames pile up behind us.  Waiting first lets us always render the
      // newest decoded frame available at the moment the swap chain can accept
      // it, which is the low-latency "latest frame wins" model.
      lk.unlock();
      if (g_renderer) g_renderer->WaitForPresentReady();
      lk.lock();
      if (!g_running.load()) break;
      if (g_decodedQueue.empty()) continue;

      frame = std::move(g_decodedQueue.back());
      if (g_decodedQueue.size() > 1) {
        RecordDecodedFrameDrop(static_cast<uint64_t>(g_decodedQueue.size() - 1));
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
      RecordDecodedFrameDrop(static_cast<uint64_t>(g_decodedQueue.size()));
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
  bool sent = false;
  for (int i = 0; i < 3; ++i) {
    sent = SendInputPacket(P2_INPUT_REQUEST_KEYFRAME, 0, 0, 0, 0, 0, 0) || sent;
    if (i < 2) Sleep(5);
  }
  Log(L"requested keyframe recovery: %s", reason ? reason : L"unknown");
  if (!sent) Log(L"keyframe request send failed: %s", reason ? reason : L"unknown");
}
