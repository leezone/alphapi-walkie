// AlphaPi Zero — 对讲机固件（ESP32-C3 + HT 协控，5×5 点阵）
// 从 One S 版 alphapi-walkie 移植：协控 0x90 帧协议同源（STEM-Radio 已验证）
// 音频：8bit@24k 播放（协控固定消耗速率，WAV 头不读）；麦克风 0x80/0x11 读帧
// 按键：A=GPIO10 B=GPIO1 C=GPIO3，PULL_DOWN 按下=HIGH
// 组合键：A按住=PTT；A+B/C=音量；B+A/C=频道；C+A/B=密码位（A=0 B=1）；C+A/B≥10s=清密码

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <string.h>
#include <atomic>

#include "core/walkie_protocol.h"
#include "core/walkie_crypto.h"
#include "core/walkie_peer.h"
#include "core/walkie_ring.h"
#include "core/walkie_rx.h"
#include "core/walkie_timing.h"
#include "core/walkie_atomic.h"
#include "core/walkie_codec.h"
#include "core/walkie_store.h"
#include "core/walkie_chime.h"
#include "core/walkie_audio.h"
#include "core/walkie_radio.h"
#include "device/config.h"

// ========== 硬件引脚 ==========
// ========== 协控 UART（STEM-Radio 已验证：RX=9 TX=8 @460929） ==========
static HardwareSerial coUart(1);

static SemaphoreHandle_t g_uartMux = nullptr;
struct UartLock {
    UartLock()  { if (g_uartMux) xSemaphoreTakeRecursive(g_uartMux, portMAX_DELAY); }
    ~UartLock() { if (g_uartMux) xSemaphoreGiveRecursive(g_uartMux); }
};
static uint8_t txBuf[260];

static void uartFlushIn()
{
    while (coUart.available() > 0) coUart.read();
}

// 写寄存器：0x90 <reg> <len> <payload...> <checksum> -> 应答 0x91 <reg> <accepted>
static bool codecWriteReg(uint8_t reg, const uint8_t *payload, size_t len,
                          uint8_t *accepted, uint16_t timeoutMs)
{
    UartLock lock;
    size_t frameLen = walkie::buildWriteFrame(txBuf, sizeof(txBuf), reg, payload, len);
    if (frameLen == 0) return false;
    uartFlushIn();
    coUart.write(txBuf, frameLen);

    uint8_t ack[3] = {0};
    coUart.setTimeout(timeoutMs);
    if (coUart.readBytes(ack, 3) != 3) return false;
    walkie::AckFrame a = walkie::parseAck(ack, 3);
    if (!a.valid || a.reg != reg) return false;
    if (accepted) *accepted = a.value;
    return true;
}

static bool codecControl(uint8_t payload)
{
    return codecWriteReg(walkie::REG_CTRL, &payload, 1, nullptr, 400);
}

static bool codecSetVolume(uint8_t vol)
{
    if (vol > 6) vol = 6;
    return codecWriteReg(walkie::REG_VOLUME, &vol, 1, nullptr, 400);
}

// ========== 播放链路（STEM-Radio 验证序列：0x14音量 → 0x10/00 → WAV头 → PCM） ==========
static bool g_playReady = false;
static uint32_t g_lastPlayMs = 0;
static walkie::Atomic<uint16_t> g_playRestart{0};
static walkie::Atomic<uint32_t> g_playOk{0};
static walkie::Atomic<uint32_t> g_playErr{0};
static walkie::Atomic<uint16_t> g_underrun{0};

static bool codecPlaybackStart()
{
    // Zero 不需要 0x0f 总线初始化（STEM-Radio 无此命令）
    uint8_t stop = 0;
    codecWriteReg(walkie::REG_CTRL, &stop, 1, nullptr, 400);   // 停录/播放启动
    uint8_t hwvol = 6;                                 // 硬件音量拉满，音量走软件
    codecWriteReg(walkie::REG_VOLUME, &hwvol, 1, nullptr, 400);

    const uint8_t wav[44] = {
        'R','I','F','F', 0xFF,0xFF,0xFF,0xFF,
        'W','A','V','E','f','m','t',' ',
        0x10,0x00,0x00,0x00,
        0x01,0x00, 0x01,0x00,
        (uint8_t)CO_RATE,(uint8_t)(CO_RATE>>8),(uint8_t)(CO_RATE>>16),(uint8_t)(CO_RATE>>24),
        (uint8_t)CO_RATE,(uint8_t)(CO_RATE>>8),(uint8_t)(CO_RATE>>16),(uint8_t)(CO_RATE>>24),
        0x01,0x00, 0x08,0x00,
        'd','a','t','a', 0xFF,0xFF,0xFF,0xFF,
    };
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++)
    {
        ok = codecWriteReg(walkie::REG_PLAY, wav, sizeof(wav), nullptr, 400);
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
    if (g_playReady && millis() - g_lastPlayMs > 800) g_playReady = false;
    if (!g_playReady && !codecPlaybackStart()) return false;

    size_t sent = 0;
    int stalled = 0;
    while (sent < len)
    {
        size_t n = len - sent;
        if (n > 160) n = 160;   // Zero 协控一帧吸收 128~192B，160 安全
        uint8_t accepted = 0;
        if (!codecWriteReg(walkie::REG_PLAY, data + sent, n, &accepted, 400))
        {
            g_playReady = false;
            g_playErr++;
            return false;
        }
        if (accepted == 0)
        {
            if (++stalled > 20) { g_playReady = false; return false; }
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
        xSemaphoreGiveRecursive(g_uartMux);
        taskYIELD();
        xSemaphoreTakeRecursive(g_uartMux, portMAX_DELAY);
    }
    return true;
}

// ========== 麦克风（运行时探测：One S 用 0x80/0x11，Zero 可能用 0x80/0x10） ==========
static uint8_t g_micReadReg = 0x11;   // 探测后确定：0x11 或 0x10
static uint8_t g_micStartPayload = 0x01; // 探测后确定：0x01 或 0x02
static bool g_micProbed = false;
static walkie::Atomic<uint16_t> g_micHeal{0};
static walkie::Atomic<uint8_t> g_txPeak{0};
static walkie::Atomic<uint8_t> g_rxPeak{0};

static bool codecStartMic() { g_playReady = false; return codecControl(g_micStartPayload); }
static bool codecStopMic()  { g_playReady = false; return codecControl(0x00); }

static size_t codecReadMicFrame(uint8_t *buf, size_t bufSize)
{
    UartLock lock;
    uartFlushIn();
    txBuf[0] = 0x80; txBuf[1] = g_micReadReg; txBuf[2] = 160;
    coUart.write(txBuf, 3);

    uint8_t head[3] = {0};
    coUart.setTimeout(15);
    if (coUart.readBytes(head, 3) != 3) return 0;
    if (head[0] != 0x81 || head[1] != g_micReadReg) return 0;

    uint8_t plen = head[2];
    if (plen == 0 || plen > bufSize) return 0;
    if (coUart.readBytes(buf, plen + 1) != (size_t)(plen + 1)) return 0;

    uint8_t sum = head[0] + head[1] + head[2];
    for (size_t i = 0; i < plen; i++) sum += buf[i];
    if (sum != buf[plen]) return 0;
    return plen;
}

// 开机探测麦克风协议：先试 One S 的 0x11/0x01，再试 Zero 手册的 0x10/0x02
static void probeMicProtocol()
{
    struct { uint8_t reg; uint8_t start; } candidates[] = {
        {0x11, 0x01},   // One S 验证过的组合
        {0x11, 0x02},   // One S reg + Zero start
        {0x10, 0x01},   // Zero reg + One S start
        {0x10, 0x02},   // Zero 手册组合
    };
    for (auto &c : candidates)
    {
        g_micReadReg = c.reg;
        g_micStartPayload = c.start;
        codecControl(0x00);  // 先停
        delay(50);
        if (!codecStartMic()) continue;
        delay(100);
        uint8_t tmp[250];
        size_t n = codecReadMicFrame(tmp, sizeof(tmp));
        if (n > 0)
        {
            g_micProbed = true;
            Serial.printf("[MIC] 探测成功: reg=0x%02X start=0x%02X (%u bytes)\n",
                          c.reg, c.start, (unsigned)n);
            codecStopMic();
            return;
        }
        codecStopMic();
        delay(50);
    }
    // 全部失败：默认用 One S 组合，运行时自愈
    g_micReadReg = 0x11;
    g_micStartPayload = 0x01;
    Serial.println("[MIC] 探测失败，默认 reg=0x11 start=0x01（运行时自愈）");
}

static void codecInit()
{
    if (!g_uartMux) g_uartMux = xSemaphoreCreateRecursiveMutex();
    coUart.setRxBufferSize(2048);
    coUart.begin(CO_BAUD, SERIAL_8N1, CO_RX, CO_TX);
    delay(HT_BOOT_DELAY_MS);   // HT 协控上电自举
    uartFlushIn();
    coUart.setTimeout(400);
    codecControl(0x00);   // 停麦
    codecSetVolume(6);
    uartFlushIn();
}

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
    UartLock lock;
    uint8_t f[9];
    mtxBuild(p, f);
    coUart.write(f, 9);
}

static bool mtxSend(const uint8_t p[25])
{
    UartLock lock;
    uint8_t f[9];
    mtxBuild(p, f);
    for (int att = 0; att < 3; att++)
    {
        uartFlushIn();
        coUart.write(f, 9);
        uint8_t resp[3] = {0};
        coUart.setTimeout(150);
        if (coUart.readBytes(resp, 3) == 3)
        {
            walkie::AckFrame a = walkie::parseAck(resp, 3);
            if (a.valid && a.reg == MTX_ADDR) return true;
        }
        delay(50);
    }
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
// 左箭头（信道-）
static const uint8_t MTX_ARR_L[25] = {
    0,1,0,0,0, 1,1,0,0,0, 1,1,1,1,1, 1,1,0,0,0, 0,1,0,0,0};
// 右箭头（信道+）
static const uint8_t MTX_ARR_R[25] = {
    0,0,1,0,0, 0,0,1,1,0, 1,1,1,1,1, 0,0,1,1,0, 0,0,1,0,0};
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
static void mtxShowChannel(uint8_t ch)
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

// ========== 全局状态（前向声明，供 initIdentity 等使用） ==========
static uint8_t gChannel = CHANNEL_DEFAULT;
static walkie::Atomic<uint8_t> gVolume{6};   // 软件音量 0..9
static uint16_t g_passBits = 0;
static uint8_t g_passLen = 0;
static walkie::CryptoState g_crypto;
static walkie::Atomic<uint16_t> g_rxBadKey{0};

// 本机身份：USB 收音机方案同款十六进制尾号（各字符均为 0-9A-F）
static char g_myName[12] = "Zero";   // beacon 发给各机器显示
static uint8_t g_myMac[6] = {0};

static void initIdentity()
{
    esp_read_mac(g_myMac, ESP_MAC_WIFI_STA);
    // ASCII 名：MAC 取 3 字节（3,4,5）拼成 6 位十六进制（0-9A-F），
    // 名字符集中在一个键位，五 ×五点阵（MTX_DIGITS 0-F）与字母字体均可显示
    snprintf(g_myName, sizeof(g_myName), "%02X%02X%02X", g_myMac[3], g_myMac[4], g_myMac[5]);

    walkie::StoredSettings st = walkie::loadSettings(CHANNEL_DEFAULT, CHANNEL_MIN, CHANNEL_MAX);
    gChannel = st.channel;
    gVolume = st.volume;
    g_passBits = st.passBits;
    g_passLen = st.passLen;
    walkie::derivePasswordKey(g_crypto, g_passBits, g_passLen);
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

// ========== 加密（与 One S 完全同源，保证互通） ==========
// 实现见 core/walkie_crypto.h

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
static walkie::Atomic<bool> gPeerJoined{false};

static void peerTouch(const uint8_t *mac, const char *name, uint32_t now, bool isVoice)
{
    bool joined = walkie::peerTouch(gPeers, walkie::kMaxPeers, mac, name, now, isVoice,
                                    walkie::PEER_TIMEOUT_MS, "Zero");
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

// ========== ESP-NOW（与 One S 完全同源） ==========
static uint8_t txSeq = 0;
static uint8_t txVoiceSeq = 0;
static walkie::Atomic<uint8_t> g_pwrLvl{1};
static walkie::Atomic<uint8_t> g_fbLoss{0};
static walkie::Atomic<int8_t> g_fbRssi{0};
static walkie::Atomic<uint32_t> g_fbMs{0};
static walkie::Atomic<uint32_t> g_lastVoiceRxMs{0};  // 最近收到语音包时刻（beacon 快报用）
static walkie::Radio g_radio;
static walkie::Atomic<uint16_t> g_seqGap{0};
static walkie::LossWindow g_lossWin;
static walkie::Atomic<uint8_t> g_lossPct{0};
static walkie::Atomic<int8_t> g_peerRssi{0};
static walkie::Atomic<uint16_t> g_rxStall{0};
static walkie::Atomic<uint16_t> g_txStarve{0};
static walkie::Atomic<uint32_t> g_rxVoice{0};
static walkie::Atomic<uint32_t> g_rxBeacon{0};
static walkie::Atomic<uint16_t> g_rxOver{0};
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
        { g_seqGap = 0; g_rxStall = 0; g_underrun = 0; g_playRestart = 0; }
        if (ev.gap)
        { g_seqGap += ev.gap; walkie::lossWindowAdd(g_lossWin, 0, ev.gap); }
        walkie::lossWindowAdd(g_lossWin, 1, 0);
        if (ev.stalled) g_rxStall++;
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

// ========== 模式 ==========
enum class Mode { Boot, Home, Transmit };
static Mode g_mode = Mode::Boot;

// ========== RX 播放任务 ==========
static walkie::Atomic<bool> g_rxTaskRun{false};
static int g_upPrev = -1;

static void rxPlayTask(void *)
{
    constexpr size_t kStartBytes = 2000;
    bool primed = false;
    int gapFrames = 0;
    for (;;)
    {
        if (!g_rxTaskRun || g_mode == Mode::Transmit)
        { primed = false; g_upPrev = -1; vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        size_t avail = walkie::ringAvailable(rxWritePos, rxReadPos, sizeof(rxAudioBuf));
        if (!primed)
        {
            if (avail < kStartBytes) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            primed = true; gapFrames = 0;
        }
        if (avail < walkie::kSamplesPerFrame)
        {
            if (gapFrames == 0) g_underrun++;
            if (++gapFrames > 50) { primed = false; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            uint8_t fill[walkie::kBytesPerFrame];
            size_t fillLen = walkie::gapFrame(g_upPrev, gVolume, fill, sizeof(fill));
            codecPlayPCM(fill, fillLen);
            taskYIELD();
            continue;
        }
        gapFrames = 0;
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
        taskYIELD();
    }
}

// ========== TX 采集任务 ==========
static walkie::Atomic<bool> g_txRun{false};
static uint8_t txMicBuf[250];

static void espnowSendPCM(const uint8_t *data, size_t len);

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
        { pos = 0; dsSum = 0; dsCnt = 0; lastOk = 0; vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        size_t n = codecReadMicFrame(txMicBuf, sizeof(txMicBuf));
        if (n == 0)
        {
            if (lastOk && millis() - lastOk > 200)
            { codecStartMic(); g_micHeal++; lastOk = millis(); pos = 0; dsSum = 0; dsCnt = 0; }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        { uint32_t t = millis(); if (lastOk && t - lastOk > 15) g_txStarve++; lastOk = t; }

        uint8_t peak = 0;
        for (size_t i = 0; i < n; i++)
        {
            uint8_t smp = txMicBuf[i];
            uint8_t amp = (uint8_t)abs((int16_t)smp - 128);
            if (amp > peak) peak = amp;
            dsSum += smp;
            if (++dsCnt == 3)   // 24k → 8k
            {
                pkt[pos++] = (uint8_t)(dsSum / 3);
                dsSum = 0; dsCnt = 0;
                if (pos >= sizeof(pkt)) { espnowSendPCM(pkt, pos); pos = 0; }
            }
        }
        if (peak > g_txPeak) g_txPeak = peak;
        taskYIELD();
    }
}

// ========== ESP-NOW 初始化 ==========

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
    esp_wifi_set_ps(WIFI_PS_NONE);
    g_radio.init(gChannel, walkie::kPwrDbm[g_pwrLvl], onDataRecv);
    delay(150);
    codecSetVolume(6);
}

static void sendBeacon()
{
    uint8_t b[20];
    size_t n = walkie::buildBeacon(b, sizeof(b), txSeq++, g_myName, g_lossPct, g_peerRssi);
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

// ========== 模式切换 ==========
static void enterHome()
{
    g_txRun = false;
    delay(15);
    codecStopMic();
    walkie::ringReset(rxWritePos, rxReadPos);
    g_mode = Mode::Home;
    mtxSendFast(MTX_CLEAR);
}

static void enterTransmit()
{
    g_txRun = false;
    delay(15);
    codecStopMic();
    walkie::ringReset(rxWritePos, rxReadPos);
    delay(5);
    codecStartMic();
    g_mode = Mode::Transmit;
    g_txRun = true;
    g_txStarve = 0; g_micHeal = 0; g_radio.txFail = 0; g_radio.txNack = 0;
    mtxSendFast(MTX_MIC);
}

// ========== 3 键组合状态机 ==========
// A 按住 = PTT
// A 按住 + B = 音量-，A 按住 + C = 音量+
// B 按住 + A = 频道-，B 按住 + C = 频道+
// C 按住 + A = 密码位 0，C 按住 + B = 密码位 1（按手一次只记一个）；C 单按 = 密码视图
// C 按住 + A 或 B ≥ 10s = 清空密码
static uint32_t g_aHeldMs = 0, g_bHeldMs = 0, g_cHeldMs = 0;
static bool g_aWas = false, g_bWas = false, g_cWas = false;
static uint32_t g_comboClearStart = 0;   // C+A/B 持续按住计时（清密码）
static bool g_comboClearDone = false;
static uint32_t g_lastComboAction = 0;
static uint32_t g_bootShieldUntil = 0;
static uint32_t g_volDisplayUntil = 0;
static uint32_t g_chDisplayUntil = 0;
static uint32_t g_passViewUntil = 0;   // 密码视图显示截止时刻（C 单按）

static void handleKeys(uint32_t now)
{
    if (now < g_bootShieldUntil) return;   // 开机 700ms 屏蔽

    bool a = digitalRead(KEY_A) == HIGH;
    bool b = digitalRead(KEY_B) == HIGH;
    bool c = digitalRead(KEY_C) == HIGH;
    // 全局边沿：松→按 = 一次点击
    bool eA = a && !g_aWas, eB = b && !g_bWas, eC = c && !g_cWas;

    // 记录按住起始时间
    if (a && !g_aWas) g_aHeldMs = now;
    if (b && !g_bWas) g_bHeldMs = now;
    if (c && !g_cWas) g_cHeldMs = now;

    // ===== 先导键（最早按下的键）决定模式：A=音量 B=信道 C=密码 =====
    uint32_t aT = a ? g_aHeldMs : UINT32_MAX;
    uint32_t bT = b ? g_bHeldMs : UINT32_MAX;
    uint32_t cT = c ? g_cHeldMs : UINT32_MAX;
    int mode = 0;                              // 0=无 1=A 2=B 3=C
    if (aT <= bT && aT <= cT) mode = a ? 1 : 0;
    if (mode == 0 && bT < aT && bT <= cT) mode = b ? 2 : 0;
    if (mode == 0 && cT < aT && cT < bT) mode = c ? 3 : 0;

    // ===== 模式 C（密码）：A=位0 B=位1；C 单按进密码视图；≥10s 清空 =====
    if (mode == 3)
    {
        if (g_comboClearStart == 0) g_comboClearStart = now;
        if (!g_comboClearDone && now - g_comboClearStart >= PASS_CLEAR_HOLD_MS)
        {
            g_comboClearDone = true;
            g_passBits = 0; g_passLen = 0;
            savePass();
            g_passViewUntil = now + 1500;         // 全灭 X → 视图显示
            mtxSendFast(MTX_X);
            Serial.println("[KEY] 密码已清空（C+A/B ≥10s）");
        }
        if (!g_comboClearDone && now - g_cHeldMs > 150)
        {
            if (eA && g_passLen < 16)
            {
                g_passBits = (g_passBits << 1) | 0;
                g_passLen++;
                g_passViewUntil = now + 800;        // 刷新密码视图
                g_lastComboAction = now;
                savePass();
                Serial.printf("[KEY] 密码位 0（len=%u）\n", g_passLen);
            }
            else if (eB && g_passLen < 16)
            {
                g_passBits = (g_passBits << 1) | 1;
                g_passLen++;
                g_passViewUntil = now + 800;        // 刷新密码视图
                g_lastComboAction = now;
                savePass();
                Serial.printf("[KEY] 密码位 1（len=%u）\n", g_passLen);
            }
        }
    }
    else
    {
        g_comboClearStart = 0;
        g_comboClearDone = false;
    }

    // ===== 模式 B（信道）：A=信道- C=信道+；B 单按显示信道号 =====
    if (mode == 2 && now - g_bHeldMs > 120)
    {
        if (eA)
        {
            applyChannel(gChannel <= 1 ? 13 : gChannel - 1);
            g_chDisplayUntil = now + 1200;
            g_lastComboAction = now;
            mtxShowChannel(gChannel);
            Serial.printf("[KEY] 信道- -> %u\n", gChannel);
        }
        else if (eC)
        {
            applyChannel(gChannel >= 13 ? 1 : gChannel + 1);
            g_chDisplayUntil = now + 1200;
            g_lastComboAction = now;
            mtxShowChannel(gChannel);
            Serial.printf("[KEY] 信道+ -> %u\n", gChannel);
        }
        else if (!a && !c)
        {
            g_chDisplayUntil = now + 1500;          // B 单按：显示信道号
            g_lastComboAction = now;
            mtxShowChannel(gChannel);
        }
    }

    // ===== 模式 A（音量）：B=音量- C=音量+；纯 A = PTT =====
    if (mode == 1)
    {
        if (!b && !c)
        {
            if (g_mode != Mode::Transmit && now - g_aHeldMs > 80)
                enterTransmit();
        }
        else
        {
            if (g_mode == Mode::Transmit) enterHome();   // 组合键优先于 PTT
            if (now - g_aHeldMs > 150)
            {
                if (eB && gVolume > 0) { gVolume--; saveVolume(); g_volDisplayUntil = now + 1200; g_lastComboAction = now; mtxSendFast(MTX_ARR_D); }
                if (eC && gVolume < 9) { gVolume++; saveVolume(); g_volDisplayUntil = now + 1200; g_lastComboAction = now; mtxSendFast(MTX_ARR_U); }
            }
        }
    }
    else if (!a && g_mode == Mode::Transmit)
    {
        enterHome();
    }

    // ===== C 单按（无 A/B 同按）→ 密码视图 =====
    if (c && !a && !b && !g_cWas)
    {
        g_passViewUntil = now + 3000;
        Serial.println("[KEY] 密码视图");
    }

    g_aWas = a; g_bWas = b; g_cWas = c;
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

static void renderMatrix(uint32_t now)
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
        mtxShowChannel(gChannel);
        return;
    }
    int t = findTalker(now);
    if (t >= 0)
    {
        // 接收中：喇叭图标
        mtxSendFast(MTX_SPK);
        return;
    }
    // 待机：身份滚动（加密时锁字形排在名字前）
    renderIdentity(now);
}

// ========== setup / loop ==========
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== AlphaPi Zero Walkie v0.1 (ESP32-C3) ===");

    pinMode(KEY_A, INPUT_PULLDOWN);
    pinMode(KEY_B, INPUT_PULLDOWN);
    pinMode(KEY_C, INPUT_PULLDOWN);
    g_bootShieldUntil = millis() + 700;

    codecInit();
    initIdentity();
    probeMicProtocol();
    initEspNow();

    // 开机点阵：全屏亮 0.5s → 清空
    mtxSend(MTX_FULL);
    delay(500);
    mtxSend(MTX_CLEAR);
    walkie::playCoinChime(codecPlayPCM);   // 开机提示音（含引导静音）

    enterHome();

    // C3 单核：RX 播放优先级最高（音频节拍），TX 采集次之
    xTaskCreatePinnedToCore(rxPlayTask, "rxplay", 4096, nullptr, 6, nullptr, 0);
    g_rxTaskRun = true;
    xTaskCreatePinnedToCore(txCaptureTask, "txcap", 4096, nullptr, 5, nullptr, 0);

    Serial.printf("[INIT] 身份: %s CH%u VOL%u ENC:%s\n",
                  g_myName,
                  gChannel, (int)gVolume, g_crypto.enabled ? "ON" : "OFF");
    Serial.println("[KEY] A=PTT A+B/C=音量 B+A/C=频道 C+A/B=密码 C+A/B≥10s=清密码");
}

void loop()
{
    uint32_t now = millis();

    handleKeys(now);
    renderMatrix(now);

    // 丢包率窗口结算
    static uint32_t lastWin = 0;
    if (now - lastWin > walkie::LOSS_WINDOW_MS)
    {
        lastWin = now;
        g_lossPct = walkie::lossWindowPercent(g_lossWin);
        walkie::lossWindowReset(g_lossWin);
    }

    // 心跳广播（TX 时让位给语音；正在收话时 500ms 快报，让发射端尽快升档）
    static uint32_t lastBeacon = 0;
    uint32_t beaconIv = (now - g_lastVoiceRxMs < walkie::VOICE_RX_RECENT_MS) ? walkie::BEACON_FAST_MS : walkie::BEACON_INTERVAL_MS;
    if (g_mode != Mode::Transmit && now - lastBeacon > beaconIv)
    { sendBeacon(); lastBeacon = now; }

    // 邻居上线提示音
    if (gPeerJoined)
    {
        gPeerJoined = false;
        if (g_mode != Mode::Transmit) walkie::playCoinChime(codecPlayPCM);
    }

    // 闭环功率控制
    static uint32_t lastPwrAdj = 0;
    if (now - lastPwrAdj > walkie::PWR_ADJ_INTERVAL_MS)
    {
        lastPwrAdj = now;
        bool fresh = (g_fbMs != 0 && now - g_fbMs < walkie::PWR_FB_FRESH_MS);
        uint8_t lvl = walkie::nextPowerLevel(g_pwrLvl, fresh, g_fbLoss, g_fbRssi,
                                             g_mode == Mode::Transmit);
        if (lvl != g_pwrLvl)
        { g_pwrLvl = lvl; g_radio.setTxPower(walkie::kPwrDbm[lvl]); }
    }

    // 邻居表维护
    static uint32_t lastPrune = 0;
    if (now - lastPrune > walkie::PRUNE_INTERVAL_MS) { lastPrune = now; prunePeers(now); }

    delay(1);
}