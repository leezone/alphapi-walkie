#include <Arduino.h>
#include <atomic>
#include <string.h>

#include "device/config.h"
#include "device/display.h"
#include "core/walkie_atomic.h"
#include "core/walkie_codec.h"
#include "core/walkie_peer.h"
#include "core/walkie_crypto.h"
#include "core/walkie_n32.h"
#include "core/walkie_rxpath.h"

// display.cpp 需要访问的 main.cpp 全局状态
extern Mode g_mode;
extern HardwareSerial coUart;
extern walkie::N32Driver g_n32;
extern uint8_t gChannel;
extern walkie::Atomic<uint8_t> gVolume;
extern uint16_t g_passBits;
extern uint8_t g_passLen;
extern char g_myName[12];
extern walkie::CryptoState g_crypto;
extern walkie::RxPath rxPath;

uint32_t g_volDisplayUntil = 0;
uint32_t g_chDisplayUntil = 0;
uint32_t g_passViewUntil = 0;   // 密码视图显示截止时刻（C 单按）

// ========== 5×5 点阵 UI ==========
// 列字节高 5 位有效：bit7=顶行, bit3=底行。全屏=0xF8。
// 25 像素图案 → 5 列字节帧 [0x90 0x08 0x05 col0..col4 checksum]

static void mtxBuild(const uint8_t p[25], uint8_t f[9])
{
    uint8_t cols[5] = {0};
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
            if (p[y * 5 + x]) cols[x] |= (1 << (4 - y));
    uint8_t payload[5];
    for (int i = 0; i < 5; i++) payload[i] = cols[i] << 3;
    size_t n = walkie::buildWriteFrame(f, 9, MTX_ADDR, payload, 5);
    if (n != 9) memset(f, 0, 9);
}

static void mtxSendFast(const uint8_t p[25])
{
    g_n32.lock();
    uint8_t f[9];
    mtxBuild(p, f);
    coUart.write(f, 9);
    g_n32.unlock();
}

static bool mtxSend(const uint8_t p[25])
{
    g_n32.lock();
    uint8_t f[9];
    mtxBuild(p, f);
    for (int att = 0; att < 3; att++)
    {
        walkie::n32FlushIn(g_n32);
        coUart.write(f, 9);
        uint8_t resp[3] = {0};
        coUart.setTimeout(150);
        if (coUart.readBytes(resp, 3) == 3)
        {
            walkie::AckFrame a = walkie::parseAck(resp, 3);
            if (a.valid && a.reg == MTX_ADDR) { g_n32.unlock(); return true; }
        }
        delay(50);
    }
    g_n32.unlock();
    return false;
}

// 图案库（25 像素，行优先）
static const uint8_t MTX_CLEAR[25] = {0};
static const uint8_t MTX_FULL[25] = {
    1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,1,1};
// 麦克风（发送中）
static const uint8_t MTX_MIC[25] = {
    0,0,1,0,0, 0,1,1,1,0, 1,1,1,1,1, 1,1,1,1,1, 0,1,1,1,0};
// 喇叭（接收中）
static const uint8_t MTX_SPK[25] = {
    0,1,0,0,0, 1,1,1,0,0, 1,1,1,1,1, 1,1,1,1,1, 1,1,1,0,0};
// 锁（已加密）
static const uint8_t MTX_LOCK[25] = {
    0,1,1,1,0, 1,0,0,0,1, 1,1,1,1,1, 1,0,1,0,1, 1,1,1,1,1};
// 大 X（未设密码 / 清空）
static const uint8_t MTX_X[25] = {
    1,0,0,0,1, 0,1,0,1,0, 0,0,1,0,0, 0,1,0,1,0, 1,0,0,0,1};
// 上箭头（音量+）
static const uint8_t MTX_ARR_U[25] = {
    0,0,1,0,0, 0,1,1,1,0, 1,0,1,0,1, 0,0,1,0,0, 0,0,1,0,0};
// 下箭头（音量-）
static const uint8_t MTX_ARR_D[25] = {
    0,0,1,0,0, 0,0,1,0,0, 1,0,1,0,1, 0,1,1,1,0, 0,0,1,0,0};
// 数字 0-F（5×5 点阵字体）
static const uint8_t MTX_DIGITS[16][25] = {
    {0,1,1,1,0, 1,0,0,0,1, 1,0,0,0,1, 1,0,0,0,1, 0,1,1,1,0}, // 0
    {0,0,1,0,0, 0,1,1,0,0, 0,0,1,0,0, 0,0,1,0,0, 0,1,1,1,0}, // 1
    {0,1,1,1,0, 1,0,0,0,1, 0,0,1,1,0, 0,1,0,0,0, 1,1,1,1,1}, // 2
    {1,1,1,1,0, 0,0,0,0,1, 0,1,1,1,0, 0,0,0,0,1, 1,1,1,1,0}, // 3
    {0,0,1,1,0, 0,1,0,1,0, 1,0,0,1,0, 1,1,1,1,1, 0,0,0,1,0}, // 4
    {1,1,1,1,1, 1,0,0,0,0, 1,1,1,1,0, 0,0,0,0,1, 1,1,1,1,0}, // 5
    {0,1,1,1,0, 1,0,0,0,0, 1,1,1,1,0, 1,0,0,0,1, 0,1,1,1,0}, // 6
    {1,1,1,1,1, 0,0,0,1,0, 0,0,1,0,0, 0,1,0,0,0, 1,0,0,0,0}, // 7
    {0,1,1,1,0, 1,0,0,0,1, 0,1,1,1,0, 1,0,0,0,1, 0,1,1,1,0}, // 8
    {0,1,1,1,0, 1,0,0,0,1, 0,1,1,1,1, 0,0,0,0,1, 0,1,1,1,0}, // 9
    {0,1,1,1,0, 1,0,0,0,1, 1,1,1,1,1, 1,0,0,0,1, 1,0,0,0,1}, // A
    {1,1,1,1,0, 1,0,0,0,1, 1,1,1,1,0, 1,0,0,0,1, 1,1,1,1,0}, // B
    {0,1,1,1,0, 1,0,0,0,0, 1,0,0,0,0, 1,0,0,0,0, 0,1,1,1,0}, // C
    {1,1,1,0,0, 1,0,0,1,0, 1,0,0,1,0, 1,0,0,1,0, 1,1,1,0,0}, // D
    {1,1,1,1,1, 1,0,0,0,0, 1,1,1,1,0, 1,0,0,0,0, 1,1,1,1,1}, // E
    {1,1,1,1,1, 1,0,0,0,0, 1,1,1,1,0, 1,0,0,0,0, 1,0,0,0,0}, // F
};

// 信道 10-13 双位合成字形（左竖=1，右侧紧凑次位），完整 5×5 一次显示
static const uint8_t MTX_CH10[25] = {
    1,0,0,1,0, 1,0,1,0,1, 1,0,1,0,1, 1,0,1,0,1, 1,0,0,1,0};
static const uint8_t MTX_CH11[25] = {
    1,0,0,1,0, 1,0,1,1,0, 1,0,0,1,0, 1,0,0,1,0, 1,0,1,1,0};
static const uint8_t MTX_CH12[25] = {
    1,0,1,1,0, 1,0,0,0,1, 1,0,0,1,0, 1,0,1,0,0, 1,0,1,1,1};
static const uint8_t MTX_CH13[25] = {
    1,0,1,1,0, 1,0,0,0,1, 1,0,0,1,0, 1,0,0,0,1, 1,0,1,1,0};

// 形状表：MAC 字节 → 形状索引（8 种形状循环）
// 信道 1-13 对应字形（1-9 用 MTX_DIGITS，10-13 用双位字形）
void uiShowChannel(uint8_t ch)
{
    switch (ch)
    {
    case 10: mtxSendFast(MTX_CH10); return;
    case 11: mtxSendFast(MTX_CH11); return;
    case 12: mtxSendFast(MTX_CH12); return;
    case 13: mtxSendFast(MTX_CH13); return;
    default: mtxSendFast(MTX_DIGITS[ch]);
    }
}


void uiFlashClear() { mtxSendFast(MTX_CLEAR); }
void uiFlashMic() { mtxSendFast(MTX_MIC); }
void uiFlashCross() { mtxSendFast(MTX_X); }
void uiFlashVolUp() { mtxSendFast(MTX_ARR_U); }
void uiFlashVolDown() { mtxSendFast(MTX_ARR_D); }
void uiBootSplash()
{
    mtxSend(MTX_FULL);
    delay(500);
    mtxSend(MTX_CLEAR);
}

// 渲染本机身份帧：按机器名逐个字符循环显示（5×5 一次一字）
static uint8_t g_identityPhase = 0;
static uint32_t g_identityNextMs = 0;

static uint8_t hexNibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0;
}

static void renderIdentity(uint32_t now)
{
    if (now < g_identityNextMs) return;
    g_identityNextMs = now + 800;   // 每 800ms 切换
    size_t len = strlen(g_myName);
    if (len == 0) return;
    uint8_t total = (uint8_t)len + (g_crypto.enabled ? 1 : 0);
    if (g_identityPhase >= total) g_identityPhase = 0;
    if (g_crypto.enabled && g_identityPhase == 0)
    {
        // 加密时锁字形排在机器名前
        mtxSendFast(MTX_LOCK);
    }
    else
    {
        int idx = g_crypto.enabled ? g_identityPhase - 1 : (int)g_identityPhase;
        mtxSendFast(MTX_DIGITS[hexNibble(g_myName[idx])]);
    }
    g_identityPhase = (uint8_t)((g_identityPhase + 1) % total);
}

// ===== 密码视图：每个密码位一盏 LED，1=亮 0=半亮(闪烁)；无密码整屏 X =====
static void mtxSendPassView()
{
    uint8_t p[25] = {0};
    if (g_passLen == 0)
    {
        mtxSendFast(MTX_X);   // X = 未设密码
        return;
    }
    bool dimOn = (millis() / 400) % 2 == 0;   // 半亮：500ms 级闪烁→视觉上半亮
    for (int i = 0; i < g_passLen; i++)
    {
        uint8_t bit = (uint8_t)((g_passBits >> (g_passLen - 1 - i)) & 1u);
        bool ledOn = bit ? true : dimOn;      // 1=恒亮 0=半亮闪
        if (ledOn) p[i] = 1;                  // 行优先填充
    }
    mtxSendFast(p);
}

// ========== 点阵状态渲染 ==========
static uint32_t g_lastMtxRender = 0;

void uiRender(uint32_t now)
{
    if (now - g_lastMtxRender < 100) return;   // 10fps 上限
    g_lastMtxRender = now;

    // 优先级：发送 > 密码视图 > 音量提示 > 信道提示 > 接收 > 邻居上线 > 身份滚动
    if (g_mode == Mode::Transmit)
    {
        // 发送中：麦克风图标 + 电平闪烁
        mtxSendFast(MTX_MIC);
        return;
    }
    if (now < g_passViewUntil)
    {
        // 密码视图：每位一个 LED，1=亮 0=半亮闪烁；无密码整屏 X
        mtxSendPassView();
        return;
    }
    if (now < g_volDisplayUntil)
    {
        mtxSendFast(MTX_DIGITS[gVolume]);
        return;
    }
    if (now < g_chDisplayUntil)
    {
        // 信道号：完整 5×5 单帧显示（10-13 用双位合成字形）
        uiShowChannel(gChannel);
        return;
    }
    int t = walkie::findTalker(rxPath, now);
    if (t >= 0)
    {
        // 接收中：喇叭图标
        mtxSendFast(MTX_SPK);
        return;
    }
    // 待机：身份滚动（加密时锁字形排在名字前）
    renderIdentity(now);
}

