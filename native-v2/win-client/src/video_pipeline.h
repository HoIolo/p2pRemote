#pragma once

#include "app_state.h"

void RunUdpVideoReceiver(uint16_t port);
void RunTcpVideoReceiver(std::wstring hostIp, uint16_t port);
void DecoderThread();
void RenderThread();
void EnterVideoRecovery(const wchar_t* reason);
