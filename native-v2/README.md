# P2P Remote LAN native v2

这是绕开 Electron/WebRTC 的第二版低延迟链路，目标是 LAN 下比 WebRTC MVP 更低、更稳定：

```text
macOS Host:    ScreenCaptureKit -> VideoToolbox H.264 hardware realtime -> UDP frame fragments
Windows Client: UDP reassembly -> Media Foundation low-latency H.264/DXVA -> NV12 GPU shader -> D3D11 flip-model present
Input:         Windows UDP binary input packets -> macOS CGEvent injection
```

## 当前状态

已完成源码版 native 链路：

- `mac-host`：Swift Package，可在 macOS 13+ 编译。
- `win-client`：CMake + Win32/Media Foundation + D3D11 客户端源码。
- `shared`：二进制协议头。

当前 Windows 容器没有 MSVC/CMake，也没有 macOS Swift/ScreenCaptureKit 运行环境，所以这里无法本机编译验证两个 native 端；需要分别在 Mac 和装有 Visual Studio Build Tools 的 Windows 上构建。

## 1) 构建 macOS Host

要求：macOS 13+，Xcode Command Line Tools。

```bash
cd native-v2/mac-host
swift build -c release
```

运行：

```bash
.build/release/p2p-native-mac-host \
  --client-ip 192.168.1.50 \
  --video-port 45000 \
  --input-port 45001 \
  --width 1920 \
  --height 1080 \
  --fps 120 \
  --bitrate 45000000
```

macOS 权限：

- Privacy & Security -> Screen Recording：允许终端或打包后的 Host App。
- Privacy & Security -> Accessibility：允许终端或打包后的 Host App。

## 2) 构建 Windows Client

要求：Windows 10/11，Visual Studio 2022 Build Tools，CMake。

```powershell
cd native-v2\win-client
.\build.ps1
```

运行：

```powershell
.\build\Release\p2p-native-win-client.exe `
  --host-ip 192.168.1.20 `
  --video-port 45000 `
  --input-port 45001 `
  --width 1920 `
  --height 1080 `
  --fps 120 `
  --fullscreen
```

或直接用 ultra 脚本：

```powershell
.\run-ultra.ps1 -HostIp 192.168.1.20 -Width 1920 -Height 1080 -Fps 120
```

Windows 防火墙需要允许 UDP 45000 入站。macOS 需要允许 UDP 45001 入站。

## 延迟目标和调参

推荐起步：

- 1080p60：`--bitrate 18000000` 到 `30000000`
- 1080p120：`--bitrate 35000000` 到 `55000000`
- 1440p60：`--bitrate 35000000` 到 `55000000`
- 4K60：`--bitrate 65000000` 起，依赖 Mac 编码器和 Windows 解码器

低延迟策略：

- 不做 TCP，不做重传；视频 UDP 丢包直接丢帧。
- receiver 队列只保留最新完整帧。
- VideoToolbox：Realtime、禁用 B 帧/重排序、1 秒关键帧。
- Media Foundation：启用 decoder low-latency mode，并给 MFT 传和 renderer 相同的 D3D11 device manager 以争取 DXVA。
- Windows present：优先从 decoder 的 `IMFDXGIBuffer` 直接拿 D3D11 texture 渲染；如果 decoder surface 不能直接绑定 SRV，则 GPU copy 到 NV12 shader texture；最后才退回 CPU NV12 upload。flip-discard swap chain，支持 tearing 时用 `DXGI_PRESENT_ALLOW_TEARING`，`Present(0, ...)` 不等 vsync。
- HUD：窗口标题实时显示 present fps、接收完成到 present 的本机耗时、GPU/CPU path 帧数。
- 输入包固定 32 字节，走独立 UDP。

## 后续要继续压延迟的点

1. 在真实机器上确认 H.264 decoder 是否稳定提供可直接 SRV 绑定的 DXGI surface；若只能 copy，则继续做 decoder output allocator / texture pool。
2. Video packet 加 pacing 和 sequence loss stats，用 ETW / signpost 记录端到端时间。
3. macOS host 改成 `.app`，加菜单栏 UI、自动发现客户端、PIN 配对。
4. H.265/HEVC 路径：Apple Silicon + 现代 Windows GPU 通常能进一步压码率，但兼容性要单独做。
5. 自适应码率：以解码队列深度和丢包率控制 bitrate/fps。
