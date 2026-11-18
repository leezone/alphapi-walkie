#pragma once

#include <cstdint>

// AlphaPi Zero 点阵屏模块：main.cpp 只负责模块编排，
// 5×5 点阵的图案库、发送、密码视图、身份滚动、状态渲染都在 display.cpp。

enum class Mode { Boot, Home, Transmit };

// UI 时序状态（main.cpp 也会读写）
extern uint32_t g_volDisplayUntil;
extern uint32_t g_chDisplayUntil;
extern uint32_t g_passViewUntil;

void uiBootSplash();    // 全屏亮 0.5s → 清空
void uiFlashClear();
void uiFlashMic();
void uiFlashCross();
void uiFlashVolUp();
void uiFlashVolDown();
void uiShowChannel(uint8_t ch);
void uiRender(uint32_t now);
