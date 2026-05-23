const $ = (id) => document.getElementById(id);

const deviceListEl = $('deviceList');
const localPinEl = $('localPin');
const localAddrEl = $('localAddr');
const selectedNameEl = $('selectedName');
const selectedStatusEl = $('selectedStatus');
const enterDesktopBtn = $('enterDesktop');
const connectSelectedBtn = $('connectSelected');
const previewImageEl = $('previewImage');
const refreshDevicesBtn = $('refreshDevices');
const refreshTopBtn = $('refreshTop');
const macPermissionCardEl = $('macPermissionCard');
const permissionCardSummaryEl = $('permissionCardSummary');
const permissionModalEl = $('permissionModal');
const closePermissionModalBtn = $('closePermissionModal');
const permissionModalHintEl = $('permissionModalHint');
const screenPermissionToggleBtn = $('screenPermissionToggle');
const accessibilityPermissionToggleBtn = $('accessibilityPermissionToggle');
const screenPermissionDetailEl = $('screenPermissionDetail');
const accessibilityPermissionDetailEl = $('accessibilityPermissionDetail');
const refreshPermissionStatusBtn = $('refreshPermissionStatus');
const openSystemPrivacySettingsBtn = $('openSystemPrivacySettings');
const manualAddBtn = $('manualAdd');
const nativeV2StatusTextEl = $('nativeV2StatusText');
const nativeV2ConnectBtn = $('nativeV2Connect');
const gameStreamStatusTextEl = $('gameStreamStatusText');
const gameStreamConnectBtn = $('gameStreamConnect');
const gameStreamPairBtn = $('gameStreamPair');
const gameStreamHostBtn = $('gameStreamHost');
const gameStreamOpenSunshineBtn = $('gameStreamOpenSunshine');
const gameStreamDownloadBtn = $('gameStreamDownload');
const gameStreamInstallProgressEl = $('gameStreamInstallProgress');
const logEl = $('log');


let devices = [];
let selectedId = null;
let nativeV2Status = null;
let appInfo = null;
let nativeV2Connecting = false;
let gameStreamStatus = null;
let gameStreamBusy = false;
let gameStreamInstallBusy = false;
let macPermissionStatus = {
  platform: 'unknown',
  screenCapture: 'unknown',
  accessibility: 'unknown',
};

function iconSvg(name, className = 'icon') {
  return `<svg class="${className}" aria-hidden="true"><use href="assets/icons.svg#${name}"></use></svg>`;
}

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function clearLegacyDashboardControlProfile() {
  localStorage.removeItem('p2p-remote-dashboard-controls-v1');
  localStorage.removeItem('captureWidth');
  localStorage.removeItem('captureHeight');
  localStorage.removeItem('maxFps');
  localStorage.removeItem('maxBitrate');
  localStorage.removeItem('captureHint');
}

function setNativeV2StatusText(message) {
  if (nativeV2StatusTextEl) nativeV2StatusTextEl.textContent = message;
}

function setGameStreamStatusText(message) {
  if (gameStreamStatusTextEl) gameStreamStatusTextEl.textContent = message;
}

function permissionGranted(status) {
  return status === 'granted';
}

function permissionLabel(status) {
  if (status === 'granted') return '已开启';
  if (status === 'denied') return '未开启';
  if (status === 'not-determined') return '未请求';
  if (status === 'restricted') return '受限制';
  return '未知';
}

function renderPermissionStatus() {
  const screenGranted = permissionGranted(macPermissionStatus.screenCapture);
  const accessibilityGranted = permissionGranted(macPermissionStatus.accessibility);
  const allGranted = screenGranted && accessibilityGranted;

  if (permissionCardSummaryEl) {
    permissionCardSummaryEl.textContent = allGranted
      ? '屏幕录制和辅助功能已开启'
      : '需要开启屏幕录制和辅助功能';
  }
  if (permissionModalHintEl) {
    permissionModalHintEl.textContent = macPermissionStatus.nativeHostAppPath
      ? '开发模式会使用 P2P Native Mac Host.app 申请权限；授权后完全退出并重新运行 npm run host。'
      : '请先执行 npm run v2:mac:build 生成 P2P Native Mac Host.app，再打开系统设置授权。';
  }

  if (screenPermissionToggleBtn) {
    screenPermissionToggleBtn.classList.toggle('on', screenGranted);
    screenPermissionToggleBtn.setAttribute('aria-checked', String(screenGranted));
    screenPermissionToggleBtn.title = screenGranted ? '重置屏幕录制权限' : '打开屏幕录制设置';
  }
  if (accessibilityPermissionToggleBtn) {
    accessibilityPermissionToggleBtn.classList.toggle('on', accessibilityGranted);
    accessibilityPermissionToggleBtn.setAttribute('aria-checked', String(accessibilityGranted));
    accessibilityPermissionToggleBtn.title = accessibilityGranted ? '重置辅助功能权限' : '请求辅助功能权限';
  }
  if (screenPermissionDetailEl) {
    screenPermissionDetailEl.textContent = `状态：${permissionLabel(macPermissionStatus.screenCapture)}。用于采集 Mac 画面。`;
  }
  if (accessibilityPermissionDetailEl) {
    accessibilityPermissionDetailEl.textContent = `状态：${permissionLabel(macPermissionStatus.accessibility)}。用于接收 Windows 端鼠标和键盘控制。`;
  }
}

async function refreshMacPermissionStatus() {
  try {
    macPermissionStatus = await window.lanRemote.getMacPermissionStatus();
  } catch (err) {
    log(`permission status failed: ${err.message || String(err)}`);
  }
  renderPermissionStatus();
}

async function openPermissionModal() {
  if (permissionModalEl) permissionModalEl.hidden = false;
  await refreshMacPermissionStatus();
}

function closePermissionModal() {
  if (permissionModalEl) permissionModalEl.hidden = true;
}

function friendlyNativeV2Error(err, device) {
  const raw = err?.message || String(err || 'Unknown error');
  if (raw.includes('屏幕录制权限') || raw.includes('Screen Recording permission denied')) {
    return 'Mac 端屏幕录制权限未授权。请在 Mac 的系统设置 > 隐私与安全性 > 屏幕录制 中允许当前运行的 App/Terminal，然后完全退出并重新打开 Mac 端。';
  }
  if (raw.includes('timed out') || raw.includes('closed before response')) {
    return [
      `Mac 端没有响应 Native v2 启动请求（${device?.address || 'unknown'}:7777）。`,
      '请确认 Mac 上运行的是最新版本 App，并且已执行 npm run v2:mac:build；如果刚授权了屏幕录制，请完全退出并重新打开 Mac 端。',
    ].join(' ');
  }
  if (raw.includes('还没有构建') || raw.includes('not built')) return raw;
  if (raw.includes('bad pin')) return 'PIN 校验失败，请刷新设备后重试。';
  return raw.replace(/^Error invoking remote method '[^']+': Error:\s*/i, '');
}

function friendlyGameStreamError(err) {
  const raw = err?.message || String(err || 'Unknown error');
  if (raw.includes('未找到 Sunshine')) return raw;
  if (raw.includes('未找到 Moonlight')) return raw;
  if (raw.includes('has not been paired') || raw.includes('not been paired')) {
    return 'Moonlight 尚未与 Sunshine 配对。点击“配对”，系统会自动生成 PIN 并提交到 Mac Sunshine。';
  }
  if (raw.includes('Failed to find application') || raw.includes('Desktop app was not ready') || raw.includes('default Desktop application')) {
    return 'Sunshine 的应用列表还没准备好 Desktop。请先重新启动 Mac 端 Sunshine，再点一次“游戏模式”或“配对”。';
  }
  if (raw.includes('certificate required') || raw.includes('TLSV1_ALERT_CERTIFICATE_REQUIRED') || raw.includes('alert number 116')) {
    return 'Sunshine 的 GameStream 端口要求 Moonlight 客户端证书。本程序已不再直接访问该端口；请确认 Mac 端也已重启到最新版本后重试。';
  }
  if (raw.includes('Moonlight 的配对请求') || raw.includes('pairing PIN')) {
    return 'Sunshine 还没收到 Moonlight 的配对请求。请确认 Mac 端 Sunshine 已启动后再点一次“配对”。';
  }
  if (raw.includes('timed out') || raw.includes('closed before response')) {
    return '远端没有响应游戏串流启动请求。请确认对端 App 在线、PIN 正确、防火墙允许本程序通信。';
  }
  if (raw.includes('ECONNREFUSED') || raw.includes('connection failed')) {
    return '无法连接远端控制通道。请刷新设备列表并确认对端 App 正在运行。';
  }
  if (raw.includes('bad pin')) return 'PIN 校验失败，请刷新设备后重试。';
  return raw.replace(/^Error invoking remote method '[^']+': Error:\s*/i, '');
}

function setGameStreamBusy(busy, message) {
  gameStreamBusy = busy;
  if (message) setGameStreamStatusText(message);
  document.body.classList.toggle('gameStreamConnecting', busy);
  renderDevices();
}

function formatBytes(value) {
  const bytes = Number(value || 0);
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024 / 1024).toFixed(1)} GB`;
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

function renderGameStreamInstallProgress(progress) {
  if (!gameStreamInstallProgressEl || !progress) return;
  const tool = progress.tool === 'sunshine' ? 'Sunshine' : 'Moonlight';
  if (progress.phase === 'download') {
    const total = Number(progress.total || 0);
    gameStreamInstallProgressEl.textContent = total > 0
      ? `${tool} 下载中：${formatBytes(progress.received)} / ${formatBytes(total)}`
      : `${tool} 下载中：${formatBytes(progress.received)}`;
    return;
  }
  const labels = {
    start: `${tool} 准备下载...`,
    extract: `${tool} 正在解压...`,
    installer: `${tool} 正在运行安装器...`,
    mount: `${tool} 正在挂载 DMG...`,
    copy: `${tool} 正在复制到 Applications...`,
    done: `${tool} 安装完成`,
  };
  gameStreamInstallProgressEl.textContent = labels[progress.phase] || `${tool} ${progress.phase || '处理中'}...`;
}

function requiredGameStreamToolStatus() {
  if (gameStreamStatus?.platform === 'darwin') return gameStreamStatus.sunshine || {};
  if (gameStreamStatus?.platform === 'win32') return gameStreamStatus.moonlight || {};
  return { available: true };
}

function requiredGameStreamToolLabel() {
  return gameStreamStatus?.platform === 'darwin' ? 'Sunshine' : 'Moonlight';
}

function isRequiredGameStreamToolInstalled() {
  return Boolean(requiredGameStreamToolStatus().available);
}


async function installGameStreamComponent() {
  if (gameStreamInstallBusy) return;
  await refreshGameStreamStatus();
  if (isRequiredGameStreamToolInstalled()) {
    const label = requiredGameStreamToolLabel();
    if (gameStreamInstallProgressEl) gameStreamInstallProgressEl.textContent = `${label} 已安装，无需重复安装。`;
    setGameStreamStatusText(`${label} 已就绪，无需重复安装。`);
    renderDevices();
    return;
  }
  const isMac = gameStreamStatus?.platform === 'darwin';
  const tool = isMac ? 'sunshine' : 'moonlight';
  const mode = gameStreamStatus?.platform === 'win32' ? 'installer' : 'dmg';
  gameStreamInstallBusy = true;
  setGameStreamBusy(true, `正在安装 ${tool === 'sunshine' ? 'Sunshine' : 'Moonlight'}...`);
  renderGameStreamInstallProgress({ phase: 'start', tool, mode });
  try {
    const result = await window.lanRemote.installGameStreamTool({ tool, mode });
    updateGameStreamStatus(result.status);
    renderGameStreamInstallProgress({ phase: 'done', tool, mode });
    setGameStreamStatusText(`${tool === 'sunshine' ? 'Sunshine' : 'Moonlight'} 已安装；可以继续启动游戏模式。`);
    log(`game-stream install completed: ${tool} ${mode}`);
  } catch (err) {
    const message = friendlyGameStreamError(err);
    setGameStreamStatusText(`安装失败：${message}`);
    if (gameStreamInstallProgressEl) gameStreamInstallProgressEl.textContent = `安装失败：${message}`;
    log(`game-stream install failed: ${message}`);
  } finally {
    gameStreamInstallBusy = false;
    setGameStreamBusy(false);
  }
}

function updateGameStreamStatus(status) {
  gameStreamStatus = status || gameStreamStatus;
  renderDevices();
  if (gameStreamBusy) return;
  if (!gameStreamStatusTextEl || !gameStreamStatus) return;

  const moonlight = gameStreamStatus.moonlight || {};
  const sunshine = gameStreamStatus.sunshine || {};
  const isWindows = gameStreamStatus.platform === 'win32';
  const isMac = gameStreamStatus.platform === 'darwin';

  if (isWindows) {
    if (!moonlight.available) {
      gameStreamStatusTextEl.textContent = '未找到 Moonlight：请安装 Moonlight Qt，或把 Moonlight.exe 加入 PATH。';
    } else if (moonlight.running) {
      gameStreamStatusTextEl.textContent = `Moonlight 游戏串流运行中，pid=${moonlight.pid}。`;
    } else if (moonlight.pairing) {
      gameStreamStatusTextEl.textContent = 'Moonlight 自动配对流程运行中；无需手动输入 PIN。';
    } else {
      gameStreamStatusTextEl.textContent = 'Moonlight 已就绪：优先使用 Sunshine/Moonlight 游戏模式，Native v2 作为 fallback。';
    }
    return;
  }

  if (isMac) {
    if (!sunshine.available) {
      gameStreamStatusTextEl.textContent = '未找到 Sunshine：请安装 Sunshine，或把 sunshine 加入 PATH。';
    } else if (sunshine.running) {
      gameStreamStatusTextEl.textContent = `Sunshine Host 运行中，pid=${sunshine.pid}；首次连接需在 Sunshine Web UI 配对 Moonlight。`;
    } else {
      gameStreamStatusTextEl.textContent = 'Sunshine 已就绪：点击启动 Host 后，用 Windows Moonlight 连接。';
    }
    return;
  }

  gameStreamStatusTextEl.textContent = '游戏串流模式需要 Sunshine Host 和 Moonlight 客户端。';
}

async function refreshGameStreamStatus() {
  if (!window.lanRemote.getGameStreamStatus) return;
  try {
    updateGameStreamStatus(await window.lanRemote.getGameStreamStatus());
  } catch (err) {
    setGameStreamStatusText(`游戏串流状态检测失败：${err.message}`);
  }
}

function setNativeV2Busy(busy, message) {
  nativeV2Connecting = busy;
  if (message) setNativeV2StatusText(message);
  document.body.classList.toggle('nativeV2Connecting', busy);
  renderDevices();
}

function platformIcon(platform) {
  if (platform === 'darwin') return iconSvg('platform-mac', 'devicePlatformIcon');
  if (platform === 'win32') return iconSvg('platform-win', 'devicePlatformIcon');
  return iconSvg('platform-device', 'devicePlatformIcon');
}

function platformName(platform) {
  if (platform === 'darwin') return 'Mac';
  if (platform === 'win32') return 'Windows';
  if (platform === 'linux') return 'Linux';
  return 'Device';
}

function defaultPreview(platform) {
  if (platform === 'darwin') return 'assets/device-mac.png';
  if (platform === 'win32') return 'assets/device-win.png';
  return 'assets/device-win.png';
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    '\'': '&#39;',
  }[char]));
}

function selectedDevice() {
  return devices.find((device) => device.id === selectedId) || devices[0] || null;
}

function selectedDeviceIsMac() {
  return selectedDevice()?.platform === 'darwin';
}

function canControlLocalSunshineHost() {
  return gameStreamStatus?.platform === 'darwin';
}

function canControlSelectedSunshineHost() {
  return canControlLocalSunshineHost() || selectedDeviceIsMac();
}

function selectedSunshineWebHost() {
  return canControlLocalSunshineHost() ? 'localhost' : selectedDevice()?.address;
}



function updateNativeV2Status(status) {
  nativeV2Status = status || nativeV2Status;
  if (macPermissionCardEl) {
    macPermissionCardEl.hidden = nativeV2Status?.platform !== 'darwin';
  }
  if (nativeV2Connecting) return;
  if (!nativeV2StatusTextEl || !nativeV2Status) return;

  const isWindows = nativeV2Status.platform === 'win32';
  const isMac = nativeV2Status.platform === 'darwin';
  const winClient = nativeV2Status.winClient || {};
  const macHost = nativeV2Status.macHost || {};

  if (isWindows) {
    if (winClient.running) {
      nativeV2StatusTextEl.textContent = `Windows Native v2 客户端运行中，pid=${winClient.pid}；连接后可在顶部状态栏切换分辨率、帧率和码率。`;
    } else if (winClient.available) {
      nativeV2StatusTextEl.textContent = 'Windows Native v2 客户端已就绪；连接后可在顶部状态栏切换分辨率、帧率和码率。';
    } else {
      nativeV2StatusTextEl.textContent = 'Windows Native v2 客户端未构建；请先执行 npm run v2:win:build。';
    }
    return;
  }

  if (isMac) {
    if (macHost.running) {
      nativeV2StatusTextEl.textContent = `macOS Native v2 Host 运行中，pid=${macHost.pid}`;
    } else if (macHost.available) {
      nativeV2StatusTextEl.textContent = 'macOS Native v2 Host 已构建；输入 Windows IP 后可启动极速 Host。';
    } else {
      nativeV2StatusTextEl.textContent = 'macOS Native v2 Host 未构建；先执行 npm run v2:mac:build。';
    }
    return;
  }

  nativeV2StatusTextEl.textContent = 'Native v2 当前仅支持 Windows 控制端和 macOS Host。';
}

async function refreshNativeV2Status() {
  if (!window.lanRemote.getNativeV2Status) return;
  try {
    updateNativeV2Status(await window.lanRemote.getNativeV2Status());
  } catch (err) {
    if (nativeV2StatusTextEl) nativeV2StatusTextEl.textContent = `Native v2 状态检测失败：${err.message}`;
  }
}

function clampEven(value, fallback = 2) {
  const number = Math.max(2, Math.round(Number(value) || fallback));
  return number % 2 === 0 ? number : number - 1;
}

function scaleResolution(width, height, maxLongEdge = 1920) {
  const longEdge = Math.max(width, height);
  if (!longEdge || longEdge <= maxLongEdge) {
    return { width: clampEven(width, 1600), height: clampEven(height, 900) };
  }
  const scale = maxLongEdge / longEdge;
  return {
    width: clampEven(width * scale, 1600),
    height: clampEven(height * scale, 900),
  };
}

function autoBitrateForPixels(pixels, fallbackBitrate) {
  let bitrate = fallbackBitrate;
  if (pixels <= 1280 * 720) bitrate = Math.max(bitrate, 6_000_000);
  else if (pixels <= 1600 * 900) bitrate = Math.max(bitrate, 8_000_000);
  else if (pixels <= 1920 * 1080) bitrate = Math.max(bitrate, 12_000_000);
  else if (pixels <= 1920 * 1200) bitrate = Math.max(bitrate, 14_000_000);
  else if (pixels <= 2560 * 1440) bitrate = Math.max(bitrate, 20_000_000);
  else bitrate = Math.max(bitrate, 28_000_000);
  return bitrate;
}

function remoteDisplayPixelSize(device) {
  const display = device?.display;
  if (!display?.width || !display?.height) return null;
  const scaleFactor = Number(display.scaleFactor || device?.scaleFactor || 1);
  const pixelWidth = Number(display.pixelWidth || display.nativeWidth || 0);
  const pixelHeight = Number(display.pixelHeight || display.nativeHeight || 0);
  if (pixelWidth > 0 && pixelHeight > 0) {
    return { width: clampEven(pixelWidth, display.width), height: clampEven(pixelHeight, display.height) };
  }
  return {
    width: clampEven(display.width * scaleFactor, display.width),
    height: clampEven(display.height * scaleFactor, display.height),
  };
}

function nativeV2FpsOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const displayFps = Number(device?.display?.displayFrequency || device?.displayFrequency || 0);
  const fps = displayFps >= 30 ? displayFps : (defaults.fps || 60);
  return Math.max(30, Math.min(60, Math.round(fps)));
}

function nativeV2DisplayOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const displayPixels = remoteDisplayPixelSize(device);
  if (!displayPixels?.width || !displayPixels?.height) {
    const width = clampEven(defaults.width || 1600, 1600);
    const height = clampEven(defaults.height || 900, 900);
    return {
      width,
      height,
      bitrate: autoBitrateForPixels(width * height, defaults.bitrate || 8_000_000),
    };
  }

  const sourceWidth = clampEven(displayPixels.width, defaults.width || 1600);
  const sourceHeight = clampEven(displayPixels.height, defaults.height || 900);
  const scaled = scaleResolution(sourceWidth, sourceHeight);
  const pixels = scaled.width * scaled.height;
  const bitrate = autoBitrateForPixels(pixels, defaults.bitrate || 8_000_000);

  return {
    width: scaled.width,
    height: scaled.height,
    bitrate,
  };
}

function gameStreamFpsOptions(device) {
  const defaults = gameStreamStatus?.defaults || {};
  const displayFps = Number(device?.display?.displayFrequency || device?.displayFrequency || 0);
  const fps = displayFps >= 30 ? displayFps : (defaults.fps || 60);
  return Math.max(30, Math.min(120, Math.round(fps)));
}

function gameStreamClientOptions(device) {
  const defaults = gameStreamStatus?.defaults || {};
  const displayPixels = remoteDisplayPixelSize(device);
  const display = displayPixels?.width && displayPixels?.height
    ? scaleResolution(displayPixels.width, displayPixels.height, 1920)
    : { width: defaults.width || 1920, height: defaults.height || 1080 };
  const pixels = display.width * display.height;
  const bitrateKbps = Math.max(Number(defaults.bitrateKbps || 30000), Math.round(autoBitrateForPixels(pixels, 12_000_000) / 1000));
  return {
    hostIp: device.address,
    appName: defaults.appName || 'Desktop',
    width: display.width,
    height: display.height,
    fps: gameStreamFpsOptions(device),
    bitrateKbps,
    videoCodec: defaults.videoCodec || 'HEVC',
    videoDecoder: defaults.videoDecoder || 'hardware',
    displayMode: defaults.displayMode || 'fullscreen',
    captureSystemKeys: defaults.captureSystemKeys || 'always',
    absoluteMouse: defaults.absoluteMouse !== false,
    framePacing: true,
    gameOptimization: true,
    performanceOverlay: true,
  };
}

function gameStreamRemoteHostOptions(device = selectedDevice()) {
  return { readyTimeoutMs: 12_000, webHost: device?.address || '' };
}

function nativeV2ClientOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const display = nativeV2DisplayOptions(device);
  const transport = defaults.transport || 'udp';
  return {
    hostIp: device.address,
    hostName: device.name,
    hostPlatform: device.platform,
    videoPort: defaults.videoPort || 45000,
    inputPort: defaults.inputPort || 45001,
    width: display.width,
    height: display.height,
    fps: nativeV2FpsOptions(device),
    bitrate: display.bitrate,
    fullscreen: true,
    transport,
  };
}

function nativeV2HostOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const clientIp = appInfo?.device?.addresses?.[0] || '';
  const display = nativeV2DisplayOptions(device);
  const transport = defaults.transport || 'udp';
  return {
    clientIp: nativeV2Status?.platform === 'win32' ? clientIp : device.address,
    videoPort: defaults.videoPort || 45000,
    inputPort: defaults.inputPort || 45001,
    width: display.width,
    height: display.height,
    fps: nativeV2FpsOptions(device),
    bitrate: display.bitrate,
    keyint: defaults.keyint || 1,
    transport,
  };
}

function nativeV2MacHostCommand(device, routeAddress = '') {
  const defaults = nativeV2Status?.defaults || {};
  const clientIp = routeAddress || appInfo?.device?.addresses?.[0] || '<Windows_IP>';
  const display = nativeV2DisplayOptions(device);
  return [
    'cd native-v2/mac-host',
    `CLIENT_IP=${clientIp} VIDEO_PORT=${defaults.videoPort || 45000} INPUT_PORT=${defaults.inputPort || 45001} WIDTH=${display.width} HEIGHT=${display.height} FPS=${nativeV2FpsOptions(device)} BITRATE=${display.bitrate} TRANSPORT=${defaults.transport || 'udp'} ./run-ultra.sh`,
  ].join('\n');
}

async function resolveNativeV2HostOptions(device) {
  const options = nativeV2HostOptions(device);
  if (nativeV2Status?.platform === 'win32' && window.lanRemote.getNativeV2RouteLocalAddress) {
    try {
      const route = await window.lanRemote.getNativeV2RouteLocalAddress(device.address);
      if (route?.address) {
        options.clientIp = route.address;
        log(`native-v2 route selected: Mac ${device.address} -> Windows ${route.address} (${route.name || 'unknown'}, ${route.reason || 'route'})`);
      }
    } catch (err) {
      log(`native-v2 route detection failed: ${err.message}`);
    }
  }
  return options;
}

function renderDevices() {
  if (!selectedId && devices.length) selectedId = devices[0].id;
  const selected = selectedDevice();

  deviceListEl.innerHTML = devices.length
    ? devices.map((device) => {
      const active = selected?.id === device.id ? ' active' : '';
      return `
        <button class="sideItem${active}" data-id="${escapeHtml(device.id)}">
          <span class="onlineDot"></span>
          <span class="deviceIcon">${platformIcon(device.platform)}</span>
          <span class="deviceText">
            <b>${escapeHtml(device.name)}</b>
            <small>${platformName(device.platform)} · ${escapeHtml(device.address)}:${escapeHtml(device.port)}</small>
          </span>
        </button>`;
    }).join('')
    : '<div class="emptyHint">暂无在线设备</div>';

  selectedNameEl.textContent = selected ? selected.name : '暂无在线设备';
  selectedStatusEl.textContent = selected ? '在线' : '等待设备';
  selectedStatusEl.className = selected ? 'pill online' : 'pill muted';
  previewImageEl.src = selected ? (selected.preview || defaultPreview(selected.platform)) : defaultPreview('win32');
  const anyBusy = nativeV2Connecting || gameStreamBusy;
  enterDesktopBtn.disabled = !selected || anyBusy;
  connectSelectedBtn.disabled = !selected || anyBusy;
  if (gameStreamConnectBtn) gameStreamConnectBtn.disabled = !selected || anyBusy;
  if (nativeV2ConnectBtn) nativeV2ConnectBtn.disabled = !selected || anyBusy;
  if (gameStreamPairBtn) gameStreamPairBtn.disabled = !selected || anyBusy || gameStreamStatus?.platform !== 'win32';
  if (gameStreamHostBtn) gameStreamHostBtn.disabled = anyBusy || !canControlSelectedSunshineHost();
  if (gameStreamOpenSunshineBtn) gameStreamOpenSunshineBtn.disabled = !canControlSelectedSunshineHost();
  if (gameStreamDownloadBtn) {
    const toolInstalled = isRequiredGameStreamToolInstalled();
    gameStreamDownloadBtn.hidden = toolInstalled && !gameStreamInstallBusy;
    gameStreamDownloadBtn.disabled = anyBusy || toolInstalled;
    gameStreamDownloadBtn.textContent = gameStreamInstallBusy ? '安装中...' : `安装 ${requiredGameStreamToolLabel()}`;
  }
  const actionLabel = '游戏模式';
  const enterText = enterDesktopBtn.querySelector('span');
  const connectText = connectSelectedBtn.querySelector('span');
  if (enterText) enterText.textContent = anyBusy ? '正在启动...' : (selected ? actionLabel : '进入桌面');
  if (connectText) connectText.textContent = anyBusy ? '正在启动...' : actionLabel;

  for (const item of deviceListEl.querySelectorAll('.sideItem[data-id]')) {
    item.addEventListener('click', () => {
      selectedId = item.dataset.id;
      renderDevices();
    });
  }
}

async function refreshDevices() {
  devices = await window.lanRemote.getDevices();
  renderDevices();
}

async function openSelected() {
  const device = selectedDevice();
  if (!device) return;
  await openGameStreamDevice(device);
}

async function openNativeV2Device(device) {
  if (nativeV2Connecting) {
    log('native-v2 start ignored: already connecting');
    return;
  }
  setNativeV2Busy(true, `正在启动 Native v2：请等待 ${device.address} 准备视频流...`);
  try {
    await refreshNativeV2Status();
    if (nativeV2Status?.platform === 'win32') {
      if (!nativeV2Status?.winClient?.available) {
        const message = 'Native v2 Windows 客户端还没构建。请先执行 npm run v2:win:build。';
        log(message);
        setNativeV2StatusText(message);
        return;
      }
      const options = nativeV2ClientOptions(device);
      const hostOptions = await resolveNativeV2HostOptions(device);
      setNativeV2StatusText(`正在先启动 Windows Native v2 接收端（${options.transport.toUpperCase()} 视频），避免 Mac 首帧/关键帧丢失...`);
      const result = await window.lanRemote.startNativeV2Client(options);
      log(`native-v2 client started pid=${result.pid}; host=${options.hostIp}:${options.videoPort}; ${options.width}x${options.height}@${options.fps}; transport=${options.transport}`);
      if (device.pin && device.port) {
        setNativeV2StatusText(`正在请求 Mac 启动 Native v2 Host：${device.address}:${device.port}`);
        log(`requesting Mac native-v2 host ${device.address}:${device.port} -> client ${hostOptions.clientIp}:${hostOptions.videoPort}; transport=${hostOptions.transport}`);
        try {
          await window.lanRemote.requestNativeV2RemoteHost(device, hostOptions);
          log('Mac native-v2 host accepted start request');
        } catch (err) {
          await window.lanRemote.stopNativeV2Client?.();
          throw err;
        }
      } else {
        log(`manual native-v2: 请先在 Mac 端启动 Host：\n${nativeV2MacHostCommand(device, hostOptions.clientIp)}`);
      }
      setNativeV2StatusText(`Native v2 客户端已启动，pid=${result.pid}；连接后可在顶部状态栏切换分辨率、帧率和码率。`);
      return;
    }

    if (nativeV2Status?.platform === 'darwin') {
      if (!nativeV2Status?.macHost?.available) {
        const message = 'Native v2 macOS Host 还没构建。请先在 Mac 上执行 npm run v2:mac:build。';
        log(message);
        setNativeV2StatusText(message);
        return;
      }
      const options = await resolveNativeV2HostOptions(device);
      setNativeV2StatusText(`正在启动 macOS Native v2 Host（${options.transport.toUpperCase()} 视频），等待 Windows 客户端 ${options.clientIp}...`);
      const result = await window.lanRemote.startNativeV2Host(options);
      log(`native-v2 host started pid=${result.pid}; client=${options.clientIp}:${options.videoPort}; ${options.width}x${options.height}@${options.fps}; transport=${options.transport}`);
      setNativeV2StatusText(`macOS Native v2 Host 已启动，pid=${result.pid}`);
      return;
    }

    const message = 'Native v2 当前仅支持 Windows 控制端和 macOS Host。';
    log(message);
    setNativeV2StatusText(message);
  } catch (err) {
    const message = friendlyNativeV2Error(err, device);
    log(`native-v2 start failed: ${message}`);
    setNativeV2StatusText(`Native v2 启动失败：${message}`);
  } finally {
    setNativeV2Busy(false);
  }
}

async function openGameStreamDevice(device) {
  if (gameStreamBusy) {
    log('game-stream start ignored: already connecting');
    return;
  }
  setGameStreamBusy(true, `正在启动游戏模式：请等待 ${device.address} 的 Sunshine/Moonlight 就绪...`);
  try {
    await refreshGameStreamStatus();
    if (gameStreamStatus?.platform === 'win32') {
      if (!gameStreamStatus?.moonlight?.available) {
        const message = 'Moonlight 未安装。请安装 Moonlight Qt 后重试。';
        log(message);
        setGameStreamStatusText(message);
        return;
      }
      if (device.pin && device.port) {
        setGameStreamStatusText(`正在请求 Mac 启动 Sunshine Host：${device.address}:${device.port}`);
        const hostResult = await window.lanRemote.requestGameStreamRemoteHost(device, {
          ...gameStreamRemoteHostOptions(device),
          requestTimeoutMs: 20_000,
        });
        if (hostResult?.skipped) {
          log(`Mac Sunshine host start returned non-blocking warning: ${hostResult.reason || 'unknown'}`);
        } else {
          log(`Mac Sunshine host accepted start request: ${device.address}`);
        }
      } else {
        log('manual game-stream: 请确认 Mac 端 Sunshine 已启动，并完成 Moonlight 配对。');
      }
      const options = gameStreamClientOptions(device);
      setGameStreamStatusText(`正在启动 Moonlight：${options.width}x${options.height}@${options.fps} ${options.videoCodec} ${Math.round(options.bitrateKbps / 1000)}Mbps...`);
      const result = await window.lanRemote.startGameStreamClient(options);
      log(`Moonlight started pid=${result.pid}; host=${options.hostIp}; ${options.width}x${options.height}@${options.fps}; codec=${options.videoCodec}; bitrate=${options.bitrateKbps}Kbps`);
      setGameStreamStatusText(`Moonlight 游戏模式已启动，pid=${result.pid}；如果提示未配对，请先点击“配对”。`);
      return;
    }

    if (gameStreamStatus?.platform === 'darwin') {
      if (!gameStreamStatus?.sunshine?.available) {
        const message = 'Sunshine 未安装。请安装 Sunshine 后重试。';
        log(message);
        setGameStreamStatusText(message);
        return;
      }
      const result = await window.lanRemote.startGameStreamHost(gameStreamRemoteHostOptions(device));
      log(`Sunshine host started pid=${result.pid}; Web UI=${result.webUrl || 'https://localhost:47990/'}`);
      setGameStreamStatusText(`Sunshine Host 已启动，pid=${result.pid}；Windows 端用 Moonlight 连接 ${appInfo?.device?.addresses?.[0] || '本机 IP'}。`);
      return;
    }

    const message = '游戏模式当前面向 Windows Moonlight 客户端和 macOS Sunshine Host。';
    log(message);
    setGameStreamStatusText(message);
  } catch (err) {
    const message = friendlyGameStreamError(err);
    log(`game-stream start failed: ${message}`);
    setGameStreamStatusText(`游戏模式启动失败：${message}`);
  } finally {
    setGameStreamBusy(false);
  }
}

function generatePairingPin() {
  return String(Math.floor(1000 + Math.random() * 9000));
}

async function pairGameStreamDevice(device) {
  if (!device) return;
  setGameStreamBusy(true, `正在启动 Moonlight 配对：${device.address}`);
  try {
    await refreshGameStreamStatus();
    if (gameStreamStatus?.platform !== 'win32') {
      setGameStreamStatusText('配对需要在 Windows Moonlight 客户端执行。');
      return;
    }
    if (!gameStreamStatus?.moonlight?.available) {
      setGameStreamStatusText('Moonlight 未安装。请安装 Moonlight Qt 后重试。');
      return;
    }
    const pairPin = generatePairingPin();
    const pairName = appInfo?.device?.name || 'Windows Moonlight';
    if (!device.pin || !device.port) {
      setGameStreamStatusText('手动 IP 无法自动提交 Sunshine PIN；请用自动发现的 Mac 设备，或手动在 Sunshine Web 输入 PIN。');
      return;
    }
    setGameStreamStatusText('正在请求 Mac 启动 Sunshine Host...');
    await window.lanRemote.requestGameStreamRemoteHost(device, gameStreamRemoteHostOptions(device));
    log(`Mac Sunshine host accepted start request for pairing: ${device.address}`);

    setGameStreamStatusText('正在启动 Moonlight 自动配对...');
    const result = await window.lanRemote.pairGameStreamClient({ hostIp: device.address, pin: pairPin });
    log(`Moonlight pairing started pid=${result.pid}; host=${device.address}`);

    setGameStreamStatusText('正在把自动生成的 PIN 提交给 Mac Sunshine...');
    const submitted = await window.lanRemote.submitGameStreamPairPin({
      host: device.address,
      pin: pairPin,
      name: pairName,
      timeoutMs: 15_000,
      retryDelayMs: 500,
    });
    log(`Mac Sunshine accepted automatic pair PIN after ${submitted.attempts || 1} attempt(s): ${device.address}`);
    setGameStreamStatusText('Moonlight 自动配对已提交；如果 Moonlight 提示成功，可直接点“启动游戏模式”。');
  } catch (err) {
    const message = friendlyGameStreamError(err);
    log(`game-stream pair failed: ${message}`);
    setGameStreamStatusText(`游戏模式配对失败：${message}`);
  } finally {
    setGameStreamBusy(false);
  }
}

async function startSelectedGameStreamHost() {
  if (gameStreamBusy) return;
  const selected = selectedDevice();
  setGameStreamBusy(true, canControlLocalSunshineHost() ? '正在启动本机 Sunshine Host...' : `正在请求 ${selected?.address || 'Mac'} 启动 Sunshine Host...`);
  try {
    if (canControlLocalSunshineHost()) {
      const result = await window.lanRemote.startGameStreamHost(gameStreamRemoteHostOptions(selected));
      log(`Sunshine host started pid=${result.pid}; Web UI=${result.webUrl || 'https://localhost:47990/'}`);
      setGameStreamStatusText(`Sunshine Host 已启动，pid=${result.pid}。首次使用请打开 Sunshine Web UI 完成设置/配对。`);
      return;
    }
    if (!selected?.pin || !selected?.port) {
      const message = '需要选中自动发现到的 Mac，才能远程启动 Sunshine Host。手动 IP 只能打开 Sunshine Web。';
      log(message);
      setGameStreamStatusText(message);
      return;
    }
    const result = await window.lanRemote.requestGameStreamRemoteHost(selected, gameStreamRemoteHostOptions(selected));
    log(`Mac Sunshine host accepted start request: ${selected.address}`);
    setGameStreamStatusText('已请求 Mac 启动 Sunshine Host；可以打开 Sunshine Web 或直接启动游戏模式。');
    if (result?.pid) log(`remote Sunshine host pid=${result.pid}`);
  } catch (err) {
    const message = friendlyGameStreamError(err);
    log(`Sunshine start failed: ${message}`);
    setGameStreamStatusText(`Sunshine 启动失败：${message}`);
  } finally {
    setGameStreamBusy(false);
  }
}

async function manualGameStreamConnect(address) {
  const device = {
    id: `manual-game-stream-${address}-${Date.now()}`,
    name: address,
    platform: gameStreamStatus?.platform === 'darwin' ? 'win32' : 'darwin',
    address,
    port: 0,
    pin: '',
  };
  await openGameStreamDevice(device);
}


async function initApp() {
  localStorage.removeItem('p2p-remote-dashboard-connection-mode-v1');
  clearLegacyDashboardControlProfile();
  const info = await window.lanRemote.getAppInfo();
  appInfo = info;
  document.body.dataset.platform = info.device.platform;
  localPinEl.textContent = info.device.pin;
  localAddrEl.textContent = info.device.addresses.length
    ? `${info.device.addresses.join(' / ')}:${info.device.port}`
    : `端口 ${info.device.port}`;
  log(`ready as ${info.device.name}; pin=${info.device.pin}; discovery=${info.discoveryPort}`);
  if (info.screenCaptureStatus && info.screenCaptureStatus !== 'unknown') {
    log(`macOS screen permission=${info.screenCaptureStatus}`);
  }
  if (info.accessibilityStatus && info.accessibilityStatus !== 'unknown') {
    log(`macOS accessibility permission=${info.accessibilityStatus}`);
  }
  await refreshMacPermissionStatus();
  await refreshNativeV2Status();
  await refreshGameStreamStatus();
  await refreshDevices();
}


function wireWindowControls() {
  for (const button of document.querySelectorAll('[data-window-action]')) {
    button.addEventListener('click', () => {
      window.lanRemote.windowAction(button.dataset.windowAction);
    });
  }
  for (const region of document.querySelectorAll('.topbar, .sidebarChrome')) {
    region.addEventListener('dblclick', (event) => {
      if (event.target.closest('button')) return;
      window.lanRemote.windowAction('toggle-maximize');
    });
  }
}

deviceListEl.addEventListener('dblclick', openSelected);
enterDesktopBtn.addEventListener('click', openSelected);
connectSelectedBtn.addEventListener('click', openSelected);
refreshDevicesBtn.addEventListener('click', refreshDevices);
refreshTopBtn.addEventListener('click', refreshDevices);
macPermissionCardEl?.addEventListener('click', openPermissionModal);
for (const button of document.querySelectorAll('.openPermissionPanel')) {
  button.addEventListener('click', openPermissionModal);
}
closePermissionModalBtn?.addEventListener('click', closePermissionModal);
permissionModalEl?.addEventListener('click', (event) => {
  if (event.target === permissionModalEl) closePermissionModal();
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && permissionModalEl && !permissionModalEl.hidden) closePermissionModal();
});
screenPermissionToggleBtn?.addEventListener('click', async () => {
  if (permissionGranted(macPermissionStatus.screenCapture)) {
    await window.lanRemote.resetScreenCapturePermission();
    log('screen permission reset; enable it again in System Settings, then fully quit and reopen the Mac app');
  } else {
    await window.lanRemote.openScreenCaptureSettings();
    log('opened Screen Recording settings; enable this app or Terminal, then fully quit and reopen the Mac app');
  }
  await refreshMacPermissionStatus();
});
accessibilityPermissionToggleBtn?.addEventListener('click', async () => {
  if (permissionGranted(macPermissionStatus.accessibility)) {
    await window.lanRemote.resetAccessibilityPermission();
    log('accessibility permission reset; enable it again in System Settings, then fully quit and reopen the Mac app');
  } else {
    await window.lanRemote.requestAccessibilityPermission();
    log('requested Accessibility permission; enable this app or Terminal, then fully quit and reopen the Mac app');
  }
  await refreshMacPermissionStatus();
});
refreshPermissionStatusBtn?.addEventListener('click', refreshMacPermissionStatus);
openSystemPrivacySettingsBtn?.addEventListener('click', () => window.lanRemote.openScreenCaptureSettings());
manualAddBtn.addEventListener('click', async () => {
  const endpoint = window.prompt(gameStreamStatus?.platform === 'darwin'
    ? '输入 Windows IP（游戏模式会启动本机 Sunshine；Native v2 fallback 仍可用）'
    : '输入 Mac IP（游戏模式使用 Sunshine/Moonlight）', '');
  if (!endpoint) return;
  const [address] = endpoint.trim().split(':');
  if (!address) return;
  await manualGameStreamConnect(address);
});
gameStreamConnectBtn?.addEventListener('click', async () => openGameStreamDevice(selectedDevice()));
nativeV2ConnectBtn?.addEventListener('click', async () => openNativeV2Device(selectedDevice()));
gameStreamPairBtn?.addEventListener('click', async () => pairGameStreamDevice(selectedDevice()));
gameStreamHostBtn?.addEventListener('click', startSelectedGameStreamHost);
gameStreamOpenSunshineBtn?.addEventListener('click', () => {
  window.lanRemote.openGameStreamSunshine?.(selectedSunshineWebHost());
});
gameStreamDownloadBtn?.addEventListener('click', installGameStreamComponent);

window.lanRemote.onDevicesUpdated((list) => {
  devices = list;
  renderDevices();
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));
window.lanRemote.onNativeV2Status?.((status) => updateNativeV2Status(status));
window.lanRemote.onGameStreamStatus?.((status) => updateGameStreamStatus(status));
window.lanRemote.onGameStreamInstallProgress?.((progress) => renderGameStreamInstallProgress(progress));


wireWindowControls();
initApp().catch((err) => log(`init failed: ${err.stack || err.message}`));
