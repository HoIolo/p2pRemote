const $ = (id) => document.getElementById(id);

const remoteNameEl = $('remoteName');
const statusDot = $('statusDot');
const statusText = $('statusText');
const reconnectBtn = $('reconnect');
const fullscreenBtn = $('fullscreen');
const settingsBtn = $('settings');
const settingsMenu = $('settingsMenu');
const toggleLogBtn = $('toggleLog');
const logStateEl = $('logState');
const topRevealZone = $('topRevealZone');
const video = $('remoteVideo');
const overlay = $('overlay');
const logEl = $('log');

let config = null;
let ws = null;
let pc = null;
let inputChannel = null;
let pressedKeys = new Set();
let pointerDown = false;
let pendingMove = null;
let moveRaf = 0;
let fullScreen = false;
let previewSaved = false;
let sawRemoteTrack = false;
let isClosing = false;
let logVisible = localStorage.getItem('remoteLogVisible') === '1';
let topbarPinned = true;
let topbarHideTimer = 0;
let topbarRevealTimer = 0;

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
  const lines = logEl.textContent.split('\n');
  if (lines.length > 260) logEl.textContent = lines.slice(0, 260).join('\n');
}

function setStatus(kind, text) {
  statusDot.className = `dot ${kind || ''}`;
  statusText.textContent = text;
}

function applyLogVisibility() {
  logEl.hidden = !logVisible;
  document.body.classList.toggle('logVisible', logVisible);
  if (logStateEl) logStateEl.textContent = logVisible ? '\u6253\u5f00' : '\u5173\u95ed';
  localStorage.setItem('remoteLogVisible', logVisible ? '1' : '0');
}

function setSettingsOpen(open) {
  settingsMenu.hidden = !open;
  settingsBtn?.setAttribute('aria-expanded', open ? 'true' : 'false');
}

function setTopbarVisible(visible) {
  document.body.classList.toggle('topbarHidden', fullScreen && !visible);
}

function scheduleTopbarHide(delay = 1600) {
  clearTimeout(topbarHideTimer);
  if (!fullScreen || topbarPinned) {
    setTopbarVisible(true);
    return;
  }
  topbarHideTimer = setTimeout(() => setTopbarVisible(false), delay);
}

function revealTopbarTemporarily() {
  if (!fullScreen) return;
  setTopbarVisible(true);
  scheduleTopbarHide(1600);
}

function wireRemoteUi() {
  applyLogVisibility();
  settingsBtn?.addEventListener('click', (event) => {
    event.stopPropagation();
    setSettingsOpen(settingsMenu.hidden);
    revealTopbarTemporarily();
  });
  toggleLogBtn?.addEventListener('click', (event) => {
    event.stopPropagation();
    logVisible = !logVisible;
    applyLogVisibility();
  });
  document.addEventListener('click', (event) => {
    if (event.target.closest('#settings') || event.target.closest('#settingsMenu')) return;
    setSettingsOpen(false);
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') setSettingsOpen(false);
    if ((event.ctrlKey || event.metaKey) && event.shiftKey && event.code === 'KeyL') {
      logVisible = !logVisible;
      applyLogVisibility();
      event.preventDefault();
    }
  });

  topRevealZone?.addEventListener('mouseenter', () => {
    if (!fullScreen) return;
    clearTimeout(topbarRevealTimer);
    topbarRevealTimer = setTimeout(() => revealTopbarTemporarily(), 260);
  });
  topRevealZone?.addEventListener('mouseleave', () => clearTimeout(topbarRevealTimer));
  document.querySelector('.remoteTopbar')?.addEventListener('mouseenter', () => {
    topbarPinned = true;
    setTopbarVisible(true);
  });
  document.querySelector('.remoteTopbar')?.addEventListener('mouseleave', () => {
    topbarPinned = false;
    scheduleTopbarHide(700);
  });
  video.addEventListener('mousemove', (event) => {
    if (!fullScreen) return;
    if (event.clientY <= 4) revealTopbarTemporarily();
  });
}

function wireWindowControls() {
  for (const button of document.querySelectorAll('[data-window-action]')) {
    button.addEventListener('click', () => {
      window.lanRemote.windowAction(button.dataset.windowAction);
    });
  }
  document.querySelector('.remoteTopbar')?.addEventListener('dblclick', (event) => {
    if (event.target.closest('button')) return;
    window.lanRemote.windowAction('toggle-maximize');
  });
}

function endpoint() {
  const host = config.address;
  const port = config.port || 7777;
  if (host.startsWith('[')) return host.includes(']:') ? `ws://${host}` : `ws://${host}:${port}`;
  if (host.includes(':') && host.includes('.')) return `ws://${host}`;
  if (host.includes(':')) return `ws://[${host}]:${port}`;
  return `ws://${host}:${port}`;
}

function sendSignal(message) {
  if (isClosing) return;
  if (ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
}

function sendInput(event) {
  if (isClosing) return;
  if (inputChannel?.readyState !== 'open') return;
  if (inputChannel.bufferedAmount > 8_192) return;
  inputChannel.send(JSON.stringify({ ...event, t: performance.now() }));
}

function cleanupConnection() {
  isClosing = true;
  if (moveRaf) cancelAnimationFrame(moveRaf);
  moveRaf = 0;
  pendingMove = null;
  pressedKeys.clear();
  pointerDown = false;
  try {
    inputChannel?.close();
  } catch {}
  inputChannel = null;
  try {
    pc?.getSenders?.().forEach((sender) => sender.track?.stop?.());
    pc?.getReceivers?.().forEach((receiver) => receiver.track?.stop?.());
    pc?.close();
  } catch {}
  pc = null;
  try {
    if (ws?.readyState === WebSocket.OPEN || ws?.readyState === WebSocket.CONNECTING) ws.close(1000, 'remote window closed');
  } catch {}
  ws = null;
  try {
    const stream = video.srcObject;
    if (stream?.getTracks) stream.getTracks().forEach((track) => track.stop());
    video.pause();
    video.srcObject = null;
  } catch {}
}

function makePeer() {
  sawRemoteTrack = false;
  pc = new RTCPeerConnection({
    iceServers: [],
    bundlePolicy: 'max-bundle',
    rtcpMuxPolicy: 'require',
    iceCandidatePoolSize: 0,
  });
  pc.addTransceiver('video', { direction: 'recvonly' });

  inputChannel = pc.createDataChannel('input', {
    ordered: false,
    maxRetransmits: 0,
  });
  inputChannel.bufferedAmountLowThreshold = 4_096;
  inputChannel.onopen = () => log('input data channel open');
  inputChannel.onclose = () => log('input data channel closed');

  pc.ontrack = (event) => {
    sawRemoteTrack = true;
    const stream = event.streams[0] || new MediaStream([event.track]);
    video.srcObject = stream;
    overlay.style.display = 'grid';
    overlay.textContent = '\u5df2\u6536\u5230\u89c6\u9891\u8f68\u9053\uff0c\u7b49\u5f85\u7b2c\u4e00\u5e27';
    const playPromise = video.play();
    if (playPromise) playPromise.catch((err) => log(`video play skipped: ${err.message}`));
    log(`remote track: ${event.track.kind}`);
    event.track.onunmute = () => {
      overlay.style.display = 'none';
      log('remote video unmuted');
    };
    event.track.onmute = () => {
      overlay.style.display = 'grid';
      overlay.textContent = '\u89c6\u9891\u8f68\u9053\u6682\u65f6\u65e0\u753b\u9762';
      log('remote video muted');
    };
  };

  pc.onicecandidate = (event) => {
    if (event.candidate) sendSignal({ type: 'candidate', candidate: event.candidate });
  };
  pc.onconnectionstatechange = () => {
    log(`peer state=${pc.connectionState}`);
    if (pc.connectionState === 'connected') setStatus('ok', sawRemoteTrack ? '\u5df2\u8fde\u63a5' : '\u5df2\u8fde\u63a5\uff0c\u7b49\u5f85\u89c6\u9891');
    if (['failed', 'closed', 'disconnected'].includes(pc.connectionState)) {
      setStatus('warn', pc.connectionState);
      overlay.style.display = 'grid';
      overlay.textContent = `\u8fde\u63a5\u72b6\u6001\uff1a${pc.connectionState}`;
    }
  };
  pc.oniceconnectionstatechange = () => log(`ice=${pc.iceConnectionState}`);
}

function saveFirstFrame() {
  if (previewSaved || !config || !video.videoWidth || !video.videoHeight) return;
  previewSaved = true;
  try {
    const canvas = document.createElement('canvas');
    const width = 960;
    const height = Math.round(width * video.videoHeight / video.videoWidth);
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d');
    ctx.drawImage(video, 0, 0, width, height);
    const dataUrl = canvas.toDataURL('image/jpeg', 0.78);
    window.lanRemote.saveDevicePreview(config.id, dataUrl).catch(() => {});
  } catch (err) {
    log(`preview capture skipped: ${err.message}`);
  }
}

async function createOffer() {
  const offer = await pc.createOffer({ offerToReceiveVideo: true, offerToReceiveAudio: false });
  await pc.setLocalDescription(offer);
  sendSignal({ type: 'offer', sdp: pc.localDescription });
  log('offer sent');
}

async function connect() {
  if (!config) return;
  cleanupConnection();
  isClosing = false;
  pressedKeys.clear();
  pointerDown = false;
  overlay.style.display = 'grid';
  overlay.textContent = '正在连接视频流';

  makePeer();
  const url = endpoint();
  setStatus('warn', '连接中');
  log(`connecting ${url}`);

  ws = new WebSocket(url);
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: 'hello', pin: config.pin }));
    log('pairing hello sent');
  };
  ws.onerror = () => setStatus('warn', '信令错误');
  ws.onclose = (event) => {
    setStatus('', `信令关闭 ${event.code || ''}`);
    log(`websocket closed code=${event.code} reason=${event.reason}`);
  };
  ws.onmessage = async (event) => {
    if (isClosing || !pc) return;
    const message = JSON.parse(event.data);
    if (message.type === 'hello-ok') {
      setStatus('warn', '协商媒体');
      log(`paired as ${message.clientId.slice(0, 8)}`);
      await createOffer();
      return;
    }
    if (message.type === 'answer') {
      await pc.setRemoteDescription(message.sdp);
      overlay.textContent = '\u5df2\u5b8c\u6210\u534f\u5546\uff0c\u7b49\u5f85\u89c6\u9891\u5e27';
      log('answer applied');
      return;
    }
    if (message.type === 'candidate' && message.candidate) {
      try {
        await pc.addIceCandidate(message.candidate);
      } catch (err) {
        log(`addIceCandidate failed: ${err.message}`);
      }
      return;
    }
    if (message.type === 'error') {
      setStatus('warn', message.error || 'remote error');
      overlay.textContent = message.error || '\u8fdc\u7a0b\u7aef\u5171\u4eab\u5931\u8d25';
      log(`remote error: ${message.error}`);
    }
  };
}

function contentRect() {
  const r = video.getBoundingClientRect();
  const vw = video.videoWidth || 16;
  const vh = video.videoHeight || 9;
  const elementRatio = r.width / r.height;
  const videoRatio = vw / vh;
  if (elementRatio > videoRatio) {
    const width = r.height * videoRatio;
    return { left: r.left + (r.width - width) / 2, top: r.top, width, height: r.height };
  }
  const height = r.width / videoRatio;
  return { left: r.left, top: r.top + (r.height - height) / 2, width: r.width, height };
}

function pointerPayload(event, kind) {
  const r = contentRect();
  const x = Math.max(0, Math.min(1, (event.clientX - r.left) / r.width));
  const y = Math.max(0, Math.min(1, (event.clientY - r.top) / r.height));
  return { kind, x, y, button: event.button || 0 };
}

function enqueueMove(event) {
  pendingMove = pointerPayload(event, 'pointerMove');
  if (moveRaf) return;
  moveRaf = requestAnimationFrame(() => {
    moveRaf = 0;
    if (pendingMove) sendInput(pendingMove);
    pendingMove = null;
  });
}

video.addEventListener('loadedmetadata', () => log(`video metadata ${video.videoWidth}x${video.videoHeight}`));
video.addEventListener('loadeddata', () => {
  overlay.style.display = 'none';
  saveFirstFrame();
});
video.addEventListener('playing', () => {
  overlay.style.display = 'none';
  setStatus('ok', '\u5df2\u8fde\u63a5');
  log('video playing');
});
video.addEventListener('waiting', () => {
  if (!sawRemoteTrack) return;
  overlay.style.display = 'grid';
  overlay.textContent = '\u6b63\u5728\u7b49\u5f85\u4e0b\u4e00\u5e27';
});
video.addEventListener('contextmenu', (event) => event.preventDefault());
video.addEventListener('mousedown', (event) => {
  event.preventDefault();
  video.focus();
  pointerDown = true;
  sendInput(pointerPayload(event, 'pointerDown'));
});
video.addEventListener('mousemove', (event) => {
  if (inputChannel?.readyState === 'open') enqueueMove(event);
});
window.addEventListener('mouseup', (event) => {
  if (!pointerDown) return;
  event.preventDefault();
  pointerDown = false;
  sendInput(pointerPayload(event, 'pointerUp'));
});
video.addEventListener('wheel', (event) => {
  event.preventDefault();
  const p = pointerPayload(event, 'wheel');
  sendInput({ ...p, dx: event.deltaX, dy: event.deltaY });
}, { passive: false });

window.addEventListener('keydown', (event) => {
  if (event.key === 'F11') {
    fullscreenBtn.click();
    event.preventDefault();
    return;
  }
  if (document.activeElement !== video || inputChannel?.readyState !== 'open') return;
  if (pressedKeys.has(event.code)) return;
  event.preventDefault();
  pressedKeys.add(event.code);
  sendInput({ kind: 'keyDown', code: event.code, key: event.key });
});
window.addEventListener('keyup', (event) => {
  if (!pressedKeys.has(event.code)) return;
  event.preventDefault();
  pressedKeys.delete(event.code);
  sendInput({ kind: 'keyUp', code: event.code, key: event.key });
});
window.addEventListener('blur', () => {
  for (const code of pressedKeys) sendInput({ kind: 'keyUp', code });
  pressedKeys.clear();
  pointerDown = false;
});
window.addEventListener('beforeunload', cleanupConnection);

reconnectBtn.addEventListener('click', () => connect().catch((err) => log(`connect failed: ${err.stack || err.message}`)));
fullscreenBtn.addEventListener('click', async () => {
  fullScreen = !fullScreen;
  fullScreen = await window.lanRemote.setWindowFullscreen(fullScreen);
  document.body.classList.toggle('fullscreenMode', fullScreen);
  const label = fullscreenBtn.querySelector('span');
  if (label) label.textContent = fullScreen ? '\u9000\u51fa\u5168\u5c4f' : '\u5168\u5c4f';
  setSettingsOpen(false);
  if (fullScreen) {
    topbarPinned = false;
    scheduleTopbarHide(900);
  } else {
    topbarPinned = true;
    setTopbarVisible(true);
  }
});

wireWindowControls();
wireRemoteUi();

window.lanRemote.getAppInfo().then((info) => {
  document.body.dataset.platform = info.device.platform;
}).catch(() => {});

window.lanRemote.getRemoteConfig().then((remoteConfig) => {
  config = remoteConfig;
  if (!config) throw new Error('Missing remote config');
  remoteNameEl.textContent = config.name;
  document.title = `${config.name} - P2P Remote LAN`;
  return connect();
}).catch((err) => {
  setStatus('warn', '连接失败');
  overlay.textContent = err.message;
  log(`init failed: ${err.stack || err.message}`);
});
