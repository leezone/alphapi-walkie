#pragma once

#include <Arduino.h>
#include "panel_alphapi.h" // LGFX_AlphaPi

// 对讲机固件主流程（基于 atomic14 对讲机架构 + mochi 的 N32 UART 协议）
//
// 流程：
//   1. 启动：初始化 ST7735 屏幕（黑屏）→ 显示 "HOLD A" 提示 → 初始化 codec + ESP-NOW
//   2. 按住 A：切 MIC 模式，把 mic PCM 广播出去，屏幕显示 "SENDING"
//   3. 松开 A：切 RECV 模式，广播收到的 PCM 通过 codec 播放，屏幕显示 "LISTEN"

// 全局显示对象
extern alphapi::LGFX_AlphaPi display;
