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

let localStream = null;
let capturePromise = null;
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
    : '<span class="small">\u672a\u627e\u5230\u975e\u5185\u7f51 IPv4 \u5730\u5740</span>';
  displayEl.textContent = `${info.display.x},${info.display.y} ${info.display.width}x${info.display.height} @${info.scaleFactor}x`;
  log(`host ready; port=${info.port}; pin=${info.pin}`);

  if (info.platform === 'darwin') {
    const status = await window.lanRemote.getScreenCaptureStatus();
    log(`macOS screen permission=${status}`);
  }
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
    const maxFps = Number(localStorage.getItem('maxFps') || 60);
    await track.applyConstraints({ frameRate: { ideal: maxFps } });
  } catch (err) {
    log(`capture constraints skipped: ${err.message}`);
  }

  const settings = track.getSettings?.();
  if (settings) {
    const size = settings.width && settings.height ? `${settings.width}x${settings.height}` : 'unknown size';
    const fps = settings.frameRate ? `@${Math.round(settings.frameRate)}fps` : '';
    log(`capture settings: ${size}${fps}`);
  }
}

async function startCapture() {
  if (localStream) return localStream;
  if (capturePromise) return capturePromise;

  capturePromise = (async () => {
    setStatus('warn', '\u7b49\u5f85\u7cfb\u7edf\u5c4f\u5e55\u6388\u6743');
    const stream = await withTimeout(
      navigator.mediaDevices.getDisplayMedia({
        audio: false,
        video: true,
      }),
      8000,
      'Mac screen capture did not start within 8s; check Screen Recording permission and restart the Mac app',
    );

    const videoTracks = stream.getVideoTracks();
    if (videoTracks.length === 0) throw new Error('No screen video track returned');

    localStream = stream;
    for (const track of videoTracks) {
      track.contentHint = 'motion';
      await tuneCaptureTrack(track);
      await waitForLiveTrack(track);
      const settings = track.getSettings?.() || {};
      log(`screen track ready: ${settings.width || '?'}x${settings.height || '?'} muted=${track.muted} state=${track.readyState}`);
      track.addEventListener('mute', () => log('screen track muted'));
      track.addEventListener('unmute', () => log('screen track unmuted'));
      track.addEventListener('ended', () => {
        log('screen capture stopped');
        setStatus('warn', '\u5171\u4eab\u5df2\u505c\u6b62');
        localStream = null;
        capturePromise = null;
        startShareBtn.disabled = false;
        for (const peer of peers.values()) peer.close();
        peers.clear();
      });
    }

    startShareBtn.disabled = true;
    setStatus('ok', '\u6b63\u5728\u5171\u4eab\u5c4f\u5e55');
    log('screen capture started');
    return localStream;
  })();

  try {
    return await capturePromise;
  } catch (err) {
    capturePromise = null;
    localStream = null;
    startShareBtn.disabled = false;
    throw err;
  }
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
  pc.onnegotiationneeded = () => log(`peer ${clientId.slice(0, 8)} negotiationneeded`);

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

function createPlaceholderTrack() {
  const canvas = document.createElement('canvas');
  canvas.width = 1280;
  canvas.height = 720;
  const ctx = canvas.getContext('2d');
  const draw = () => {
    ctx.fillStyle = '#0f172a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#93c5fd';
    ctx.font = '600 42px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('P2P Remote LAN', canvas.width / 2, canvas.height / 2 - 20);
    ctx.fillStyle = '#e5e7eb';
    ctx.font = '28px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif';
    ctx.fillText('Mac 正在准备屏幕画面...', canvas.width / 2, canvas.height / 2 + 32);
  };
  draw();
  const timer = setInterval(draw, 1000);
  const stream = canvas.captureStream?.(2);
  const track = stream?.getVideoTracks?.()[0] || null;
  if (track) {
    track.contentHint = 'detail';
    track._p2pCleanup = () => clearInterval(timer);
  } else {
    clearInterval(timer);
  }
  return track;
}

function cleanupTrack(track) {
  try {
    track?._p2pCleanup?.();
    track?.stop?.();
  } catch {}
}

async function attachScreenStream(pc, stream, videoTransceiver) {
  for (const track of stream.getTracks()) {
    if (track.kind === 'video' && videoTransceiver?.sender) {
      const previousTrack = videoTransceiver.sender.track;
      await videoTransceiver.sender.replaceTrack(track);
      if (previousTrack && previousTrack !== track) cleanupTrack(previousTrack);
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
      log(`offer received from ${clientId.slice(0, 8)}; preparing screen stream`);
      sendSignalProgress(clientId, 'offer-received', 'Mac received offer, preparing WebRTC answer');

      if (pc) pc.close();
      pc = makePeer(clientId);

      await pc.setRemoteDescription(message.sdp);
      const videoTransceiver = ensureVideoSender(pc);
      const placeholderTrack = createPlaceholderTrack();
      if (placeholderTrack) await videoTransceiver.sender.replaceTrack(placeholderTrack);
      preferH264(pc, videoTransceiver.sender);
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      window.lanRemote.sendSignal({ clientId, message: { type: 'answer', sdp: pc.localDescription } });
      sendSignalProgress(clientId, 'answer-sent', 'Mac sent WebRTC answer');
      log(`answered offer from ${clientId.slice(0, 8)}`);

      sendSignalProgress(clientId, 'capture-starting', 'Mac is starting screen capture');
      const stream = await startCapture();
      sendSignalProgress(clientId, 'capture-ready', 'Mac screen capture is ready');
      await attachScreenStream(pc, stream, videoTransceiver);
      log(`screen stream attached for ${clientId.slice(0, 8)}`);
    } catch (err) {
      const messageText = err?.message || String(err);
      log(`screen share failed for ${clientId.slice(0, 8)}: ${messageText}`);
      window.lanRemote.sendSignal({ clientId, message: { type: 'error', error: messageText } });
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

startShareBtn.addEventListener('click', async () => {
  try {
    await startCapture();
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

window.lanRemote.hostRendererReady?.();
initInfo().catch((err) => log(`init failed: ${err.message}`));
