const $ = (id) => document.getElementById(id);

const remoteNameEl = $('remoteName');
const statusDot = $('statusDot');
const statusText = $('statusText');
const reconnectBtn = $('reconnect');
const fullscreenBtn = $('fullscreen');
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

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function setStatus(kind, text) {
  statusDot.className = `dot ${kind || ''}`;
  statusText.textContent = text;
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
  if (ws?.readyState === WebSocket.OPEN) ws.send(JSON.stringify(message));
}

function sendInput(event) {
  if (inputChannel?.readyState !== 'open') return;
  if (inputChannel.bufferedAmount > 8_192) return;
  inputChannel.send(JSON.stringify({ ...event, t: performance.now() }));
}

function makePeer() {
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
    const stream = event.streams[0] || new MediaStream([event.track]);
    video.srcObject = stream;
    overlay.style.display = 'none';
    video.play().catch((err) => log(`video play skipped: ${err.message}`));
    log(`remote track: ${event.track.kind}`);
    event.track.onunmute = () => log('remote video unmuted');
  };

  pc.onicecandidate = (event) => {
    if (event.candidate) sendSignal({ type: 'candidate', candidate: event.candidate });
  };
  pc.onconnectionstatechange = () => {
    log(`peer state=${pc.connectionState}`);
    if (pc.connectionState === 'connected') setStatus('ok', '已连接');
    if (['failed', 'closed', 'disconnected'].includes(pc.connectionState)) setStatus('warn', pc.connectionState);
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
  const offer = await pc.createOffer();
  await pc.setLocalDescription(offer);
  sendSignal({ type: 'offer', sdp: pc.localDescription });
  log('offer sent');
}

async function connect() {
  if (!config) return;
  if (ws) ws.close();
  if (pc) pc.close();
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
    const message = JSON.parse(event.data);
    if (message.type === 'hello-ok') {
      setStatus('warn', '协商媒体');
      log(`paired as ${message.clientId.slice(0, 8)}`);
      await createOffer();
      return;
    }
    if (message.type === 'answer') {
      await pc.setRemoteDescription(message.sdp);
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
      overlay.textContent = message.error || '远程端共享失败';
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
video.addEventListener('loadeddata', saveFirstFrame);
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

reconnectBtn.addEventListener('click', () => connect().catch((err) => log(`connect failed: ${err.stack || err.message}`)));
fullscreenBtn.addEventListener('click', async () => {
  fullScreen = !fullScreen;
  await window.lanRemote.setWindowFullscreen(fullScreen);
  fullscreenBtn.textContent = fullScreen ? '退出全屏' : '全屏';
});

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
