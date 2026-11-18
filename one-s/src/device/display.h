#pragma once

#include <cstdint>

// AlphaPi One S 屏幕模块：main.cpp 只负责模块编排，
// 所有绘制/推屏细节（sprite、HZK16 渲染、页面）都在 display.cpp。

enum class Mode { Boot, Home, Transmit, Pairing };

// UI 时序状态（main.cpp 也会读写）
extern uint32_t gVolDisplayUntil;

void displayInit();
void showBoot();
void drawHomeFrame(uint32_t now);
void drawHomeStatus(uint32_t now);
void drawPairing();
void showVolumeBar(uint8_t vol);
void drawDiag();
