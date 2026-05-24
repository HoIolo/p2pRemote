#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct P2VideoHeader {
  char magic[4];
  uint8_t version;
  uint8_t type;
  uint16_t headerBytes;
  uint64_t frameId;
  uint64_t ptsUs;
  uint32_t frameBytes;
  uint16_t fragIndex;
  uint16_t fragCount;
  uint16_t payloadBytes;
  uint16_t flags;
};

struct P2TcpVideoHeader {
  char magic[4];
  uint8_t version;
  uint8_t type;
  uint16_t headerBytes;
  uint64_t frameId;
  uint64_t ptsUs;
  uint32_t frameBytes;
  uint16_t flags;
  uint16_t reserved;
};

struct P2InputPacket {
  char magic[4];
  uint8_t version;
  uint8_t kind;
  uint16_t bytes;
  uint32_t seq;
  float x;
  float y;
  int32_t dx;
  int32_t dy;
  uint16_t button;
  uint16_t keyCode;
};
#pragma pack(pop)

static constexpr uint8_t P2_VERSION = 1;
static constexpr uint8_t P2_VIDEO_H264_ANNEXB = 1;
static constexpr uint16_t P2_FLAG_KEYFRAME = 1u << 0;
static constexpr uint16_t P2_FLAG_CONFIG = 1u << 1;
static constexpr uint16_t P2_FLAG_FEC = 1u << 2;
static constexpr uint16_t P2_MOD_SHIFT = 1u << 0;
static constexpr uint16_t P2_MOD_CONTROL = 1u << 1;
static constexpr uint16_t P2_MOD_OPTION = 1u << 2;
static constexpr uint16_t P2_MOD_COMMAND = 1u << 3;

enum P2InputKind : uint8_t {
  P2_INPUT_MOVE = 1,
  P2_INPUT_DOWN = 2,
  P2_INPUT_UP = 3,
  P2_INPUT_WHEEL = 4,
  P2_INPUT_KEY_DOWN = 5,
  P2_INPUT_KEY_UP = 6,
  P2_INPUT_REQUEST_KEYFRAME = 7,
  P2_INPUT_SET_VIDEO_PROFILE = 8,
  P2_INPUT_SET_VIDEO_BITRATE = 9,
  P2_INPUT_TEXT = 10,
  P2_INPUT_HEARTBEAT = 11,
  P2_INPUT_CLIENT_STATS = 12,
};
