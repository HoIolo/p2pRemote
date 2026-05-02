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
    const stream = await navigator.mediaDevices.getDisplayMedia({
      audio: false,
      video: true,
    });

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
    try {
      log(`offer received from ${clientId.slice(0, 8)}; preparing screen stream`);
      const stream = await startCapture();

      if (pc) pc.close();
      pc = makePeer(clientId);

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
      log(`answered offer from ${clientId.slice(0, 8)}`);
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
