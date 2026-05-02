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

let localStream = null;
const peers = new Map();

function log(message) {
  const line = `[${new Date().toLocaleTimeString()}] ${message}`;
  logEl.textContent = `${line}\n${logEl.textContent}`;
}

function setStatus(kind, text) {
  statusDot.className = `dot ${kind || ''}`;
  statusText.textContent = text;
}

async function initInfo() {
  const info = await window.lanRemote.getHostInfo();
  pinEl.textContent = info.pin;
  portEl.textContent = String(info.port);
  addressesEl.innerHTML = info.addresses.length
    ? info.addresses.map((a) => `<span class="code">${a.address}</span> <span class="small">${a.name}</span>`).join('<br />')
    : '<span class="small">未找到非内网 IPv4 地址</span>';
  displayEl.textContent = `${info.display.x},${info.display.y} ${info.display.width}x${info.display.height} @${info.scaleFactor}x`;
  log(`host ready; port=${info.port}; pin=${info.pin}`);
}

async function startCapture() {
  if (localStream) return localStream;

  setStatus('warn', '等待系统屏幕授权');
  localStream = await navigator.mediaDevices.getDisplayMedia({
    audio: false,
    video: {
      cursor: 'always',
      displaySurface: 'monitor',
      frameRate: { ideal: 60, max: 120 },
      width: { ideal: 2560 },
      height: { ideal: 1440 },
    },
  });

  for (const track of localStream.getVideoTracks()) {
    track.contentHint = 'motion';
    track.addEventListener('ended', () => {
      log('screen capture stopped');
      setStatus('warn', '共享已停止');
      localStream = null;
      for (const peer of peers.values()) peer.close();
      peers.clear();
    });
  }

  startShareBtn.disabled = true;
  setStatus('ok', '正在共享屏幕');
  log('screen capture started');
  return localStream;
}

function preferH264(pc, sender) {
  try {
    const caps = RTCRtpSender.getCapabilities('video');
    const transceiver = pc.getTransceivers().find((t) => t.sender === sender);
    if (!caps?.codecs || !transceiver?.setCodecPreferences) return;
    const h264 = caps.codecs.filter((c) => c.mimeType.toLowerCase() === 'video/h264');
    const rest = caps.codecs.filter((c) => c.mimeType.toLowerCase() !== 'video/h264');
    if (h264.length) transceiver.setCodecPreferences([...h264, ...rest]);
  } catch (err) {
    log(`codec preference skipped: ${err.message}`);
  }
}

async function tuneSender(sender) {
  try {
    const params = sender.getParameters();
    if (!params.encodings || params.encodings.length === 0) params.encodings = [{}];
    params.encodings[0].maxBitrate = Number(localStorage.getItem('maxBitrate') || 35_000_000);
    params.encodings[0].maxFramerate = Number(localStorage.getItem('maxFps') || 60);
    params.encodings[0].networkPriority = 'high';
    params.degradationPreference = 'maintain-framerate';
    await sender.setParameters(params);
  } catch (err) {
    log(`sender tuning skipped: ${err.message}`);
  }
}

function makePeer(clientId) {
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

  pc.onconnectionstatechange = () => {
    peerStateEl.textContent = `${clientId.slice(0, 8)}: ${pc.connectionState}`;
    log(`peer ${clientId.slice(0, 8)} state=${pc.connectionState}`);
  };

  pc.oniceconnectionstatechange = () => {
    log(`peer ${clientId.slice(0, 8)} ice=${pc.iceConnectionState}`);
  };

  pc.ondatachannel = (event) => {
    const channel = event.channel;
    channel.binaryType = 'arraybuffer';
    log(`data channel opened: ${channel.label}`);
    channel.onmessage = (msg) => {
      try {
        const input = JSON.parse(msg.data);
        window.lanRemote.sendInput(input);
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
    if (!localStream) {
      log('client offered before screen sharing; click “开始共享屏幕” first');
      window.lanRemote.sendSignal({ clientId, message: { type: 'error', error: 'host screen is not shared yet' } });
      return;
    }

    if (pc) pc.close();
    pc = makePeer(clientId);

    for (const track of localStream.getTracks()) {
      const sender = pc.addTrack(track, localStream);
      if (track.kind === 'video') {
        preferH264(pc, sender);
        await tuneSender(sender);
      }
    }

    await pc.setRemoteDescription(message.sdp);
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    window.lanRemote.sendSignal({ clientId, message: { type: 'answer', sdp: pc.localDescription } });
    log(`answered offer from ${clientId.slice(0, 8)}`);
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

startShareBtn.addEventListener('click', async () => {
  try {
    await startCapture();
  } catch (err) {
    setStatus('warn', '共享失败');
    log(`screen capture failed: ${err.message}`);
  }
});

window.lanRemote.onSignalMessage((payload) => {
  handleSignal(payload).catch((err) => log(`signal error: ${err.stack || err.message}`));
});
window.lanRemote.onClientConnected(({ clientId, remoteAddress }) => log(`client paired: ${clientId.slice(0, 8)} from ${remoteAddress}`));
window.lanRemote.onClientDisconnected(({ clientId }) => {
  const pc = peers.get(clientId);
  if (pc) pc.close();
  peers.delete(clientId);
  peerStateEl.textContent = '未连接';
  log(`client disconnected: ${clientId.slice(0, 8)}`);
});
window.lanRemote.onHostLog((entry) => log(`${entry.level || 'info'}: ${entry.message}`));

initInfo().catch((err) => log(`init failed: ${err.message}`));
