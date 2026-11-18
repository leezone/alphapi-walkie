#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <string.h>
#include <atomic>

#include "device/config.h"
#include "device/panel_alphapi.h"
#include "device/display.h"
#include "core/walkie_atomic.h"
#include "core/walkie_cnfont.h"
#include "core/walkie_protocol.h"
#include "core/walkie_timing.h"
#include "core/walkie_peer.h"

// display.cpp 需要访问的 main.cpp 全局状态
extern Mode g_mode;
extern char gMyName[12];
extern uint8_t gChannel;
extern uint16_t g_passBits;
extern uint8_t g_passLen;
extern bool g_passEdit;
extern uint16_t g_tmpBits;
extern uint8_t g_tmpLen;
extern walkie::Atomic<uint8_t> gVolume;
extern walkie::Atomic<uint8_t> g_pwrLvl;
extern walkie::PeerEntry gPeers[walkie::kMaxPeers];
extern int8_t g_chRssi[14];
extern uint8_t g_chCount[14];
extern bool g_scanned;
extern walkie::Atomic<bool> g_scanBusy;
int findTalker(uint32_t now);

alphapi::LGFX_AlphaPi display;

// 所有绘制都先落到 PSRAM sprite，再整帧一次推屏。
// 直接往 LCD 上做局部重绘会撕裂/闪烁（配对页每秒整屏刷尤其明显）。
static LGFX_Sprite canvas(&display);

// 推屏：pushSprite 是异步的，必须等 DMA 读完 sprite 才能开始下一帧绘制，
// 否则下一帧的内容会覆盖正在被 DMA 读取的缓冲 → 花屏。
// 注意：要用 display.waitDMA()，LGFX_Sprite::waitDMA() 是空实现！
static walkie::Atomic<bool> g_spriteOk{false};

// 所有 UI 绘制都走这个指针：sprite 可用时画到离屏缓冲，
// 分配失败时退回直接画屏（会闪，但绝不会黑屏）。
static LovyanGFX *g_gfx = &canvas;

static void present()
{
    if (!g_spriteOk)
    {
        // sprite 分配失败时直接画屏（会闪屏，但不会黑屏）
        return;
    }
    canvas.pushSprite(0, 0);
    display.waitDMA();
}


// ========== 中文 16x16 渲染（HZK16 切割） ==========
static void drawCNGlyph(int32_t x, int32_t y, uint8_t u0, uint8_t u1, uint8_t u2, uint16_t color)
{
    const walkie::CnGlyph* g = nullptr;
    // 3756 条目按 UTF-8 升序，用二分查找
    int lo = 0, hi = (int)walkie::kCnGlyphCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const walkie::CnGlyph& e = walkie::kCnGlyphTbl[mid];
        int cmp = (u0 != e.u0) ? (int)u0 - (int)e.u0
                : (u1 != e.u1) ? (int)u1 - (int)e.u1
                : (int)u2 - (int)e.u2;
        if (cmp == 0) { g = &e; break; }
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }

    if (g == nullptr)
    {
        g_gfx->drawRect(x, y, 16, 16, color);
        return;
    }

    for (int row = 0; row < 16; row++)
    {
        uint16_t bits = (uint16_t(g->data[row * 2]) << 8) | g->data[row * 2 + 1];
        for (int col = 0; col < 16; col++)
        {
            if (bits & (1 << (15 - col)))
                g_gfx->drawPixel(x + col, y + row, color);
        }
    }
}

// 返回绘制后的 x（ASCII 8px/字，中文 17px/字）
static int32_t drawCNText(int32_t x, int32_t y, const char *utf8, uint16_t color, int32_t advance = 17)
{
    int32_t cx = x;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p)
    {
        if (*p < 0x80)
        {
            g_gfx->setTextColor(color, TFT_BLACK);
            g_gfx->setTextSize(1);
            g_gfx->setCursor(cx, y + 4);
            g_gfx->printf("%c", (char)*p);
            cx += 8;
            p++;
            continue;
        }
        uint8_t u0 = *p++, u1 = *p++, u2 = *p++;
        drawCNGlyph(cx, y, u0, u1, u2, color);
        cx += advance;
    }
    return cx;
}

// 估算 UTF-8 文本像素宽（中文 17，ASCII 8）
static int textWidth(const char *utf8)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p)
    {
        if (*p < 0x80) { w += 8; p++; }
        else { w += 17; p += 3; }
    }
    return w;
}

static void drawCentered(int32_t y, const char *utf8, uint16_t color)
{
    drawCNText((SCREEN_WIDTH - textWidth(utf8)) / 2, y, utf8, color);
}

uint32_t gVolDisplayUntil = 0;

static const int BAR_Y = 78, BAR_H = 10;
static const int HINT_Y = 108;

static void drawTitle()
{
    // 单行：AlphaPi Walkie by leezone（25 字符 ×6px=150，恰好居中）
    g_gfx->setTextSize(1);
    const char *t = "AlphaPi Walkie ";
    const char *by = "by leezone";
    int w = 6 * (int)(strlen(t) + strlen(by));
    int x = (SCREEN_WIDTH - w) / 2;
    g_gfx->setTextColor(TFT_CYAN, TFT_BLACK);
    g_gfx->setCursor(x, 5);
    g_gfx->printf("%s", t);
    g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    g_gfx->printf("%s", by);
    g_gfx->drawFastHLine(8, 17, SCREEN_WIDTH - 16, TFT_DARKGREY);
}

// ===== 信号等级条：显示闭环功率当前档位（P0/P1/P2 = 8.5/13/17dBm）=====
static void drawSignal(int x, int y)
{
    // 三根阶梯柱：亮柱数 = 档位+1，直观反映发射强度
    for (int i = 0; i < 3; i++)
    {
        int h = 4 + i * 3;
        uint16_t col = (i <= g_pwrLvl) ? TFT_GREEN : TFT_DARKGREY;
        g_gfx->fillRect(x + i * 6, y + 10 - h, 4, h, col);
    }
}

void drawDiag()
{
    return;   // v0.4 正式版隐藏监控参数
#if 0
    // 精简监控：只留电平/音量 + 欠载U/重建R/错误E/信道。
    // P（写入计数）已证明链路通，撤下；I2C/N32/SPK 启动信息不再常驻。
    char l0[24], l1[32];
    snprintf(l0, sizeof(l0), "MIC%3d SPK%3d V%d", (int)g_txPeak, (int)g_rxPeak, (int)(int)gVolume);
    // 三行常驻：发送/接收指标同屏，松手后不消失
    char l2[32];
    snprintf(l1, sizeof(l1), "T%u F%u N%u P%d",
             (unsigned)g_txStarve, (unsigned)g_radio.txFail.load(),
             (unsigned)g_radio.txNack.load(), (int)g_pwrLvl);
    snprintf(l2, sizeof(l2), "U%u R%u G%u S%u",
             (unsigned)g_n32.underrun, (unsigned)g_n32.playRestart,
             (unsigned)g_seqGap, (unsigned)g_rxStall);

    if (g_mode == Mode::Transmit)
    {
        // TX 期间直写 LCD 三行小字（~1.5KB SPI），不推 40KB 整帧
        display.fillRect(0, 88, SCREEN_WIDTH, 20, TFT_BLACK);
        display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display.setTextSize(1);
        display.setCursor(2, 88);  display.printf("%s", l0);
        display.setCursor(2, 97);  display.printf("%s", l1);
        display.setCursor(2, 106); display.printf("%s", l2);
    }
    else
    {
        g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g_gfx->setTextSize(1);
        g_gfx->setCursor(2, 88);  g_gfx->printf("%s", l0);
        g_gfx->setCursor(2, 97);  g_gfx->printf("%s", l1);
        g_gfx->setCursor(2, 106); g_gfx->printf("%s", l2);
        present();
    }

    g_rxPeak = 0;   // 电平表每周期清零，便于看出实时读数
#endif
}

static void drawHint(const char *utf8)
{
    int32_t cx = 1;
    const uint8_t *p = (const uint8_t *)utf8;
    while (*p)
    {
        if (*p < 0x80)
        {
            g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
            g_gfx->setTextSize(1);
            g_gfx->setCursor(cx, 114);
            g_gfx->printf("%c", (char)*p);
            cx += 7;
            p++;
        }
        else
        {
            uint8_t u0 = *p++, u1 = *p++, u2 = *p++;
            drawCNGlyph(cx, 110, u0, u1, u2, TFT_DARKGREY);
            cx += 15;
        }
    }
}

void drawHomeStatus(uint32_t now)
{
    // 状态区 y44..y68 清掉重画
    g_gfx->fillRect(0, 44, SCREEN_WIDTH, 26, TFT_BLACK);
    if (g_mode == Mode::Transmit)
    {
        drawCentered(50, "发送中", TFT_GREEN);
        present();
        return;
    }
    if (now < gVolDisplayUntil)
    {
        char vl[16];
        snprintf(vl, sizeof(vl), "VOL %d/9", (int)gVolume);
        g_gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
        g_gfx->setTextSize(2);
        int w = 12 * (int)strlen(vl);
        g_gfx->setCursor((SCREEN_WIDTH - w) / 2, 50);
        g_gfx->printf("%s", vl);
        g_gfx->setTextSize(1);
        present();
        return;
    }
    int t = findTalker(now);
    char line[40];
    if (t >= 0)
    {
        snprintf(line, sizeof(line), "%s 说话中", gPeers[t].name);
        drawCentered(50, line, TFT_GREEN);
        g_gfx->fillRect(4, 54, 8, 8, TFT_GREEN);
    }
    else
    {
        drawCentered(50, "待机中", TFT_DARKGREY);
    }
    present();
}

void drawHomeFrame(uint32_t now)
{
    g_gfx->fillScreen(TFT_BLACK);
    drawTitle();
    drawCentered(26, gMyName, TFT_YELLOW);
    drawHomeStatus(now);
    if (g_mode == Mode::Transmit)
    {
        g_gfx->fillRect(10, BAR_Y, SCREEN_WIDTH - 20, BAR_H, TFT_DARKGREY);
    }
    if (g_mode != Mode::Transmit)
    {
        // 信息行：信道居左、电池居右，视觉配重
        char ch[8];
        snprintf(ch, sizeof(ch), "CH %d", gChannel);
        g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
        g_gfx->setTextSize(1);
        g_gfx->setCursor(14, 82);
        g_gfx->printf("%s", ch);
        drawSignal(132, 81);
    }
    g_gfx->drawFastHLine(8, 100, SCREEN_WIDTH - 16, TFT_DARKGREY);
    drawHint("A:说 B:配 C:音- D:音+");
    present();
}

void drawPairing()
{
    g_gfx->fillScreen(TFT_BLACK);
    // 标题行：配对 | 频道 N
    drawCNText(4, 2, "配对", TFT_YELLOW);
    char chLine[16];
    snprintf(chLine, sizeof(chLine), "频道 %d", gChannel);
    drawCNText(SCREEN_WIDTH - 4 - textWidth(chLine), 2, chLine, TFT_CYAN);
    g_gfx->drawFastHLine(0, 20, SCREEN_WIDTH, TFT_DARKGREY);

    // 邻居列表
    uint32_t now = millis();
    int row = 0;
    for (int i = 0; i < 8 && row < 4; i++)
    {
        if (!gPeers[i].used) continue;
        int y = 26 + row * 19;
        bool talking = (now - gPeers[i].lastVoice < walkie::TALK_TIMEOUT_MS);
        g_gfx->fillRect(6, y + 4, 8, 8, talking ? TFT_RED : TFT_GREEN);
        drawCNText(20, y, gPeers[i].name, talking ? TFT_RED : TFT_WHITE);
        row++;
    }
    // 信道占用图（y70..104）：柱=AP数，红=强干扰 绿=安静，黄框=当前
    if (g_scanBusy) drawCentered(84, "扫描中...", TFT_DARKGREY);
    if (g_scanned)
    {
        for (int ch = 1; ch <= 13; ch++)
        {
            int x = 4 + (ch - 1) * 12;
            int h = g_chCount[ch] * 6;
            if (h > 28) h = 28;
            uint16_t col = g_chCount[ch] == 0 ? TFT_DARKGREEN :
                           (g_chRssi[ch] > -60 ? TFT_RED : TFT_ORANGE);
            if (h > 0) g_gfx->fillRect(x, 100 - h, 9, h, col);
            else g_gfx->drawFastHLine(x, 100, 9, TFT_DARKGREEN);
            if (ch == gChannel) g_gfx->drawRect(x - 1, 70, 11, 32, TFT_YELLOW);
            g_gfx->setTextColor(ch == gChannel ? TFT_YELLOW : TFT_DARKGREY, TFT_BLACK);
            g_gfx->setTextSize(1);
            g_gfx->setCursor(x + (ch < 10 ? 2 : 0), 103);
            g_gfx->printf("%d", ch);
        }
    }
    if (row == 0)
    {
        drawCentered(40, "搜索中", TFT_DARKGREY);
    }

    // 密码行（y64）：编辑态显示正在打的位串+光标，否则显示已存位串
    {
        char bs[24];
        uint16_t bits = g_passEdit ? g_tmpBits : g_passBits;
        uint8_t len = g_passEdit ? g_tmpLen : g_passLen;
        for (int i = 0; i < len; i++)
            bs[i] = (bits >> (len - 1 - i)) & 1 ? '1' : '0';
        bs[len] = 0;
        g_gfx->setTextSize(1);
        g_gfx->setCursor(6, 64);
        if (g_passEdit)
        {
            g_gfx->setTextColor(TFT_YELLOW, TFT_BLACK);
            g_gfx->printf("%s_", bs);
            g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
            g_gfx->setCursor(120, 64);
            g_gfx->printf("%d/16", len);
        }
        else
        {
            g_gfx->setTextColor(len ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
            if (len) g_gfx->printf("密 %s", bs);
            else g_gfx->printf("加密 关");
            g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
            g_gfx->setCursor(110, 64);
            g_gfx->printf("B:设");
        }
    }
    drawDiag();
    drawHint(g_passEdit ? " A:存 B:清 C:0 D:1" : " A:说 B:密 C:频- D:频+");
    present();
}

void showBoot()
{
    g_gfx->fillScreen(TFT_BLACK);
    drawTitle();
    drawCentered(28, gMyName, TFT_YELLOW);
    g_gfx->setTextColor(TFT_DARKGREY, TFT_BLACK);
    g_gfx->setTextSize(1);
    g_gfx->setCursor(56, 56);
    g_gfx->printf("v0.4");
    drawCentered(80, "频道", TFT_DARKGREY);
    char n[4];
    snprintf(n, sizeof(n), "%d", gChannel);
    g_gfx->setTextColor(TFT_WHITE, TFT_BLACK);
    g_gfx->setCursor(80, 84);
    g_gfx->printf("%s", n);
    present();
}


static unsigned long lastVolUpdate = 0;
void showVolumeBar(uint8_t vol)
{
    if (g_mode != Mode::Transmit) return;
    lastVolUpdate = millis();
    // 直写 LCD 小区域：整帧 pushSprite+waitDMA 要几十 ms，
    // 每 200ms 一次会把 TX 任务饿出周期性欠载（U 每 3-5s +1）。
    int w = (int)((uint32_t)vol * (SCREEN_WIDTH - 20) / 255);
    display.fillRect(10, BAR_Y, SCREEN_WIDTH - 20, BAR_H, TFT_DARKGREY);
    if (w > 0) display.fillRect(10, BAR_Y, w, BAR_H, TFT_GREEN);
}



void displayInit()
{
    display.init();
    display.setRotation(0);
    // 离屏缓冲：先试 PSRAM 再退内部 RAM，并且实测回读校验。
    // lolin_s2_mini 的板卡定义只给了 -DBOARD_HAS_PSRAM，不保证真的初始化成功；
    // 往未初始化的 PSRAM 写会踩内存（表现为跑一会黑屏/死机）。
    auto tryAlloc = [&](bool usePsram) -> bool {
        canvas.setPsram(usePsram);
        if (!canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT)) return false;
        canvas.fillScreen(TFT_WHITE);
        volatile uint16_t probe = canvas.readPixel(0, 0);
        if (probe != TFT_WHITE) return false;
        g_gfx->fillScreen(TFT_BLACK);
        probe = canvas.readPixel(SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
        if (probe != TFT_BLACK) return false;
        return true;
    };
    bool spriteOk = false;
    if (ESP.getPsramSize() > 0) spriteOk = tryAlloc(true);
    if (!spriteOk) spriteOk = tryAlloc(false);
    if (!spriteOk) canvas.deleteSprite();
    g_spriteOk = spriteOk;
    g_gfx = spriteOk ? (LovyanGFX *)&canvas : (LovyanGFX *)&display;
    g_gfx->fillScreen(TFT_BLACK);
}
