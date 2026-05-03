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

let devices = [];
let selectedId = null;
let localStream = null;
let nativeV2Status = null;
let appInfo = null;
let nativeV2Connecting = false;
const peers = new Map();
const pendingPeerCandidates = new Map();

function iconSvg(name, className = 'icon') {
  return `<svg class="${className}" aria-hidden="true"><use href="assets/icons.svg#${name}"></use></svg>`;
}

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function setNativeV2StatusText(message) {
  if (nativeV2StatusTextEl) nativeV2StatusTextEl.textContent = message;
}

function friendlyNativeV2Error(err, device) {
  const raw = err?.message || String(err || 'Unknown error');
  if (raw.includes('timed out') || raw.includes('closed before response')) {
    return [
      `Mac 端没有响应 Native v2 启动请求（${device?.address || 'unknown'}:7777）。`,
      '请确认 Mac 上运行的是最新版本 App，并且 Mac 端已执行 npm run v2:mac:build；否则先切回“稳定 WebRTC”。',
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
    "'": '&#39;',
  })[char]);
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
      nativeV2StatusTextEl.textContent = `Windows Native v2 客户端运行中，pid=${winClient.pid}`;
    } else if (winClient.available) {
      nativeV2StatusTextEl.textContent = 'Windows Native v2 客户端已就绪；Mac 端先启动 Host 后可一键进入极限模式。';
    } else {
      nativeV2StatusTextEl.textContent = 'Windows Native v2 客户端未构建；先执行 npm run v2:win:build，当前可继续使用 WebRTC。';
    }
    return;
  }

  if (isMac) {
    if (macHost.running) {
      nativeV2StatusTextEl.textContent = `macOS Native v2 Host 运行中，pid=${macHost.pid}`;
    } else if (macHost.available) {
      nativeV2StatusTextEl.textContent = 'macOS Native v2 Host 已构建；输入 Windows IP 后可启动极限 Host。';
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

function nativeV2ClientOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  return {
    hostIp: device.address,
    videoPort: defaults.videoPort || 45000,
    inputPort: defaults.inputPort || 45001,
    width: defaults.width || 1920,
    height: defaults.height || 1080,
    fps: defaults.fps || 120,
    fullscreen: true,
    transport: 'tcp',
  };
}

function nativeV2HostOptions(device) {
  const defaults = nativeV2Status?.defaults || {};
  const clientIp = appInfo?.device?.addresses?.[0] || '';
  return {
    clientIp: nativeV2Status?.platform === 'win32' ? clientIp : device.address,
    videoPort: defaults.videoPort || 45000,
    inputPort: defaults.inputPort || 45001,
    width: defaults.width || 1920,
    height: defaults.height || 1080,
    fps: defaults.fps || 120,
    bitrate: defaults.bitrate || 45_000_000,
    keyint: defaults.keyint || 1,
    transport: 'tcp',
  };
}

function nativeV2MacHostCommand(device, routeAddress = '') {
  const defaults = nativeV2Status?.defaults || {};
  const clientIp = routeAddress || appInfo?.device?.addresses?.[0] || '<Windows_IP>';
  return [
    'cd native-v2/mac-host',
    `CLIENT_IP=${clientIp} VIDEO_PORT=${defaults.videoPort || 45000} INPUT_PORT=${defaults.inputPort || 45001} WIDTH=${defaults.width || 1920} HEIGHT=${defaults.height || 1080} FPS=${defaults.fps || 120} BITRATE=${defaults.bitrate || 45000000} TRANSPORT=tcp ./run-ultra.sh`,
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
  const actionLabel = connectionMode() === 'native-v2' ? 'Native v2 极限' : '远程桌面';
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
  setNativeV2Busy(true, `正在启动 Native v2：请求 ${device.address} 准备视频流...`);
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
      setNativeV2StatusText('正在先启动 Windows Native v2 接收端，避免 Mac 首帧/关键帧丢失...');
      const result = await window.lanRemote.startNativeV2Client(options);
      log(`native-v2 client started pid=${result.pid}; host=${options.hostIp}:${options.videoPort}; ${options.width}x${options.height}@${options.fps}`);
      if (device.pin && device.port) {
        setNativeV2StatusText(`正在请求 Mac 启动 Native v2 Host：${device.address}:${device.port}`);
        log(`requesting Mac native-v2 host ${device.address}:${device.port} -> client ${hostOptions.clientIp}:${hostOptions.videoPort}`);
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
      setNativeV2StatusText(`Native v2 客户端已启动，pid=${result.pid}`);
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
      setNativeV2StatusText(`正在启动 macOS Native v2 Host，等待 Windows 客户端 ${options.clientIp}...`);
      const result = await window.lanRemote.startNativeV2Host(options);
      log(`native-v2 host started pid=${result.pid}; client=${options.clientIp}:${options.videoPort}; ${options.width}x${options.height}@${options.fps}`);
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

async function waitForLiveTrack(track, timeoutMs = 1500) {
  if (track.readyState === 'live' && !track.muted) return;
  await new Promise((resolve) => {
    const timer = setTimeout(resolve, timeoutMs);
    track.addEventListener('unmute', () => {
      clearTimeout(timer);
      resolve();
    }, { once: true });
  });
}

function withTimeout(promise, timeoutMs, message) {
  let timer = null;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(message)), timeoutMs);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function sendSignalProgress(clientId, stage, detail = '') {
  window.lanRemote.sendSignal({
    clientId,
    message: {
      type: 'progress',
      stage,
      detail,
      ts: Date.now(),
    },
  });
}

async function tuneCaptureTrack(track) {
  try {
    await track.applyConstraints({ frameRate: { ideal: 60 } });
  } catch (err) {
    log(`capture constraints skipped: ${err.message}`);
  }
}

async function startCapture() {
  if (localStream) return localStream;
  localStream = await withTimeout(
    navigator.mediaDevices.getDisplayMedia({ audio: false, video: true }),
    8000,
    'Mac screen capture did not start within 8s; check Screen Recording permission and restart the Mac app',
  );
  const tracks = localStream.getVideoTracks();
  if (tracks.length === 0) throw new Error('No screen video track returned');

  for (const track of tracks) {
    track.contentHint = 'motion';
    await tuneCaptureTrack(track);
    await waitForLiveTrack(track);
    const settings = track.getSettings?.() || {};
    log(`screen track ready: ${settings.width || '?'}x${settings.height || '?'} muted=${track.muted} state=${track.readyState}`);
    track.addEventListener('mute', () => log('screen track muted'));
    track.addEventListener('unmute', () => log('screen track unmuted'));
    track.addEventListener('ended', () => {
      log('local screen capture stopped');
      localStream = null;
      for (const peer of peers.values()) peer.close();
      peers.clear();
      pendingPeerCandidates.clear();
    });
  }
  log('local screen capture started');
  return localStream;
}

function preferH264(pc, sender) {
  try {
    const caps = RTCRtpSender.getCapabilities('video');
    const transceiver = pc.getTransceivers().find((item) => item.sender === sender);
    if (!caps?.codecs || !transceiver?.setCodecPreferences) return;
    const h264 = caps.codecs.filter((codec) => codec.mimeType.toLowerCase() === 'video/h264');
    const rest = caps.codecs.filter((codec) => codec.mimeType.toLowerCase() !== 'video/h264');
    if (h264.length) transceiver.setCodecPreferences([...h264, ...rest]);
  } catch (err) {
    log(`codec preference skipped: ${err.message}`);
  }
}

async function tuneSender(sender) {
  try {
    const params = sender.getParameters();
    if (!params.encodings || params.encodings.length === 0) params.encodings = [{}];
    params.encodings[0].maxBitrate = 35_000_000;
    params.encodings[0].maxFramerate = 60;
    params.degradationPreference = 'maintain-framerate';
    await sender.setParameters(params);
  } catch (err) {
    log(`sender tuning skipped: ${err.message}`);
  }
}

function makeHostPeer(clientId) {
  const pc = new RTCPeerConnection({
    iceServers: [
      { urls: 'stun:stun.l.google.com:19302' },
      { urls: 'stun:stun.cloudflare.com:3478' },
    ],
    bundlePolicy: 'max-bundle',
    rtcpMuxPolicy: 'require',
    iceCandidatePoolSize: 0,
  });

  pc.onicecandidate = (event) => {
    if (event.candidate) {
      window.lanRemote.sendSignal({ clientId, message: { type: 'candidate', candidate: event.candidate } });
    }
  };
  pc.onconnectionstatechange = () => log(`incoming ${clientId.slice(0, 8)} state=${pc.connectionState}`);
  pc.oniceconnectionstatechange = () => log(`incoming ${clientId.slice(0, 8)} ice=${pc.iceConnectionState}`);
  pc.onnegotiationneeded = () => log(`incoming ${clientId.slice(0, 8)} negotiationneeded`);
  pc.ondatachannel = (event) => {
    const channel = event.channel;
    channel.onmessage = (msg) => {
      try {
        window.lanRemote.sendInput(JSON.parse(msg.data));
      } catch (err) {
        log(`bad input packet: ${err.message}`);
      }
    };
  };

  peers.set(clientId, pc);
  return pc;
}

async function addPeerCandidate(clientId, pc, candidate) {
  if (!candidate) return;
  if (!pc || !pc.remoteDescription) {
    const pending = pendingPeerCandidates.get(clientId) || [];
    pending.push(candidate);
    pendingPeerCandidates.set(clientId, pending);
    log(`queued ICE candidate from ${clientId.slice(0, 8)} (${pending.length}) until peer/offer is ready`);
    return;
  }
  try {
    await pc.addIceCandidate(candidate);
    log(`ICE candidate applied from ${clientId.slice(0, 8)}`);
  } catch (err) {
    log(`addIceCandidate failed from ${clientId.slice(0, 8)}: ${err.message}`);
  }
}

async function flushPeerCandidates(clientId, pc) {
  if (!pc?.remoteDescription) return;
  const pending = pendingPeerCandidates.get(clientId) || [];
  if (!pending.length) return;
  pendingPeerCandidates.delete(clientId);
  log(`applying ${pending.length} queued ICE candidates from ${clientId.slice(0, 8)}`);
  for (const candidate of pending) {
    await addPeerCandidate(clientId, pc, candidate);
  }
}

function ensureVideoSender(pc) {
  let transceiver = pc.getTransceivers().find((item) => (
    item.receiver?.track?.kind === 'video' || item.sender?.track?.kind === 'video'
  ));
  if (!transceiver) transceiver = pc.addTransceiver('video', { direction: 'sendonly' });
  try {
    transceiver.direction = 'sendonly';
  } catch (err) {
    log(`set video transceiver direction skipped: ${err.message}`);
  }
  return transceiver;
}

async function attachScreenStream(pc, stream, videoTransceiver) {
  for (const track of stream.getTracks()) {
    if (track.kind === 'video' && videoTransceiver?.sender) {
      track.contentHint = 'detail';
      await videoTransceiver.sender.replaceTrack(track);
      await tuneSender(videoTransceiver.sender);
      continue;
    }
    const sender = pc.addTrack(track, stream);
    if (track.kind === 'video') await tuneSender(sender);
  }
}

async function handleSignal({ clientId, message }) {
  if (!message?.type) return;
  let pc = peers.get(clientId);

  if (message.type === 'offer') {
    try {
      log(`offer received from ${clientId.slice(0, 8)}; starting capture`);
      sendSignalProgress(clientId, 'offer-received', 'Mac received offer, preparing WebRTC answer');
      if (pc) pc.close();
      pc = makeHostPeer(clientId);

      await pc.setRemoteDescription(message.sdp);
      await flushPeerCandidates(clientId, pc);
      const videoTransceiver = ensureVideoSender(pc);
      sendSignalProgress(clientId, 'capture-starting', 'Mac is starting screen capture');
      const stream = await startCapture();
      sendSignalProgress(clientId, 'capture-ready', 'Mac screen capture is ready');
      await attachScreenStream(pc, stream, videoTransceiver);
      log(`screen stream attached for ${clientId.slice(0, 8)}`);
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      window.lanRemote.sendSignal({ clientId, message: { type: 'answer', sdp: pc.localDescription } });
      sendSignalProgress(clientId, 'answer-sent', 'Mac sent WebRTC answer');
      log(`answered remote desktop request from ${clientId.slice(0, 8)}`);
    } catch (err) {
      window.lanRemote.sendSignal({ clientId, message: { type: 'error', error: err.message } });
      log(`screen share failed: ${err.message}`);
    }
    return;
  }

  if (message.type === 'candidate' && message.candidate) {
    await addPeerCandidate(clientId, pc, message.candidate);
  }
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
connectionModeEl?.addEventListener('change', () => {
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
  handleSignal(payload).catch((err) => log(`signal error: ${err.stack || err.message}`));
});
window.lanRemote.onClientConnected(({ clientId, remoteAddress }) => log(`incoming pair: ${clientId.slice(0, 8)} from ${remoteAddress}`));
window.lanRemote.onClientDisconnected(({ clientId }) => {
  const pc = peers.get(clientId);
  if (pc) pc.close();
  peers.delete(clientId);
  pendingPeerCandidates.delete(clientId);
  log(`incoming disconnected: ${clientId.slice(0, 8)}`);
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));
window.lanRemote.onNativeV2Status?.((status) => {
  updateNativeV2Status(status);
});

wireWindowControls();
window.lanRemote.hostRendererReady?.();
initApp().catch((err) => log(`init failed: ${err.stack || err.message}`));
