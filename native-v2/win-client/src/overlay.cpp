#include "overlay.h"

#include "input.h"
#include "video_pipeline.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

static constexpr int kToolbarIconSize = 42;
static constexpr int kToolbarWidth = 430;
static constexpr int kToolbarHeight = 48;
static constexpr int kMenuWidth = 360;
static constexpr int kMenuHeight = 390;
static constexpr int kStatsWidth = 430;
static constexpr int kStatsHeight = 708;
static constexpr wchar_t kNativeClientVersion[] = L"native v2 0.1.0";

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static HFONT CreateUiFont(int px, int weight = FW_NORMAL) {
  return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}


static void DrawTextRect(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, HFONT font, UINT format) {
  HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, font));
  SetTextColor(hdc, color);
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, text.c_str(), -1, &rc, format);
  SelectObject(hdc, oldFont);
}

static void DrawSignalBars(HDC hdc, int x, int y, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, brush));
  HPEN pen = CreatePen(PS_SOLID, 1, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  const int widths[] = {4, 4, 4};
  const int heights[] = {11, 18, 25};
  for (int i = 0; i < 3; ++i) {
    int left = x + i * 9;
    RoundRect(hdc, left, y + 28 - heights[i], left + widths[i], y + 28, 4, 4);
  }
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(pen);
  DeleteObject(brush);
}

static void DrawMenuIcon(HDC hdc, int kind, int x, int y, COLORREF color, int size = 22) {
  const int oldMode = SetMapMode(hdc, MM_ANISOTROPIC);
  SIZE oldWindowExt{}, oldViewportExt{};
  POINT oldViewportOrg{}, oldWindowOrg{};
  GetWindowExtEx(hdc, &oldWindowExt);
  GetViewportExtEx(hdc, &oldViewportExt);
  GetViewportOrgEx(hdc, &oldViewportOrg);
  GetWindowOrgEx(hdc, &oldWindowOrg);
  SetWindowExtEx(hdc, 28, 28, nullptr);
  SetViewportExtEx(hdc, size, size, nullptr);
  SetViewportOrgEx(hdc, x, y, nullptr);
  SetWindowOrgEx(hdc, 0, 0, nullptr);

  HPEN pen = CreatePen(PS_SOLID, 2, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));

  switch (kind) {
    case 0:
      RoundRect(hdc, 2, 3, 24, 20, 4, 4);
      MoveToEx(hdc, 13, 20, nullptr); LineTo(hdc, 13, 25);
      MoveToEx(hdc, 8, 25, nullptr); LineTo(hdc, 18, 25);
      break;
    case 1:
      Rectangle(hdc, 3, 4, 24, 23);
      MoveToEx(hdc, 6, 17, nullptr); LineTo(hdc, 12, 11); LineTo(hdc, 16, 15); LineTo(hdc, 22, 8);
      Ellipse(hdc, 7, 6, 12, 11);
      break;
    case 2:
      RoundRect(hdc, 3, 3, 24, 24, 4, 4);
      MoveToEx(hdc, 9, 3, nullptr); LineTo(hdc, 9, 10); LineTo(hdc, 3, 10);
      MoveToEx(hdc, 18, 24, nullptr); LineTo(hdc, 18, 17); LineTo(hdc, 24, 17);
      break;
    case 3:
      MoveToEx(hdc, 4, 14, nullptr); LineTo(hdc, 10, 14); LineTo(hdc, 17, 7); LineTo(hdc, 17, 21); LineTo(hdc, 10, 14);
      Arc(hdc, 12, 7, 28, 21, 22, 8, 22, 20);
      break;
    case 4: {
      POINT pts[] = {{14, 2}, {24, 6}, {21, 20}, {14, 26}, {7, 20}, {4, 6}};
      Polygon(hdc, pts, ARRAYSIZE(pts));
      MoveToEx(hdc, 10, 14, nullptr); LineTo(hdc, 13, 17); LineTo(hdc, 19, 10);
      break;
    }
    case 5:
      MoveToEx(hdc, 14, 3, nullptr); LineTo(hdc, 14, 23);
      MoveToEx(hdc, 8, 10, nullptr); LineTo(hdc, 14, 16); LineTo(hdc, 21, 9);
      Ellipse(hdc, 11, 21, 17, 27);
      Rectangle(hdc, 18, 6, 24, 12);
      break;
    case 6:
      for (int yy = 0; yy < 2; ++yy) for (int xx = 0; xx < 2; ++xx) {
        RoundRect(hdc, 3 + xx * 12, 4 + yy * 12, 11 + xx * 12, 12 + yy * 12, 3, 3);
      }
      break;
    case 7:
      MoveToEx(hdc, 4, 11, nullptr); LineTo(hdc, 4, 4); LineTo(hdc, 11, 4);
      MoveToEx(hdc, 17, 4, nullptr); LineTo(hdc, 24, 4); LineTo(hdc, 24, 11);
      MoveToEx(hdc, 24, 17, nullptr); LineTo(hdc, 24, 24); LineTo(hdc, 17, 24);
      MoveToEx(hdc, 11, 24, nullptr); LineTo(hdc, 4, 24); LineTo(hdc, 4, 17);
      break;
    case 8:
      MoveToEx(hdc, 3, 5, nullptr); LineTo(hdc, 15, 5); MoveToEx(hdc, 3, 23, nullptr); LineTo(hdc, 15, 23);
      MoveToEx(hdc, 3, 5, nullptr); LineTo(hdc, 3, 23);
      MoveToEx(hdc, 11, 14, nullptr); LineTo(hdc, 25, 14);
      MoveToEx(hdc, 20, 9, nullptr); LineTo(hdc, 25, 14); LineTo(hdc, 20, 19);
      break;
  }

  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
  SetMapMode(hdc, oldMode);
  SetWindowOrgEx(hdc, oldWindowOrg.x, oldWindowOrg.y, nullptr);
  SetViewportOrgEx(hdc, oldViewportOrg.x, oldViewportOrg.y, nullptr);
  if (oldMode == MM_ANISOTROPIC || oldMode == MM_ISOTROPIC) {
    SetWindowExtEx(hdc, oldWindowExt.cx, oldWindowExt.cy, nullptr);
    SetViewportExtEx(hdc, oldViewportExt.cx, oldViewportExt.cy, nullptr);
  }
}

static void ApplyRoundedRegion(HWND hwnd, int radius) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HRGN region = CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, rc.bottom - rc.top + 1, radius, radius);
  SetWindowRgn(hwnd, region, TRUE);
}

static void ClampToMonitor(int& x, int& y, int width, int height) {
  HMONITOR monitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  RECT r = mi.rcMonitor;
  const int minX = static_cast<int>(r.left + 12);
  const int maxX = static_cast<int>(r.right - width - 12);
  const int minY = static_cast<int>(r.top + 12);
  const int maxY = static_cast<int>(r.bottom - height - 12);
  x = std::max(minX, std::min(x, maxX));
  y = std::max(minY, std::min(y, maxY));
}

void UpdateOverlayLayout() {
  if (!g_hwnd || !g_toolbarHwnd) return;
  RECT owner{};
  GetWindowRect(g_hwnd, &owner);
  RECT client{};
  GetClientRect(g_hwnd, &client);
  POINT clientOrigin{0, 0};
  ClientToScreen(g_hwnd, &clientOrigin);
  const int clientLeft = clientOrigin.x;
  const int clientTop = clientOrigin.y;
  const int clientRight = clientLeft + (client.right - client.left);
  const int toolbarWidth = g_toolbarExpanded ? kToolbarWidth : kToolbarIconSize;
  const int toolbarHeight = g_toolbarExpanded ? kToolbarHeight : kToolbarIconSize;
  int toolbarX = std::max(clientLeft + 12, clientRight - toolbarWidth - 12);
  int toolbarY = clientTop + 12;
  ClampToMonitor(toolbarX, toolbarY, toolbarWidth, toolbarHeight);

  SetWindowPos(g_toolbarHwnd, HWND_TOPMOST, toolbarX, toolbarY, toolbarWidth, toolbarHeight,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ApplyRoundedRegion(g_toolbarHwnd, g_toolbarExpanded ? 22 : kToolbarIconSize);

  if (g_menuHwnd && IsWindowVisible(g_menuHwnd)) {
    int x = toolbarX + toolbarWidth - kMenuWidth;
    int y = toolbarY + toolbarHeight + 8;
    ClampToMonitor(x, y, kMenuWidth, kMenuHeight);
    SetWindowPos(g_menuHwnd, HWND_TOPMOST, x, y, kMenuWidth, kMenuHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRoundedRegion(g_menuHwnd, 16);
  }

  if (g_statsHwnd && IsWindowVisible(g_statsHwnd)) {
    int x = toolbarX + toolbarWidth - kStatsWidth;
    int y = toolbarY + toolbarHeight + 8;
    ClampToMonitor(x, y, kStatsWidth, kStatsHeight);
    SetWindowPos(g_statsHwnd, HWND_TOPMOST, x, y, kStatsWidth, kStatsHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRoundedRegion(g_statsHwnd, 16);
  }
}

void HideNativePopups() {
  if (g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  if (g_toolbarExpanded) {
    g_toolbarExpanded = false;
    UpdateOverlayLayout();
  }
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

bool NativeOverlayIsOpen() {
  return g_toolbarExpanded
    || (g_menuHwnd && IsWindowVisible(g_menuHwnd))
    || (g_statsHwnd && IsWindowVisible(g_statsHwnd));
}

static void ShowOnlyPopup(HWND hwnd) {
  g_toolbarExpanded = true;
  if (g_menuHwnd && hwnd != g_menuHwnd) ShowWindow(g_menuHwnd, SW_HIDE);
  if (g_statsHwnd && hwnd != g_statsHwnd) ShowWindow(g_statsHwnd, SW_HIDE);
  if (hwnd) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateOverlayLayout();
    InvalidateRect(hwnd, nullptr, TRUE);
  }
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
}

static void TogglePopup(HWND hwnd) {
  if (!hwnd) return;
  if (IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_HIDE);
    if ((!g_menuHwnd || !IsWindowVisible(g_menuHwnd)) && (!g_statsHwnd || !IsWindowVisible(g_statsHwnd))) {
      g_toolbarExpanded = false;
      UpdateOverlayLayout();
    } else {
      UpdateOverlayLayout();
    }
  } else {
    ShowOnlyPopup(hwnd);
  }
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
  if (g_hwnd) SetFocus(g_hwnd);
}

static void SetNativeFullscreen(bool fullScreen) {
  if (!g_hwnd || g_cfg.fullscreen == fullScreen) return;
  g_cfg.fullscreen = fullScreen;
  HideNativePopups();

  if (fullScreen) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int x = std::max(40, GetSystemMetrics(SM_CXSCREEN) / 2 - 640);
    int y = std::max(40, GetSystemMetrics(SM_CYSCREEN) / 2 - 380);
    SetWindowPos(g_hwnd, HWND_TOP, x, y, 1280, 760,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  UpdateOverlayLayout();
}

void ToggleNativeFullscreen() {
  SetNativeFullscreen(!g_cfg.fullscreen);
}

static void DrawMiniChevron(HDC hdc, int centerX, int centerY, bool left, COLORREF color) {
  HPEN pen = CreatePen(PS_SOLID, 2, color);
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  if (left) {
    MoveToEx(hdc, centerX + 3, centerY - 6, nullptr); LineTo(hdc, centerX - 3, centerY); LineTo(hdc, centerX + 3, centerY + 6);
  } else {
    MoveToEx(hdc, centerX - 3, centerY - 6, nullptr); LineTo(hdc, centerX + 3, centerY); LineTo(hdc, centerX - 3, centerY + 6);
  }
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawSelectorButton(HDC hdc, RECT rc, bool left) {
  HBRUSH fill = CreateSolidBrush(RGB(226, 232, 236));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(210, 217, 222));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, fill));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(fill);
  DrawMiniChevron(hdc, (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2, left, RGB(28, 37, 48));
}

static void DrawToolbar(HDC hdc, RECT rc) {
  const NativeUiStats stats = CurrentUiStats();
  HBRUSH bg = CreateSolidBrush(RGB(249, 251, 253));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(219, 225, 232));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, g_toolbarExpanded ? 22 : rc.right, g_toolbarExpanded ? 22 : rc.bottom);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  if (!g_toolbarExpanded) {
    HPEN dotPen = CreatePen(PS_SOLID, 3, RGB(22, 30, 40));
    HPEN oldDotPen = reinterpret_cast<HPEN>(SelectObject(hdc, dotPen));
    MoveToEx(hdc, 14, 18, nullptr); LineTo(hdc, 14, 24);
    MoveToEx(hdc, 21, 18, nullptr); LineTo(hdc, 21, 24);
    MoveToEx(hdc, 28, 18, nullptr); LineTo(hdc, 28, 24);
    SelectObject(hdc, oldDotPen);
    DeleteObject(dotPen);
    return;
  }

  HBRUSH closeFill = CreateSolidBrush(RGB(242, 245, 248));
  HPEN closeBorder = CreatePen(PS_SOLID, 1, RGB(224, 229, 235));
  HBRUSH oldCloseFill = reinterpret_cast<HBRUSH>(SelectObject(hdc, closeFill));
  HPEN oldCloseBorder = reinterpret_cast<HPEN>(SelectObject(hdc, closeBorder));
  Ellipse(hdc, 11, 10, 38, 37);
  SelectObject(hdc, oldCloseBorder);
  SelectObject(hdc, oldCloseFill);
  DeleteObject(closeBorder);
  DeleteObject(closeFill);

  HPEN ink = CreatePen(PS_SOLID, 2, RGB(22, 30, 40));
  HPEN oldInk = reinterpret_cast<HPEN>(SelectObject(hdc, ink));
  MoveToEx(hdc, 20, 19, nullptr); LineTo(hdc, 29, 28);
  MoveToEx(hdc, 29, 19, nullptr); LineTo(hdc, 20, 28);
  SelectObject(hdc, oldInk);
  DeleteObject(ink);

  HBRUSH controlPill = CreateSolidBrush(RGB(236, 241, 246));
  HBRUSH oldControlPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, controlPill));
  HPEN controlPen = CreatePen(PS_SOLID, 1, RGB(218, 225, 233));
  HPEN oldControlPen = reinterpret_cast<HPEN>(SelectObject(hdc, controlPen));
  RoundRect(hdc, 48, 7, 206, 41, 16, 16);
  SelectObject(hdc, oldControlPen);
  SelectObject(hdc, oldControlPill);
  DeleteObject(controlPen);
  DeleteObject(controlPill);

  HPEN controlInk = CreatePen(PS_SOLID, 2, RGB(24, 33, 44));
  HPEN oldControlInk = reinterpret_cast<HPEN>(SelectObject(hdc, controlInk));
  RoundRect(hdc, 61, 15, 79, 21, 4, 4);
  RoundRect(hdc, 61, 27, 79, 33, 4, 4);
  MoveToEx(hdc, 65, 18, nullptr); LineTo(hdc, 75, 18);
  MoveToEx(hdc, 65, 30, nullptr); LineTo(hdc, 75, 30);
  SelectObject(hdc, oldControlInk);
  DeleteObject(controlInk);

  HFONT controlFont = CreateUiFont(13, FW_SEMIBOLD);
  HFONT controlSubFont = CreateUiFont(10, FW_NORMAL);
  RECT controlRc{88, 9, 198, 25};
  RECT controlSubRc{88, 23, 198, 39};
  const VideoProfile activeProfile = ActiveVideoProfile();
  std::wstring controlSummary = FormatResolution(activeProfile.width, activeProfile.height)
                               + L" / " + std::to_wstring(activeProfile.fps) + L" fps / "
                              + FormatBitrate(g_currentBitrate.load(std::memory_order_relaxed));
  DrawTextRect(hdc, L"显示控制", controlRc, RGB(18, 24, 34), controlFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, controlSummary, controlSubRc, RGB(104, 114, 126), controlSubFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(controlFont);
  DeleteObject(controlSubFont);

  HBRUSH pill = CreateSolidBrush(RGB(236, 245, 240));
  HBRUSH oldPill = reinterpret_cast<HBRUSH>(SelectObject(hdc, pill));
  HPEN noPen = CreatePen(PS_SOLID, 1, RGB(210, 228, 217));
  HPEN oldNoPen = reinterpret_cast<HPEN>(SelectObject(hdc, noPen));
  RoundRect(hdc, 216, 7, 330, 41, 16, 16);
  SelectObject(hdc, oldNoPen);
  SelectObject(hdc, oldPill);
  DeleteObject(noPen);
  DeleteObject(pill);

  DrawSignalBars(hdc, 228, 7, RGB(17, 190, 122));

  wchar_t statsText[96];
  if (stats.rxToPresentMs > 0.0 || stats.mbps > 0.0) {
    swprintf_s(statsText, L"%s · %.0f ms · %.1f Mbps", g_cfg.udpVideo ? L"UDP" : L"TCP", stats.rxToPresentMs, stats.mbps);
  } else {
    swprintf_s(statsText, L"%s 直连", g_cfg.udpVideo ? L"UDP" : L"TCP");
  }
  HFONT statsFont = CreateUiFont(12, FW_SEMIBOLD);
  HFONT statsSubFont = CreateUiFont(9, FW_NORMAL);
  RECT statsRc{270, 9, 322, 24};
  RECT statsSubRc{270, 23, 322, 38};
  DrawTextRect(hdc, L"链路概览", statsRc, RGB(24, 92, 52), statsFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, statsText, statsSubRc, RGB(72, 118, 90), statsSubFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(statsFont);
  DeleteObject(statsSubFont);

  HBRUSH timerFill = CreateSolidBrush(RGB(243, 246, 250));
  HPEN timerBorder = CreatePen(PS_SOLID, 1, RGB(222, 228, 235));
  HBRUSH oldTimerFill = reinterpret_cast<HBRUSH>(SelectObject(hdc, timerFill));
  HPEN oldTimerBorder = reinterpret_cast<HPEN>(SelectObject(hdc, timerBorder));
  RoundRect(hdc, 340, 7, 420, 41, 16, 16);
  SelectObject(hdc, oldTimerBorder);
  SelectObject(hdc, oldTimerFill);
  DeleteObject(timerBorder);
  DeleteObject(timerFill);

  HFONT timeFont = CreateUiFont(14, FW_SEMIBOLD);
  HFONT timeSubFont = CreateUiFont(9, FW_NORMAL);
  RECT timeRc{348, 10, 412, 27};
  RECT timeSubRc{348, 24, 412, 39};
  DrawTextRect(hdc, FormatElapsed(), timeRc, RGB(48, 58, 72), timeFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, L"本次会话", timeSubRc, RGB(123, 133, 144), timeSubFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(timeFont);
  DeleteObject(timeSubFont);
}

static void DrawMenuRow(HDC hdc, int y, int icon, const std::wstring& label, const std::wstring& value, bool chevron, bool danger = false) {
  COLORREF ink = danger ? RGB(232, 62, 52) : RGB(28, 37, 48);
  COLORREF subtle = danger ? RGB(232, 62, 52) : RGB(120, 130, 142);
  DrawMenuIcon(hdc, icon, 18, y + 6, ink, 19);
  HFONT labelFont = CreateUiFont(14, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(11, FW_NORMAL);
  RECT labelRc{52, y + 4, 202, y + 29};
  DrawTextRect(hdc, label, labelRc, ink, labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  if (!value.empty()) {
    RECT valueRc{174, y + 6, chevron ? 318 : 338, y + 29};
    DrawTextRect(hdc, value, valueRc, subtle, valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
  if (chevron) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(26, 34, 44));
    HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 326, y + 11, nullptr); LineTo(hdc, 333, y + 17); LineTo(hdc, 326, y + 23);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
  }
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawMenuSelectorRow(HDC hdc, int y, int icon, const std::wstring& label, const std::wstring& value) {
  DrawMenuIcon(hdc, icon, 18, y + 6, RGB(28, 37, 48), 19);
  HFONT labelFont = CreateUiFont(14, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(11, FW_NORMAL);
  RECT labelRc{52, y + 4, 145, y + 29};
  DrawTextRect(hdc, label, labelRc, RGB(28, 37, 48), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

  RECT leftRc{188, y + 3, 216, y + 29};
  RECT rightRc{306, y + 3, 334, y + 29};
  DrawSelectorButton(hdc, leftRc, true);
  DrawSelectorButton(hdc, rightRc, false);

  RECT valueRc{224, y + 4, 298, y + 29};
  DrawTextRect(hdc, value, valueRc, RGB(110, 120, 132), valueFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawSeparator(HDC hdc, int y) {
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(216, 222, 226));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 28, y, nullptr);
  LineTo(hdc, kMenuWidth - 28, y);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawStatsSeparator(HDC hdc, int y) {
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(232, 235, 238));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 32, y, nullptr);
  LineTo(hdc, kStatsWidth - 32, y);
  SelectObject(hdc, oldPen);
  DeleteObject(pen);
}

static void DrawMenu(HDC hdc, RECT rc) {
  const NativeUiStats stats = CurrentUiStats();
  const VideoProfile activeProfile = CurrentVideoProfile();
  const bool pendingChanges = !SameVideoProfile(activeProfile, g_pendingProfile);

  HBRUSH bg = CreateSolidBrush(RGB(250, 252, 254));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(214, 221, 228));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  DrawMenuRow(hdc, 14, 0, DisplayLabel(), FormatCompactProfile(activeProfile), false);
  DrawSeparator(hdc, 62);
  DrawMenuSelectorRow(hdc, 78, 0, L"分辨率", FormatResolution(g_pendingProfile.width, g_pendingProfile.height));
  DrawMenuSelectorRow(hdc, 122, 1, L"帧率", std::to_wstring(g_pendingProfile.fps) + L" fps");
  DrawMenuSelectorRow(hdc, 166, 4, L"码率", FormatProfileBitrate(g_pendingProfile.bitrate));
  DrawSeparator(hdc, 214);
  DrawMenuRow(hdc, 230, 6, L"立即应用", pendingChanges ? FormatCompactProfile(g_pendingProfile) : L"当前已生效", false);
  wchar_t statsValue[64];
  if (stats.presentFps > 0.1 || stats.rxToPresentMs > 0.0) {
    swprintf_s(statsValue, L"%.0f fps / %.0f ms", stats.presentFps, stats.rxToPresentMs);
  } else {
    swprintf_s(statsValue, L"%s", g_cfg.udpVideo ? L"UDP 直连" : L"TCP 直连");
  }
  DrawMenuRow(hdc, 274, 5, L"链路统计", statsValue, true);
  DrawMenuRow(hdc, 318, 7, g_cfg.fullscreen ? L"退出全屏幕" : L"进入全屏幕", L"F11", false);
  DrawSeparator(hdc, 358);
  DrawMenuRow(hdc, 362, 8, L"退出远控", L"", false, true);
}

static void DrawStatsRow(HDC hdc, int y, const std::wstring& label, const std::wstring& value) {
  HFONT labelFont = CreateUiFont(12, FW_SEMIBOLD);
  HFONT valueFont = CreateUiFont(13, FW_NORMAL);
  RECT labelRc{24, y, 168, y + 22};
  RECT valueRc{178, y, kStatsWidth - 24, y + 22};
  DrawTextRect(hdc, label, labelRc, RGB(146, 151, 160), labelFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextRect(hdc, value, valueRc, RGB(18, 24, 36), valueFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  DeleteObject(labelFont);
  DeleteObject(valueFont);
}

static void DrawStats(HDC hdc, RECT rc) {
  NativeUiStats stats = CurrentUiStats();
  HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
  HPEN border = CreatePen(PS_SOLID, 1, RGB(220, 224, 228));
  HBRUSH oldBrush = reinterpret_cast<HBRUSH>(SelectObject(hdc, bg));
  HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  RoundRect(hdc, 0, 0, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, oldPen);
  SelectObject(hdc, oldBrush);
  DeleteObject(border);
  DeleteObject(bg);

  DrawSignalBars(hdc, 28, 20, RGB(17, 190, 122));
  HFONT titleFont = CreateUiFont(16, FW_BOLD);
  std::wstring latency = stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms";
  RECT titleRc{78, 22, kStatsWidth - 24, 56};
  DrawTextRect(hdc, L"显示尾延时: " + latency, titleRc, RGB(15, 22, 36), titleFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DeleteObject(titleFont);

  HPEN line = CreatePen(PS_SOLID, 1, RGB(232, 235, 238));
  HPEN oldLine = reinterpret_cast<HPEN>(SelectObject(hdc, line));
  MoveToEx(hdc, 0, 76, nullptr); LineTo(hdc, kStatsWidth, 76);
  SelectObject(hdc, oldLine);
  DeleteObject(line);

  DrawStatsRow(hdc, 96, L"收帧后延时:", stats.rxToPresentMs > 0.0 ? FormatDouble(stats.rxToPresentMs, L" ms", 0) : L"-- ms");
  const VideoProfile activeProfile = ActiveVideoProfile();
  DrawStatsRow(hdc, 126, L"显示帧率:", stats.presentFps > 0.1 ? FormatDouble(stats.presentFps, L"", 0) : FormatDouble(activeProfile.fps, L"", 0));
  DrawStatsRow(hdc, 156, L"接收完整帧率:", stats.completeFps > 0.1 ? FormatDouble(stats.completeFps, L"", 0) : L"--");
  DrawStatsRow(hdc, 186, L"显示抖动:", FormatDouble(stats.presentJitterMs, L" ms", 1));
  DrawStatsRow(hdc, 216, L"峰值尾延时:", stats.rxToPresentMaxMs > 0.0 ? FormatDouble(stats.rxToPresentMaxMs, L" ms", 0) : L"-- ms");

  DrawStatsSeparator(hdc, 256);
  DrawStatsRow(hdc, 276, L"带宽占用:", FormatDouble(stats.mbps, L" Mbps", 1));
  DrawStatsRow(hdc, 306, L"客户端丢旧帧(当前):", FormatDouble(stats.queueDropPct, L"%", 1));
  DrawStatsRow(hdc, 336, L"网络拼帧废弃(当前):", FormatDouble(stats.networkDropPct, L"%", 1));
  DrawStatsRow(hdc, 366, L"传输通道:", g_cfg.udpVideo ? L"UDP 局域网直连" : L"TCP 局域网直连");
  DrawStatsRow(hdc, 396, L"被控端系统:", PlatformLabel(g_cfg.hostPlatform));

  DrawStatsSeparator(hdc, 434);
  DrawStatsRow(hdc, 448, L"当前发送码率:", FormatBitrate(stats.adaptiveBitrate > 0 ? stats.adaptiveBitrate : g_currentBitrate.load(std::memory_order_relaxed)));
  DrawStatsRow(hdc, 472, L"FEC 恢复帧:", std::to_wstring(stats.fecRecovered));
  DrawStatsRow(hdc, 496, L"关键帧请求:", std::to_wstring(stats.keyframeRequests));
  DrawStatsRow(hdc, 520, L"编码队列:", std::to_wstring(stats.queueDepth) + L" / " + std::to_wstring(stats.queueTarget) + L" 帧");
  DrawStatsRow(hdc, 544, L"显示队列:", std::to_wstring(stats.decodedQueueDepth) + L" / " + std::to_wstring(stats.decodedQueueTarget) + L" 帧");
  DrawStatsRow(hdc, 568, L"显示丢旧帧:", std::to_wstring(stats.renderDropped));
  DrawStatsRow(hdc, 592, L"编解码器:", L"H.264 / Media Foundation");
  DrawStatsRow(hdc, 616, L"编码模式:", stats.gpuFrames > 0 ? L"硬编 / 硬解" : L"硬编 / 硬解优先");
  DrawStatsRow(hdc, 640, L"采集方式:", g_cfg.hostPlatform == L"win32" ? L"DXGI" : L"ScreenCaptureKit");
  wchar_t target[128];
  swprintf_s(target, L"%dx%d @ %d fps / %d Mbps",
             activeProfile.width,
             activeProfile.height,
             activeProfile.fps,
             std::max(1, (activeProfile.bitrate + 500'000) / 1'000'000));
  DrawStatsRow(hdc, 664, L"目标档位:", target);
}

static void HandleToolbarClick(int x, int y) {
  if (!g_toolbarExpanded) {
    g_toolbarExpanded = true;
    UpdateOverlayLayout();
    if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, TRUE);
    return;
  }

  if (x >= 11 && x <= 38 && y >= 10 && y <= 37) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  } else if (x >= 48 && x <= 206 && y >= 7 && y <= 41) {
    TogglePopup(g_menuHwnd);
  } else if (x >= 216 && x <= 330 && y >= 7 && y <= 41) {
    TogglePopup(g_statsHwnd);
  } else {
    g_toolbarExpanded = false;
    HideNativePopups();
  }
}

static void SendVideoProfileCommand(const VideoProfile& profile) {
  const uint16_t bitrateMbps = static_cast<uint16_t>(std::clamp((profile.bitrate + 500'000) / 1'000'000, 1, 200));
  for (int i = 0; i < 3; ++i) {
    SendInputPacket(P2_INPUT_SET_VIDEO_PROFILE, 0, 0, profile.width, profile.height,
                    static_cast<uint16_t>(std::clamp(profile.fps, 30, 240)), bitrateMbps);
    if (i < 2) Sleep(15);
  }
}

static void SendVideoBitrateCommand(int bitrate) {
  const int clamped = std::clamp(bitrate, 2'000'000, 80'000'000);
  for (int i = 0; i < 2; ++i) {
    SendInputPacket(P2_INPUT_SET_VIDEO_BITRATE, 0, 0, clamped, 0, 0, 0);
    if (i == 0) Sleep(10);
  }
}

static bool SameStreamShape(const VideoProfile& lhs, const VideoProfile& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fps == rhs.fps;
}

static bool RequestProfileApply(const VideoProfile& requestedProfile, const wchar_t* reason) {
  VideoProfile profile = requestedProfile;
  profile.width = ClampEven(profile.width, g_cfg.width);
  profile.height = ClampEven(profile.height, g_cfg.height);
  profile.fps = std::clamp(profile.fps, 30, 240);
  profile.bitrate = std::clamp(profile.bitrate, 2'000'000, 80'000'000);
  const VideoProfile activeProfile = CurrentVideoProfile();
  if (SameVideoProfile(activeProfile, profile)) {
    HideNativePopups();
    return false;
  }

  SyncPendingProfileToIndices(profile);
  if (!WriteProfileFile(g_cfg.profileFile, profile)) {
    MessageBoxW(g_hwnd, L"Failed to save the updated native-v2 profile.", L"P2P Native", MB_ICONERROR);
    return false;
  }

  const bool streamShapeChanged = !SameStreamShape(activeProfile, profile);
  g_lastProfileApplyQpc.store(QpcNow(), std::memory_order_relaxed);
  if (streamShapeChanged) {
    SendVideoProfileCommand(profile);
    CommitActiveVideoProfile(profile);
    g_videoProfileGeneration.fetch_add(1, std::memory_order_relaxed);
    Log(L"profile apply requested: %dx%d@%d bitrate=%d (%s)",
        profile.width, profile.height, profile.fps, profile.bitrate, reason ? reason : L"manual");
    EnterVideoRecovery(L"profile changed");
  } else {
    SendVideoBitrateCommand(profile.bitrate);
    CommitActiveVideoProfile(profile);
    Log(L"bitrate apply requested: %d (%s)", profile.bitrate, reason ? reason : L"manual");
  }
  HideNativePopups();
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
  if (g_toolbarHwnd) InvalidateRect(g_toolbarHwnd, nullptr, FALSE);
  if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
  if (g_statsHwnd) InvalidateRect(g_statsHwnd, nullptr, FALSE);
  return true;
}

static void ApplyPendingVideoProfile() {
  RequestProfileApply(g_pendingProfile, L"manual");
}

static bool HandleSelectorClick(int x, int y, int rowY, void (*cycleFn)(int)) {
  if (y < rowY || y >= rowY + 34) return false;
  if (x >= 188 && x <= 216) {
    cycleFn(-1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  if (x >= 306 && x <= 334) {
    cycleFn(1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  if (x >= 224 && x <= 298) {
    cycleFn(1);
    if (g_menuHwnd) InvalidateRect(g_menuHwnd, nullptr, FALSE);
    return true;
  }
  return false;
}

static void HandleMenuClick(int x, int y) {
  if (HandleSelectorClick(x, y, 78, CycleResolution)) return;
  if (HandleSelectorClick(x, y, 122, CycleFps)) return;
  if (HandleSelectorClick(x, y, 166, CycleBitrate)) return;

  if (y >= 230 && y < 262) {
    ApplyPendingVideoProfile();
  } else if (y >= 274 && y < 306) {
    ShowOnlyPopup(g_statsHwnd);
  } else if (y >= 318 && y < 350) {
    ToggleNativeFullscreen();
  } else if (y >= 362 && y < 386) {
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  }
}

void CreateOverlayWindows(HINSTANCE hInst) {
  WNDCLASSW overlay{};
  overlay.lpfnWndProc = OverlayWndProc;
  overlay.hInstance = hInst;
  overlay.hCursor = LoadCursor(nullptr, IDC_ARROW);
  overlay.lpszClassName = L"P2PNativeOverlay";
  RegisterClassW(&overlay);

  DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
  g_toolbarHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                  0, 0, kToolbarWidth, kToolbarHeight, g_hwnd, nullptr, hInst, nullptr);
  g_menuHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                               0, 0, kMenuWidth, kMenuHeight, g_hwnd, nullptr, hInst, nullptr);
  g_statsHwnd = CreateWindowExW(exStyle, overlay.lpszClassName, L"", WS_POPUP,
                                0, 0, kStatsWidth, kStatsHeight, g_hwnd, nullptr, hInst, nullptr);
  SetWindowLongPtrW(g_toolbarHwnd, GWLP_USERDATA, 1);
  SetWindowLongPtrW(g_menuHwnd, GWLP_USERDATA, 2);
  SetWindowLongPtrW(g_statsHwnd, GWLP_USERDATA, 3);
  UpdateOverlayLayout();
  ShowWindow(g_toolbarHwnd, SW_SHOWNOACTIVATE);
}

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  const LONG_PTR kind = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursor(nullptr, IDC_ARROW));
      return TRUE;
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lp);
      int y = GET_Y_LPARAM(lp);
      if (kind == 1) HandleToolbarClick(x, y);
      else if (kind == 2) HandleMenuClick(x, y);
      if (g_hwnd) SetFocus(g_hwnd);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      SetBkMode(hdc, TRANSPARENT);
      if (kind == 1) DrawToolbar(hdc, rc);
      else if (kind == 2) DrawMenu(hdc, rc);
      else if (kind == 3) DrawStats(hdc, rc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
