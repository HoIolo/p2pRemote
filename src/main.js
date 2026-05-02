const { app, BrowserWindow, ipcMain, screen, dialog } = require('electron');
const crypto = require('crypto');
const os = require('os');
const path = require('path');
const WebSocket = require('ws');
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

  // Double-click packaged behavior for the intended product split:
  // macOS runs as the controlled host, Windows runs as the controller.
  return process.platform === 'darwin' ? 'host' : 'client';
}

const ROLE = resolveRole();
const SIGNAL_PORT = Number(process.env.P2P_REMOTE_PORT || 7777);
const PIN = String(crypto.randomInt(100000, 999999));

let win = null;
let wss = null;
const clients = new Map();

const gotSingleInstanceLock = app.requestSingleInstanceLock();
if (!gotSingleInstanceLock) app.quit();
else {
  app.on('second-instance', () => {
    if (!win) return;
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

function createWindow(file, options = {}) {
  const browserWindow = new BrowserWindow({
    width: options.width || 1180,
    height: options.height || 760,
    minWidth: 900,
    minHeight: 560,
    backgroundColor: '#0b1020',
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

function startSignalServer() {
  wss = new WebSocket.Server({ port: SIGNAL_PORT, host: '0.0.0.0' });

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
        win?.webContents.send('client-connected', { clientId, remoteAddress });
        return;
      }

      // Relay WebRTC offer/ICE from paired client to the macOS host renderer.
      win?.webContents.send('signal-message', { clientId, message: msg });
    });

    socket.on('close', () => {
      clearTimeout(helloTimer);
      clients.delete(clientId);
      win?.webContents.send('client-disconnected', { clientId, remoteAddress });
    });
  });

  wss.on('error', (err) => {
    dialog.showErrorBox('Signal server failed', `${err.message}\nPort: ${SIGNAL_PORT}`);
  });
}

async function startHost() {
  startSignalServer();
  win = createWindow('host.html', { title: 'P2P Remote LAN - macOS Host' });
}

async function startClient() {
  win = createWindow('client.html', { title: 'P2P Remote LAN - Windows Client' });
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
    win?.webContents.send('host-log', {
      level: 'error',
      message: `input injection failed: ${err && err.message ? err.message : String(err)}`,
    });
  }
});

app.whenReady().then(async () => {
  if (ROLE === 'host') await startHost();
  else await startClient();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      if (ROLE === 'host') startHost();
      else startClient();
    }
  });
});

app.on('window-all-closed', () => {
  if (wss) wss.close();
  if (process.platform !== 'darwin') app.quit();
});


