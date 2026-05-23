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
const net = require('net');
const https = require('https');
const { execFile, spawn } = require('child_process');
const gameStreamInstaller = require('./game-stream-installer');

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
const NATIVE_V2_HOST_READY_TIMEOUT_MS = 12_000;
const NATIVE_V2_REMOTE_HOST_TIMEOUT_MS = 15_000;
const GAME_STREAM_HOST_READY_TIMEOUT_MS = 12_000;
const GAME_STREAM_REMOTE_HOST_TIMEOUT_MS = 15_000;
const GAME_STREAM_API_TIMEOUT_MS = 8_000;
const GAME_STREAM_PAIR_PIN_DELAY_MS = 1_200;
const GAME_STREAM_WEB_USERNAME = 'p2p-remote-lan';
const GAME_STREAM_WEB_PASSWORD = 'p2p-remote-lan-local';
const GAME_STREAM_DOWNLOADS = Object.freeze({
  sunshine: 'https://github.com/LizardByte/Sunshine/releases/latest',
  moonlight: 'https://github.com/moonlight-stream/moonlight-qt/releases/latest',
});
const GAME_STREAM_DEFAULTS = Object.freeze({
  appName: 'Desktop',
  width: 1920,
  height: 1080,
  fps: 60,
  bitrateKbps: 30_000,
  videoCodec: 'HEVC',
  videoDecoder: 'hardware',
  displayMode: 'fullscreen',
  captureSystemKeys: 'always',
  absoluteMouse: true,
  performanceOverlay: true,
});
const PIN = String(crypto.randomInt(100000, 999999));
const BUNDLE_ID = 'com.p2premotelan.app';
const NATIVE_MAC_HOST_BUNDLE_ID = 'com.p2premotelan.native.mac-host';

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
let nativeV2ClientProfileWatchPath = null;
let nativeV2ClientProfileLastJson = '';
let nativeV2ClientProfileChangeInFlight = false;
let nativeV2ClientProfilePending = null;
let gameStreamClientProcess = null;
let gameStreamPairProcess = null;
let gameStreamHostProcess = null;

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
  const scaleFactor = Number(primary?.scaleFactor) || 1;
  const pointBounds = primary?.bounds || null;
  const pixelBounds = pointBounds ? {
    x: Math.round(pointBounds.x * scaleFactor),
    y: Math.round(pointBounds.y * scaleFactor),
    width: Math.round(pointBounds.width * scaleFactor),
    height: Math.round(pointBounds.height * scaleFactor),
  } : null;
  return {
    type: 'p2p-remote-lan:announce',
    version: 1,
    id: ensureDeviceId(),
    name: deviceName(),
    platform: process.platform,
    port: SIGNAL_PORT,
    pin: PIN,
    addresses: lanAddresses().map((item) => item.address),
    display: pointBounds ? {
      x: pointBounds.x,
      y: pointBounds.y,
      width: pointBounds.width,
      height: pointBounds.height,
      pixelWidth: pixelBounds.width,
      pixelHeight: pixelBounds.height,
      displayFrequency: Number(primary.displayFrequency) || 60,
    } : null,
    scaleFactor,
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
  const pathEntries = String(process.env.PATH || '').split(path.delimiter).filter(Boolean);
  const extensions = process.platform === 'win32'
    ? String(process.env.PATHEXT || '.EXE;.CMD;.BAT;.COM').split(';').filter(Boolean)
    : [''];
  for (const candidate of candidates) {
    try {
      if (fs.existsSync(candidate)) return candidate;
      if (canSpawnShellCommand(candidate)) {
        for (const entry of pathEntries) {
          if (path.extname(candidate)) {
            const resolved = path.join(entry, candidate);
            if (fs.existsSync(resolved)) return resolved;
          } else {
            for (const ext of extensions) {
              const resolved = path.join(entry, `${candidate}${ext}`);
              if (fs.existsSync(resolved)) return resolved;
            }
          }
        }
      }
    } catch {
      // ignore invalid candidate paths
    }
  }
  return null;
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
    path.join(process.resourcesPath || '', 'native-v2', 'mac-host', 'P2PRemoteMacHost.app'),
    path.join(process.resourcesPath || '', 'native-v2', 'mac-host', 'P2P Native Mac Host.app'),
    path.join(__dirname, '..', 'native-v2', 'dist', 'mac-host', 'P2PRemoteMacHost.app'),
    path.join(__dirname, '..', 'native-v2', 'mac-host', '.build', 'P2PRemoteMacHost.app'),
    path.join(__dirname, '..', 'native-v2', 'mac-host', '.build', 'P2P Native Mac Host.app'),
  ];
}

function moonlightExecutableCandidates() {
  if (process.platform === 'win32') {
    const programFiles = [process.env.ProgramFiles, process.env['ProgramFiles(x86)'], process.env.LOCALAPPDATA]
      .filter(Boolean);
    return [
      ...programFiles.map((base) => path.join(base, 'Moonlight Game Streaming', 'Moonlight.exe')),
      path.join(gameStreamToolDir('moonlight'), 'Moonlight.exe'),
      path.join(gameStreamToolDir('moonlight'), 'Moonlight Game Streaming', 'Moonlight.exe'),
      'Moonlight.exe',
      'moonlight.exe',
    ];
  }
  if (process.platform === 'darwin') {
    return [
      '/Applications/Moonlight.app/Contents/MacOS/Moonlight',
      'moonlight',
    ];
  }
  return ['moonlight'];
}

function sunshineExecutableCandidates() {
  if (process.platform === 'darwin') {
    return [
      path.join(gameStreamToolDir('sunshine'), 'Sunshine.app', 'Contents', 'MacOS', 'sunshine'),
      path.join(gameStreamToolDir('sunshine'), 'Sunshine.app', 'Contents', 'MacOS', 'Sunshine'),
      '/Applications/Sunshine.app/Contents/MacOS/sunshine',
      '/Applications/Sunshine.app/Contents/MacOS/Sunshine',
      '/opt/homebrew/bin/sunshine',
      '/usr/local/bin/sunshine',
      'sunshine',
    ];
  }
  if (process.platform === 'win32') {
    const programFiles = [process.env.ProgramFiles, process.env['ProgramFiles(x86)'], process.env.LOCALAPPDATA]
      .filter(Boolean);
    return [
      path.join(gameStreamToolDir('sunshine'), 'sunshine.exe'),
      path.join(gameStreamToolDir('sunshine'), 'Sunshine', 'sunshine.exe'),
      ...programFiles.map((base) => path.join(base, 'Sunshine', 'sunshine.exe')),
      'sunshine.exe',
    ];
  }
  return ['/usr/bin/sunshine', '/usr/local/bin/sunshine', 'sunshine'];
}

function gameStreamDataDir() {
  return path.join(app.getPath('userData'), 'game-stream');
}

function gameStreamToolsDir() {
  return path.join(app.getPath('userData'), 'game-stream-tools');
}

function gameStreamToolDir(tool) {
  return path.join(gameStreamToolsDir(), tool);
}

function gameStreamDownloadDir() {
  return path.join(gameStreamToolsDir(), 'downloads');
}

function gameStreamSunshineConfigPath() {
  return path.join(gameStreamDataDir(), 'sunshine.conf');
}

function gameStreamSunshineAppsPath() {
  return path.join(gameStreamDataDir(), 'apps.json');
}

function normalizeGameStreamNumber(value, fallback, min, max) {
  const number = Number(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.max(min, Math.min(max, Math.round(number)));
}

function normalizeMoonlightChoice(value, allowed, fallback) {
  const text = String(value || fallback).trim();
  return allowed.some((choice) => choice.toLowerCase() === text.toLowerCase()) ? text : fallback;
}

function gameStreamOptions(options = {}) {
  return {
    appName: String(options.appName || GAME_STREAM_DEFAULTS.appName),
    width: normalizeGameStreamNumber(options.width, GAME_STREAM_DEFAULTS.width, 640, 7680),
    height: normalizeGameStreamNumber(options.height, GAME_STREAM_DEFAULTS.height, 360, 4320),
    fps: normalizeGameStreamNumber(options.fps, GAME_STREAM_DEFAULTS.fps, 30, 240),
    bitrateKbps: normalizeGameStreamNumber(options.bitrateKbps || options.bitrate, GAME_STREAM_DEFAULTS.bitrateKbps, 500, 500_000),
    videoCodec: normalizeMoonlightChoice(options.videoCodec, ['auto', 'H.264', 'HEVC', 'AV1'], GAME_STREAM_DEFAULTS.videoCodec),
    videoDecoder: normalizeMoonlightChoice(options.videoDecoder, ['auto', 'software', 'hardware'], GAME_STREAM_DEFAULTS.videoDecoder),
    displayMode: normalizeMoonlightChoice(options.displayMode, ['fullscreen', 'windowed', 'borderless'], GAME_STREAM_DEFAULTS.displayMode),
    captureSystemKeys: normalizeMoonlightChoice(options.captureSystemKeys, ['never', 'fullscreen', 'always'], GAME_STREAM_DEFAULTS.captureSystemKeys),
    absoluteMouse: options.absoluteMouse !== false,
    framePacing: options.framePacing !== false,
    gameOptimization: options.gameOptimization !== false,
    performanceOverlay: options.performanceOverlay !== false,
    quitAfter: options.quitAfter === true,
  };
}

function gameStreamSunshineStatePath() {
  return path.join(gameStreamDataDir(), 'sunshine_state.json');
}

function gameStreamSunshineCredentialsPath() {
  return path.join(gameStreamDataDir(), 'sunshine_credentials.json');
}

function gameStreamSunshineCredentialsPathForConfig() {
  const credentialsPath = gameStreamSunshineCredentialsPath();
  try {
    if (fs.existsSync(credentialsPath)) {
      const parsed = JSON.parse(fs.readFileSync(credentialsPath, 'utf8'));
      if (parsed?.username === GAME_STREAM_WEB_USERNAME && !parsed?.salt && !parsed?.password) {
        fs.rmSync(credentialsPath, { force: true });
      }
    }
  } catch {
    // Leave user-created Sunshine credentials untouched if they are not parseable here.
  }
  return credentialsPath;
}
function ensureGameStreamSunshineFiles() {
  const dataDir = gameStreamDataDir();
  fs.mkdirSync(dataDir, { recursive: true });
  const appsPath = gameStreamSunshineAppsPath();
  const configPath = gameStreamSunshineConfigPath();
  const statePath = gameStreamSunshineStatePath();
  const credentialsPath = gameStreamSunshineCredentialsPathForConfig();
  if (!fs.existsSync(appsPath)) {
    fs.writeFileSync(appsPath, `${JSON.stringify({ apps: [{ name: GAME_STREAM_DEFAULTS.appName, 'image-path': 'desktop.png' }] }, null, 2)}\n`);
  }
  const lines = [
    'sunshine_name = P2P Remote LAN Game Stream',
    'min_log_level = info',
    `file_apps = ${appsPath}`,
    `file_state = ${statePath}`,
    `credentials_file = ${credentialsPath}`,
    'lan_encryption_mode = 0',
    'origin_web_ui_allowed = lan',
    'vt_realtime = enabled',
    'hevc_mode = 0',
    'av1_mode = 0',
    'fec_percentage = 20',
    '',
  ];
  fs.writeFileSync(configPath, lines.join('\n'));
  return { dataDir, appsPath, configPath, statePath, credentialsPath };
}

function canSpawnShellCommand(candidate) {
  return !candidate.includes('/') && !candidate.includes('\\') && !path.isAbsolute(candidate);
}

function spawnManagedProcess(executable, args, options = {}) {
  return spawn(executable, args, {
    cwd: options.cwd || (canSpawnShellCommand(executable) ? process.cwd() : path.dirname(executable)),
    env: options.env || process.env,
    windowsHide: Boolean(options.windowsHide),
    shell: canSpawnShellCommand(executable),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}

function relayGameStreamProcessOutput(kind, proc, chunk) {
  const text = chunk.toString('utf8');
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    sendToMainWindow('host-log', { level: 'game-stream', message: `${kind}: ${line}` });
  }
}

function isPortOpen(host, port, timeoutMs = 600) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host, port });
    let settled = false;
    const finish = (ok) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      resolve(ok);
    };
    socket.setTimeout(timeoutMs);
    socket.once('connect', () => finish(true));
    socket.once('timeout', () => finish(false));
    socket.once('error', () => finish(false));
  });
}

async function waitForPort(host, port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await isPortOpen(host, port)) return true;
    await new Promise((resolve) => setTimeout(resolve, 300));
  }
  return false;
}

function gameStreamApiRequest(host, requestPath, options = {}) {
  const body = options.body ? JSON.stringify(options.body) : '';
  const headers = {
    Accept: 'application/json',
    ...options.headers,
  };
  if (body) {
    headers['Content-Type'] = 'application/json';
    headers['Content-Length'] = Buffer.byteLength(body);
  }
  if (options.username || options.password) {
    headers.Authorization = `Basic ${Buffer.from(`${options.username || ''}:${options.password || ''}`).toString('base64')}`;
  }
  return new Promise((resolve, reject) => {
    const req = https.request({
      host,
      port: options.port || 47990,
      method: options.method || 'GET',
      path: requestPath,
      headers,
      rejectUnauthorized: false,
      timeout: options.timeoutMs || GAME_STREAM_API_TIMEOUT_MS,
    }, (res) => {
      const chunks = [];
      res.on('data', (chunk) => chunks.push(chunk));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        if (text) {
          try { json = JSON.parse(text); } catch { /* Sunshine may return plain text on errors. */ }
        }
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve({ statusCode: res.statusCode, headers: res.headers, text, json });
          return;
        }
        const message = json?.error || text || `HTTP ${res.statusCode}`;
        const err = new Error(message);
        err.statusCode = res.statusCode;
        err.response = { text, json };
        reject(err);
      });
    });
    req.on('timeout', () => req.destroy(new Error(`Sunshine API request timed out: ${host}${requestPath}`)));
    req.on('error', reject);
    if (body) req.write(body);
    req.end();
  });
}

async function ensureSunshineWebCredentials(host) {
  try {
    await gameStreamApiRequest(host, '/api/config', {
      username: GAME_STREAM_WEB_USERNAME,
      password: GAME_STREAM_WEB_PASSWORD,
    });
    return { ready: true, created: false };
  } catch (err) {
    if (err.statusCode && err.statusCode !== 401 && (err.statusCode < 300 || err.statusCode >= 400)) throw err;
  }
  await gameStreamApiRequest(host, '/api/password', {
    method: 'POST',
    body: {
      currentUsername: '',
      currentPassword: '',
      newUsername: GAME_STREAM_WEB_USERNAME,
      newPassword: GAME_STREAM_WEB_PASSWORD,
      confirmNewPassword: GAME_STREAM_WEB_PASSWORD,
    },
  });
  return { ready: true, created: true };
}

async function submitSunshinePairPin(options = {}) {
  const host = String(options.host || options.hostIp || 'localhost');
  const pin = String(options.pin || '').trim();
  if (!/^\d{4}$/.test(pin)) throw new Error('Sunshine pairing PIN must be 4 digits');
  await ensureSunshineWebCredentials(host);
  const response = await gameStreamApiRequest(host, '/api/pin', {
    method: 'POST',
    username: GAME_STREAM_WEB_USERNAME,
    password: GAME_STREAM_WEB_PASSWORD,
    body: {
      pin,
      name: String(options.name || os.hostname() || 'Windows Moonlight'),
    },
  });
  if (response.json && response.json.status === false) {
    throw new Error(response.json.error || 'Sunshine rejected the pairing PIN');
  }
  return { ok: true, host, pin, status: response.json?.status ?? true };
}

function gameStreamStatusPayload() {
  const moonlightPath = findFirstExistingPath(moonlightExecutableCandidates());
  const sunshinePath = findFirstExistingPath(sunshineExecutableCandidates());
  return {
    platform: process.platform,
    downloads: GAME_STREAM_DOWNLOADS,
    defaults: { ...GAME_STREAM_DEFAULTS },
    install: gameStreamInstaller.installStatus(process.platform, gameStreamToolsDir(), gameStreamDownloadDir()),
    moonlight: {
      available: Boolean(moonlightPath),
      path: moonlightPath,
      running: Boolean(gameStreamClientProcess && !gameStreamClientProcess.killed),
      pairing: Boolean(gameStreamPairProcess && !gameStreamPairProcess.killed),
      pid: gameStreamClientProcess?.pid || null,
    },
    sunshine: {
      available: Boolean(sunshinePath),
      path: sunshinePath,
      running: Boolean(gameStreamHostProcess && !gameStreamHostProcess.killed),
      pid: gameStreamHostProcess?.pid || null,
      configPath: gameStreamSunshineConfigPath(),
      appsPath: gameStreamSunshineAppsPath(),
      statePath: gameStreamSunshineStatePath(),
      credentialsPath: gameStreamSunshineCredentialsPath(),
      webUrl: 'https://localhost:47990/',
    },
  };
}

function broadcastGameStreamStatus(extra = {}) {
  sendToMainWindow('game-stream-status', {
    ...gameStreamStatusPayload(),
    ...extra,
  });
}

function emitGameStreamInstallProgress(payload) {
  sendToMainWindow('game-stream-install-progress', payload);
}

async function installGameStreamTool(options = {}) {
  const tool = options.tool === 'sunshine' ? 'sunshine' : 'moonlight';
  const existingPath = findFirstExistingPath(tool === 'sunshine' ? sunshineExecutableCandidates() : moonlightExecutableCandidates());
  if (existingPath && !options.force) {
    return { ok: true, skipped: true, reason: 'already-installed', tool, path: existingPath, status: gameStreamStatusPayload() };
  }
  const result = await gameStreamInstaller.installTool({
    platform: process.platform,
    arch: process.arch,
    tool,
    mode: options.mode || (process.platform === 'win32' ? 'installer' : 'dmg'),
    downloadDir: gameStreamDownloadDir(),
    toolDir: gameStreamToolDir(tool),
    emitProgress: emitGameStreamInstallProgress,
    onOutput: (chunk) => relayGameStreamProcessOutput('install', null, chunk),
  });
  broadcastGameStreamStatus();
  return { ...result, status: gameStreamStatusPayload() };
}

function stopGameStreamProcess(kind) {
  const proc = kind === 'host'
    ? gameStreamHostProcess
    : (kind === 'pair' ? gameStreamPairProcess : gameStreamClientProcess);
  if (!proc || proc.killed) return false;
  try {
    proc.kill();
    return true;
  } catch {
    return false;
  }
}

function attachGameStreamProcess(kind, proc) {
  proc.stdout?.on('data', (chunk) => relayGameStreamProcessOutput(kind, proc, chunk));
  proc.stderr?.on('data', (chunk) => relayGameStreamProcessOutput(kind, proc, chunk));
  proc.once('exit', (code, signal) => {
    sendToMainWindow('host-log', { level: 'info', message: `game-stream ${kind} exited code=${code ?? ''} signal=${signal ?? ''}` });
    if (kind === 'host' && gameStreamHostProcess === proc) gameStreamHostProcess = null;
    if (kind === 'client' && gameStreamClientProcess === proc) gameStreamClientProcess = null;
    if (kind === 'pair' && gameStreamPairProcess === proc) gameStreamPairProcess = null;
    broadcastGameStreamStatus();
  });
  proc.once('error', (err) => {
    sendToMainWindow('host-log', { level: 'error', message: `game-stream ${kind} failed: ${err.message}` });
    if (kind === 'host' && gameStreamHostProcess === proc) gameStreamHostProcess = null;
    if (kind === 'client' && gameStreamClientProcess === proc) gameStreamClientProcess = null;
    if (kind === 'pair' && gameStreamPairProcess === proc) gameStreamPairProcess = null;
    broadcastGameStreamStatus({ error: err.message });
  });
}

async function startGameStreamHost(options = {}) {
  if (process.platform !== 'darwin' && process.platform !== 'win32' && process.platform !== 'linux') {
    throw new Error('Sunshine host is only supported on desktop platforms');
  }
  const exePath = findFirstExistingPath(sunshineExecutableCandidates());
  if (!exePath) {
    throw new Error('未找到 Sunshine。请安装 Sunshine，或把 sunshine 加入 PATH。下载地址：https://github.com/LizardByte/Sunshine/releases/latest');
  }
  const webUrl = options.webHost ? `https://${options.webHost}:47990/` : 'https://localhost:47990/';
  if (gameStreamHostProcess && !gameStreamHostProcess.killed) {
    broadcastGameStreamStatus();
    return {
      ok: true,
      alreadyRunning: true,
      pid: gameStreamHostProcess.pid,
      exePath,
      webUrl,
    };
  }

  const files = ensureGameStreamSunshineFiles();
  const proc = spawnManagedProcess(exePath, [files.configPath], { cwd: files.dataDir });
  gameStreamHostProcess = proc;
  attachGameStreamProcess('host', proc);
  sendToMainWindow('host-log', { level: 'info', message: `game-stream host starting Sunshine pid=${proc.pid} config=${files.configPath}` });
  broadcastGameStreamStatus();

  const ready = await waitForPort('127.0.0.1', 47990, normalizeGameStreamNumber(options.readyTimeoutMs, GAME_STREAM_HOST_READY_TIMEOUT_MS, 1000, 60_000));
  if (!ready) {
    throw new Error('Sunshine 已启动但 Web/API 端口 47990 未及时就绪。请检查 Sunshine 权限、防火墙或端口占用。');
  }
  return {
    ok: true,
    pid: proc.pid,
    exePath,
    configPath: files.configPath,
    appsPath: files.appsPath,
    statePath: files.statePath,
    credentialsPath: files.credentialsPath,
    webUrl,
  };
}

function moonlightStreamArgs(hostIp, options = {}) {
  const normalized = gameStreamOptions(options);
  const args = [
    'stream',
    '--resolution', `${normalized.width}x${normalized.height}`,
    '--fps', String(normalized.fps),
    '--bitrate', String(normalized.bitrateKbps),
    '--video-codec', normalized.videoCodec,
    '--video-decoder', normalized.videoDecoder,
    '--display-mode', normalized.displayMode,
    '--capture-system-keys', normalized.captureSystemKeys,
  ];
  args.push(normalized.absoluteMouse ? '--absolute-mouse' : '--no-absolute-mouse');
  args.push(normalized.framePacing ? '--frame-pacing' : '--no-frame-pacing');
  args.push(normalized.gameOptimization ? '--game-optimization' : '--no-game-optimization');
  args.push(normalized.performanceOverlay ? '--performance-overlay' : '--no-performance-overlay');
  args.push(normalized.quitAfter ? '--quit-after' : '--no-quit-after');
  args.push(String(hostIp), normalized.appName);
  return args;
}

function startGameStreamClient(options = {}) {
  if (process.platform !== 'win32' && process.platform !== 'darwin' && process.platform !== 'linux') {
    throw new Error('Moonlight client is only supported on desktop platforms');
  }
  if (!options.hostIp) {
    throw new Error('Moonlight needs the Sunshine host IP address');
  }
  const exePath = findFirstExistingPath(moonlightExecutableCandidates());
  if (!exePath) {
    throw new Error('未找到 Moonlight。请安装 Moonlight Qt，或把 Moonlight.exe 加入 PATH。下载地址：https://github.com/moonlight-stream/moonlight-qt/releases/latest');
  }
  if (gameStreamClientProcess && !gameStreamClientProcess.killed) stopGameStreamProcess('client');
  const args = moonlightStreamArgs(options.hostIp, options);
  const proc = spawnManagedProcess(exePath, args, { windowsHide: false });
  gameStreamClientProcess = proc;
  attachGameStreamProcess('client', proc);
  sendToMainWindow('host-log', { level: 'info', message: `game-stream client started Moonlight pid=${proc.pid} host=${options.hostIp} app=${gameStreamOptions(options).appName}` });
  broadcastGameStreamStatus();
  return {
    ok: true,
    pid: proc.pid,
    exePath,
    args,
    hostIp: String(options.hostIp),
    ...gameStreamOptions(options),
  };
}

function pairGameStreamClient(options = {}) {
  if (!options.hostIp) throw new Error('Moonlight pairing needs the Sunshine host IP address');
  const pin = String(options.pin || '').trim();
  if (pin && !/^\d{4}$/.test(pin)) throw new Error('Moonlight pairing PIN must be 4 digits');
  const exePath = findFirstExistingPath(moonlightExecutableCandidates());
  if (!exePath) {
    throw new Error('未找到 Moonlight。请先安装 Moonlight Qt。');
  }
  if (gameStreamPairProcess && !gameStreamPairProcess.killed) stopGameStreamProcess('pair');
  const args = ['pair'];
  if (pin) args.push('--pin', pin);
  args.push(String(options.hostIp));
  const proc = spawnManagedProcess(exePath, args, { windowsHide: false });
  gameStreamPairProcess = proc;
  attachGameStreamProcess('pair', proc);
  sendToMainWindow('host-log', { level: 'info', message: `game-stream pairing started Moonlight pid=${proc.pid} host=${options.hostIp}${pin ? ` pin=${pin}` : ''}` });
  broadcastGameStreamStatus();
  return { ok: true, pid: proc.pid, exePath, args, hostIp: String(options.hostIp) };
}

function requestGameStreamRemoteHost(device, options = {}) {
  return new Promise((resolve, reject) => {
    if (!device || !device.address || !device.port || !device.pin) {
      reject(new Error('Game stream remote host request needs discovered Mac address, port and PIN'));
      return;
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
      finish(new Error(`Game stream remote host request timed out: ${url}`));
    }, GAME_STREAM_REMOTE_HOST_TIMEOUT_MS);

    socket.on('open', () => {
      socket.send(JSON.stringify({ type: 'hello', pin: String(device.pin) }));
    });
    socket.on('message', (buf) => {
      let message;
      try {
        message = JSON.parse(buf.toString('utf8'));
      } catch {
        finish(new Error('Game stream remote host returned invalid JSON'));
        return;
      }
      if (message.type === 'hello-ok') {
        const type = options.pairPin ? 'game-stream-submit-pair-pin' : 'game-stream-start-host';
        socket.send(JSON.stringify({ type, requestId, options }));
        return;
      }
      if (message.type === 'game-stream-host-started' && message.requestId === requestId) {
        finish(null, message.result || { ok: true });
        return;
      }
      if (message.type === 'game-stream-host-error' && message.requestId === requestId) {
        finish(new Error(message.error || 'Game stream remote host failed'));
        return;
      }
      if (message.type === 'game-stream-pair-pin-submitted' && message.requestId === requestId) {
        finish(null, message.result || { ok: true });
        return;
      }
      if (message.type === 'game-stream-pair-pin-error' && message.requestId === requestId) {
        finish(new Error(message.error || 'Game stream remote pairing failed'));
        return;
      }
      if (message.type === 'error') {
        finish(new Error(message.error || 'Game stream remote host request failed'));
      }
    });
    socket.on('error', (err) => finish(new Error(`Game stream remote host connection failed: ${err.message}`)));
    socket.on('close', () => {
      if (!settled) finish(new Error('Game stream remote host connection closed before response'));
    });
  });
}

function nativeV2ClientProfilePath() {
  return path.join(app.getPath('userData'), 'native-v2-win-client-profile.json');
}

function readNativeV2ClientProfile(profileFile = nativeV2ClientProfilePath()) {
  try {
    const parsed = JSON.parse(fs.readFileSync(profileFile, 'utf8'));
    let profile = {
      width: Number(parsed.width) || undefined,
      height: Number(parsed.height) || undefined,
      fps: Number(parsed.fps) || undefined,
      bitrate: Number(parsed.bitrate) || undefined,
      profileVersion: Number(parsed.profileVersion) || 0,
    };
    if (!profile.profileVersion && profile.width && profile.height && Math.max(profile.width, profile.height) > 1920) {
      const scale = 1600 / Math.max(profile.width, profile.height);
      profile = {
        ...profile,
        width: Math.max(640, Math.round(profile.width * scale / 2) * 2),
        height: Math.max(360, Math.round(profile.height * scale / 2) * 2),
        bitrate: Math.min(Number(profile.bitrate) || 10_000_000, 10_000_000),
      };
    }
    return profile;
  } catch {
    return {};
  }
}

function writeNativeV2ClientProfile(profileFile = nativeV2ClientProfilePath(), profile = {}) {
  const payload = {
    profileVersion: 2,
    width: Number(profile.width) || 1600,
    height: Number(profile.height) || 900,
    fps: Number(profile.fps) || 60,
    bitrate: Number(profile.bitrate) || 10_000_000,
  };
  fs.mkdirSync(path.dirname(profileFile), { recursive: true });
  fs.writeFileSync(profileFile, JSON.stringify(payload, null, 2));
  return payload;
}

function nativeV2ClientProfileSignature(profile = {}) {
  return JSON.stringify({
    width: Number(profile.width) || 0,
    height: Number(profile.height) || 0,
    fps: Number(profile.fps) || 0,
    bitrate: Number(profile.bitrate) || 0,
  });
}

function stopNativeV2ClientProfileWatcher() {
  if (!nativeV2ClientProfileWatchPath) return;
  try {
    fs.unwatchFile(nativeV2ClientProfileWatchPath);
  } catch {
    // ignore watcher shutdown errors
  }
  nativeV2ClientProfileWatchPath = null;
  nativeV2ClientProfileLastJson = '';
  nativeV2ClientProfilePending = null;
  nativeV2ClientProfileChangeInFlight = false;
}

async function processNativeV2ClientProfileChange(profile) {
  const previous = nativeV2ClientLastOptions ? { ...nativeV2ClientLastOptions } : null;
  const normalized = {
    width: normalizeNativeV2Number(profile.width, nativeV2ClientLastOptions?.width || 1600, 640, 7680),
    height: normalizeNativeV2Number(profile.height, nativeV2ClientLastOptions?.height || 900, 360, 4320),
    fps: normalizeNativeV2Number(profile.fps, nativeV2ClientLastOptions?.fps || 60, 30, 240),
    bitrate: normalizeNativeV2Number(profile.bitrate, nativeV2ClientLastOptions?.bitrate || 10_000_000, 1_000_000, 200_000_000),
    transport: 'udp',
  };
  if (nativeV2ClientLastOptions) {
    nativeV2ClientLastOptions = {
      ...nativeV2ClientLastOptions,
      ...normalized,
    };
  }
  if (nativeV2LastRemoteHostRequest?.options) {
    nativeV2LastRemoteHostRequest.options = {
      ...nativeV2LastRemoteHostRequest.options,
      ...normalized,
    };
  }
  broadcastNativeV2Status();
  const shapeChanged = previous
    ? previous.width !== normalized.width || previous.height !== normalized.height || previous.fps !== normalized.fps
    : true;
  sendToMainWindow('host-log', {
    level: 'info',
    message: shapeChanged
      ? `native-v2 profile changed to ${normalized.width}x${normalized.height}@${normalized.fps} bitrate=${normalized.bitrate}; live control packet sent by Windows client, skipping slow Electron host restart`
      : `native-v2 bitrate changed to ${normalized.bitrate}; applied live without host restart`,
  });
}

function queueNativeV2ClientProfileChange(profile) {
  nativeV2ClientProfilePending = profile;
  if (nativeV2ClientProfileChangeInFlight) return;
  nativeV2ClientProfileChangeInFlight = true;
  (async () => {
    try {
      while (nativeV2ClientProfilePending) {
        const next = nativeV2ClientProfilePending;
        nativeV2ClientProfilePending = null;
        await processNativeV2ClientProfileChange(next);
      }
    } catch (err) {
      sendToMainWindow('host-log', {
        level: 'error',
        message: `native-v2 profile apply failed: ${err.message || String(err)}`,
      });
    } finally {
      nativeV2ClientProfileChangeInFlight = false;
      if (nativeV2ClientProfilePending) queueNativeV2ClientProfileChange(nativeV2ClientProfilePending);
    }
  })();
}

function startNativeV2ClientProfileWatcher(profileFile) {
  stopNativeV2ClientProfileWatcher();
  nativeV2ClientProfileWatchPath = profileFile;
  nativeV2ClientProfileLastJson = nativeV2ClientProfileSignature(readNativeV2ClientProfile(profileFile));
  fs.watchFile(profileFile, { interval: 350 }, () => {
    const profile = readNativeV2ClientProfile(profileFile);
    const json = nativeV2ClientProfileSignature(profile);
    if (!json || json === nativeV2ClientProfileLastJson) return;
    nativeV2ClientProfileLastJson = json;
    queueNativeV2ClientProfileChange(profile);
  });
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
      width: Number(savedClientProfile.width) || 1600,
      height: Number(savedClientProfile.height) || 900,
      fps: Number(savedClientProfile.fps) || 60,
      bitrate: Number(savedClientProfile.bitrate) || 10_000_000,
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

function nativeV2HostReadyResult(options, exePath, args, pid) {
  const videoPort = normalizeNativeV2Number(options.videoPort, 45000, 1, 65535);
  const inputPort = normalizeNativeV2Number(options.inputPort, 45001, 1, 65535);
  const width = normalizeNativeV2Number(options.width, 1600, 640, 7680);
  const height = normalizeNativeV2Number(options.height, 900, 360, 4320);
  const fps = normalizeNativeV2Number(options.fps, 60, 30, 240);
  const bitrate = normalizeNativeV2Number(options.bitrate, 10_000_000, 1_000_000, 200_000_000);
  const keyint = normalizeNativeV2Number(options.keyint, 1, 1, 300);
  const requestedTransport = options.transport === 'tcp' ? 'tcp' : 'udp';
  const allowUdpVideo = options.allowUdpVideo !== false && process.env.P2P_NATIVE_V2_DISABLE_UDP !== '1';
  const transport = requestedTransport === 'udp' && allowUdpVideo ? 'udp' : 'tcp';
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

function summarizeNativeV2HostFailure(lines, fallback) {
  const text = lines.join('\n');
  const fatalLine = [...lines].reverse().find((line) => line.includes('[fatal]'));
  const permissionLine = [...lines].reverse().find((line) => (
    line.includes('Screen Recording permission denied') ||
    line.includes('screen recording permission missing')
  ));
  if (permissionLine || text.includes('Screen Recording permission denied')) {
    return 'Mac 端屏幕录制权限未授权。请在系统设置 > 隐私与安全性 > 屏幕录制 中允许当前运行的 App/Terminal，然后完全退出并重新打开 Mac 端。';
  }
  if (fatalLine) return fatalLine.replace(/^\[fatal\]\s*/, '');
  return fallback;
}

function stopNativeV2Process(kind) {
  const proc = kind === 'host' ? nativeV2HostProcess : nativeV2ClientProcess;
  if (!proc || proc.killed) return false;
  if (kind === 'client') stopNativeV2ClientProfileWatcher();
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
  const width = normalizeNativeV2Number(options.width, 1600, 640, 7680);
  const height = normalizeNativeV2Number(options.height, 900, 360, 4320);
  const fps = normalizeNativeV2Number(options.fps, 60, 30, 240);
  const bitrate = normalizeNativeV2Number(options.bitrate, 10_000_000, 0, 200_000_000);
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
  startNativeV2ClientProfileWatcher(profileFile);

  nativeV2ClientProcess = spawn(exePath, args, {
    cwd: path.dirname(exePath),
    env: process.env,
    windowsHide: false,
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  const proc = nativeV2ClientProcess;
  const pid = nativeV2ClientProcess.pid;
  sendToMainWindow('host-log', {
    level: 'info',
    message: `native-v2 client started pid=${pid} host=${options.hostIp}:${videoPort} transport=${transport}`,
  });

  proc.stdout?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('client', proc, chunk);
  });
  proc.stderr?.on('data', (chunk) => {
    relayNativeV2ProcessOutput('client', proc, chunk);
  });
  proc.once('exit', (code, signal) => {
    sendToMainWindow('host-log', { level: 'info', message: `native-v2 client exited code=${code ?? ''} signal=${signal ?? ''}` });
    if (nativeV2ClientProcess === proc) {
      nativeV2ClientProcess = null;
      stopNativeV2ClientProfileWatcher();
    }
    broadcastNativeV2Status();
  });
  proc.once('error', (err) => {
    sendToMainWindow('host-log', { level: 'error', message: `native-v2 client failed: ${err.message}` });
    if (nativeV2ClientProcess === proc) {
      nativeV2ClientProcess = null;
      stopNativeV2ClientProfileWatcher();
    }
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

async function waitForNativeV2HostReady(proc, options, exePath, args, pid) {
  const lines = [];
  let settled = false;

  return new Promise((resolve, reject) => {
    const settle = (err, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      proc.stdout?.off('data', onOutput);
      proc.stderr?.off('data', onOutput);
      proc.off('exit', onEarlyExit);
      proc.off('error', onEarlyError);
      if (err) reject(err);
      else resolve(result);
    };

    const onOutput = (chunk) => {
      const text = chunk.toString('utf8');
      for (const rawLine of text.split(/\r?\n/)) {
        const line = rawLine.trim();
        if (!line) continue;
        lines.push(line);
        if (lines.length > 60) lines.shift();

        if (line.includes('[ready]')) {
          settle(null, nativeV2HostReadyResult(options, exePath, args, pid));
          return;
        }

        if (line.includes('[fatal]') || line.includes('Screen Recording permission denied')) {
          settle(new Error(summarizeNativeV2HostFailure(lines, 'Native v2 macOS host failed before it became ready')));
          return;
        }
      }
    };

    const onEarlyExit = (code, signal) => {
      const fallback = `Native v2 macOS host exited before ready code=${code ?? ''} signal=${signal ?? ''}`;
      settle(new Error(summarizeNativeV2HostFailure(lines, fallback)));
    };

    const onEarlyError = (err) => {
      settle(new Error(`Native v2 macOS host failed before ready: ${err.message || String(err)}`));
    };

    const timer = setTimeout(() => {
      settle(new Error(summarizeNativeV2HostFailure(lines, 'Native v2 macOS host did not become ready in time')));
    }, NATIVE_V2_HOST_READY_TIMEOUT_MS);

    proc.stdout?.on('data', onOutput);
    proc.stderr?.on('data', onOutput);
    proc.once('exit', onEarlyExit);
    proc.once('error', onEarlyError);
  });
}

async function startNativeV2Host(options = {}) {
  if (process.platform !== 'darwin') {
    throw new Error('Native v2 macOS host can only run on macOS');
  }
  const allowUdpVideo = options.allowUdpVideo !== false && process.env.P2P_NATIVE_V2_DISABLE_UDP !== '1';
  if (!options.clientIp) {
    throw new Error('Native v2 host needs the Windows client IP address');
  }

  // Ensure Electron itself has screen recording permission before spawning mac-host.
  // The child process inherits TCC permission from the parent Electron process.
  const screenStatus = getScreenCaptureStatus();
  if (screenStatus !== 'granted') {
    sendToMainWindow('host-log', {
      level: 'warn',
      message: `屏幕录制权限未授权 (status=${screenStatus})。请在系统设置 > 隐私与安全性 > 屏幕录制 中允许当前运行的 Electron/P2P Remote 应用，然后重启应用。`,
    });
    await openMacPrivacyPane('Privacy_ScreenCapture');
    throw new Error('Mac 端屏幕录制权限未授权。请在系统设置 > 隐私与安全性 > 屏幕录制 中允许当前应用后重启。');
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
  const width = normalizeNativeV2Number(options.width, 1600, 640, 7680);
  const height = normalizeNativeV2Number(options.height, 900, 360, 4320);
  const fps = normalizeNativeV2Number(options.fps, 60, 30, 240);
  const bitrate = normalizeNativeV2Number(options.bitrate, 10_000_000, 1_000_000, 200_000_000);
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

  // Spawn the binary directly from the .app bundle path.
  // Screen recording permission is inherited from the parent Electron process.
  // The binary path inside .app/Contents/MacOS/ lets macOS identify the bundle context.
  const binPath = path.join(exePath, 'Contents', 'MacOS', 'p2p-native-mac-host');
  const actualExe = fs.existsSync(binPath) ? binPath : exePath;
  nativeV2HostProcess = spawn(actualExe, args, {
    cwd: path.dirname(actualExe),
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  const proc = nativeV2HostProcess;
  const pid = nativeV2HostProcess.pid;
  const readyPromise = waitForNativeV2HostReady(proc, options, exePath, args, pid);
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
  try {
    return await readyPromise;
  } catch (err) {
    if (nativeV2HostProcess === proc && !proc.killed) {
      try {
        proc.kill();
      } catch {
        // ignore shutdown errors
      }
    }
    if (nativeV2HostProcess === proc) nativeV2HostProcess = null;
    broadcastNativeV2Status({ error: err.message || String(err) });
    throw err;
  }
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
    }, NATIVE_V2_REMOTE_HOST_TIMEOUT_MS);

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
        const err = message.error || '';
        finish(new Error(err || 'Native v2 remote host request failed'));
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

function getAccessibilityStatus(prompt = false) {
  if (process.platform !== 'darwin') return 'unknown';
  try {
    return systemPreferences.isTrustedAccessibilityClient(Boolean(prompt)) ? 'granted' : 'denied';
  } catch {
    return 'unknown';
  }
}

function nativeV2MacHostAppPath() {
  const exePath = findFirstExistingPath(nativeV2MacHostCandidates());
  if (!exePath) return null;
  const marker = `${path.sep}P2P Native Mac Host.app${path.sep}`;
  const index = exePath.indexOf(marker);
  if (index < 0) return null;
  return exePath.slice(0, index + marker.length - 1);
}

function macPermissionStatus() {
  return {
    platform: process.platform,
    screenCapture: getScreenCaptureStatus(),
    accessibility: getAccessibilityStatus(false),
    nativeHostAppPath: nativeV2MacHostAppPath(),
  };
}

async function openMacPrivacyPane(pane) {
  if (process.platform !== 'darwin') return false;
  await shell.openExternal(`x-apple.systempreferences:com.apple.preference.security?${pane}`);
  return true;
}

async function resetMacTccPermission(service) {
  if (process.platform !== 'darwin') return false;
  const bundleId = service === 'Accessibility' || service === 'ScreenCapture'
    ? NATIVE_MAC_HOST_BUNDLE_ID
    : BUNDLE_ID;
  await new Promise((resolve, reject) => {
    execFile('/usr/bin/tccutil', ['reset', service, bundleId], (err) => {
      if (err) reject(err);
      else resolve();
    });
  });
  return true;
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

      if (msg.type === 'game-stream-start-host') {
        const requestId = msg.requestId || null;
        Promise.resolve()
          .then(() => startGameStreamHost(msg.options || {}))
          .then((result) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'game-stream-host-started', requestId, result }));
            }
          })
          .catch((err) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'game-stream-host-error', requestId, error: err.message || String(err) }));
            }
          });
        return;
      }

      if (msg.type === 'game-stream-submit-pair-pin') {
        const requestId = msg.requestId || null;
        Promise.resolve()
          .then(() => startGameStreamHost(msg.options || {}))
          .then(() => submitSunshinePairPin({
            host: 'localhost',
            pin: msg.options?.pairPin,
            name: msg.options?.pairName,
          }))
          .then((result) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'game-stream-pair-pin-submitted', requestId, result }));
            }
          })
          .catch((err) => {
            if (socket.readyState === WebSocket.OPEN) {
              socket.send(JSON.stringify({ type: 'game-stream-pair-pin-error', requestId, error: err.message || String(err) }));
            }
          });
        return;
      }

      // silently ignore unknown message types (e.g. stray ICE candidates from other tools)

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
    accessibilityStatus: getAccessibilityStatus(false),
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

ipcMain.handle('game-stream-status', () => gameStreamStatusPayload());

ipcMain.handle('game-stream-install-status', () => gameStreamInstaller.installStatus(process.platform, gameStreamToolsDir(), gameStreamDownloadDir()));

ipcMain.handle('game-stream-install-tool', (_event, options) => installGameStreamTool(options));

ipcMain.handle('game-stream-start-host', (_event, options) => startGameStreamHost(options));

ipcMain.handle('game-stream-stop-host', () => {
  const stopped = stopGameStreamProcess('host');
  broadcastGameStreamStatus();
  return { ok: true, stopped };
});

ipcMain.handle('game-stream-start-client', (_event, options) => startGameStreamClient(options));

ipcMain.handle('game-stream-stop-client', () => {
  const stopped = stopGameStreamProcess('client');
  broadcastGameStreamStatus();
  return { ok: true, stopped };
});

ipcMain.handle('game-stream-pair-client', (_event, options) => pairGameStreamClient(options));

ipcMain.handle('game-stream-submit-pair-pin', (_event, options) => submitSunshinePairPin(options));

ipcMain.handle('game-stream-request-remote-host', (_event, payload = {}) => {
  return requestGameStreamRemoteHost(payload.device, payload.options);
});

ipcMain.handle('game-stream-open-sunshine', (_event, host) => {
  const target = host ? String(host).replace(/^https?:\/\//i, '').replace(/\/$/, '') : 'localhost:47990';
  return shell.openExternal(`https://${target.includes(':') ? target : `${target}:47990`}/`);
});

ipcMain.handle('game-stream-open-download', (_event, target) => {
  const url = target === 'sunshine' ? GAME_STREAM_DOWNLOADS.sunshine : GAME_STREAM_DOWNLOADS.moonlight;
  return shell.openExternal(url);
});

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

ipcMain.handle('mac-permission-status', () => macPermissionStatus());

ipcMain.handle('open-screen-capture-settings', async () => {
  return openMacPrivacyPane('Privacy_ScreenCapture');
});

ipcMain.handle('reset-screen-capture-permission', async () => {
  const ok = await resetMacTccPermission('ScreenCapture');
  if (!ok) return false;
  await openMacPrivacyPane('Privacy_ScreenCapture');
  return true;
});

ipcMain.handle('request-accessibility-permission', async () => {
  if (process.platform !== 'darwin') return false;
  const granted = getAccessibilityStatus(true) === 'granted';
  if (!granted) await openMacPrivacyPane('Privacy_Accessibility');
  return granted;
});

ipcMain.handle('open-accessibility-settings', async () => {
  return openMacPrivacyPane('Privacy_Accessibility');
});

ipcMain.handle('reset-accessibility-permission', async () => {
  const ok = await resetMacTccPermission('Accessibility');
  if (!ok) return false;
  await openMacPrivacyPane('Privacy_Accessibility');
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
  stopGameStreamProcess('pair');
  stopGameStreamProcess('client');
  stopGameStreamProcess('host');
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  appIsQuitting = true;
  stopNativeV2Process('client');
  stopNativeV2Process('host');
  stopGameStreamProcess('pair');
  stopGameStreamProcess('client');
  stopGameStreamProcess('host');
});
