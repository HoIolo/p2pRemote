const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('lanRemote', {
  getHostInfo: () => ipcRenderer.invoke('host-info'),
  sendSignal: (payload) => ipcRenderer.send('signal-send', payload),
  sendInput: (payload) => ipcRenderer.send('input-event', payload),
  onSignalMessage: (callback) => ipcRenderer.on('signal-message', (_event, payload) => callback(payload)),
  onClientConnected: (callback) => ipcRenderer.on('client-connected', (_event, payload) => callback(payload)),
  onClientDisconnected: (callback) => ipcRenderer.on('client-disconnected', (_event, payload) => callback(payload)),
  onHostLog: (callback) => ipcRenderer.on('host-log', (_event, payload) => callback(payload)),
});
