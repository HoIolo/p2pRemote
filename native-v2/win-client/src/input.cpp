#include "input.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

uint16_t VkToMacKeyCode(WPARAM vk, LPARAM lp) {
  if (vk == VK_SHIFT) {
    vk = MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
  } else if (vk == VK_CONTROL) {
    vk = (lp & 0x01000000) ? VK_RCONTROL : VK_LCONTROL;
  } else if (vk == VK_MENU) {
    vk = (lp & 0x01000000) ? VK_RMENU : VK_LMENU;
  }
  if (vk >= 'A' && vk <= 'Z') {
    static const uint16_t map[26] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    return map[vk - 'A'];
  }
  if (vk >= '0' && vk <= '9') {
    static const uint16_t map[10] = {29,18,19,20,21,23,22,26,28,25};
    return map[vk - '0'];
  }
  switch (vk) {
    case VK_RETURN: return 36; case VK_TAB: return 48; case VK_SPACE: return 49; case VK_BACK: return 51; case VK_ESCAPE: return 53;
    case VK_LEFT: return 123; case VK_RIGHT: return 124; case VK_DOWN: return 125; case VK_UP: return 126;
    case VK_DELETE: return 117; case VK_HOME: return 115; case VK_END: return 119; case VK_PRIOR: return 116; case VK_NEXT: return 121;
    case VK_LSHIFT: case VK_SHIFT: return 56; case VK_RSHIFT: return 60;
    case VK_LCONTROL: case VK_CONTROL: return 59; case VK_RCONTROL: return 62;
    case VK_LMENU: case VK_MENU: return 58; case VK_RMENU: return 61;
    case VK_LWIN: return 55; case VK_RWIN: return 54; case VK_CAPITAL: return 57;
    case VK_OEM_MINUS: return 27; case VK_OEM_PLUS: return 24; case VK_OEM_4: return 33; case VK_OEM_6: return 30;
    case VK_OEM_5: return 42; case VK_OEM_1: return 41; case VK_OEM_7: return 39; case VK_OEM_COMMA: return 43;
    case VK_OEM_PERIOD: return 47; case VK_OEM_2: return 44; case VK_OEM_3: return 50;
    case VK_NUMPAD0: return 82; case VK_NUMPAD1: return 83; case VK_NUMPAD2: return 84; case VK_NUMPAD3: return 85; case VK_NUMPAD4: return 86;
    case VK_NUMPAD5: return 87; case VK_NUMPAD6: return 88; case VK_NUMPAD7: return 89; case VK_NUMPAD8: return 91; case VK_NUMPAD9: return 92;
    case VK_DECIMAL: return 65; case VK_MULTIPLY: return 67; case VK_ADD: return 69; case VK_DIVIDE: return 75; case VK_SUBTRACT: return 78;
    default:
      if (vk >= VK_F1 && vk <= VK_F12) {
        static const uint16_t f[12] = {122,120,99,118,96,97,98,100,101,109,103,111};
        return f[vk - VK_F1];
      }
      return 0xffff;
  }
}


bool IsModifierVirtualKey(WPARAM vk) {
  switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
      return true;
    default:
      return false;
  }
}

bool HasNonTextModifierDown() {
  return (GetKeyState(VK_CONTROL) & 0x8000) ||
         (GetKeyState(VK_MENU) & 0x8000) ||
         (GetKeyState(VK_LWIN) & 0x8000) ||
         (GetKeyState(VK_RWIN) & 0x8000);
}

bool IsTextVirtualKey(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return true;
  if (vk >= '0' && vk <= '9') return true;
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return true;
  switch (vk) {
    case VK_SPACE: case VK_OEM_MINUS: case VK_OEM_PLUS: case VK_OEM_4:
    case VK_OEM_6: case VK_OEM_5: case VK_OEM_1: case VK_OEM_7:
    case VK_OEM_COMMA: case VK_OEM_PERIOD: case VK_OEM_2: case VK_OEM_3:
    case VK_DECIMAL: case VK_MULTIPLY: case VK_ADD: case VK_DIVIDE: case VK_SUBTRACT:
      return true;
    default:
      return false;
  }
}

uint16_t MacModifierMaskForCurrentWinKeys() {
  uint16_t mask = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) mask |= P2_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) mask |= P2_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000) mask |= P2_MOD_OPTION;
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mask |= P2_MOD_COMMAND;
  return mask;
}

bool SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode) {
  if (g_inputSock == INVALID_SOCKET) return false;
  P2InputPacket p{};
  memcpy(p.magic, "P2I2", 4);
  p.version = P2_VERSION;
  p.kind = kind;
  p.bytes = sizeof(P2InputPacket);
  p.seq = g_inputSeq.fetch_add(1);
  p.x = x; p.y = y; p.dx = dx; p.dy = dy; p.button = button; p.keyCode = keyCode;
  int rc = sendto(g_inputSock, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&g_inputAddr), sizeof(g_inputAddr));
  return rc == sizeof(p);
}

bool InitInputSocket() {
  g_inputSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_inputSock == INVALID_SOCKET) return false;
  u_long nonBlocking = 1;
  ioctlsocket(g_inputSock, FIONBIO, &nonBlocking);
  int sndbuf = 64 * 1024;
  setsockopt(g_inputSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
  int tos = 0x10; // IPTOS_LOWDELAY
  setsockopt(g_inputSock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
  g_inputAddr.sin_family = AF_INET;
  g_inputAddr.sin_port = htons(g_cfg.inputPort);
  std::string ip = WideToUtf8(g_cfg.hostIp);
  return inet_pton(AF_INET, ip.c_str(), &g_inputAddr.sin_addr) == 1;
}
