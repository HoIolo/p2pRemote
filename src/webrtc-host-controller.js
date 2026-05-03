(function attachWebRtcHostController(global) {
  function noop() {}

  function createWebRtcHostController(options = {}) {
    const {
      log = noop,
      setStatus = noop,
      updatePeerState = noop,
      setCaptureActive = noop,
      onCaptureStarted = noop,
      onCaptureEnded = noop,
    } = options;

    let localStream = null;
    let capturePromise = null;
    let disposed = false;
    const peers = new Map();
    const pendingPeerCandidates = new Map();

    function writeLog(message) {
      if (!disposed) log(message);
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

    function sendSignal(clientId, message) {
      if (disposed) return;
      global.lanRemote.sendSignal({ clientId, message });
    }

    function sendSignalProgress(clientId, stage, detail = '') {
      sendSignal(clientId, {
        type: 'progress',
        stage,
        detail,
        ts: Date.now(),
      });
    }

    function parseCandidate(candidateLike) {
      const raw = candidateLike?.candidate || '';
      const match = raw.match(/^candidate:\S+\s+\d+\s+(\w+)\s+\d+\s+([0-9a-fA-F\.:]+)\s+(\d+)\s+typ\s+(\w+)/i);
      if (!match) return null;
      return {
        protocol: String(match[1] || '').toLowerCase(),
        address: match[2] || '',
        port: Number(match[3] || 0),
        type: String(match[4] || '').toLowerCase(),
      };
    }

    function describeCandidate(candidateLike) {
      const parsed = parseCandidate(candidateLike);
      if (!parsed) return candidateLike?.candidate || 'unknown-candidate';
      return `${parsed.type}/${parsed.protocol} ${parsed.address}:${parsed.port}`;
    }

    function shouldUseCandidate(candidateLike) {
      const parsed = parseCandidate(candidateLike);
      if (!parsed) return false;
      if (parsed.protocol !== 'udp' || parsed.type !== 'host') return false;
      if (!/^\d+\.\d+\.\d+\.\d+$/.test(parsed.address)) return false;
      if (parsed.address.startsWith('127.')) return false;
      return true;
    }

    async function tuneCaptureTrack(track) {
      try {
        const maxFps = Number(localStorage.getItem('maxFps') || 60);
        await track.applyConstraints({ frameRate: { ideal: maxFps } });
      } catch (err) {
        writeLog(`capture constraints skipped: ${err.message}`);
      }

      const settings = track.getSettings?.();
      if (settings) {
        const size = settings.width && settings.height ? `${settings.width}x${settings.height}` : 'unknown size';
        const fps = settings.frameRate ? `@${Math.round(settings.frameRate)}fps` : '';
        writeLog(`capture settings: ${size}${fps}`);
      }
    }

    function cleanupTrack(track) {
      try {
        track?._p2pCleanup?.();
        track?.stop?.();
      } catch {}
    }

    function closePeer(clientId) {
      const pc = peers.get(clientId);
      if (pc) {
        try {
          pc.close();
        } catch {}
      }
      peers.delete(clientId);
      pendingPeerCandidates.delete(clientId);
    }

    function closeAllPeers() {
      for (const [clientId] of peers) closePeer(clientId);
      updatePeerState(null, '未连接');
    }

    async function startCapture() {
      if (localStream) return localStream;
      if (capturePromise) return capturePromise;

      capturePromise = (async () => {
        setStatus('warn', '等待系统屏幕授权');
        const stream = await withTimeout(
          navigator.mediaDevices.getDisplayMedia({ audio: false, video: true }),
          8000,
          'Mac screen capture did not start within 8s; check Screen Recording permission and restart the Mac app',
        );

        const tracks = stream.getVideoTracks();
        if (tracks.length === 0) throw new Error('No screen video track returned');

        localStream = stream;
        for (const track of tracks) {
          track.contentHint = 'motion';
          await tuneCaptureTrack(track);
          await waitForLiveTrack(track);
          const settings = track.getSettings?.() || {};
          writeLog(`screen track ready: ${settings.width || '?'}x${settings.height || '?'} muted=${track.muted} state=${track.readyState}`);
          track.addEventListener('mute', () => writeLog('screen track muted'));
          track.addEventListener('unmute', () => writeLog('screen track unmuted'));
          track.addEventListener('ended', () => {
            writeLog('screen capture stopped');
            setStatus('warn', '共享已停止');
            localStream = null;
            capturePromise = null;
            setCaptureActive(false);
            closeAllPeers();
            onCaptureEnded();
          });
        }

        setCaptureActive(true);
        setStatus('ok', '正在共享屏幕');
        writeLog('screen capture started');
        onCaptureStarted(stream);
        return localStream;
      })();

      try {
        return await capturePromise;
      } catch (err) {
        capturePromise = null;
        localStream = null;
        setCaptureActive(false);
        throw err;
      }
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
        writeLog(`codec preference skipped: ${err.message}`);
      }
    }

    async function tuneSender(sender) {
      try {
        const params = sender.getParameters();
        if (!Array.isArray(params.encodings) || params.encodings.length === 0) params.encodings = [{}];
        params.encodings[0].maxBitrate = Number(localStorage.getItem('maxBitrate') || 35_000_000);
        params.encodings[0].maxFramerate = Number(localStorage.getItem('maxFps') || 60);
        if ('degradationPreference' in params) params.degradationPreference = 'maintain-framerate';
        await sender.setParameters(params);
      } catch (err) {
        writeLog(`sender tuning skipped: ${err.message}`);
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
        if (!event.candidate) return;
        if (!shouldUseCandidate(event.candidate)) {
          writeLog(`skip local ICE candidate ${describeCandidate(event.candidate)}`);
          return;
        }
        writeLog(`send local ICE candidate ${describeCandidate(event.candidate)}`);
        sendSignal(clientId, { type: 'candidate', candidate: event.candidate });
      };

      pc.onconnectionstatechange = () => {
        updatePeerState(clientId, pc.connectionState);
        writeLog(`peer ${clientId.slice(0, 8)} state=${pc.connectionState}`);
      };

      pc.oniceconnectionstatechange = () => {
        writeLog(`peer ${clientId.slice(0, 8)} ice=${pc.iceConnectionState}`);
      };
      pc.onnegotiationneeded = () => writeLog(`peer ${clientId.slice(0, 8)} negotiationneeded`);

      pc.ondatachannel = (event) => {
        const channel = event.channel;
        channel.binaryType = 'arraybuffer';
        writeLog(`data channel opened: ${channel.label}`);
        channel.onmessage = (msg) => {
          try {
            global.lanRemote.sendInput(JSON.parse(msg.data));
          } catch (err) {
            writeLog(`bad input packet: ${err.message}`);
          }
        };
      };

      peers.set(clientId, pc);
      return pc;
    }

    async function addPeerCandidate(clientId, pc, candidate) {
      if (!candidate) return;
      if (!shouldUseCandidate(candidate)) {
        writeLog(`ignored remote ICE candidate from ${clientId.slice(0, 8)}: ${describeCandidate(candidate)}`);
        return;
      }
      if (!pc || !pc.remoteDescription) {
        const pending = pendingPeerCandidates.get(clientId) || [];
        pending.push(candidate);
        pendingPeerCandidates.set(clientId, pending);
        writeLog(`queued ICE candidate from ${clientId.slice(0, 8)} (${pending.length}) until peer/offer is ready: ${describeCandidate(candidate)}`);
        return;
      }
      try {
        await pc.addIceCandidate(candidate);
        writeLog(`ICE candidate applied from ${clientId.slice(0, 8)}: ${describeCandidate(candidate)}`);
      } catch (err) {
        writeLog(`addIceCandidate failed from ${clientId.slice(0, 8)}: ${err.message}`);
      }
    }

    async function flushPeerCandidates(clientId, pc) {
      if (!pc?.remoteDescription) return;
      const pending = pendingPeerCandidates.get(clientId) || [];
      if (!pending.length) return;
      pendingPeerCandidates.delete(clientId);
      writeLog(`applying ${pending.length} queued ICE candidates from ${clientId.slice(0, 8)}`);
      for (const candidate of pending) {
        await addPeerCandidate(clientId, pc, candidate);
      }
    }

    function ensureVideoSender(pc, stream = null) {
      let sender = pc.getSenders().find((item) => item.track?.kind === 'video') || null;
      const track = stream?.getVideoTracks?.()[0] || null;
      if (!sender && track) {
        sender = pc.addTrack(track, stream);
      }
      if (!sender) {
        const transceiver = pc.addTransceiver('video', { direction: 'sendonly' });
        sender = transceiver.sender;
      }
      const transceiver = pc.getTransceivers().find((item) => item.sender === sender);
      try {
        if (transceiver) transceiver.direction = 'sendonly';
      } catch (err) {
        writeLog(`set video transceiver direction skipped: ${err.message}`);
      }
      return sender;
    }

    function createPlaceholderStream() {
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
        ctx.fillText('Mac is preparing screen...', canvas.width / 2, canvas.height / 2 + 32);
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
      return { stream, track };
    }

    async function attachScreenStream(stream, videoSender) {
      for (const track of stream.getTracks()) {
        if (track.kind === 'video' && videoSender) {
          const previousTrack = videoSender.track;
          track.contentHint = 'motion';
          try {
            videoSender.setStreams?.(stream);
          } catch {}
          await videoSender.replaceTrack(track);
          if (previousTrack && previousTrack !== track) cleanupTrack(previousTrack);
          await tuneSender(videoSender);
          continue;
        }
      }
    }

    async function handleSignal({ clientId, message }) {
      if (disposed || !message?.type) return;

      let pc = peers.get(clientId);
      if (message.type === 'offer') {
        try {
          writeLog(`offer received from ${clientId.slice(0, 8)}; preparing screen stream`);
          sendSignalProgress(clientId, 'offer-received', 'Mac received offer, preparing WebRTC answer');

          if (pc) closePeer(clientId);
          pc = makePeer(clientId);

          await pc.setRemoteDescription(message.sdp);
          await flushPeerCandidates(clientId, pc);
          const placeholder = createPlaceholderStream();
          const videoSender = ensureVideoSender(pc, placeholder.stream);
          if (placeholder.track && videoSender.track !== placeholder.track) {
            await videoSender.replaceTrack(placeholder.track);
          }
          preferH264(pc, videoSender);

          const answer = await pc.createAnswer();
          await pc.setLocalDescription(answer);
          sendSignal(clientId, { type: 'answer', sdp: pc.localDescription });
          sendSignalProgress(clientId, 'answer-sent', 'Mac sent WebRTC answer');
          writeLog(`answered offer from ${clientId.slice(0, 8)}`);

          sendSignalProgress(clientId, 'capture-starting', 'Mac is starting screen capture');
          const stream = await startCapture();
          sendSignalProgress(clientId, 'capture-ready', 'Mac screen capture is ready');
          await attachScreenStream(stream, videoSender);
          writeLog(`screen stream attached for ${clientId.slice(0, 8)}`);
        } catch (err) {
          const messageText = err?.message || String(err);
          writeLog(`screen share failed for ${clientId.slice(0, 8)}: ${messageText}`);
          sendSignal(clientId, { type: 'error', error: messageText });
        }
        return;
      }

      if (message.type === 'candidate' && message.candidate) {
        await addPeerCandidate(clientId, pc, message.candidate);
      }
    }

    function disconnectClient(clientId) {
      closePeer(clientId);
      updatePeerState(null, '未连接');
    }

    function dispose() {
      disposed = true;
      closeAllPeers();
      try {
        localStream?.getTracks?.().forEach((track) => track.stop());
      } catch {}
      localStream = null;
      capturePromise = null;
      setCaptureActive(false);
    }

    return {
      startCapture,
      handleSignal,
      disconnectClient,
      dispose,
    };
  }

  global.createWebRtcHostController = createWebRtcHostController;
}(window));
