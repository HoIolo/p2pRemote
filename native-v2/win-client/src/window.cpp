#include "window.h"

#include "input.h"
#include "overlay.h"
#include "renderer.h"

#include <windowsx.h>

#include <algorithm>

static void NormalizedPoint(HWND hwnd, LPARAM lp, float& x, float& y) {
  RECT rc{}; GetClientRect(hwnd, &rc);
  int w = std::max(1L, rc.right - rc.left);
  int h = std::max(1L, rc.bottom - rc.top);
  x = std::clamp(float(GET_X_LPARAM(lp)) / float(w), 0.0f, 1.0f);
  y = std::clamp(float(GET_Y_LPARAM(lp)) / float(h), 0.0f, 1.0f);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      SetFocus(hwnd);
      SetTimer(hwnd, 1, 500, nullptr);
      return 0;
    case WM_TIMER: {
      SendInputPacket(P2_INPUT_HEARTBEAT, 0, 0, 0, 0, 0, 0);
      MaybeSendClientStats(false);
      static uint64_t lastFrames = 0;
      static uint64_t lastComplete = 0;
      static uint64_t lastPackets = 0;
      static uint64_t lastBytes = 0;
      static uint64_t lastClientDropped = 0;
      static uint64_t lastNetworkDropped = 0;
      static uint64_t lastQpc = QpcNow();
      uint64_t now = QpcNow();
      uint64_t frames = g_framesPresented.load(std::memory_order_relaxed);
      uint64_t complete = g_framesComplete.load(std::memory_order_relaxed);
      uint64_t packets = g_packetsRx.load(std::memory_order_relaxed);
      uint64_t bytes = g_bytesRx.load(std::memory_order_relaxed);
      double seconds = double(QpcDeltaUs(lastQpc, now)) / 1'000'000.0;
      const uint64_t frameDelta = frames >= lastFrames ? (frames - lastFrames) : frames;
      const uint64_t completeDelta = complete >= lastComplete ? (complete - lastComplete) : complete;
      const uint64_t packetDelta = packets >= lastPackets ? (packets - lastPackets) : packets;
      const uint64_t byteDelta = bytes >= lastBytes ? (bytes - lastBytes) : bytes;
      double fps = seconds > 0.001 ? double(frameDelta) / seconds : 0.0;
      double cfps = seconds > 0.001 ? double(completeDelta) / seconds : 0.0;
      double pps = seconds > 0.001 ? double(packetDelta) / seconds : 0.0;
      double mbps = seconds > 0.001 ? double(byteDelta) * 8.0 / seconds / 1'000'000.0 : 0.0;
      uint64_t gpu = g_gpuFrames.load(std::memory_order_relaxed);
      uint64_t cpu = g_cpuFrames.load(std::memory_order_relaxed);
      double rxMs = double(g_lastRxToPresentUs.load(std::memory_order_relaxed)) / 1000.0;
      double packetAgeMs = double(QpcDeltaUs(g_lastPacketQpc.load(std::memory_order_relaxed), now)) / 1000.0;
      double frameAgeMs = double(QpcDeltaUs(g_lastCompleteQpc.load(std::memory_order_relaxed), now)) / 1000.0;
      uint64_t dropped = g_framesDropped.load(std::memory_order_relaxed);
      uint64_t clientDropped = g_clientFramesDropped.load(std::memory_order_relaxed);
      uint64_t networkDropped = g_networkFramesDropped.load(std::memory_order_relaxed);
      uint64_t decodeFails = g_decodeFails.load(std::memory_order_relaxed);
      uint64_t gpuRenderFails = g_gpuRenderFails.load(std::memory_order_relaxed);
      NativeUiStats stats{};
      stats.presentFps = fps;
      stats.completeFps = cfps;
      stats.mbps = mbps;
      stats.packetRate = pps;
      stats.rxToPresentMs = rxMs;
      stats.packetAgeMs = packetAgeMs;
      stats.frameAgeMs = frameAgeMs;
      stats.presentJitterMs = double(g_presentJitterUs.load(std::memory_order_relaxed)) / 1000.0;
      stats.rxToPresentMaxMs = double(g_maxRxToPresentUs.load(std::memory_order_relaxed)) / 1000.0;
      stats.dropped = dropped;
      stats.clientDropped = clientDropped;
      stats.networkDropped = networkDropped;
      stats.decodeFails = decodeFails;
      stats.gpuRenderFails = gpuRenderFails;
      stats.gpuFrames = gpu;
      stats.cpuFrames = cpu;
      stats.queueDepth = g_encodedQueueDepthNow.load(std::memory_order_relaxed);
      stats.queueTarget = g_encodedQueueTargetNow.load(std::memory_order_relaxed);
      stats.decodedQueueDepth = g_decodedQueueDepthNow.load(std::memory_order_relaxed);
      stats.decodedQueueTarget = g_decodedQueueTargetNow.load(std::memory_order_relaxed);
      stats.renderDropped = g_renderFramesDropped.load(std::memory_order_relaxed);
      stats.fecRecovered = g_fecRecoveredFrames.load(std::memory_order_relaxed);
      stats.keyframeRequests = g_keyframeRequests.load(std::memory_order_relaxed);
      stats.adaptiveBitrate = g_currentBitrate.load(std::memory_order_relaxed);
      const uint64_t clientDroppedDelta = clientDropped >= lastClientDropped ? (clientDropped - lastClientDropped) : clientDropped;
      const uint64_t networkDroppedDelta = networkDropped >= lastNetworkDropped ? (networkDropped - lastNetworkDropped) : networkDropped;
      // completeDelta is computed before rolling lastComplete forward, otherwise drop % is always wrong.
      const uint64_t clientKnownFrames = completeDelta + clientDroppedDelta;
      const uint64_t networkKnownFrames = completeDelta + networkDroppedDelta;
      stats.networkDropPct = networkKnownFrames ? (double(networkDroppedDelta) * 100.0 / double(networkKnownFrames)) : 0.0;
      stats.queueDropPct = clientKnownFrames ? (double(clientDroppedDelta) * 100.0 / double(clientKnownFrames)) : 0.0;
      lastFrames = frames;
      lastComplete = complete;
      lastPackets = packets;
      lastBytes = bytes;
      lastQpc = now;
      lastClientDropped = clientDropped;
      lastNetworkDropped = networkDropped;
      StoreUiStats(stats);

      wchar_t title[512];
      swprintf_s(title, L"P2P Native v2 %s -> %s | present %.0f fps complete %.0f fps | %.1f Mbps %.0f pkt/s | last pkt %.0f ms frame %.0f ms | rx-present %.2f ms | drop local %llu net %llu | GPU %llu CPU %llu | bitrate %d Mbps",
                 g_cfg.udpVideo ? L"UDP" : L"TCP", g_cfg.hostIp.c_str(), fps, cfps, mbps, pps, packetAgeMs, frameAgeMs, rxMs,
                 static_cast<unsigned long long>(clientDropped),
                 static_cast<unsigned long long>(networkDropped),
                 static_cast<unsigned long long>(gpu),
                 static_cast<unsigned long long>(cpu),
                 std::max(1, g_currentBitrate.load(std::memory_order_relaxed) / 1'000'000));
      SetWindowTextW(hwnd, title);
      if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
      if (g_menuHwnd && IsWindowVisible(g_menuHwnd)) InvalidateRect(g_menuHwnd, nullptr, FALSE);
      if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) InvalidateRect(g_statsHwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOVE:
      UpdateOverlayLayout();
      return 0;
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {
        HideNativePopups();
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_HIDE);
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) {
        HideNativePopups();
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_HIDE);
      } else {
        if (g_toolbarHwnd) ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
        UpdateOverlayLayout();
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT && g_framesPresented.load(std::memory_order_relaxed) > 0) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = (wp & MK_RBUTTON) ? 2 : ((wp & MK_MBUTTON) ? 1 : 0);
      SendInputPacket(P2_INPUT_MOVE, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: {
      if (NativeOverlayIsOpen()) HideNativePopups();
      SetCapture(hwnd); SetFocus(hwnd);
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONDOWN ? 2 : (msg == WM_MBUTTONDOWN ? 1 : 0);
      SendInputPacket(P2_INPUT_DOWN, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: {
      ReleaseCapture();
      float x, y; NormalizedPoint(hwnd, lp, x, y);
      uint16_t b = msg == WM_RBUTTONUP ? 2 : (msg == WM_MBUTTONUP ? 1 : 0);
      SendInputPacket(P2_INPUT_UP, x, y, 0, 0, b, 0);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &pt);
      LPARAM clp = MAKELPARAM(pt.x, pt.y);
      float x, y; NormalizedPoint(hwnd, clp, x, y);
      SendInputPacket(P2_INPUT_WHEEL, x, y, 0, -GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
      return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
      if (wp == VK_F11) {
        ToggleNativeFullscreen();
        return 0;
      }
      if (wp == VK_ESCAPE && ((g_menuHwnd && IsWindowVisible(g_menuHwnd)) || (g_statsHwnd && IsWindowVisible(g_statsHwnd)))) {
        HideNativePopups();
        return 0;
      }
      if (!IsModifierVirtualKey(wp) && !HasNonTextModifierDown() && IsTextVirtualKey(wp)) return 0;
      uint16_t mac = VkToMacKeyCode(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_DOWN, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
      if (!IsModifierVirtualKey(wp) && !HasNonTextModifierDown() && IsTextVirtualKey(wp)) return 0;
      uint16_t mac = VkToMacKeyCode(wp, lp);
      if (mac != 0xffff) SendInputPacket(P2_INPUT_KEY_UP, 0, 0, 0, 0, 0, mac);
      return 0;
    }
    case WM_CHAR: {
      if (wp >= 0x20 && wp != 0x7f) {
        SendInputPacket(P2_INPUT_TEXT, 0, 0, 0, 0, MacModifierMaskForCurrentWinKeys(), static_cast<uint16_t>(wp & 0xffff));
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      if (g_renderer && g_framesPresented.load(std::memory_order_relaxed) > 0) {
        EndPaint(hwnd, &ps);
        return 0;
      }
      BgraFrame frame;
      {
        std::lock_guard lk(g_frameMu);
        frame = g_latestFrame;
      }
      if (!frame.bytes.empty()) {
        RECT rc{}; GetClientRect(hwnd, &rc);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = frame.width;
        bmi.bmiHeader.biHeight = -frame.height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, frame.width, frame.height, frame.bytes.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
      } else {
        RECT rc{}; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(hdc, RGB(220, 230, 255)); SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, L"Waiting for native v2 video stream...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, 1);
  g_running.store(false);
  g_encodedCv.notify_all();
  g_decodedCv.notify_all();
  if (g_toolbarHwnd) { DestroyWindow(g_toolbarHwnd); g_toolbarHwnd = nullptr; }
  if (g_menuHwnd) { DestroyWindow(g_menuHwnd); g_menuHwnd = nullptr; }
  if (g_statsHwnd) { DestroyWindow(g_statsHwnd); g_statsHwnd = nullptr; }
  PostQuitMessage(g_exitCode);
  return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}
