const $ = (id) => document.getElementById(id);

const hostInput = $('host');
const portInput = $('port');
const pinInput = $('pin');
const connectBtn = $('connect');
const video = $('remoteVideo');
const overlay = $('overlay');
const statusDot = $('statusDot');
const statusText = $('statusText');
const logEl = $('log');

let ws = null;
let pc = null;
let inputChannel = null;
let pressedKeys = new Set();
let pointerDown = false;
let pendingMove = null;
let moveRaf = 0;

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function setStatus(kind, text) {
  statusDot.className = `dot ${kind || ''}`;
  statusText.textContent = text;
}

function endpoint() {
  const rawHost = hostInput.value.trim().replace(/^wss?:\/\//, '').replace(/\/$/, '');
  const port = portInput.value.trim() || '7777';
  if (!rawHost) throw new Error('请输入 Mac 主机 IP');
  if (rawHost.startsWith('[')) return rawHost.includes(']:') ? `ws://${rawHost}` : `ws://${rawHost}:${port}`;
  if (rawHost.includes(':') && rawHost.includes('.')) return `ws://${rawHost}`; // IPv4:port
  if (rawHost.includes(':')) return `ws://[${rawHost}]:${port}`; // IPv6 literal
  return `ws://${rawHost}:${port}`;
}

function sendSignal(message) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify(message));
}

function sendInput(event) {
  if (!inputChannel || inputChannel.readyState !== 'open') return;
  // Drop input if the SCTP buffer is backed up; fresh input is better than late input.
  if (inputChannel.bufferedAmount > 8_192) return;
  inputChannel.send(JSON.stringify({ ...event, t: performance.now() }));
}

function makePeer() {
  pc = new RTCPeerConnection({
    iceServers: [
      { urls: 'stun:stun.l.google.com:19302' },
      { urls: 'stun:stun.cloudflare.com:3478' },
    ],
    bundlePolicy: 'max-bundle',
    rtcpMuxPolicy: 'require',
    iceCandidatePoolSize: 0,
  });
  pc.addTransceiver('video', { direction: 'recvonly' });

  inputChannel = pc.createDataChannel('input', {
    ordered: false,
    maxRetransmits: 0,
    negotiated: false,
  });
  inputChannel.bufferedAmountLowThreshold = 4_096;
  inputChannel.onopen = () => log('input data channel open');
  inputChannel.onclose = () => log('input data channel closed');

  pc.ontrack = (event) => {
    video.srcObject = event.streams[0] || new MediaStream([event.track]);
    overlay.style.display = 'none';
    video.play().catch(() => {});
    log(`remote track: ${event.track.kind}`);
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

async function createOffer() {
  const offer = await pc.createOffer({ offerToReceiveVideo: true, offerToReceiveAudio: false });
  await pc.setLocalDescription(offer);
  sendSignal({ type: 'offer', sdp: pc.localDescription });
  log('offer sent');
}

async function connect() {
  if (ws) ws.close();
  if (pc) pc.close();
  pressedKeys.clear();
  overlay.style.display = 'grid';
  overlay.innerHTML = '等待视频流<br /><span class="small">连接后请点击画面使键盘输入聚焦</span>';

  const pin = pinInput.value.trim();
  if (!/^\d{4,8}$/.test(pin)) throw new Error('请输入正确 PIN');

  makePeer();
  const url = endpoint();
  setStatus('warn', '连接信令中');
  log(`connecting ${url}`);

  ws = new WebSocket(url);
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: 'hello', pin }));
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
      setStatus('warn', '已配对，协商媒体');
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
      setStatus('warn', message.error || 'host error');
      log(`host error: ${message.error}`);
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

connectBtn.addEventListener('click', () => connect().catch((err) => {
  setStatus('warn', '连接失败');
  log(`connect failed: ${err.stack || err.message}`);
}));

for (const input of [hostInput, portInput, pinInput]) {
  input.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') connectBtn.click();
  });
}

log('client ready');
