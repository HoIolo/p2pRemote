const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

let helper = null;
let helperPath = null;

function findHelper() {
  if (helperPath) return helperPath;
  const projectRoot = path.join(__dirname, '..');
  const exe = process.platform === 'win32' ? 'macos-input-helper.exe' : 'macos-input-helper';
  const candidates = [
    path.join(projectRoot, 'native', 'macos-input-helper', '.build', 'release', exe),
    path.join(projectRoot, 'bin', exe),
    process.resourcesPath ? path.join(process.resourcesPath, exe) : null,
    process.resourcesPath ? path.join(process.resourcesPath, 'macos-input-helper', exe) : null,
  ].filter(Boolean);

  helperPath = candidates.find((candidate) => fs.existsSync(candidate));
  if (!helperPath) {
    throw new Error(
      `macOS input helper not found. On the Mac host run: npm run build:mac-helper. ` +
        `Checked: ${candidates.join(', ')}`,
    );
  }
  return helperPath;
}

function ensureHelper() {
  if (process.platform !== 'darwin') return null;
  if (helper && !helper.killed && helper.exitCode === null) return helper;

  const binary = findHelper();
  helper = spawn(binary, [], {
    stdio: ['pipe', 'pipe', 'pipe'],
    windowsHide: true,
  });

  helper.stdout.on('data', (buf) => {
    const text = buf.toString('utf8').trim();
    if (text) console.log(`[macos-input-helper] ${text}`);
  });
  helper.stderr.on('data', (buf) => {
    const text = buf.toString('utf8').trim();
    if (text) console.error(`[macos-input-helper] ${text}`);
  });
  helper.on('exit', (code, signal) => {
    console.error(`[macos-input-helper] exited code=${code} signal=${signal}`);
    helper = null;
  });
  return helper;
}

async function injectInput(event, bounds) {
  if (!event || typeof event.kind !== 'string') return;
  const child = ensureHelper();
  if (!child) return;

  // Prefer fresh pointer state over old queued motion packets.
  if (event.kind === 'pointerMove' && child.stdin.writableLength > 4096) return;

  const payload = JSON.stringify({ event, bounds });
  child.stdin.write(`${payload}\n`);
}

module.exports = { injectInput };

