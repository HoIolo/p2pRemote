#pragma once

#include "app_state.h"

void CreateOverlayWindows(HINSTANCE hInst);
void UpdateOverlayLayout();
void HideNativePopups();
bool NativeOverlayIsOpen();
void ToggleNativeFullscreen();
