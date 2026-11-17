#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <Wire.h>
#include <string.h>
#include <atomic>
#include <LovyanGFX.hpp>

#include "device/walkie.h"
#include "device/config.h"
#include "core/walkie_cnfont.h"
#include "core/walkie_protocol.h"
#include "core/walkie_crypto.h"
#include "core/walkie_peer.h"
#include "core/walkie_ring.h"
#include "core/walkie_rx.h"
#include "core/walkie_timing.h"
#include "core/walkie_atomic.h"
#include "core/walkie_codec.h"
#include "core/walkie_store.h"
#include "core/walkie_audio.h"
#include "core/walkie_chime.h"
#include "core/walkie_radio.h"

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

// ========== 本机身份：生肖 + MAC尾 ==========
static char gMyName[12] = "同伴";   // "龙虎7A"
static uint8_t gChannel = CHANNEL_DEFAULT;
// ===== 通信密码：二进制位串（C=0 D=1 逐位打），空=不加密。
// SHA256 派生 AES-128-CTR 密钥；CTR 不增包长，序号当计数器。
static uint16_t g_passBits = 0;
static uint8_t g_passLen = 0;      // 0..16
static walkie::CryptoState g_crypto;
static bool g_passEdit = false;
static uint16_t g_tmpBits = 0;
static uint8_t g_tmpLen = 0;
static uint32_t g_bHeldMs = 0;
static walkie::Atomic<uint16_t> g_rxBadKey{0};
static walkie::Atomic<uint8_t> gVolume{6};   // 软件音量 0..9
static uint32_t gVolDisplayUntil = 0;

static void initIdentity()
{
    walkie::StoredSettings st = walkie::loadSettings(CHANNEL_DEFAULT, CHANNEL_MIN, CHANNEL_MAX);
    String n = st.name;
    bool invalid = (n.length() < 4);
    if (!invalid && n.endsWith("00")) invalid = true;   // 旧bug生成的坏名，自愈重新生成
    if (invalid)
    {
        // 生肖一个字 + MAC 尾两位：不出现"猪狗""狗鸡"这类歧义组合
        const char *zodiac[12] = {"鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"};
        uint32_t seed = esp_random();
        int a = seed % 12;
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);   // efuse 直读，无需 WiFi 启动
        snprintf(gMyName, sizeof(gMyName), "%s%02X%02X", zodiac[a], mac[4], mac[5]);
        walkie::saveName(String(gMyName));
    }
    else
    {
        strncpy(gMyName, n.c_str(), sizeof(gMyName) - 1);
        gMyName[sizeof(gMyName) - 1] = 0;
    }
    gChannel = st.channel;
    gVolume = st.volume;
    g_passBits = st.passBits;
    g_passLen = st.passLen;
    walkie::derivePasswordKey(g_crypto, g_passBits, g_passLen);
}

static void saveChannel() { walkie::saveChannel(gChannel); }

static void savePass()
{
    walkie::savePassword(g_passBits, g_passLen);
    walkie::derivePasswordKey(g_crypto, g_passBits, g_passLen);
}

static void saveVolume() { walkie::saveVolume(gVolume); }

// ========== 邻居表 ==========
static walkie::PeerEntry gPeers[walkie::kMaxPeers];
static walkie::Atomic<bool> gPeersDirty{false};
static walkie::Atomic<bool> gPeerJoined{false};   // 有新邻居上线，主循环响提示音

static void peerTouch(const uint8_t *mac, const char *name, uint32_t now, bool isVoice)
{
    bool joined = walkie::peerTouch(gPeers, walkie::kMaxPeers, mac, name, now, isVoice,
                                    walkie::PEER_TIMEOUT_MS, "同伴");
    gPeersDirty = true;
    if (joined) gPeerJoined = true;
}

static int findTalker(uint32_t now)
{
    return walkie::findTalker(gPeers, walkie::kMaxPeers, now, walkie::TALK_TIMEOUT_MS);
}

static void prunePeers(uint32_t now)
{
    if (walkie::prunePeers(gPeers, walkie::kMaxPeers, now, walkie::PEER_TIMEOUT_MS)) gPeersDirty = true;
}

// ========== N32 Codec 协议 ==========

static walkie::Atomic<bool> g_n32Inited{false}; // 0x0f 外设总线初始化是否成功
static SemaphoreHandle_t g_uartMux = nullptr;
struct UartLock {
    // 必须递归：codecPlayPCM 持锁后会调 codecPlaybackStart -> codecWriteReg 再取同一把锁
    UartLock()  { if (g_uartMux) xSemaphoreTakeRecursive(g_uartMux, portMAX_DELAY); }
    ~UartLock() { if (g_uartMux) xSemaphoreGiveRecursive(g_uartMux); }
};
static uint8_t txBuf[260];

static bool codecSend(const uint8_t *frame, size_t len)
{
    return Serial1.write(frame, len) == len;
}

// N32 对命令应答是定长 3 字节，发命令前必须清空残留字节，否则 ACK 永远对不上
static void uartFlushIn()
{
    while (Serial1.available() > 0) Serial1.read();
}

// 写寄存器：0x90 <reg> <len> <payload...> <checksum> -> 应答 0x91 <reg> <accepted>
static bool codecWriteReg(uint8_t reg, const uint8_t *payload, size_t len,
                          uint8_t *accepted, uint16_t timeoutMs)
{
    UartLock lock;
    size_t frameLen = walkie::buildWriteFrame(txBuf, sizeof(txBuf), reg, payload, len);
    if (frameLen == 0) return false;
    uartFlushIn();
    if (!codecSend(txBuf, frameLen)) return false;

    uint8_t ack[3] = {0};
    Serial1.setTimeout(timeoutMs);
    if (Serial1.readBytes(ack, 3) != 3) return false;
    walkie::AckFrame a = walkie::parseAck(ack, 3);
    if (!a.valid || a.reg != reg) return false;
    if (accepted) *accepted = a.value;
    return true;
}

// 原厂 boot.py 必做：0x0f 初始化 N32 外设总线。不发这条，N32 命令照样 ACK，
// 但 DAC/功放不出声。注意 WiFi 射频启动时的电源跌落会让 N32 复位丢失该状态，
// 所以每次重建播放链路都要重发。
static bool n32BusInit()
{
    uint8_t one = 1;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        uartFlushIn();
        size_t frameLen = walkie::buildWriteFrame(txBuf, sizeof(txBuf), walkie::REG_BUS_INIT, &one, 1);
        if (frameLen == 0) return false;
        codecSend(txBuf, frameLen);
        Serial1.setTimeout(200);
        uint8_t rsp[1];
        if (Serial1.readBytes(rsp, 1) >= 1) { g_n32Inited = true; return true; }
        uartFlushIn();
        delay(20);
    }
    return false;
}

static bool codecControl(uint8_t payload)
{
    return codecWriteReg(walkie::REG_CTRL, &payload, 1, nullptr, 400);
}

// N32 播放前必须收到 44 字节 WAV 头，声明 24000Hz / 8bit / mono（流式，长度 0xFFFFFFFF）
static bool g_playReady = false;
static uint32_t g_lastPlayMs = 0;
static char g_i2cScan[24] = "I2C:?";     // I2C 扫描结果

// 扫描 I2C 总线（功放 NS41508 手册标称 0x18，加速度计也在这条总线上）
static void i2cScan()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
    char *p = g_i2cScan;
    int n = snprintf(p, sizeof(g_i2cScan), "I2C:");
    p += n;
    for (uint8_t addr = 0x08; addr < 0x78; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            int room = (int)sizeof(g_i2cScan) - (int)(p - g_i2cScan);
            if (room > 4) p += snprintf(p, room, "%02X ", addr);
        }
    }
    if (p == g_i2cScan + 4) snprintf(g_i2cScan, sizeof(g_i2cScan), "I2C:none");
}
static walkie::Atomic<uint32_t> g_playErr{0}; // 播放写失败次数
static walkie::Atomic<uint16_t> g_underrun{0};
static walkie::Atomic<uint16_t> g_playRestart{0};
static walkie::Atomic<uint32_t> g_playOk{0};  // 播放写成功次数
static walkie::Atomic<uint32_t> g_rxVoice{0}; // 收到的语音包数
static walkie::Atomic<uint32_t> g_rxBeacon{0};// 收到的心跳包数
static walkie::Atomic<uint8_t> g_txPeak{0};   // 麦克风采集到的峰值（0..127）
static walkie::Atomic<uint8_t> g_rxPeak{0};   // 送去播放的峰值（0..127）
static walkie::Atomic<uint16_t> g_micHeal{0}; // 发送端麦克风自愈次数
static walkie::Atomic<uint16_t> g_rxOver{0};  // 接收环形缓冲溢出丢弃字节数

static bool codecPlaybackStart()
{
    // WiFi 启动的电源跌落可能让 N32 复位丢掉外设总线初始化，每次重建都重发
    n32BusInit();
    uint8_t stop = 0;
    codecWriteReg(0x10, &stop, 1, nullptr, 400);        // 半双工：先停录音
    uint8_t hwvol = 6;                                   // 硬件音量拉满，音量走软件
    codecWriteReg(0x14, &hwvol, 1, nullptr, 400);

    constexpr uint32_t rate = 24000;
    const uint8_t wav[44] = {
        'R','I','F','F', 0xFF,0xFF,0xFF,0xFF,
        'W','A','V','E','f','m','t',' ',
        0x10,0x00,0x00,0x00,                            // fmt chunk size = 16
        0x01,0x00,                                      // PCM
        0x01,0x00,                                      // mono
        (uint8_t)rate,(uint8_t)(rate>>8),(uint8_t)(rate>>16),(uint8_t)(rate>>24),
        (uint8_t)rate,(uint8_t)(rate>>8),(uint8_t)(rate>>16),(uint8_t)(rate>>24),
        0x01,0x00,                                      // block align = 1
        0x08,0x00,                                      // 8 bits per sample
        'd','a','t','a', 0xFF,0xFF,0xFF,0xFF,
    };
    // WAV 头是关键命令，失败重试（原厂注释：短超时会引发链路异常）
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++)
    {
        ok = codecWriteReg(0x15, wav, sizeof(wav), nullptr, 400);
        if (!ok) { uartFlushIn(); delay(30); }
    }
    if (!ok) return false;
    g_playReady = true;
    g_lastPlayMs = millis();
    g_playRestart++;
    return true;
}

static bool codecPlayPCM(const uint8_t *data, size_t len)
{
    UartLock lock;
    if (len == 0) return false;
    // 静音超过 800ms 后 N32 可能退出播放态，需要重发 WAV 头
    if (g_playReady && millis() - g_lastPlayMs > 800) g_playReady = false;
    if (!g_playReady && !codecPlaybackStart()) return false;

    size_t sent = 0;
    int stalled = 0;
    while (sent < len)
    {
        size_t n = len - sent;
        if (n > 200) n = 200;
        uint8_t accepted = 0;
        if (!codecWriteReg(0x15, data + sent, n, &accepted, 400))
        {
            g_playReady = false;
            g_playErr++;
            return false;
        }
        if (accepted == 0)
        {
            if (++stalled > 20) { g_playReady = false; return false; }
            // 背压时让出 UART，否则 TX 侧读麦克风被饿死（厂商同样短等）
            xSemaphoreGiveRecursive(g_uartMux);
            vTaskDelay(pdMS_TO_TICKS(5));
            xSemaphoreTakeRecursive(g_uartMux, portMAX_DELAY);
            continue;
        }
        if (accepted > n) accepted = (uint8_t)n;
        sent += accepted;
        stalled = 0;
        g_playOk++;
        g_lastPlayMs = millis();
        // 每块之间让出总线，给按键/UI/TX 机会（厂商做法）
        xSemaphoreGiveRecursive(g_uartMux);
        taskYIELD();
        xSemaphoreTakeRecursive(g_uartMux, portMAX_DELAY);
    }
    return true;
}



static bool codecStartMic() { g_playReady = false; return codecControl(0x01); }
static bool codecStopMic()   { g_playReady = false; return codecControl(0x00); }

static bool codecSetVolume(uint8_t vol)
{
    if (vol > 6) vol = 6;
    return codecWriteReg(walkie::REG_VOLUME, &vol, 1, nullptr, 400);
}

static size_t codecReadMicFrame(uint8_t *buf, size_t bufSize = 250)
{
    UartLock lock;
    // 一次读 160 字节（6.7ms 音频），留足余量，否则 N32 麦克风 FIFO 会溢出后停摆
    uartFlushIn();
    txBuf[0] = 0x80; txBuf[1] = 0x11; txBuf[2] = 160;
    if (!codecSend(txBuf, 3)) return 0;

    uint8_t head[3] = {0};
    Serial1.setTimeout(15);
    if (Serial1.readBytes(head, 3) != 3) return 0;
    if (head[0] != 0x81 || head[1] != 0x11) return 0;

    uint8_t plen = head[2];
    if (plen == 0 || plen > bufSize) return 0;

    // 帧体读进调用者自己的缓冲：原来读进全局 rxBuf，与 codecWriteReg
    // 的 ACK 解析（也写 rxBuf）跨任务竞态，校验和随机失败 → 读麦返 0 → 断流(S)
    if (Serial1.readBytes(buf, plen + 1) != (size_t)(plen + 1)) return 0;

    uint8_t sum = head[0] + head[1] + head[2];
    for (size_t i = 0; i < plen; i++) sum += buf[i];
    if (sum != buf[plen]) return 0;
    return plen;
}

static void codecInit()
{
    if (!g_uartMux) g_uartMux = xSemaphoreCreateRecursiveMutex();
    Serial1.begin(AUDIO_BAUD, SERIAL_8N1, AUDIO_RX_PIN, AUDIO_TX_PIN);
    // N32 是独立 MCU，上电自举需要时间，否则首条命令必丢
    delay(N32_BOOT_DELAY_MS);
    uartFlushIn();
    Serial1.setTimeout(400);

    // 原厂 boot.py 必做：0x0f 初始化 N32 协处理器（返回 1 字节才算成功），
    // 失败要等 20ms 重试一次。不发这条，N32 处在未初始化状态：命令能 ACK，
    // 但 DAC/功放不出声。

    n32BusInit();
    codecControl(0x00);  // 停麦
    codecSetVolume(6);   // 硬件音量拉满，音量走软件
    uartFlushIn();
}

// ========== ESP-NOW ==========
static uint8_t txSeq = 0;
static uint8_t txVoiceSeq = 0;   // 语音独立序号，beacon 不占坑
// 闭环功率控制：收端测丢包率经 beacon 回传，发端三档自动切换。
// 高丢包+弱信号=距离远→升档；高丢包+强信号=近距饱和→降档。
static walkie::Atomic<uint8_t> g_pwrLvl{1};            // 当前档（默认 13dBm）
static walkie::Atomic<uint8_t> g_fbLoss{0};            // 对端回传的丢包率%
static walkie::Atomic<int8_t> g_fbRssi{0};      // 对端收我的 RSSI
static walkie::Atomic<uint32_t> g_fbMs{0};             // 反馈时间戳
static walkie::Radio g_radio;
static walkie::Atomic<uint16_t> g_seqGap{0};    // 空口丢包数（序号缺口）
static walkie::LossWindow g_lossWin;   // 2s 窗口丢包率
static walkie::Atomic<uint8_t> g_lossPct{0};    // 最近窗口丢包率%（beacon 回传用）
static walkie::Atomic<int8_t> g_peerRssi{0};    // 最近语音包 RSSI
static walkie::Atomic<uint16_t> g_rxStall{0};   // 收包间隔>60ms 次数（发射侧断流）
static walkie::Atomic<uint16_t> g_txStarve{0};  // TX 读麦间隔>15ms 次数（采集被饿）
static walkie::Atomic<uint32_t> g_lastVoiceRxMs{0};  // 最近收到语音包时刻（beacon 快报用）
static uint8_t rxAudioBuf[4096];
static walkie::Atomic<size_t> rxWritePos{0};
static walkie::Atomic<size_t> rxReadPos{0};

static void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!data || len < 4 || len > 250) return;
    if (data[0] != walkie::kMagic) return;
    uint32_t now = millis();

    if (data[1] == walkie::PKT_VOICE)
    {
        static walkie::VoiceRxTracker g_rxTracker;
        uint8_t seq = data[2];
        walkie::VoiceRxTracker::Event ev =
            g_rxTracker.onVoicePacket(seq, now, walkie::VOICE_GAP_RESET_MS, walkie::VOICE_STALL_MIN_MS,
                                      walkie::VOICE_STALL_MAX_MS);
        if (ev.newSession)
        {
            // 新一句开始：清零会话计数，读数只反映当前这句
            g_seqGap = 0; g_rxStall = 0; g_underrun = 0; g_playRestart = 0;
        }
        if (ev.gap)
        {
            g_seqGap += ev.gap;   // 空口丢包
            walkie::lossWindowAdd(g_lossWin, 0, ev.gap);
        }
        walkie::lossWindowAdd(g_lossWin, 1, 0);
        if (ev.stalled) g_rxStall++;                       // 发射侧断流
        g_lastVoiceRxMs = now;
        g_rxVoice++;
        g_peerRssi = info->rx_ctrl->rssi;
        peerTouch(info->src_addr, nullptr, now, true);
        uint8_t dec[250];
        walkie::VoiceDecoded vd = walkie::decodeVoicePacket(g_crypto, dec, sizeof(dec), data, len, seq);
        if (vd.badKey) { g_rxBadKey++; return; }
        if (!vd.payload || vd.len <= 0) return;
        for (int i = 0; i < vd.len; i++)
        {
            if (!walkie::ringPush(rxAudioBuf, sizeof(rxAudioBuf), rxWritePos, rxReadPos, vd.payload[i]))
            { g_rxOver++; break; }
        }
    }
    else if (data[1] == walkie::PKT_BEACON)
    {
        g_rxBeacon++;
        walkie::BeaconFeedback fb = walkie::parseBeacon(data, len);
        if (fb.valid)
        {
            char nm[12] = {0};
            memcpy(nm, data + 3, fb.nameLen);
            peerTouch(info->src_addr, nm, now, false);
            g_fbLoss = fb.lossPct;
            g_fbRssi = fb.rssi;
            g_fbMs = now;
        }
    }
}


enum class Mode { Boot, Home, Transmit, Pairing };
static Mode g_mode = Mode::Boot;

// ===== RX 播放任务（参考 CardieTalkie：音频必须独立于 UI 任务）=====
// 之前音频和 40KB sprite 刷屏挤在同一个 loop() 里，刷屏一卡就断音。
static walkie::Atomic<bool> g_rxTaskRun{false};
static int g_upPrev = -1;

static void rxPlayTask(void *)
{
    // 起播水位：攒够 ~30ms 音频再开始，避免一开始就欠载
    constexpr size_t kStartBytes = 2000;  // ~250ms @8k，仅首次起播用
    bool primed = false;
    int gapFrames = 0;

    for (;;)
    {
        if (!g_rxTaskRun || g_mode == Mode::Transmit)
        {
            primed = false;
            g_upPrev = -1;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t avail = walkie::ringAvailable(rxWritePos, rxReadPos, sizeof(rxAudioBuf));
        if (!primed)
        {
            if (avail < kStartBytes) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            primed = true;
            gapFrames = 0;
        }
        if (avail < walkie::kSamplesPerFrame)   // 不足一帧（8.25ms）
        {
            // 丢帧补偿（CardieTalkie 思路）：不停流、不重攒 250ms，
            // 喂"上一样本按 3/4 衰减"的补偿帧保活 N32 链路，
            // 数据一回来立即续播。原来欠载→重新预缓冲把断口放大成 250ms+重建。
            if (gapFrames == 0) g_underrun++;   // 每次断供只计 1 次
            if (++gapFrames > 50)               // 断供 >400ms 才回到完整预缓冲
            {
                primed = false;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            uint8_t fill[walkie::kBytesPerFrame];
            size_t fillLen = walkie::gapFrame(g_upPrev, gVolume, fill, sizeof(fill));
            codecPlayPCM(fill, fillLen);
            taskYIELD();
            continue;
        }
        gapFrames = 0;

        // 一帧 = 66 个 8k 样本 ≈ 8.25ms 音频，插值成 198 字节 24k
        uint8_t chunk[walkie::kBytesPerFrame];
        size_t i = 0;
        while (i + walkie::kUpScale <= sizeof(chunk))
        {
            uint8_t b = 0;
            if (!walkie::ringPop(rxAudioBuf, sizeof(rxAudioBuf), rxReadPos, rxWritePos, b)) break;
            int smp = (int)b - walkie::kSilence;
            uint8_t amp = walkie::amplitudeOf(smp);
            if (amp > g_rxPeak) g_rxPeak = amp;
            i += walkie::upsample(smp, g_upPrev, gVolume, chunk + i, sizeof(chunk) - i);
        }
        if (i > 0) codecPlayPCM(chunk, i);
        // 不加人为延时：UART 写入 + N32 accepted 背压天然就是实时节拍
        // （厂商 N32Audio::Write 同样只在缓冲满时短等，块间 taskYIELD）
        taskYIELD();
    }
}

static void espnowSendPCM(const uint8_t *data, size_t len);

// ===== TX 采集任务：与 loop() 的刷屏/按键彻底解耦 =====
// U148 ≈ 每0.5s欠载一次，周期正好是 loop 里的 UI 刷新：
// 刷屏时没人读麦克风 → 断供。采集必须有自己的任务。
static walkie::Atomic<bool> g_txRun{false};
static uint8_t txMicBuf[250];   // 独立缓冲，不与帧解析的 rxBuf 共用

static void txCaptureTask(void *)
{
    uint8_t pkt[200];
    size_t pos = 0;
    uint32_t dsSum = 0;
    uint8_t dsCnt = 0;
    uint32_t lastOk = 0;

    for (;;)
    {
        if (!g_txRun)
        {
            pos = 0; dsSum = 0; dsCnt = 0; lastOk = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t n = codecReadMicFrame(txMicBuf, sizeof(txMicBuf));
        if (n == 0)
        {
            // 只在真正断供 200ms 后重开麦（实测周期性"保活"有害）
            if (lastOk && millis() - lastOk > 200)
            {
                codecStartMic();
                g_micHeal++;
                lastOk = millis();
                pos = 0; dsSum = 0; dsCnt = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        {
            uint32_t t = millis();
            if (lastOk && t - lastOk > 15) g_txStarve++;   // 正常读麦周期 ~7ms
            lastOk = t;
        }

        uint8_t peak = 0;
        for (size_t i = 0; i < n; i++)
        {
            uint8_t smp = txMicBuf[i];
            uint8_t amp = (uint8_t)abs((int16_t)smp - 128);
            if (amp > peak) peak = amp;
            dsSum += smp;
            if (++dsCnt == 3)               // 24k → 8k，3取1平均即抗混叠
            {
                pkt[pos++] = (uint8_t)(dsSum / 3);
                dsSum = 0; dsCnt = 0;
                if (pos >= sizeof(pkt))
                {
                    espnowSendPCM(pkt, pos);   // 满 25ms 立即发
                    pos = 0;
                }
            }
        }
        if (peak > g_txPeak) g_txPeak = peak;
        // 读麦本身 ~7ms UART 阻塞就是节拍，不加人为延时
        taskYIELD();
    }
}


static int8_t g_chRssi[14];      // 各信道最强 AP 信号，0=未扫到
static uint8_t g_chCount[14];    // 各信道 AP 数
static bool g_scanned = false;

static void scanChannels();
static walkie::Atomic<bool> g_scanBusy{false};
static walkie::Atomic<bool> g_scanReq{false};

static void scanTask(void *)
{
    for (;;)
    {
        if (g_scanReq && !g_scanBusy)
        {
            g_scanReq = false;
            g_scanBusy = true;
            scanChannels();
            g_scanBusy = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void scanChannels()
{
    memset(g_chRssi, 0, sizeof(g_chRssi));
    memset(g_chCount, 0, sizeof(g_chCount));
    int n = WiFi.scanNetworks(false, false, false, 120);
    for (int i = 0; i < n && i < 32; i++)
    {
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 13)
        {
            g_chCount[ch]++;
            int32_t r = WiFi.RSSI(i);
            if (g_chRssi[ch] == 0 || r > g_chRssi[ch]) g_chRssi[ch] = (int8_t)r;
        }
    }
    WiFi.scanDelete();
    g_radio.setChannel(gChannel);  // 扫完回到工作信道
    g_scanned = true;
}

static void applyChannel(uint8_t ch)
{
    gChannel = ch;
    g_radio.setChannel(ch);
    saveChannel();
}

static void initEspNow()
{
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    // 必须关省电：省电模式下 STA 周期性休眠，ESP-NOW 广播帧大量丢失，
    // 表现为"对讲几秒后突然断掉"。参考实现同样这么处理。
    esp_wifi_set_ps(WIFI_PS_NONE);
    // 近距离 RX 射频饱和会造成突发丢包（G 成串增长、F/N=0）。
    // 13dBm 折中：8.5 近距好但 6-7m 丢 25%，19.5 近距饱和。
    // 初始 13dBm，之后闭环自动调。
    g_radio.init(gChannel, walkie::kPwrDbm[g_pwrLvl], onDataRecv);

    // WiFi 射频启动瞬间的电源跌落/干扰会让 N32 的音频前端失效，
    // 之后命令照样 ACK 但不出声。这里等电源稳定后重建一次链路。
    delay(150);
    n32BusInit();
    codecSetVolume(6);
}

static void sendBeacon()
{
    uint8_t b[20];
    size_t n = walkie::buildBeacon(b, sizeof(b), txSeq++, gMyName, g_lossPct, g_peerRssi);
    if (n > 0) g_radio.send(b, (int)n);
}

static void espnowSendPCM(const uint8_t *data, size_t len)
{
    while (len > 0)
    {
        int chunk = len > walkie::kVoiceChunkMax ? walkie::kVoiceChunkMax : (int)len;
        uint8_t pkt[250];
        uint8_t seq = txVoiceSeq++;
        int pktLen = walkie::buildVoicePacket(g_crypto, pkt, sizeof(pkt), seq, data, chunk);
        if (pktLen <= 0) break;
        if (!g_radio.send(pkt, pktLen)) { /* txFail 已由 Radio 计数 */ }
        data += chunk; len -= chunk;
    }
}

// ========== UI / 模式 ==========

static unsigned long pairingStart = 0;
static bool prevKeyA = false, prevKeyB = false, prevKeyC = false, prevKeyD = false;
static unsigned long lastVolUpdate = 0;
static unsigned long lastBeacon = 0;
static unsigned long lastBarRedraw = 0;
static unsigned long lastHomeCheck = 0;
static unsigned long lastPairDraw = 0;
static int lastShownTalker = -2;   // -2=未画过, -1=没人
static uint8_t lastDrawnVol = 0xFF;

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

static void drawDiag()
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
             (unsigned)g_underrun, (unsigned)g_playRestart,
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

static void drawHomeStatus(uint32_t now)
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

static void drawHomeFrame(uint32_t now)
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

static void drawPairing()
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

static void showBoot()
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

// ========== 模式切换 ==========
static void enterHome()
{
    g_passEdit = false;
    g_txRun = false;          // 先停采集任务，再动麦克风
    delay(15);
    codecStopMic();
    walkie::ringReset(rxWritePos, rxReadPos);
    g_mode = Mode::Home;
    lastShownTalker = -2;
    lastDrawnVol = 0xFF;
    drawHomeFrame(millis());
}

static void enterTransmit()
{
    g_txRun = false;
    delay(15);
    codecStopMic();
    // 半双工换向：丢掉之前攒下的接收音频，否则松手后会补播"上一句"
    walkie::ringReset(rxWritePos, rxReadPos);
    delay(5);
    codecStartMic();
    g_mode = Mode::Transmit;
    g_txRun = true;           // 麦克风就绪后再放行采集
    g_txStarve = 0; g_micHeal = 0; g_radio.txFail = 0; g_radio.txNack = 0;   // 本次通话从零计
    // 注意：G/S 在接收端看，U/R 也在接收端；发射端清零自己的 U 不影响对端
    lastDrawnVol = 0xFF;
    drawHomeFrame(millis());
}

static void enterPairing()
{
    codecStopMic();
    if (!g_scanned && !g_scanBusy) g_scanReq = true;   // 异步扫描，不卡 UI
    g_mode = Mode::Pairing;
    pairingStart = millis();
    lastPairDraw = 0;
    drawPairing();
}

static void showVolumeBar(uint8_t vol)
{
    if (g_mode != Mode::Transmit) return;
    lastVolUpdate = millis();
    // 直写 LCD 小区域：整帧 pushSprite+waitDMA 要几十 ms，
    // 每 200ms 一次会把 TX 任务饿出周期性欠载（U 每 3-5s +1）。
    int w = (int)((uint32_t)vol * (SCREEN_WIDTH - 20) / 255);
    display.fillRect(10, BAR_Y, SCREEN_WIDTH - 20, BAR_H, TFT_DARKGREY);
    if (w > 0) display.fillRect(10, BAR_Y, w, BAR_H, TFT_GREEN);
}

void setup()
{
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

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

    initIdentity();
    showBoot();
    delay(400);

    i2cScan();
    codecInit();
    initEspNow();

    // 全部按键：INPUT_PULLDOWN，按下=HIGH（A 已实测有效）
    pinMode(KEY_A, INPUT_PULLDOWN);
    pinMode(KEY_B, INPUT_PULLDOWN);
    pinMode(KEY_C, INPUT_PULLDOWN);
    pinMode(KEY_D, INPUT_PULLDOWN);

    enterHome();

    xTaskCreatePinnedToCore(rxPlayTask, "rxplay", 4096, nullptr, 5, nullptr, 0);
    g_rxTaskRun = true;
    xTaskCreatePinnedToCore(txCaptureTask, "txcap", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(scanTask, "scan", 4096, nullptr, 1, nullptr, 0);
}

static constexpr size_t PLAY_CHUNK = 300;  // 100 个 8k 样本插值成 24k

void loop()
{
    uint32_t now = millis();
    bool a = digitalRead(KEY_A) == HIGH;
    bool b = digitalRead(KEY_B) == HIGH;
    bool c = digitalRead(KEY_C) == HIGH;
    bool d = digitalRead(KEY_D) == HIGH;
    // 全局按键边沿（松→按 = 一次点击，按住不重复）
    bool eA = a && !prevKeyA, eB = b && !prevKeyB, eC = c && !prevKeyC, eD = d && !prevKeyD;

    if (g_mode == Mode::Home)
    {
        if (eA)
        {
            enterTransmit();
        }
        else if (eB)
        {
            enterPairing();
        }
        else if (eC)
        {
            if (gVolume > 0) gVolume--;
            saveVolume();
            gVolDisplayUntil = now + walkie::VOICE_RX_RECENT_MS;
            drawHomeStatus(now);
        }
        else if (eD)
        {
            if (gVolume < 9) gVolume++;
            saveVolume();
            gVolDisplayUntil = now + walkie::VOICE_RX_RECENT_MS;
            drawHomeStatus(now);
        }
    }
    else if (g_mode == Mode::Pairing)
    {
        if (g_passEdit)
        {
            // 键位（用户定稿）：C=0 D=1 B=清除 A=确定(保存并退回配置页)
            // 三键板(C3)兼容预案：B=0 C=1 A=确定，长按B=清除——
            // 语义层已按"位1/位0/清除/确定"四个动作抽象，换板只改映射。
            // 按键边沿触发：上升沿才进位，松→按=一次点击
            if (eC && g_tmpLen < 16)
            {
                g_tmpBits = (g_tmpBits << 1) | 0;   // C = 位 0
                g_tmpLen++;
                drawPairing();
            }
            else if (eD && g_tmpLen < 16)
            {
                g_tmpBits = (g_tmpBits << 1) | 1;   // D = 位 1
                g_tmpLen++;
                drawPairing();
            }
            else if (eB)
            {
                // 短按删一位；长按 800ms 全清
                if (g_bHeldMs == 0) g_bHeldMs = now;
                else if (now - g_bHeldMs > 800 && g_tmpLen)
                {
                    g_bHeldMs = now + 100000;   // 防重复触发
                    g_tmpBits = 0; g_tmpLen = 0;
                    drawPairing();
                }
            }
            else
            {
                if (g_bHeldMs && g_bHeldMs < now && now - g_bHeldMs < 800)
                {
                    if (g_tmpLen) { g_tmpLen--; g_tmpBits >>= 1; }
                    drawPairing();
                }
                g_bHeldMs = 0;
            }
            if (eA)
            {
                g_passBits = g_tmpBits;
                g_passLen = g_tmpLen;
                savePass();
                g_passEdit = false;   // 确定 → 回配置页
                drawPairing();
            }
        }
        else
        {
            if (eB)
            {
                g_passEdit = true;
                g_tmpBits = g_passBits;
                g_tmpLen = g_passLen;
                drawPairing();
            }
            else if (eA)
            {
                enterHome();
            }
            else if (eC)
            {
                applyChannel(gChannel <= 1 ? 13 : gChannel - 1);
                drawPairing();
            }
            else if (eD)
            {
                applyChannel(gChannel >= 13 ? 1 : gChannel + 1);
                drawPairing();
            }
            else if (now - pairingStart > 30000)
            {
                enterHome();
            }
        }
    }
    else if (g_mode == Mode::Transmit)
    {
        if (!a)
        {
            enterHome();
        }
        else
        {
            // 采集/发包已全部在 txCaptureTask，loop 只画音量条
            if (now - lastBarRedraw > 200)
            {
                uint8_t vol = (uint8_t)(g_txPeak * 2);
                showVolumeBar(vol);
                lastBarRedraw = now;
                g_txPeak = 0;
            }
        }
    }

    // 丢包率窗口结算：供 beacon 回传给对端
    static uint32_t lastWin = 0;
    if (now - lastWin > walkie::LOSS_WINDOW_MS)
    {
        lastWin = now;
        g_lossPct = walkie::lossWindowPercent(g_lossWin);
        walkie::lossWindowReset(g_lossWin);
    }

    // 心跳广播（TX 时让位给语音；正在收话时 500ms 快报，让发射端尽快升档）
    uint32_t beaconIv = (now - g_lastVoiceRxMs < walkie::VOICE_RX_RECENT_MS) ? walkie::BEACON_FAST_MS : walkie::BEACON_INTERVAL_MS;
    if (g_mode != Mode::Transmit && now - lastBeacon > beaconIv)
    {
        sendBeacon();
        lastBeacon = now;
    }

    // 邻居上线提示音
    if (gPeerJoined)
    {
        gPeerJoined = false;
        if (g_mode != Mode::Transmit) walkie::playCoinChime(codecPlayPCM);
    }

    // 闭环功率控制：依据对端反馈，1s 最多动一档
    static uint32_t lastPwrAdj = 0;
    if (now - lastPwrAdj > walkie::PWR_ADJ_INTERVAL_MS)
    {
        lastPwrAdj = now;
        bool fresh = (g_fbMs != 0 && now - g_fbMs < walkie::PWR_FB_FRESH_MS);
        uint8_t lvl = walkie::nextPowerLevel(g_pwrLvl, fresh, g_fbLoss, g_fbRssi,
                                             g_mode == Mode::Transmit);
        if (lvl != g_pwrLvl)
        {
            g_pwrLvl = lvl;
            g_radio.setTxPower(walkie::kPwrDbm[lvl]);
        }
    }

    // 邻居表维护
    static uint32_t lastPrune = 0;
    if (now - lastPrune > walkie::PRUNE_INTERVAL_MS)
    {
        lastPrune = now;
        prunePeers(now);
    }

    // 诊断行周期刷新
    static uint32_t lastDiag = 0;
    if (now - lastDiag > DIAG_INTERVAL_MS)
    {
        lastDiag = now;
        drawDiag();
    }

    // 首页：谁在说话变化时/音量提示超时时只刷状态行
    if (g_mode == Mode::Home && now - lastHomeCheck > 150)
    {
        lastHomeCheck = now;
        int t = findTalker(now);
        bool volShowing = (now < gVolDisplayUntil);
        static bool lastVolShowing = false;
        if (t != lastShownTalker || volShowing || lastVolShowing != volShowing)
        {
            lastShownTalker = t;
            lastVolShowing = volShowing;
            drawHomeStatus(now);
        }
    }

    // 配对页：邻居变化或有周期刷新
    if (g_mode == Mode::Pairing && (gPeersDirty || now - lastPairDraw > 1000))
    {
        gPeersDirty = false;
        lastPairDraw = now;
        drawPairing();
    }

    prevKeyA = a; prevKeyB = b; prevKeyC = c; prevKeyD = d;

    delay(1);
}
