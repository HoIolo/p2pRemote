const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('lanRemote', {
  getAppInfo: () => ipcRenderer.invoke('app-info'),
  getHostInfo: () => ipcRenderer.invoke('host-info'),
  getDevices: () => ipcRenderer.invoke('devices-list'),
  refreshDevices: () => ipcRenderer.invoke('refresh-devices'),
  openRemoteWindow: (device) => ipcRenderer.invoke('open-remote-window', device),
  getRemoteConfig: () => ipcRenderer.invoke('remote-config'),
  saveDevicePreview: (id, dataUrl) => ipcRenderer.invoke('save-device-preview', id, dataUrl),
  setWindowFullscreen: (fullScreen) => ipcRenderer.invoke('set-window-fullscreen', fullScreen),
  windowAction: (action) => ipcRenderer.invoke('window-action', action),
  getScreenCaptureStatus: () => ipcRenderer.invoke('screen-capture-status'),
  openScreenCaptureSettings: () => ipcRenderer.invoke('open-screen-capture-settings'),
  resetScreenCapturePermission: () => ipcRenderer.invoke('reset-screen-capture-permission'),
  hostRendererReady: () => ipcRenderer.send('host-renderer-ready'),
  sendSignal: (payload) => ipcRenderer.send('signal-send', payload),
  sendInput: (payload) => ipcRenderer.send('input-event', payload),
  onSignalMessage: (callback) => ipcRenderer.on('signal-message', (_event, payload) => callback(payload)),
  onClientConnected: (callback) => ipcRenderer.on('client-connected', (_event, payload) => callback(payload)),
  onClientDisconnected: (callback) => ipcRenderer.on('client-disconnected', (_event, payload) => callback(payload)),
  onDevicesUpdated: (callback) => ipcRenderer.on('devices-updated', (_event, payload) => callback(payload)),
  onHostLog: (callback) => ipcRenderer.on('host-log', (_event, payload) => callback(payload)),
});
