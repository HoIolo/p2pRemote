const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const macHostDir = path.join(root, 'native-v2', 'mac-host');
const binaryName = 'p2p-native-mac-host';
const appName = 'P2P Native Mac Host';
const bundleId = 'com.p2premotelan.native.mac-host';
const appDir = path.join(macHostDir, '.build', `${appName}.app`);
const contentsDir = path.join(appDir, 'Contents');
const macosDir = path.join(contentsDir, 'MacOS');
const resourcesDir = path.join(contentsDir, 'Resources');
const builtBinary = path.join(macHostDir, '.build', 'release', binaryName);
const appBinary = path.join(macosDir, binaryName);

function plistEscape(value) {
  return String(value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function firstSigningIdentity() {
  try {
    const output = execFileSync('/usr/bin/security', ['find-identity', '-v', '-p', 'codesigning'], {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    });
    const identities = [...output.matchAll(/\)\s+[A-F0-9]+\s+"([^"]+)"/g)].map((match) => match[1]);
    return identities.find((name) => name.includes('Developer ID Application'))
      || identities.find((name) => name.includes('Apple Development'))
      || identities[0]
      || '-';
  } catch {
    return '-';
  }
}

function writeInfoPlist() {
  const plist = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>zh_CN</string>
  <key>CFBundleDisplayName</key>
  <string>${plistEscape(appName)}</string>
  <key>CFBundleExecutable</key>
  <string>${plistEscape(binaryName)}</string>
  <key>CFBundleIdentifier</key>
  <string>${plistEscape(bundleId)}</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>${plistEscape(appName)}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSMinimumSystemVersion</key>
  <string>13.0</string>
  <key>LSUIElement</key>
  <true/>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSScreenCaptureUsageDescription</key>
  <string>P2P Native Mac Host needs Screen Recording permission to stream this Mac when you start a remote session.</string>
  <key>NSAppleEventsUsageDescription</key>
  <string>P2P Native Mac Host uses local automation only while you explicitly control this Mac.</string>
</dict>
</plist>
`;
  fs.writeFileSync(path.join(contentsDir, 'Info.plist'), plist);
}

function createHelperApp() {
  if (!fs.existsSync(builtBinary)) {
    throw new Error(`expected native-v2 output missing: ${builtBinary}`);
  }
  fs.rmSync(appDir, { recursive: true, force: true });
  fs.mkdirSync(macosDir, { recursive: true });
  fs.mkdirSync(resourcesDir, { recursive: true });
  fs.copyFileSync(builtBinary, appBinary);
  fs.chmodSync(appBinary, 0o755);
  writeInfoPlist();
  fs.writeFileSync(path.join(contentsDir, 'PkgInfo'), 'APPL????');

  const identity = firstSigningIdentity();
  execFileSync('/usr/bin/codesign', ['--force', '--deep', '--sign', identity, appDir], {
    stdio: 'inherit',
  });
  console.log(`created: ${path.relative(root, appDir)} (${identity === '-' ? 'ad-hoc signed' : identity})`);
}

execFileSync('/usr/bin/swift', ['build', '-c', 'release'], {
  cwd: macHostDir,
  stdio: 'inherit',
});
createHelperApp();
