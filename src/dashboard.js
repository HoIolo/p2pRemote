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
const logEl = $('log');

let devices = [];
let selectedId = null;
let localStream = null;
const peers = new Map();

function iconSvg(name, className = 'icon') {
  return `<svg class="${className}" aria-hidden="true"><use href="assets/icons.svg#${name}"></use></svg>`;
}

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
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
  enterDesktopBtn.disabled = !selected;
  connectSelectedBtn.disabled = !selected;

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
  await window.lanRemote.openRemoteWindow(device);
}

async function initApp() {
  const info = await window.lanRemote.getAppInfo();
  document.body.dataset.platform = info.device.platform;
  localPinEl.textContent = info.device.pin;
  localAddrEl.textContent = info.device.addresses.length
    ? `${info.device.addresses.join(' / ')}:${info.device.port}`
    : `端口 ${info.device.port}`;
  log(`ready as ${info.device.name}; pin=${info.device.pin}; discovery=${info.discoveryPort}`);
  if (info.screenCaptureStatus && info.screenCaptureStatus !== 'unknown') {
    log(`macOS screen permission=${info.screenCaptureStatus}`);
  }
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

async function tuneCaptureTrack(track) {
  try {
    await track.applyConstraints({ frameRate: { ideal: 60 } });
  } catch (err) {
    log(`capture constraints skipped: ${err.message}`);
  }
}

async function startCapture() {
  if (localStream) return localStream;
  localStream = await navigator.mediaDevices.getDisplayMedia({ audio: false, video: true });
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
    iceServers: [],
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

async function handleSignal({ clientId, message }) {
  if (!message?.type) return;
  let pc = peers.get(clientId);

  if (message.type === 'offer') {
    try {
      const stream = await startCapture();
      if (pc) pc.close();
      pc = makeHostPeer(clientId);

      for (const track of stream.getTracks()) {
        const sender = pc.addTrack(track, stream);
        if (track.kind === 'video') {
          preferH264(pc, sender);
          await tuneSender(sender);
        }
      }

      await pc.setRemoteDescription(message.sdp);
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      window.lanRemote.sendSignal({ clientId, message: { type: 'answer', sdp: pc.localDescription } });
      log(`answered remote desktop request from ${clientId.slice(0, 8)}`);
    } catch (err) {
      window.lanRemote.sendSignal({ clientId, message: { type: 'error', error: err.message } });
      log(`screen share failed: ${err.message}`);
    }
    return;
  }

  if (message.type === 'candidate' && pc && message.candidate) {
    try {
      await pc.addIceCandidate(message.candidate);
    } catch (err) {
      log(`addIceCandidate failed: ${err.message}`);
    }
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
openScreenSettingsBtn.addEventListener('click', () => window.lanRemote.openScreenCaptureSettings());
resetScreenPermissionBtn.addEventListener('click', async () => {
  await window.lanRemote.resetScreenCapturePermission();
  log('screen permission record reset; enable this app again, then fully quit and reopen it');
});
manualAddBtn.addEventListener('click', async () => {
  const endpoint = window.prompt('输入设备 IP 或 IP:端口', '');
  if (!endpoint) return;
  const pin = window.prompt('输入对方 PIN', '');
  if (!pin) return;
  const [address, portText] = endpoint.trim().split(':');
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
  log(`incoming disconnected: ${clientId.slice(0, 8)}`);
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));

wireWindowControls();
initApp().catch((err) => log(`init failed: ${err.stack || err.message}`));
