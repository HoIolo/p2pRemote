const {
  app,
  BrowserWindow,
  ipcMain,
  screen,
  dialog,
  systemPreferences,
  shell,
} = require('electron');
const crypto = require('crypto');
const dgram = require('dgram');
const fs = require('fs');
const os = require('os');
const path = require('path');
const WebSocket = require('ws');
const { execFile, spawn } = require('child_process');

// Native v2 owns the media/input hot path. Keep Chromium timers responsive for the dashboard.
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');

function resolveRole() {
  if (process.argv.includes('--host')) return 'host';
  // Legacy flag: the old standalone client UI has been removed.
  // Keep --client working, but route it to the new dashboard shell.
  if (process.argv.includes('--client')) return 'dashboard';
  if (process.env.P2P_REMOTE_ROLE === 'host') return 'host';
  if (process.env.P2P_REMOTE_ROLE === 'client') return 'dashboard';

  return 'dashboard';
}

const ROLE = resolveRole();
const SIGNAL_PORT = Number(process.env.P2P_REMOTE_PORT || 7777);
const DISCOVERY_PORT = Number(process.env.P2P_REMOTE_DISCOVERY_PORT || 47777);
const PIN = String(crypto.randomInt(100000, 999999));
const BUNDLE_ID = 'com.p2premotelan.app';

let win = null;
let wss = null;
let discoverySocket = null;
let discoveryTimer = null;
let pruneTimer = null;
let deviceId = null;
const clients = new Map();
const devices = new Map();
const devicePreviews = new Map();
let nativeV2ClientProcess = null;
let nativeV2HostProcess = null;
let nativeV2ClientLastOptions = null;
let nativeV2LastRemoteHostRequest = null;
let appIsQuitting = false;
let nativeV2HostAutoStopTimer = null;

function mainWindow() {
  if (win && !win.isDestroyed()) return win;
  const focused = BrowserWindow.getFocusedWindow();
  if (focused && !focused.isDestroyed()) return focused;
  return BrowserWindow.getAllWindows().find((browserWindow) => !browserWindow.isDestroyed()) || null;
}

function sendToWindow(browserWindow, channel, payload) {
  try {
    if (!browserWindow || browserWindow.isDestroyed()) return false;
    const contents = browserWindow.webContents;
    if (!contents || contents.isDestroyed()) return false;
    contents.send(channel, payload);
    return true;
  } catch {
    return false;
  }
}

function sendToMainWindow(channel, payload) {
  return sendToWindow(mainWindow(), channel, payload);
}


const gotSingleInstanceLock = app.requestSingleInstanceLock();
if (!gotSingleInstanceLock) app.quit();
else {
  app.on('second-instance', () => {
    if (!win || win.isDestroyed()) return;
    if (win.isMinimized()) win.restore();
    win.focus();
  });
}

function lanAddresses() {
  const out = [];
  for (const [name, addrs] of Object.entries(os.networkInterfaces())) {
    for (const addr of addrs || []) {
      if (addr.family === 'IPv4' && !addr.internal) out.push({ name, address: addr.address, netmask: addr.netmask || '' });
    }
  }
  return out;
}

function ipv4ToInt(address) {
  const parts = String(address || '').split('.').map((part) => Number(part));
  if (parts.length !== 4 || parts.some((n) => !Number.isInteger(n) || n < 0 || n > 255)) return null;
  return (((parts[0] << 24) >>> 0) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) >>> 0;
}

function sameSubnet(address, target, netmask) {
  const ip = ipv4ToInt(address);
  const dst = ipv4ToInt(target);
  const mask = ipv4ToInt(netmask);
  if (ip === null || dst === null || mask === null) return false;
  return (ip & mask) === (dst & mask);
}

function routeLocalAddressForTarget(targetIp, fallbackIp = '') {
  const target = String(targetIp || '').trim();
  const fallback = String(fallbackIp || '').trim();
  const addrs = lanAddresses();
  const subnetMatch = addrs.find((addr) => addr.netmask && sameSubnet(addr.address, target, addr.netmask));
  if (subnetMatch) return { ...subnetMatch, reason: 'same-subnet' };
  if (fallback && addrs.some((addr) => addr.address === fallback)) {
    const item = addrs.find((addr) => addr.address === fallback);
    return { ...item, reason: 'provided' };
  }
  return addrs[0] ? { ...addrs[0], reason: 'first-interface' } : null;
}

function subnetBroadcast(address, netmask) {
  const ip = address.split('.').map((part) => Number(part));
  const mask = netmask.split('.').map((part) => Number(part));
  if (ip.length !== 4 || mask.length !== 4 || ip.some((n) => !Number.isInteger(n)) || mask.some((n) => !Number.isInteger(n))) {
    return null;
  }
  return ip.map((part, index) => (part | (~mask[index] & 255)) & 255).join('.');
}

function broadcastAddresses() {
  const out = new Set(['255.255.255.255']);
  for (const addrs of Object.values(os.networkInterfaces())) {
    for (const addr of addrs || []) {
      if (addr.family !== 'IPv4' || addr.internal || !addr.netmask) continue;
      const broadcast = subnetBroadcast(addr.address, addr.netmask);
      if (broadcast) out.add(broadcast);
    }
  }
  return [...out];
}

function ensureDeviceId() {
  if (deviceId) return deviceId;
  const file = path.join(app.getPath('userData'), 'device-id');
  try {
    deviceId = fs.readFileSync(file, 'utf8').trim();
  } catch {
    deviceId = crypto.randomUUID();
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, deviceId);
  }
  return deviceId;
}

function deviceName() {
  return os.hostname() || (process.platform === 'darwin' ? 'Mac' : process.platform === 'win32' ? 'Windows PC' : 'Remote Device');
}

function localDevicePayload() {
  const primary = screen.getPrimaryDisplay();
  return {
    type: 'p2p-remote-lan:announce',
    version: 1,
    id: ensureDeviceId(),
    name: deviceName(),
    platform: process.platform,
    port: SIGNAL_PORT,
    pin: PIN,
    addresses: lanAddresses().map((item) => item.address),
    display: primary?.bounds ? {
      x: primary.bounds.x,
      y: primary.bounds.y,
      width: primary.bounds.width,
      height: primary.bounds.height,
    } : null,
    scaleFactor: primary?.scaleFactor || 1,
    ts: Date.now(),
  };
}

function serializeDevice(device) {
  return {
    id: device.id,
    name: device.name || 'Unknown Device',
    platform: device.platform || 'unknown',
    address: device.address,
    port: Number(device.port || SIGNAL_PORT),
    pin: String(device.pin || ''),
    addresses: device.addresses || [],
    display: device.display || null,
    scaleFactor: Number(device.scaleFactor || 1),
    lastSeen: device.lastSeen,
    preview: devicePreviews.get(device.id) || null,
  };
}

function sendDeviceList() {
  const now = Date.now();
  const list = [...devices.values()]
    .filter((device) => now - device.lastSeen < 15_000)
    .map(serializeDevice)
    .sort((a, b) => a.name.localeCompare(b.name));
  sendToMainWindow('devices-updated', list);
}

function windowChromeOptions(options) {
  if (options.frame === true) return { frame: true };

  if (process.platform === 'darwin') {
    return {
      titleBarStyle: 'hiddenInset',
      trafficLightPosition: options.trafficLightPosition || { x: 16, y: 17 },
    };
  }

  if (process.platform === 'win32') {
    return {
      titleBarStyle: 'hidden',
      titleBarOverlay: {
        color: options.titleBarColor || '#f7f7f5',
        symbolColor: options.titleBarSymbolColor || '#5f6368',
        height: options.titleBarHeight || 66,
      },
    };
  }

  return { frame: false };
}

function createWindow(file, options = {}) {
  const browserWindow = new BrowserWindow({
    width: options.width || 1180,
    height: options.height || 760,
    minWidth: 900,
    minHeight: 560,
    backgroundColor: '#0b1020',
    ...windowChromeOptions(options),
    title: options.title || 'P2P Remote LAN',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
      backgroundThrottling: false,
    },
  });
  browserWindow.loadFile(path.join(__dirname, file));
  return browserWindow;
}


function findFirstExistingPath(candidates) {
  return candidates.find((candidate) => {
    try {
      return fs.existsSync(candidate);
    } catch {
      return false;
    }
  }) || null;
}

function nativeV2WinClientCandidates() {
  return [
    path.join(process.resourcesPath || '', 'native-v2', 'win-client', 'p2p-native-win-client.exe'),
    path.join(__dirname, '..', 'native-v2', 'win-client', 'build', 'Release', 'p2p-native-win-client.exe'),
    path.join(__dirname, '..', 'native-v2', 'win-client', 'build', 'RelWithDebInfo', 'p2p-native-win-client.exe'),
    path.join(__dirname, '..', 'native-v2', 'win-client', 'build', 'Debug', 'p2p-native-win-client.exe'),
  ];
}

function nativeV2MacHostCandidates() {
  return [
    path.join(process.resourcesPath || '', 'native-v2', 'mac-host', 'p2p-native-mac-host'),
    path.join(__dirname, '..', 'native-v2', 'mac-host', '.build', 'release', 'p2p-native-mac-host'),
    path.join(__dirname, '..', 'native-v2', 'mac-host', '.build', 'arm64-apple-macosx', 'release', 'p2p-native-mac-host'),
    path.join(__dirname, '..', 'native-v2', 'mac-host', '.build', 'x86_64-apple-macosx', 'release', 'p2p-native-mac-host'),
  ];
}

function nativeV2GStreamerRootCandidates() {
  if (process.platform !== 'win32') return [];
  const userProfile = process.env.USERPROFILE || os.homedir?.() || '';
  return [
    process.env.GSTREAMER_1_0_ROOT_MSVC_X86_64,
    process.env.GSTREAMER_1_0_ROOT_MINGW_X86_64,
    path.join(userProfile, 'gstreamer-sdk', '1.0', 'msvc_x86_64'),
    'C:\\gstreamer\\1.0\\msvc_x86_64',
    'C:\\gstreamer\\1.0\\mingw_x86_64',
  ].filter(Boolean);
}

function nativeV2GStreamerEnv() {
  const root = findFirstExistingPath(nativeV2GStreamerRootCandidates());
  if (!root) return { env: process.env, root: null };
  const bin = path.join(root, 'bin');
  const libPkgConfig = path.join(root, 'lib', 'pkgconfig');
  return {
    root,
    env: {
      ...process.env,
      GSTREAMER_1_0_ROOT_MSVC_X86_64: root,
      PKG_CONFIG: path.join(bin, 'pkg-config.exe'),
      PKG_CONFIG_PATH: libPkgConfig,
      PATH: `${bin}${path.delimiter}${process.env.PATH || ''}`,
    },
  };
}

function nativeV2ClientProfilePath() {
  return path.join(app.getPath('userData'), 'native-v2-win-client-profile.json');
}

function readNativeV2ClientProfile(profileFile = nativeV2ClientProfilePath()) {
  try {
    const parsed = JSON.parse(fs.readFileSync(profileFile, 'utf8'));
    return {
      width: Number(parsed.width) || undefined,
      height: Number(parsed.height) || undefined,
      fps: Number(parsed.fps) || undefined,
      bitrate: Number(parsed.bitrate) || undefined,
    };
  } catch {
    return {};
  }
}

function writeNativeV2ClientProfile(profileFile = nativeV2ClientProfilePath(), profile = {}) {
  const payload = {
    width: Number(profile.width) || 1920,
    height: Number(profile.height) || 1080,
    fps: Number(profile.fps) || 60,
    bitrate: Number(profile.bitrate) || 14_000_000,
  };
  fs.mkdirSync(path.dirname(profileFile), { recursive: true });
  fs.writeFileSync(profileFile, JSON.stringify(payload, null, 2));
  return payload;
}

function nativeV2StatusPayload() {
  const savedClientProfile = readNativeV2ClientProfile();
  return {
    platform: process.platform,
    winClient: {
      available: Boolean(findFirstExistingPath(nativeV2WinClientCandidates())),
      path: findFirstExistingPath(nativeV2WinClientCandidates()),
      running: Boolean(nativeV2ClientProcess && !nativeV2ClientProcess.killed),
      pid: nativeV2ClientProcess?.pid || null,
      gstreamerRoot: nativeV2GStreamerEnv().root,
    },
    macHost: {
      available: Boolean(findFirstExistingPath(nativeV2MacHostCandidates())),
      path: findFirstExistingPath(nativeV2MacHostCandidates()),
      running: Boolean(nativeV2HostProcess && !nativeV2HostProcess.killed),
      pid: nativeV2HostProcess?.pid || null,
    },
    defaults: {
      videoPort: 45000,
      inputPort: 45001,
      width: Number(savedClientProfile.width) || 1920,
      height: Number(savedClientProfile.height) || 1080,
      fps: Number(savedClientProfile.fps) || 60,
      bitrate: Number(savedClientProfile.bitrate) || 14_000_000,
      keyint: 1,
      transport: 'udp',
    },
  };
}

function broadcastNativeV2Status(extra = {}) {
  sendToMainWindow('native-v2-status', {
    ...nativeV2StatusPayload(),
    ...extra,
  });
}

async function restartNativeV2RemoteHostForClientProfile(profile) {
  const saved = nativeV2LastRemoteHostRequest;
  if (!saved?.device?.address || !saved?.device?.port || !saved?.device?.pin) return false;
  const nextOptions = {
    ...saved.options,
    width: Number(profile.width) || saved.options.width,
    height: Number(profile.height) || saved.options.height,
    fps: Number(profile.fps) || saved.options.fps,
    bitrate: Number(profile.bitrate) || saved.options.bitrate,
    transport: profile.transport === 'udp' ? 'udp' : (saved.options.transport === 'udp' ? 'udp' : 'tcp'),
  };
  sendToMainWindow('host-log', {
    level: 'info',
    message: `native-v2 reconfigure: requesting Mac host restart ${saved.device.address}:${saved.device.port} -> ${nextOptions.width}x${nextOptions.height}@${nextOptions.fps} bitrate=${nextOptions.bitrate}`,
  });
  await requestNativeV2RemoteHost(saved.device, nextOptions);
  return true;
}

function relayNativeV2ProcessOutput(kind, proc, chunk) {
  const text = chunk.toString('utf8');
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    sendToMainWindow('host-log', { level: 'native-v2', message: line });
    if (kind === 'host' && proc === nativeV2HostProcess && line.includes('[tcp] Windows video client connected')) {
      if (nativeV2HostAutoStopTimer) {
        clearTimeout(nativeV2HostAutoStopTimer);
        nativeV2HostAutoStopTimer = null;
      }
    }
    if (kind === 'host' && proc === nativeV2HostProcess && line.includes('[tcp] Windows video client disconnected')) {
      if (nativeV2HostAutoStopTimer) clearTimeout(nativeV2HostAutoStopTimer);
      sendToMainWindow('host-log', { level: 'info', message: 'native-v2 host will auto-stop in 5s unless the Windows client reconnects' });
      nativeV2HostAutoStopTimer = setTimeout(() => {
        nativeV2HostAutoStopTimer = null;
        if (nativeV2HostProcess === proc && !proc.killed) stopNativeV2Process('host');
      }, 5000);
    }
  }
}

function stopNativeV2Process(kind) {
  const proc = kind === 'host' ? nativeV2HostProcess : nativeV2ClientProcess;
  if (!proc || proc.killed) return false;
  if (kind === 'host' && nativeV2HostAutoStopTimer) {
    clearTimeout(nativeV2HostAutoStopTimer);
    nativeV2HostAutoStopTimer = null;
  }
  try {
    proc.kill();
    return true;
  } catch {
    return false;
  }
}

function normalizeNativeV2Number(value, fallback, min, max) {
  const number = Number(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.max(min, Math.min(max, Math.round(number)));
}

function startNativeV2Client(options = {}) {
  if (process.platform !== 'win32') {
    throw new Error('Native v2 Windows client can only run on Windows');
  }
  if (!options.hostIp) {
    throw new Error('Native v2 needs the Mac host IP address');
  }
  if (nativeV2ClientProcess && !nativeV2ClientProcess.killed) {
    stopNativeV2Process('client');
  }

  const exePath = findFirstExistingPath(nativeV2WinClientCandidates());
  if (!exePath) {
    throw new Error('Native v2 Windows client is not built yet. Run npm run v2:win:build first, or use an installer that includes native-v2.');
  }

  const videoPort = normalizeNativeV2Number(options.videoPort, 45000, 1, 65535);
  const inputPort = normalizeNativeV2Number(options.inputPort, 45001, 1, 65535);
  const width = normalizeNativeV2Number(options.width, 1920, 640, 7680);
  const height = normalizeNativeV2Number(options.height, 1080, 360, 4320);
  const fps = normalizeNativeV2Number(options.fps, 60, 30, 240);
  const bitrate = normalizeNativeV2Number(options.bitrate, 14_000_000, 0, 200_000_000);
  const transport = 'udp';
  const profileFile = String(options.profileFile || nativeV2ClientProfilePath());
  writeNativeV2ClientProfile(profileFile, { width, height, fps, bitrate });
  const args = [
    '--host-ip', String(options.hostIp),
    '--host-name', String(options.hostName || 'Remote Device'),
    '--host-platform', String(options.hostPlatform || 'unknown'),
    '--video-port', String(videoPort),
    '--input-port', String(inputPort),
    '--width', String(width),
    '--height', String(height),
    '--fps', String(fps),
    '--bitrate', String(bitrate),
    '--profile-file', profileFile,
  ];
  args.push('--transport', transport);
  if (options.fullscreen !== false) args.push('--fullscreen');

  nativeV2ClientLastOptions = {
    hostIp: String(options.hostIp),
    hostName: String(options.hostName || 'Remote Device'),
    hostPlatform: String(options.hostPlatform || 'unknown'),
    videoPort,
    inputPort,
    width,
    height,
    fps,
    bitrate,
    fullscreen: options.fullscreen !== false,
    transport,
    profileFile,
  };

  const gstEnv = nativeV2GStreamerEnv();
  if (!gstEnv.root) {
    throw new Error('GStreamer runtime not found. Set GSTREAMER_1_0_ROOT_MSVC_X86_64 or install the bundled SDK before starting the Windows client.');
  }

  nativeV2ClientProcess = spawn(exePath, args, {
    cwd: path.dirname(exePath),
    env: gstEnv.env,
    windowsHide: false,
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  const proc = nativeV2ClientProcess;
  const pid = nativeV2ClientProcess.pid;
  sendToMainWindow('host-log', {
    level: 'info',
    message: `native-v2 client started pid=${pid} host=${options.hostIp}:${videoPort} transport=${transport} gstreamer=${gstEnv.root}`,
  });

  proc.stdout?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('client', proc, chunk);
  });
  proc.stderr?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('client', proc, chunk);
  });
  proc.once('exit', (code, signal) => {
    sendToMainWindow('host-log', { level: 'info', message: `native-v2 client exited code=${code ?? ''} signal=${signal ?? ''}` });
    if (nativeV2ClientProcess === proc) nativeV2ClientProcess = null;
    broadcastNativeV2Status();
  });
  proc.once('error', (err) => {
    sendToMainWindow('host-log', { level: 'error', message: `native-v2 client failed: ${err.message}` });
    if (nativeV2ClientProcess === proc) nativeV2ClientProcess = null;
    broadcastNativeV2Status({ error: err.message });
  });

  broadcastNativeV2Status();
  return {
    ok: true,
    pid,
    exePath,
    args,
    hostIp: String(options.hostIp),
    videoPort,
    inputPort,
    width,
    height,
    fps,
    bitrate,
    fullscreen: options.fullscreen !== false,
    transport,
    profileFile,
  };
}

function startNativeV2Host(options = {}) {
  if (process.platform !== 'darwin') {
    throw new Error('Native v2 macOS host can only run on macOS');
  }
  const allowUdpVideo = options.allowUdpVideo !== false && process.env.P2P_NATIVE_V2_DISABLE_UDP !== '1';
  if (!options.clientIp) {
    throw new Error('Native v2 host needs the Windows client IP address');
  }
  if (nativeV2HostProcess && !nativeV2HostProcess.killed) {
    stopNativeV2Process('host');
  }

  const exePath = findFirstExistingPath(nativeV2MacHostCandidates());
  if (!exePath) {
    throw new Error('Native v2 macOS host is not built yet. Run npm run v2:mac:build on the Mac first, or use an installer that includes native-v2 host.');
  }

  const videoPort = normalizeNativeV2Number(options.videoPort, 45000, 1, 65535);
  const inputPort = normalizeNativeV2Number(options.inputPort, 45001, 1, 65535);
  const width = normalizeNativeV2Number(options.width, 1920, 640, 7680);
  const height = normalizeNativeV2Number(options.height, 1080, 360, 4320);
  const fps = normalizeNativeV2Number(options.fps, 60, 30, 240);
  const bitrate = normalizeNativeV2Number(options.bitrate, 14_000_000, 1_000_000, 200_000_000);
  const keyint = normalizeNativeV2Number(options.keyint, 1, 1, 300);
  const requestedTransport = options.transport === 'tcp' ? 'tcp' : 'udp';
  const transport = requestedTransport === 'udp' && allowUdpVideo ? 'udp' : 'tcp';
  const args = [
    '--client-ip', String(options.clientIp),
    '--video-port', String(videoPort),
    '--input-port', String(inputPort),
    '--width', String(width),
    '--height', String(height),
    '--fps', String(fps),
    '--bitrate', String(bitrate),
    '--keyint', String(keyint),
    '--transport', transport,
  ];

  nativeV2HostProcess = spawn(exePath, args, {
    cwd: path.dirname(exePath),
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  const proc = nativeV2HostProcess;
  const pid = nativeV2HostProcess.pid;
  sendToMainWindow('host-log', {
    level: 'info',
    message: `native-v2 host started pid=${pid} client=${options.clientIp}:${videoPort} transport=${transport}`,
  });

  proc.stdout?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('host', proc, chunk);
  });
  proc.stderr?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('host', proc, chunk);
  });
  proc.once('exit', (code, signal) => {
    sendToMainWindow('host-log', { level: 'info', message: `native-v2 host exited code=${code ?? ''} signal=${signal ?? ''}` });
    if (nativeV2HostAutoStopTimer) {
      clearTimeout(nativeV2HostAutoStopTimer);
      nativeV2HostAutoStopTimer = null;
    }
    if (nativeV2HostProcess === proc) nativeV2HostProcess = null;
    broadcastNativeV2Status();
  });
  proc.once('error', (err) => {
    sendToMainWindow('host-log', { level: 'error', message: `native-v2 host failed: ${err.message}` });
    if (nativeV2HostProcess === proc) nativeV2HostProcess = null;
    broadcastNativeV2Status({ error: err.message });
  });

  broadcastNativeV2Status();
  return {
    ok: true,
    pid,
    exePath,
    args,
    clientIp: String(options.clientIp),
    videoPort,
    inputPort,
    width,
    height,
    fps,
    bitrate,
    keyint,
    transport,
  };
}

function requestNativeV2RemoteHost(device, options = {}) {
  return new Promise((resolve, reject) => {
    if (!device || !device.address || !device.port || !device.pin) {
      reject(new Error('Native v2 remote host request needs discovered Mac address, port and PIN'));
      return;
    }

    const route = routeLocalAddressForTarget(device.address, options.clientIp);
    const requestOptions = { ...options };
    if (route?.address) {
      if (requestOptions.clientIp && requestOptions.clientIp !== route.address) {
        sendToMainWindow('host-log', {
          level: 'info',
          message: `native-v2 route override: requested client ${requestOptions.clientIp}, using ${route.address} (${route.name || 'unknown'}, ${route.reason}) for Mac ${device.address}`,
        });
      } else {
        sendToMainWindow('host-log', {
          level: 'info',
          message: `native-v2 route: Mac ${device.address} -> local Windows ${route.address} (${route.name || 'unknown'}, ${route.reason})`,
        });
      }
      requestOptions.clientIp = route.address;
    }

    const requestId = crypto.randomUUID();
    const url = `ws://${device.address}:${device.port}`;
    const socket = new WebSocket(url);
    let settled = false;

    const finish = (err, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      try {
        socket.close();
      } catch {
        // ignore close errors
      }
      if (err) reject(err);
      else resolve(result);
    };

    const timer = setTimeout(() => {
      finish(new Error(`Native v2 remote host request timed out: ${url}`));
    }, 10_000);

    socket.on('open', () => {
      socket.send(JSON.stringify({ type: 'hello', pin: String(device.pin) }));
    });

    socket.on('message', (buf) => {
      let message;
      try {
        message = JSON.parse(buf.toString('utf8'));
      } catch {
        finish(new Error('Native v2 remote host returned invalid JSON'));
        return;
      }

      if (message.type === 'hello-ok') {
        socket.send(JSON.stringify({
          type: 'native-v2-start-host',
          requestId,
          options: requestOptions,
        }));
        return;
      }

      if (message.type === 'native-v2-host-started' && message.requestId === requestId) {
        nativeV2LastRemoteHostRequest = {
          device: {
            id: device.id,
            name: device.name,
            platform: device.platform,
            address: device.address,
            port: Number(device.port || SIGNAL_PORT),
            pin: String(device.pin || ''),
          },
          options: { ...requestOptions },
        };
        finish(null, message.result || { ok: true });
        return;
      }

      if (message.type === 'native-v2-host-error' && message.requestId === requestId) {
        finish(new Error(message.error || 'Native v2 remote host failed'));
        return;
      }

      if (message.type === 'error') {
        finish(new Error(message.error || 'Native v2 remote host request failed'));
      }
    });

    socket.on('error', (err) => {
      finish(new Error(`Native v2 remote host connection failed: ${err.message}`));
    });

    socket.on('close', () => {
      if (!settled) finish(new Error('Native v2 remote host connection closed before response'));
    });
  });
}

function getScreenCaptureStatus() {
  if (process.platform !== 'darwin') return 'unknown';
  try {
    return systemPreferences.getMediaAccessStatus('screen');
  } catch {
    return 'unknown';
  }
}


function startSignalServer() {
  if (wss) return;
  wss = new WebSocket.Server({ port: SIGNAL_PORT, host: '0.0.0.0' });

  wss.on('listening', () => {
    sendToMainWindow('host-log', { level: 'info', message: `signal server listening on ${SIGNAL_PORT}` });
  });

  wss.on('connection', (socket, req) => {
    const clientId = crypto.randomUUID();
    const remoteAddress = req.socket.remoteAddress || 'unknown';
    let authorized = false;

    const helloTimer = setTimeout(() => {
      if (!authorized && socket.readyState === WebSocket.OPEN) {
        socket.close(4001, 'pairing timeout');
      }
    }, 10_000);

    socket.on('message', (buf) => {
      let msg;
      try {
        msg = JSON.parse(buf.toString('utf8'));
      } catch {
        socket.close(4002, 'invalid json');
        return;
      }

      if (!authorized) {
        if (msg.type !== 'hello' || msg.pin !== PIN) {
          socket.send(JSON.stringify({ type: 'error', error: 'bad pin' }));
          socket.close(4003, 'bad pin');
          return;
        }
        authorized = true;
        clearTimeout(helloTimer);
        clients.set(clientId, socket);
        socket.send(JSON.stringify({ type: 'hello-ok', clientId }));
        sendToMainWindow('host-log', { level: 'info', message: `paired client ${clientId.slice(0, 8)} from ${remoteAddress}` });
        return;
      }

      if (msg.type === 'native-v2-start-host') {
        const requestId = msg.requestId || null;
        Promise.resolve()
          .then(() => startNativeV2Host(msg.options || {}))
          .then((result) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'native-v2-host-started', requestId, result }));
            }
          })
          .catch((err) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'native-v2-host-error', requestId, error: err.message || String(err) }));
            }
          });
        return;
      }

      sendToMainWindow('host-log', { level: 'debug', message: `ignored unsupported message ${msg.type || 'unknown'} from ${clientId.slice(0, 8)}` });
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type: 'error', error: `unsupported message type: ${msg.type || 'unknown'}` }));
      }
    });

    socket.on('close', (code, reason) => {
      clearTimeout(helloTimer);
      clients.delete(clientId);
      sendToMainWindow('host-log', { level: 'info', message: `client ${clientId.slice(0, 8)} disconnected code=${code} reason=${reason || ''}` });
    });
  });

  wss.on('error', (err) => {
    dialog.showErrorBox('Signal server failed', `${err.message}\nPort: ${SIGNAL_PORT}`);
  });
}

function announcePresence() {
  if (!discoverySocket) return;
  const payload = Buffer.from(JSON.stringify(localDevicePayload()));
  for (const address of broadcastAddresses()) {
    discoverySocket.send(payload, 0, payload.length, DISCOVERY_PORT, address);
  }
}

function startDiscovery() {
  if (discoverySocket) return;
  discoverySocket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

  discoverySocket.on('message', (buf, rinfo) => {
    let message;
    try {
      message = JSON.parse(buf.toString('utf8'));
    } catch {
      return;
    }
    if (message?.type !== 'p2p-remote-lan:announce' || message.id === ensureDeviceId()) return;

    devices.set(message.id, {
      ...message,
      address: rinfo.address,
      lastSeen: Date.now(),
    });
    sendDeviceList();
  });

  discoverySocket.on('error', (err) => {
    sendToMainWindow('host-log', { level: 'error', message: `discovery failed: ${err.message}` });
  });

  discoverySocket.bind(DISCOVERY_PORT, () => {
    discoverySocket.setBroadcast(true);
    announcePresence();
  });

  discoveryTimer = setInterval(announcePresence, 2_000);
  pruneTimer = setInterval(sendDeviceList, 5_000);
}

async function startHost() {
  win = createWindow('dashboard.html', { title: 'P2P Remote LAN - macOS Host' });
  startSignalServer();
  startDiscovery();
}

async function startDashboard() {
  win = createWindow('dashboard.html', { title: 'P2P Remote LAN' });
  startSignalServer();
  startDiscovery();
}


ipcMain.handle('app-info', () => {
  const primary = screen.getPrimaryDisplay();
  return {
    device: localDevicePayload(),
    discoveryPort: DISCOVERY_PORT,
    display: primary.bounds,
    scaleFactor: primary.scaleFactor,
    screenCaptureStatus: getScreenCaptureStatus(),
  };
});

ipcMain.handle('devices-list', () => {
  const now = Date.now();
  const list = [...devices.values()]
    .filter((device) => now - device.lastSeen < 15_000)
    .map(serializeDevice)
    .sort((a, b) => a.name.localeCompare(b.name));
  sendToMainWindow('devices-updated', list);
  return list;
});

ipcMain.handle('refresh-devices', () => {
  announcePresence();
  sendDeviceList();
  return true;
});


ipcMain.handle('native-v2-status', () => nativeV2StatusPayload());

ipcMain.handle('native-v2-start-client', (_event, options) => startNativeV2Client(options));

ipcMain.handle('native-v2-stop-client', () => {
  const stopped = stopNativeV2Process('client');
  broadcastNativeV2Status();
  return { ok: true, stopped };
});

ipcMain.handle('native-v2-start-host', (_event, options) => startNativeV2Host(options));

ipcMain.handle('native-v2-request-remote-host', (_event, payload = {}) => {
  return requestNativeV2RemoteHost(payload.device, payload.options);
});

ipcMain.handle('native-v2-route-local-address', (_event, targetIp) => {
  return routeLocalAddressForTarget(targetIp);
});

ipcMain.handle('native-v2-stop-host', () => {
  const stopped = stopNativeV2Process('host');
  broadcastNativeV2Status();
  return { ok: true, stopped };
});

ipcMain.handle('save-device-preview', (_event, id, dataUrl) => {
  if (!id || typeof dataUrl !== 'string' || !dataUrl.startsWith('data:image/')) return false;
  devicePreviews.set(String(id), dataUrl);
  sendDeviceList();
  return true;
});

ipcMain.handle('set-window-fullscreen', (event, fullScreen) => {
  const browserWindow = BrowserWindow.fromWebContents(event.sender);
  if (!browserWindow) return false;
  browserWindow.setFullScreen(Boolean(fullScreen));
  return browserWindow.isFullScreen();
});

ipcMain.handle('window-action', (event, action) => {
  const browserWindow = BrowserWindow.fromWebContents(event.sender);
  if (!browserWindow) return false;
  if (action === 'minimize') browserWindow.minimize();
  if (action === 'toggle-maximize') {
    if (browserWindow.isMaximized()) browserWindow.unmaximize();
    else browserWindow.maximize();
  }
  if (action === 'close') browserWindow.close();
  return true;
});

ipcMain.handle('screen-capture-status', () => getScreenCaptureStatus());

ipcMain.handle('open-screen-capture-settings', async () => {
  if (process.platform !== 'darwin') return false;
  await shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture');
  return true;
});

ipcMain.handle('reset-screen-capture-permission', async () => {
  if (process.platform !== 'darwin') return false;
  await new Promise((resolve, reject) => {
    execFile('/usr/bin/tccutil', ['reset', 'ScreenCapture', BUNDLE_ID], (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
  await shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture');
  return true;
});


app.whenReady().then(async () => {
  if (ROLE === 'host') await startHost();
  else await startDashboard();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      if (ROLE === 'host') startHost();
      else startDashboard();
    }
  });
});

app.on('window-all-closed', () => {
  if (wss) {
    wss.close();
    wss = null;
  }
  if (discoveryTimer) clearInterval(discoveryTimer);
  if (pruneTimer) clearInterval(pruneTimer);
  discoveryTimer = null;
  pruneTimer = null;
  if (discoverySocket) {
    discoverySocket.close();
    discoverySocket = null;
  }
  stopNativeV2Process('client');
  stopNativeV2Process('host');
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  appIsQuitting = true;
  stopNativeV2Process('client');
  stopNativeV2Process('host');
});
