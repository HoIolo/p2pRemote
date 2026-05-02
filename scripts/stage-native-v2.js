const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const stageRoot = path.join(root, 'build', 'native-v2');

fs.rmSync(stageRoot, { recursive: true, force: true });
fs.mkdirSync(path.join(stageRoot, 'win-client'), { recursive: true });
fs.mkdirSync(path.join(stageRoot, 'mac-host'), { recursive: true });

function copyIfExists(from, to) {
  if (!fs.existsSync(from)) {
    console.log(`[native-v2] skip missing ${from}`);
    return;
  }
  fs.mkdirSync(path.dirname(to), { recursive: true });
  fs.copyFileSync(from, to);
  console.log(`[native-v2] staged ${from} -> ${to}`);
}

copyIfExists(
  path.join(root, 'native-v2', 'win-client', 'build', 'Release', 'p2p-native-win-client.exe'),
  path.join(stageRoot, 'win-client', 'p2p-native-win-client.exe')
);
copyIfExists(
  path.join(root, 'native-v2', 'mac-host', '.build', 'release', 'p2p-native-mac-host'),
  path.join(stageRoot, 'mac-host', 'p2p-native-mac-host')
);
