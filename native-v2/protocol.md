# Native v2 protocol

All integer fields are little-endian. Pack structs with 1-byte alignment.

## Video packet: host -> client UDP

Magic: `P2V2`

```c
struct P2VideoHeader {
  char     magic[4];       // P2V2
  uint8_t  version;        // 1
  uint8_t  type;           // 1 = h264 annex-b frame fragment
  uint16_t headerBytes;    // sizeof(P2VideoHeader)
  uint64_t frameId;        // monotonically increasing
  uint64_t ptsUs;          // host monotonic presentation timestamp, us
  uint32_t frameBytes;     // whole encoded frame bytes
  uint16_t fragIndex;      // 0-based
  uint16_t fragCount;
  uint16_t payloadBytes;
  uint16_t flags;          // bit0 keyframe, bit1 config included
};
```

Payload is an H.264 Annex-B bytestream fragment. Keyframes include SPS/PPS before IDR NALs.

## Input packet: client -> host UDP

Magic: `P2I2`

```c
struct P2InputPacket {
  char     magic[4];       // P2I2
  uint8_t  version;        // 1
  uint8_t  kind;           // 1 move, 2 down, 3 up, 4 wheel, 5 keyDown, 6 keyUp
  uint16_t bytes;          // sizeof(P2InputPacket)
  uint32_t seq;
  float    x;              // normalized 0..1, video/display area
  float    y;              // normalized 0..1
  int32_t  dx;             // wheel pixels
  int32_t  dy;
  uint16_t button;         // 0 left, 1 middle, 2 right
  uint16_t keyCode;        // macOS CGKeyCode, already mapped by client
};
```
