const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

let helper = null;
let helperPath = null;
let warnedUnsupported = false;

function findHelper() {
  if (helperPath) return helperPath;
  const projectRoot = path.join(__dirname, '..');
  const exe = process.platform === 'win32' ? 'macos-input-helper.exe' : 'macos-input-helper';
  const candidates = [
    path.join(projectRoot, 'native', 'macos-input-helper', '.build', 'apple', 'Products', 'Release', exe),
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

function windowsHelperScript() {
  return `
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class RemoteInput {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, int data, UIntPtr extraInfo);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extraInfo);
}
"@

$vk = @{
  KeyA=0x41; KeyB=0x42; KeyC=0x43; KeyD=0x44; KeyE=0x45; KeyF=0x46; KeyG=0x47; KeyH=0x48; KeyI=0x49; KeyJ=0x4A; KeyK=0x4B; KeyL=0x4C; KeyM=0x4D; KeyN=0x4E; KeyO=0x4F; KeyP=0x50; KeyQ=0x51; KeyR=0x52; KeyS=0x53; KeyT=0x54; KeyU=0x55; KeyV=0x56; KeyW=0x57; KeyX=0x58; KeyY=0x59; KeyZ=0x5A;
  Digit0=0x30; Digit1=0x31; Digit2=0x32; Digit3=0x33; Digit4=0x34; Digit5=0x35; Digit6=0x36; Digit7=0x37; Digit8=0x38; Digit9=0x39;
  Enter=0x0D; Tab=0x09; Space=0x20; Backspace=0x08; Escape=0x1B; Delete=0x2E; Home=0x24; End=0x23; PageUp=0x21; PageDown=0x22;
  ArrowLeft=0x25; ArrowUp=0x26; ArrowRight=0x27; ArrowDown=0x28;
  ShiftLeft=0xA0; ShiftRight=0xA1; ControlLeft=0xA2; ControlRight=0xA3; AltLeft=0xA4; AltRight=0xA5; MetaLeft=0x5B; MetaRight=0x5C;
  Minus=0xBD; Equal=0xBB; BracketLeft=0xDB; BracketRight=0xDD; Backslash=0xDC; Semicolon=0xBA; Quote=0xDE; Backquote=0xC0; Comma=0xBC; Period=0xBE; Slash=0xBF;
  F1=0x70; F2=0x71; F3=0x72; F4=0x73; F5=0x74; F6=0x75; F7=0x76; F8=0x77; F9=0x78; F10=0x79; F11=0x7A; F12=0x7B
}

function Clamp01($value) {
  if ($null -eq $value) { return 0.0 }
  $n = [double]$value
  if ($n -lt 0) { return 0.0 }
  if ($n -gt 1) { return 1.0 }
  return $n
}

while (($line = [Console]::In.ReadLine()) -ne $null) {
  try {
    $msg = $line | ConvertFrom-Json
    $event = $msg.event
    $bounds = $msg.bounds
    $x = [int]([double]$bounds.x + (Clamp01 $event.x) * [Math]::Max(1.0, [double]$bounds.width - 1.0))
    $y = [int]([double]$bounds.y + (Clamp01 $event.y) * [Math]::Max(1.0, [double]$bounds.height - 1.0))

    switch ($event.kind) {
      'pointerMove' { [RemoteInput]::SetCursorPos($x, $y) | Out-Null }
      'pointerDown' {
        [RemoteInput]::SetCursorPos($x, $y) | Out-Null
        if ($event.button -eq 2) { [RemoteInput]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero) }
        elseif ($event.button -eq 1) { [RemoteInput]::mouse_event(0x0020, 0, 0, 0, [UIntPtr]::Zero) }
        else { [RemoteInput]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) }
      }
      'pointerUp' {
        [RemoteInput]::SetCursorPos($x, $y) | Out-Null
        if ($event.button -eq 2) { [RemoteInput]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero) }
        elseif ($event.button -eq 1) { [RemoteInput]::mouse_event(0x0040, 0, 0, 0, [UIntPtr]::Zero) }
        else { [RemoteInput]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) }
      }
      'wheel' {
        [RemoteInput]::SetCursorPos($x, $y) | Out-Null
        [RemoteInput]::mouse_event(0x0800, 0, 0, [int](-[double]$event.dy), [UIntPtr]::Zero)
      }
      'keyDown' {
        if ($vk.ContainsKey($event.code)) { [RemoteInput]::keybd_event([byte]$vk[$event.code], 0, 0, [UIntPtr]::Zero) }
      }
      'keyUp' {
        if ($vk.ContainsKey($event.code)) { [RemoteInput]::keybd_event([byte]$vk[$event.code], 0, 0x0002, [UIntPtr]::Zero) }
      }
    }
  } catch {
    [Console]::Error.WriteLine("bad input json: " + $_.Exception.Message)
  }
}
`;
}

function ensureWindowsHelper() {
  if (process.platform !== 'win32') return null;
  if (helper && !helper.killed && helper.exitCode === null) return helper;

  helper = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', windowsHelperScript()], {
    stdio: ['pipe', 'pipe', 'pipe'],
    windowsHide: true,
  });

  helper.stderr.on('data', (buf) => {
    const text = buf.toString('utf8').trim();
    if (text) console.error(`[win-input-helper] ${text}`);
  });
  helper.on('exit', (code, signal) => {
    console.error(`[win-input-helper] exited code=${code} signal=${signal}`);
    helper = null;
  });
  return helper;
}

async function injectInput(event, bounds) {
  if (!event || typeof event.kind !== 'string') return;
  const child = process.platform === 'darwin' ? ensureHelper() : ensureWindowsHelper();
  if (!child && process.platform !== 'darwin' && process.platform !== 'win32' && !warnedUnsupported) {
    warnedUnsupported = true;
    throw new Error(`input injection is not supported on ${process.platform}`);
  }
  if (!child) return;

  // Prefer fresh pointer state over old queued motion packets.
  if (event.kind === 'pointerMove' && child.stdin.writableLength > 4096) return;

  const payload = JSON.stringify({ event, bounds });
  child.stdin.write(`${payload}\n`);
}

module.exports = { injectInput };
