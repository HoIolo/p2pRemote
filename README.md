# P2P Remote LAN

局域网内低延迟远程控制：**Windows 控制端 -> macOS 主机端**。

当前只保留 **Native v2 极限性能链路**：

```text
macOS ScreenCaptureKit -> VideoToolbox H.264 realtime -> UDP video
Windows UDP/TCP receive -> Media Foundation H.264/DXVA -> D3D11 present
Windows input -> UDP binary packet -> macOS CGEvent
```

旧的浏览器媒体远程桌面模式已从入口和 IPC 中移除；Electron 现在只负责发现设备、配对请求、启动 native 进程和展示状态。

## 快速运行

### 1) macOS 主机

```bash
cd p2p-remote-lan
npm install
npm run v2:mac:build
npm run host
```

首次运行需要在 macOS 授权：

1. System Settings -> Privacy & Security -> Screen Recording：允许 Terminal/Electron/本应用或 native host。
2. System Settings -> Privacy & Security -> Accessibility：允许 Terminal/Electron/本应用或 native host。
3. 保持 App 打开，等待 Windows 端通过 PIN 请求启动 Native v2 Host；也可以在 Mac 端手动输入 Windows IP 启动。

### 2) Windows 控制端

```powershell
cd C:\path\to\p2p-remote-lan
npm install
npm run v2:win:build
npm run client
```

在设备列表里选择 Mac，点击 **Native v2 极速**。默认走 UDP：视频 `45000`，输入 `45001`。

## Native v2 调参

推荐起步：

- 1080p60：18-30 Mbps
- 1080p120：35-55 Mbps
- 1440p60：35-55 Mbps
- 4K60：65 Mbps 起，取决于 Mac 编码器、Windows 解码器和网络

默认策略：

- 视频优先 UDP，低延迟和丢旧帧优先；如果 Windows 防火墙拦截 UDP 入站导致黑屏，再手动切 TCP。
- macOS 捕获队列深度 `1`。
- VideoToolbox：Realtime、禁用 B 帧/重排序、1 秒关键帧。
- Windows receiver / decoder / renderer 队列只保留极少帧，渲染端始终取最新帧。
- Windows present：D3D11 flip-model，`Present(0, ...)`，支持 tearing 时允许 tearing。

更详细构建和运行参数见：`native-v2/README.md`。

## 封装成软件

Windows：

```powershell
npm run dist:win
```

macOS：

```bash
npm run v2:mac:build
npm run dist:mac
```

打包前会执行 `npm run stage:native-v2`，把已构建的 native-v2 产物放入 app resources。

## 现有限制

- macOS Host 需要真实 Mac、macOS 13+、Xcode Command Line Tools，并授权 Screen Recording / Accessibility。
- Windows Client 需要 Windows 10/11、Visual Studio 2022 Build Tools、CMake。
- 目标：LAN 下 1080p60 端到端 8-20ms；4K60 取决于硬件编码/解码能力和显示刷新。

## 目录

```text
src/main.js                         Electron 主进程：窗口、发现、PIN 配对、native-v2 进程管理
src/preload.js                      renderer/main 安全桥
src/dashboard.html                  Native v2 控制台
src/dashboard.js                    设备列表、手动连接、Native v2 启停
native-v2/mac-host/...              macOS ScreenCaptureKit + VideoToolbox + CGEvent Host
native-v2/win-client/...            Windows Media Foundation + D3D11 Client
scripts/stage-native-v2.js          打包前 staging native-v2 产物
```
