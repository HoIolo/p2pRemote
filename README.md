# P2P Remote LAN

一个局域网内的低延迟远程控制 MVP：**Windows 控制端 -> macOS 主机端**。

- 屏幕：macOS host 使用 Electron/Chromium `getDisplayMedia()` 捕获屏幕，通过 WebRTC 直连发送给 Windows client。
- 输入：Windows client 捕获视频窗口上的鼠标/键盘事件，通过 WebRTC DataChannel 发送；macOS host 用 Swift/CoreGraphics helper 注入本机输入。
- 信令：macOS host 内置 WebSocket，只用于 PIN 配对与 WebRTC offer/answer/ICE；媒体和输入不经过云端。
- 安全边界：只做显式授权、前台可见、局域网配对；不包含隐藏运行、绕过权限、持久化或未授权访问能力。

## 快速运行

### 1) macOS 主机

```bash
cd p2p-remote-lan
npm install
npm run build:mac-helper
npm run host
```

首次运行需要在 macOS 授权：

1. System Settings -> Privacy & Security -> Screen Recording：允许 Terminal/Electron/本应用。
2. System Settings -> Privacy & Security -> Accessibility：允许 Terminal/Electron/本应用和 `macos-input-helper`。
3. App 打开后点击 **开始共享屏幕**，屏幕选择器里请选择主屏幕。
4. 记下界面显示的 LAN IP、端口和 PIN。

### 2) Windows 控制端

```powershell
cd C:\path\to\p2p-remote-lan
npm install
npm run client
```

输入 Mac IP、端口 `7777`、PIN，点击连接。连接成功后点击视频画面，使键盘焦点进入远程画面。


## 封装成软件

### Windows 控制端安装包

在 Windows 上执行：

```powershell
cd C:\Users\admin\Desktop\learn\p2p-remote-lan
npm install
npm run dist:win
```

已生成的产物：

```text
C:\Users\admin\Desktop\learn\p2p-remote-lan\dist\P2P Remote LAN-0.1.0-Setup-x64.exe
C:\Users\admin\Desktop\learn\p2p-remote-lan\dist\P2P Remote LAN-0.1.0-Portable-x64.exe
```

- `Setup-x64.exe`：安装版，会创建桌面和开始菜单快捷方式。
- `Portable-x64.exe`：免安装版，拷贝到 Windows 机器直接运行。
- Windows 双击默认进入 **控制端 Client**。

### macOS 主机端 App / DMG

macOS 包必须在 Mac 上构建，因为 Swift helper、Screen Recording/Accessibility 权限和 DMG 生成都依赖 macOS：

```bash
cd p2p-remote-lan
npm install
npm run build:mac-helper
npm run dist:mac
```

生成位置：

```text
p2p-remote-lan/dist/*.dmg
p2p-remote-lan/dist/*.zip
```

- macOS 双击默认进入 **主机端 Host**。
- `native/macos-input-helper/.build/release/macos-input-helper` 会被打进 app 的 Resources，用于低延迟输入注入。

## 延迟优化开关

默认策略偏向低延迟：

- WebRTC `iceServers: []`：只走局域网候选，不走 STUN/TURN。
- DataChannel：`ordered: false, maxRetransmits: 0`，输入包宁愿丢弃也不排队。
- 编码参数：目标 60fps、35Mbps、`degradationPreference=maintain-framerate`。
- 鼠标移动：客户端按 `requestAnimationFrame` 合并，只保留最新坐标。
- 输入注入：Mac 端常驻 Swift helper，通过 stdin JSON lines 收包，避免 Node 原生插件 ABI 问题。

可在 host DevTools 的 LocalStorage 改：

```js
localStorage.setItem('maxBitrate', '50000000') // 50 Mbps
localStorage.setItem('maxFps', '120')          // 120 fps，需要显示器/采集/编码都支持
```


## Native v2 超低延迟版本

我已新增 native v2 源码，位置：

```text
native-v2/
```

核心链路：

```text
macOS ScreenCaptureKit -> VideoToolbox H.264 realtime -> UDP
Windows UDP -> Media Foundation H.264 decoder/DXVA -> D3D11 NV12 shader -> flip-model present
Windows 输入 -> UDP binary packet -> macOS CGEvent
```

详细构建和运行见：`native-v2/README.md`。

## 现有限制

Electron/WebRTC 版适合快速可用；`native-v2` 已切到原生低延迟链路，但还处于源码版阶段：

- macOS Host 需要在真实 Mac 上编译和授权 Screen Recording / Accessibility。
- Windows Client 需要 Visual Studio Build Tools + CMake 编译。
- v2 当前 Windows 渲染已升级为 D3D11 flip-model present + NV12 pixel shader；并加入 `IMFDXGIBuffer`/D3D11 texture 优先直渲染路径，拿不到可绑定 surface 时自动退回 GPU copy 或 CPU NV12 upload。
- 目标：LAN 下 1080p60 端到端 8-20ms；4K60 取决于编码器、解码器和显示器刷新。

## 目录

```text
src/main.js                         Electron 主进程，窗口、信令、IPC
src/preload.js                      renderer/main 安全桥
src/host.js                         macOS host：屏幕捕获、WebRTC answer、接收输入
src/client.js                       Windows client：WebRTC offer、渲染视频、发送输入
src/injector.js                     启动并喂给 macOS 输入 helper
native/macos-input-helper/...       Swift/CoreGraphics 输入注入 helper
```



