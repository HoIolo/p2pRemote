const $ = (id) => document.getElementById(id);

const pinEl = $('pin');
const portEl = $('port');
const addressesEl = $('addresses');
const displayEl = $('display');
const peerStateEl = $('peerState');
const statusDot = $('statusDot');
const statusText = $('statusText');
const logEl = $('log');
const startShareBtn = $('startShare');
const openScreenSettingsBtn = $('openScreenSettings');
const resetScreenPermissionBtn = $('resetScreenPermission');

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function setStatus(kind, text) {
  statusDot.className = `dot ${kind || ''}`;
  statusText.textContent = text;
}

function updatePeerState(clientId, state) {
  peerStateEl.textContent = clientId ? `${clientId.slice(0, 8)}: ${state}` : (state || '未连接');
}

const controller = window.createWebRtcHostController({
  log,
  setStatus,
  updatePeerState,
  setCaptureActive(active) {
    startShareBtn.disabled = active;
  },
});

async function initInfo() {
  const info = await window.lanRemote.getHostInfo();
  pinEl.textContent = info.pin;
  portEl.textContent = String(info.port);
  addressesEl.innerHTML = info.addresses.length
    ? info.addresses.map((a) => `<span class="code">${a.address}</span> <span class="small">${a.name}</span>`).join('<br />')
    : '<span class="small">未找到非内网 IPv4 地址</span>';
  displayEl.textContent = `${info.display.x},${info.display.y} ${info.display.width}x${info.display.height} @${info.scaleFactor}x`;
  log(`host ready; port=${info.port}; pin=${info.pin}`);

  if (info.platform === 'darwin') {
    const status = await window.lanRemote.getScreenCaptureStatus();
    log(`macOS screen permission=${status}`);
  }
}

startShareBtn.addEventListener('click', async () => {
  try {
    await controller.startCapture();
  } catch (err) {
    setStatus('warn', '共享失败');
    const status = await window.lanRemote.getScreenCaptureStatus().catch(() => 'unknown');
    const hint = status === 'granted'
      ? 'macOS reports granted; if you just enabled it, fully quit and reopen the app'
      : 'if the switch is already on, click 重置权限, enable it again, then fully quit and reopen the app';
    log(`screen capture failed: ${err.message}; macOS screen permission=${status}; ${hint}`);
  }
});

openScreenSettingsBtn?.addEventListener('click', () => {
  window.lanRemote.openScreenCaptureSettings().catch((err) => log(`open settings failed: ${err.message}`));
});

resetScreenPermissionBtn?.addEventListener('click', async () => {
  try {
    await window.lanRemote.resetScreenCapturePermission();
    log('screen permission record reset; enable P2P Remote LAN again, then fully quit and reopen the app');
  } catch (err) {
    log(`reset screen permission failed: ${err.message}`);
  }
});

window.lanRemote.onSignalMessage((payload) => {
  controller.handleSignal(payload).catch((err) => log(`signal error: ${err.stack || err.message}`));
});
window.lanRemote.onClientConnected(({ clientId, remoteAddress }) => log(`client paired: ${clientId.slice(0, 8)} from ${remoteAddress}`));
window.lanRemote.onClientDisconnected(({ clientId }) => {
  controller.disconnectClient(clientId);
  log(`client disconnected: ${clientId.slice(0, 8)}`);
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));

window.addEventListener('beforeunload', () => controller.dispose());

window.lanRemote.hostRendererReady?.();
initInfo().catch((err) => log(`init failed: ${err.message}`));
