const fs = require('fs');
const path = require('path');
const https = require('https');
const { spawn } = require('child_process');

const DOWNLOAD_TIMEOUT_MS = 10 * 60_000;

const RELEASES = Object.freeze({
  moonlight: Object.freeze({
    version: '6.1.0',
    winInstaller: 'https://github.com/moonlight-stream/moonlight-qt/releases/download/v6.1.0/MoonlightSetup-6.1.0.exe',
    winPortableX64: 'https://github.com/moonlight-stream/moonlight-qt/releases/download/v6.1.0/MoonlightPortable-x64-6.1.0.zip',
    macDmg: 'https://github.com/moonlight-stream/moonlight-qt/releases/download/v6.1.0/Moonlight-6.1.0.dmg',
  }),
  sunshine: Object.freeze({
    macArm64Dmg: 'https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-macOS-arm64.dmg',
    macX64Dmg: 'https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-macOS-x86_64.dmg',
    winInstallerX64: 'https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.exe',
    winPortableX64: 'https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-portable.zip',
  }),
  displayplacer: Object.freeze({
    macArm64: 'https://github.com/jakehilborn/displayplacer/releases/download/v1.4.0/displayplacer-apple-v140',
    macX64: 'https://github.com/jakehilborn/displayplacer/releases/download/v1.4.0/displayplacer-intel-v140',
  }),
});

function installStatus(platform, toolsDir, downloadDir) {
  const downloads = [];
  if (platform === 'win32') {
    downloads.push({ tool: 'moonlight', kind: 'installer', url: RELEASES.moonlight.winInstaller });
    downloads.push({ tool: 'moonlight', kind: 'portable', url: RELEASES.moonlight.winPortableX64 });
    downloads.push({ tool: 'sunshine', kind: 'installer', url: RELEASES.sunshine.winInstallerX64 });
    downloads.push({ tool: 'sunshine', kind: 'portable', url: RELEASES.sunshine.winPortableX64 });
  } else if (platform === 'darwin') {
    downloads.push({ tool: 'sunshine', kind: 'dmg', url: process.arch === 'arm64' ? RELEASES.sunshine.macArm64Dmg : RELEASES.sunshine.macX64Dmg });
    downloads.push({ tool: 'moonlight', kind: 'dmg', url: RELEASES.moonlight.macDmg });
    downloads.push({ tool: 'displayplacer', kind: 'binary', url: process.arch === 'arm64' ? RELEASES.displayplacer.macArm64 : RELEASES.displayplacer.macX64 });
  }
  return {
    toolsDir,
    downloadDir,
    supported: platform === 'win32' || platform === 'darwin',
    downloads,
  };
}

function assetFor(platform, arch, tool, mode = '') {
  const normalizedTool = ['sunshine', 'displayplacer'].includes(tool) ? tool : 'moonlight';
  const normalizedMode = String(mode || '').toLowerCase();
  if (platform === 'win32') {
    if (normalizedTool === 'moonlight') {
      return normalizedMode === 'portable'
        ? { tool: normalizedTool, mode: 'portable', url: RELEASES.moonlight.winPortableX64, filename: 'MoonlightPortable-x64.zip' }
        : { tool: normalizedTool, mode: 'installer', url: RELEASES.moonlight.winInstaller, filename: 'MoonlightSetup.exe' };
    }
    return normalizedMode === 'portable'
      ? { tool: normalizedTool, mode: 'portable', url: RELEASES.sunshine.winPortableX64, filename: 'Sunshine-Windows-AMD64-portable.zip' }
      : { tool: normalizedTool, mode: 'installer', url: RELEASES.sunshine.winInstallerX64, filename: 'Sunshine-Windows-AMD64-installer.exe' };
  }
  if (platform === 'darwin') {
    if (normalizedTool === 'sunshine') {
      return { tool: normalizedTool, mode: 'dmg', url: arch === 'arm64' ? RELEASES.sunshine.macArm64Dmg : RELEASES.sunshine.macX64Dmg, filename: `Sunshine-macOS-${arch === 'arm64' ? 'arm64' : 'x86_64'}.dmg` };
    }
    if (normalizedTool === 'displayplacer') {
      return { tool: normalizedTool, mode: 'binary', url: arch === 'arm64' ? RELEASES.displayplacer.macArm64 : RELEASES.displayplacer.macX64, filename: `displayplacer-${arch === 'arm64' ? 'apple' : 'intel'}-v140` };
    }
    return { tool: normalizedTool, mode: 'dmg', url: RELEASES.moonlight.macDmg, filename: 'Moonlight.dmg' };
  }
  throw new Error('自动安装目前只支持 Windows 和 macOS。Linux 请用系统包管理器安装 Sunshine/Moonlight。');
}

function downloadAsset(asset, downloadDir, emitProgress = () => {}) {
  fs.mkdirSync(downloadDir, { recursive: true });
  const targetPath = path.join(downloadDir, asset.filename);
  return new Promise((resolve, reject) => {
    const request = https.get(asset.url, {
      timeout: DOWNLOAD_TIMEOUT_MS,
      headers: { 'User-Agent': 'P2P-Remote-LAN' },
    }, (response) => {
      if ([301, 302, 303, 307, 308].includes(response.statusCode) && response.headers.location) {
        response.resume();
        downloadAsset({ ...asset, url: new URL(response.headers.location, asset.url).toString() }, downloadDir, emitProgress).then(resolve, reject);
        return;
      }
      if (response.statusCode !== 200) {
        response.resume();
        reject(new Error(`下载失败：HTTP ${response.statusCode}`));
        return;
      }
      const total = Number(response.headers['content-length'] || 0);
      let received = 0;
      const file = fs.createWriteStream(targetPath);
      response.on('data', (chunk) => {
        received += chunk.length;
        emitProgress({ phase: 'download', tool: asset.tool, mode: asset.mode, received, total, path: targetPath });
      });
      response.pipe(file);
      file.once('finish', () => file.close(() => resolve(targetPath)));
      file.once('error', reject);
    });
    request.once('timeout', () => request.destroy(new Error('下载超时')));
    request.once('error', reject);
  });
}

function runProcess(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const proc = spawn(command, args, {
      cwd: options.cwd || process.cwd(),
      env: options.env || process.env,
      shell: Boolean(options.shell),
      windowsHide: false,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    proc.stdout?.on('data', (chunk) => options.onOutput?.(chunk));
    proc.stderr?.on('data', (chunk) => options.onOutput?.(chunk));
    proc.once('error', reject);
    proc.once('exit', (code, signal) => {
      if (code === 0) resolve({ code, signal });
      else reject(new Error(`安装命令失败 code=${code ?? ''} signal=${signal ?? ''}`));
    });
  });
}

async function unzipWithPowerShell(zipPath, targetDir, onOutput) {
  fs.rmSync(targetDir, { recursive: true, force: true });
  fs.mkdirSync(targetDir, { recursive: true });
  await runProcess('powershell.exe', [
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-Command',
    'Expand-Archive -LiteralPath $env:ZIP_PATH -DestinationPath $env:TARGET_DIR -Force',
  ], {
    env: { ...process.env, ZIP_PATH: zipPath, TARGET_DIR: targetDir },
    onOutput,
  });
}

async function installWindowsAsset(asset, filePath, toolDir, emitProgress, onOutput) {
  if (asset.mode === 'portable') {
    emitProgress({ phase: 'extract', tool: asset.tool, mode: asset.mode, path: toolDir });
    await unzipWithPowerShell(filePath, toolDir, onOutput);
    return { installed: true, mode: asset.mode, path: toolDir };
  }
  emitProgress({ phase: 'installer', tool: asset.tool, mode: asset.mode, path: filePath });
  await runProcess(filePath, ['/S'], { cwd: path.dirname(filePath), onOutput });
  return { installed: true, mode: asset.mode, path: filePath };
}

async function installMacBinaryAsset(asset, filePath, emitProgress) {
  const targetDir = path.join(process.env.HOME || '/usr/local', '.p2p-remote-lan', 'bin');
  const target = path.join(targetDir, asset.tool === 'displayplacer' ? 'displayplacer' : asset.filename);
  emitProgress({ phase: 'copy', tool: asset.tool, mode: asset.mode, path: target });
  fs.mkdirSync(targetDir, { recursive: true });
  fs.copyFileSync(filePath, target);
  fs.chmodSync(target, 0o755);
  return { installed: true, mode: asset.mode, path: target };
}

async function installMacAsset(asset, filePath, emitProgress, onOutput) {
  const mountPoint = path.join('/Volumes', asset.tool === 'sunshine' ? 'Sunshine' : 'Moonlight');
  emitProgress({ phase: 'mount', tool: asset.tool, mode: asset.mode, path: filePath });
  await runProcess('/usr/bin/hdiutil', ['attach', filePath, '-nobrowse', '-quiet'], { onOutput });
  try {
    const appName = asset.tool === 'sunshine' ? 'Sunshine.app' : 'Moonlight.app';
    const source = path.join(mountPoint, appName);
    const target = path.join('/Applications', appName);
    emitProgress({ phase: 'copy', tool: asset.tool, mode: asset.mode, path: target });
    fs.rmSync(target, { recursive: true, force: true });
    fs.cpSync(source, target, { recursive: true });
    return { installed: true, mode: asset.mode, path: target };
  } finally {
    await runProcess('/usr/bin/hdiutil', ['detach', mountPoint, '-quiet'], { onOutput }).catch(() => null);
  }
}

async function installTool({ platform, arch, tool, mode, downloadDir, toolDir, emitProgress, onOutput }) {
  const asset = assetFor(platform, arch, tool, mode);
  emitProgress({ phase: 'start', tool: asset.tool, mode: asset.mode, url: asset.url });
  const filePath = await downloadAsset(asset, downloadDir, emitProgress);
  let result;
  if (platform === 'win32') result = await installWindowsAsset(asset, filePath, toolDir, emitProgress, onOutput);
  else if (platform === 'darwin') result = asset.mode === 'binary'
    ? await installMacBinaryAsset(asset, filePath, emitProgress)
    : await installMacAsset(asset, filePath, emitProgress, onOutput);
  else throw new Error('自动安装目前只支持 Windows 和 macOS。');
  emitProgress({ phase: 'done', tool: asset.tool, mode: asset.mode, result });
  return { ok: true, asset, filePath, result };
}

module.exports = {
  RELEASES,
  installStatus,
  assetFor,
  downloadAsset,
  installTool,
};
