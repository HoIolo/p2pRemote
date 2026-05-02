const {
  app,
  BrowserWindow,
  ipcMain,
  screen,
  dialog,
  desktopCapturer,
  session,
  systemPreferences,
  shell,
} = require('electron');
const crypto = require('crypto');
const dgram = require('dgram');
const fs = require('fs');
const os = require('os');
const path = require('path');
const WebSocket = require('ws');
const { execFile } = require('child_process');
const { injectInput } = require('./injector');

// Latency-oriented Chromium defaults. This is a LAN-only consent-based app;
// exposing host candidates makes WebRTC direct LAN pairing more reliable.
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-features', 'WebRtcHideLocalIpsWithMdns');

function resolveRole() {
  if (process.argv.includes('--host')) return 'host';
  if (process.argv.includes('--client')) return 'client';
  if (process.env.P2P_REMOTE_ROLE === 'host') return 'host';
  if (process.env.P2P_REMOTE_ROLE === 'client') return 'client';

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
const remoteConfigs = new Map();
const devicePreviews = new Map();

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
      if (addr.family === 'IPv4' && !addr.internal) out.push({ name, address: addr.address });
    }
  }
  return out;
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
  return {
    type: 'p2p-remote-lan:announce',
    version: 1,
    id: ensureDeviceId(),
    name: deviceName(),
    platform: process.platform,
    port: SIGNAL_PORT,
    pin: PIN,
    addresses: lanAddresses().map((item) => item.address),
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

function createRemoteWindow(device) {
  const remoteWindow = createWindow('remote.html', {
    width: 1280,
    height: 820,
    title: `${device.name || device.address} - P2P Remote LAN`,
    titleBarHeight: 54,
  });
  const webContentsId = remoteWindow.webContents.id;
  remoteConfigs.set(webContentsId, serializeDevice(device));
  remoteWindow.once('closed', () => {
    remoteConfigs.delete(webContentsId);
  });
  return remoteWindow;
}

function getScreenCaptureStatus() {
  if (process.platform !== 'darwin') return 'unknown';
  try {
    return systemPreferences.getMediaAccessStatus('screen');
  } catch {
    return 'unknown';
  }
}

function registerDisplayMediaHandler() {
  session.defaultSession.setDisplayMediaRequestHandler(async (request, callback) => {
    try {
      if (!request.videoRequested) {
        callback({});
        return;
      }

      const sources = await desktopCapturer.getSources({
        types: ['screen'],
        thumbnailSize: { width: 1, height: 1 },
      });
      const primaryDisplayId = String(screen.getPrimaryDisplay().id);
      const primarySource = sources.find((source) => source.display_id === primaryDisplayId);
      const source = primarySource || sources[0];

      if (!source) {
        sendToMainWindow('host-log', {
          level: 'error',
          message: `no screen capture source found; macOS screen permission=${getScreenCaptureStatus()}`,
        });
        callback({});
        return;
      }

      sendToMainWindow('host-log', {
        level: 'info',
        message: `screen source selected: ${source.name || source.id}`,
      });
      callback({ video: source, audio: false });
    } catch (err) {
      sendToMainWindow('host-log', {
        level: 'error',
        message: `screen source selection failed: ${err && err.message ? err.message : String(err)}; macOS screen permission=${getScreenCaptureStatus()}`,
      });
      callback({});
    }
  }, {
    // For the LAN remote-desktop flow we already have an explicit app UI + PIN
    // pairing. Using the macOS system picker can leave getDisplayMedia()
    // pending in the background, so the Windows side can stay on "connecting
    // video stream" forever. Always use our handler and pick the primary
    // display directly.
    useSystemPicker: false,
  });
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
        sendToMainWindow('client-connected', { clientId, remoteAddress });
        sendToMainWindow('host-log', { level: 'info', message: `paired client ${clientId.slice(0, 8)} from ${remoteAddress}` });
        return;
      }

      // Relay WebRTC offer/ICE from paired client to the macOS host renderer.
      sendToMainWindow('host-log', { level: 'debug', message: `signal ${msg.type || 'unknown'} from ${clientId.slice(0, 8)}` });
      sendToMainWindow('signal-message', { clientId, message: msg });
    });

    socket.on('close', (code, reason) => {
      clearTimeout(helloTimer);
      clients.delete(clientId);
      sendToMainWindow('client-disconnected', { clientId, remoteAddress });
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
  startSignalServer();
  startDiscovery();
  win = createWindow('host.html', { title: 'P2P Remote LAN - macOS Host', frame: true });
}

async function startClient() {
  win = createWindow('client.html', { title: 'P2P Remote LAN - Windows Client', frame: true });
}

async function startDashboard() {
  startSignalServer();
  startDiscovery();
  win = createWindow('dashboard.html', { title: 'P2P Remote LAN' });
}

ipcMain.handle('host-info', () => {
  const primary = screen.getPrimaryDisplay();
  return {
    role: ROLE,
    port: SIGNAL_PORT,
    pin: PIN,
    addresses: lanAddresses(),
    display: primary.bounds,
    scaleFactor: primary.scaleFactor,
    platform: process.platform,
  };
});

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

ipcMain.handle('open-remote-window', (_event, device) => {
  if (!device || !device.address || !device.port || !device.pin) {
    throw new Error('Device is missing connection details');
  }
  createRemoteWindow(device);
  return true;
});

ipcMain.handle('remote-config', (event) => {
  return remoteConfigs.get(event.sender.id) || null;
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

ipcMain.on('signal-send', (_event, payload) => {
  if (!payload || !payload.clientId || !payload.message) return;
  const client = clients.get(payload.clientId);
  if (client && client.readyState === WebSocket.OPEN) {
    client.send(JSON.stringify(payload.message));
  }
});

ipcMain.on('input-event', async (_event, event) => {
  try {
    const bounds = screen.getPrimaryDisplay().bounds;
    await injectInput(event, bounds);
  } catch (err) {
    sendToMainWindow('host-log', {
      level: 'error',
      message: `input injection failed: ${err && err.message ? err.message : String(err)}`,
    });
  }
});

app.whenReady().then(async () => {
  registerDisplayMediaHandler();

  if (ROLE === 'host') await startHost();
  else if (ROLE === 'client') await startClient();
  else await startDashboard();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      if (ROLE === 'host') startHost();
      else if (ROLE === 'client') startClient();
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
  if (process.platform !== 'darwin') app.quit();
});
