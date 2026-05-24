#pragma once

#include "app_state.h"

uint16_t VkToMacKeyCode(WPARAM vk, LPARAM lp);
bool IsModifierVirtualKey(WPARAM vk);
bool HasNonTextModifierDown();
bool IsTextVirtualKey(WPARAM vk);
uint16_t MacModifierMaskForCurrentWinKeys();
bool SendInputPacket(uint8_t kind, float x, float y, int32_t dx, int32_t dy, uint16_t button, uint16_t keyCode);
void MaybeSendClientStats(bool force = false);
void MaybeRequestKeyframeRecovery(const wchar_t* reason);
bool InitInputSocket();
