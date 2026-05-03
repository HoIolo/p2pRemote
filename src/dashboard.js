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
const openScreenSettingsBtn = $('openScreenSettings');
const resetScreenPermissionBtn = $('resetScreenPermission');
const manualAddBtn = $('manualAdd');
const connectionModeEl = $('connectionMode');
const nativeV2StatusTextEl = $('nativeV2StatusText');
const logEl = $('log');

const CONNECTION_MODE_KEY = 'p2p-remote-dashboard-connection-mode-v1';

let devices = [];
let selectedId = null;
let nativeV2Status = null;
let appInfo = null;
let nativeV2Connecting = false;

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

function friendlyNativeV2Error(err, device) {
  const raw = err?.message || String(err || 'Unknown error');
  if (raw.includes('timed out') || raw.includes('closed before response')) {
    return [
      `Mac 端没有响应 Native v2 启动请求（${device?.address || 'unknown'}:7777）。`,
      '请确认 Mac 上运行的是最新版本 App，并且已执行 npm run v2:mac:build；否则先切回“稳定 WebRTC”。',
    ].join(' ');
  }
  if (raw.includes('还没有构建') || raw.includes('not built')) return raw;
  if (raw.includes('bad pin')) return 'PIN 校验失败，请刷新设备后重试。';
  return raw.replace(/^Error invoking remote method '[^']+': Error:\s*/i, '');
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

function connectionMode() {
  return connectionModeEl?.value || 'webrtc';
}

function updateNativeV2Status(status) {
  nativeV2Status = status || nativeV2Status;
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
      nativeV2StatusTextEl.textContent = 'Windows Native v2 客户端未构建；先执行 npm run v2:win:build，当前可继续使用 WebRTC。';
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

function scaleResolution(width, height, maxLongEdge = 1600) {
  const longEdge = Math.max(width, height);
  if (!longEdge || longEdge <= maxLongEdge) {
    return { width: clampEven(width, 1920), height: clampEven(height, 1080) };
  }
  const scale = maxLongEdge / longEdge;
  return {
    width: clampEven(width * scale, 1920),
    height: clampEven(height * scale, 1080),
  };
}

function autoBitrateForPixels(pixels, fallbackBitrate) {
  let bitrate = fallbackBitrate;
  if (pixels <= 1280 * 720) bitrate = Math.max(bitrate, 8_000_000);
  else if (pixels <= 1600 * 900) bitrate = Math.max(bitrate, 10_000_000);
  else if (pixels <= 1920 * 1080) bitrate = Math.max(bitrate, 12_000_000);
  else if (pixels <= 1920 * 1200) bitrate = Math.max(bitrate, 14_000_000);
  else if (pixels <= 2560 * 1440) bitrate = Math.max(bitrate, 18_000_000);
  else bitrate = Math.max(bitrate, 24_000_000);
  return bitrate;
}

function nativeV2DisplayOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const display = device?.display;
  const scaleFactor = Number(device?.scaleFactor || 1);
  if (!display?.width || !display?.height) {
    const width = clampEven(defaults.width || 1920, 1920);
    const height = clampEven(defaults.height || 1080, 1080);
    return {
      width,
      height,
      bitrate: autoBitrateForPixels(width * height, defaults.bitrate || 14_000_000),
    };
  }

  const sourceWidth = clampEven(display.width * scaleFactor, defaults.width || 1920);
  const sourceHeight = clampEven(display.height * scaleFactor, defaults.height || 1080);
  const scaled = scaleResolution(sourceWidth, sourceHeight);
  const pixels = scaled.width * scaled.height;
  const bitrate = autoBitrateForPixels(pixels, defaults.bitrate || 14_000_000);

  return {
    width: scaled.width,
    height: scaled.height,
    bitrate,
  };
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
    fps: defaults.fps || 60,
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
    fps: defaults.fps || 60,
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
    `CLIENT_IP=${clientIp} VIDEO_PORT=${defaults.videoPort || 45000} INPUT_PORT=${defaults.inputPort || 45001} WIDTH=${display.width} HEIGHT=${display.height} FPS=${defaults.fps || 60} BITRATE=${display.bitrate} TRANSPORT=${defaults.transport || 'udp'} ./run-ultra.sh`,
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
  enterDesktopBtn.disabled = !selected || nativeV2Connecting;
  connectSelectedBtn.disabled = !selected || nativeV2Connecting;
  const actionLabel = connectionMode() === 'native-v2' ? 'Native v2 极速' : '远程桌面';
  const enterText = enterDesktopBtn.querySelector('span');
  const connectText = connectSelectedBtn.querySelector('span');
  if (enterText) enterText.textContent = nativeV2Connecting ? '正在启动...' : (selected ? actionLabel : '进入桌面');
  if (connectText) connectText.textContent = nativeV2Connecting ? '正在启动...' : actionLabel;

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
  if (connectionMode() === 'native-v2') {
    await openNativeV2Device(device);
    return;
  }
  await window.lanRemote.openRemoteWindow(device);
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
        const message = 'Native v2 Windows 客户端还没构建。请先执行 npm run v2:win:build；现在可切回“稳定 WebRTC”。';
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
        const message = 'Native v2 macOS Host 还没构建。请先在 Mac 上执行 npm run v2:mac:build；现在可继续使用 WebRTC。';
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

async function manualNativeV2Connect(address) {
  const device = {
    id: `manual-native-v2-${address}-${Date.now()}`,
    name: address,
    platform: nativeV2Status?.platform === 'darwin' ? 'win32' : 'darwin',
    address,
    port: 0,
    pin: '',
  };
  await openNativeV2Device(device);
}

async function initApp() {
  const savedMode = localStorage.getItem(CONNECTION_MODE_KEY);
  if (savedMode && connectionModeEl?.querySelector(`option[value="${savedMode}"]`)) {
    connectionModeEl.value = savedMode;
  }
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
  await refreshNativeV2Status();
  await refreshDevices();
}

const controller = window.createWebRtcHostController({
  log,
  setStatus(_kind, _text) {},
  updatePeerState(clientId, state) {
    if (clientId) log(`incoming ${clientId.slice(0, 8)} state=${state}`);
  },
  setCaptureActive() {},
});

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
connectionModeEl?.addEventListener('change', () => {
  localStorage.setItem(CONNECTION_MODE_KEY, connectionMode());
  renderDevices();
  log(`connection mode=${connectionMode()}`);
});
openScreenSettingsBtn.addEventListener('click', () => window.lanRemote.openScreenCaptureSettings());
resetScreenPermissionBtn.addEventListener('click', async () => {
  await window.lanRemote.resetScreenCapturePermission();
  log('screen permission record reset; enable this app again, then fully quit and reopen it');
});
manualAddBtn.addEventListener('click', async () => {
  const endpoint = window.prompt(connectionMode() === 'native-v2' ? '输入 Mac IP（Native v2 不使用 PIN）' : '输入设备 IP 或 IP:端口', '');
  if (!endpoint) return;
  const [address, portText] = endpoint.trim().split(':');
  if (connectionMode() === 'native-v2') {
    await manualNativeV2Connect(address);
    return;
  }
  const pin = window.prompt('输入对方 PIN', '');
  if (!pin) return;
  await window.lanRemote.openRemoteWindow({
    id: `manual-${address}-${Date.now()}`,
    name: address,
    platform: 'unknown',
    address,
    port: Number(portText || 7777),
    pin,
  });
});

window.lanRemote.onDevicesUpdated((list) => {
  devices = list;
  renderDevices();
});
window.lanRemote.onSignalMessage((payload) => {
  controller.handleSignal(payload).catch((err) => log(`signal error: ${err.stack || err.message}`));
});
window.lanRemote.onClientConnected(({ clientId, remoteAddress }) => log(`incoming pair: ${clientId.slice(0, 8)} from ${remoteAddress}`));
window.lanRemote.onClientDisconnected(({ clientId }) => {
  controller.disconnectClient(clientId);
  log(`incoming disconnected: ${clientId.slice(0, 8)}`);
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));
window.lanRemote.onNativeV2Status?.((status) => updateNativeV2Status(status));

window.addEventListener('beforeunload', () => controller.dispose());

wireWindowControls();
window.lanRemote.hostRendererReady?.();
initApp().catch((err) => log(`init failed: ${err.stack || err.message}`));
