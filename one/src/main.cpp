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

#include "device/config.h"
#include "device/display.h"
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
#include "core/walkie_n32.h"
#include "core/walkie_audio.h"
#include "core/walkie_chime.h"
#include "core/walkie_radio.h"

// ========== 本机身份：生肖 + MAC尾 ==========
char gMyName[12] = "同伴";   // "龙虎7A"
uint8_t gChannel = CHANNEL_DEFAULT;
// ===== 通信密码：二进制位串（C=0 D=1 逐位打），空=不加密。
// SHA256 派生 AES-128-CTR 密钥；CTR 不增包长，序号当计数器。
uint16_t g_passBits = 0;
uint8_t g_passLen = 0;      // 0..16
static walkie::CryptoState g_crypto;
bool g_passEdit = false;
uint16_t g_tmpBits = 0;
uint8_t g_tmpLen = 0;
static uint32_t g_bHeldMs = 0;
static bool g_bCleared = false;
static walkie::Atomic<uint16_t> g_rxBadKey{0};
walkie::Atomic<uint8_t> gVolume{6};   // 软件音量 0..9

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
walkie::PeerEntry gPeers[walkie::kMaxPeers];
static walkie::Atomic<bool> gPeersDirty{false};
static walkie::Atomic<bool> gPeerJoined{false};   // 有新邻居上线，主循环响提示音

static void peerTouch(const uint8_t *mac, const char *name, uint32_t now, bool isVoice)
{
    bool joined = walkie::peerTouch(gPeers, walkie::kMaxPeers, mac, name, now, isVoice,
                                    walkie::PEER_TIMEOUT_MS, "同伴");
    gPeersDirty = true;
    if (joined) gPeerJoined = true;
}

int findTalker(uint32_t now)
{
    return walkie::findTalker(gPeers, walkie::kMaxPeers, now, walkie::TALK_TIMEOUT_MS);
}

static void prunePeers(uint32_t now)
{
    if (walkie::prunePeers(gPeers, walkie::kMaxPeers, now, walkie::PEER_TIMEOUT_MS)) gPeersDirty = true;
}

// ========== N32 协处理器驱动（walkie::N32Driver，core/walkie_n32.h）==========
static SemaphoreHandle_t g_uartMux = nullptr;
static walkie::N32Driver g_n32{Serial1};
static walkie::Atomic<uint16_t> g_rxVoice{0}; // 收到的语音包数
static walkie::Atomic<uint16_t> g_rxBeacon{0}; // 收到的心跳包数
static walkie::Atomic<uint8_t> g_txPeak{0};   // 麦克风采集到的峰值（0..127）
static walkie::Atomic<uint8_t> g_rxPeak{0};   // 送去播放的峰值（0..127）
static walkie::Atomic<uint16_t> g_micHeal{0}; // 发送端麦克风自愈次数
static walkie::Atomic<uint16_t> g_rxOver{0};  // 接收环形缓冲溢出丢弃字节数
static walkie::Atomic<uint16_t> g_txStarve{0};  // TX 读麦间隔>15ms 次数（采集被饿）

static bool codecPlayPCM(const uint8_t *data, size_t len) { return walkie::n32PlayPCM(g_n32, data, len); }
static size_t codecReadMicFrame(uint8_t *buf, size_t bufSize) { return walkie::n32ReadMicFrame(g_n32, buf, bufSize); }
static bool codecStartMic() { g_n32.playReady = false; return walkie::n32Control(g_n32, 0x01); }
static bool codecStopMic()  { g_n32.playReady = false; return walkie::n32Control(g_n32, 0x00); }

// ===== AlphaPi One 按键：全部在 N32 寄存器 0x00，ESP32 侧无按键 GPIO
// 读事务: 0x80 <reg> <len> -> 应答 0x81 <reg> <len> <value> <checksum>
// 极性说明：厂商手册写"低电平有效"，但厂商已烧录固件按"置位=按下"取掩码
// （无按键=0x00）。这里按运行时行为实现，并留出开关便于实机校正。
static uint8_t n32ReadKeys()
{
    if (g_n32.mux) xSemaphoreTakeRecursive(g_n32.mux, portMAX_DELAY);
    walkie::n32FlushIn(g_n32);
    g_n32.txBuf[0] = 0x80; g_n32.txBuf[1] = N32_REG_KEYS; g_n32.txBuf[2] = 1;
    g_n32.uart.write(g_n32.txBuf, 3);

    uint8_t rsp[5] = {0};
    g_n32.uart.setTimeout(20);
    if (g_n32.uart.readBytes(rsp, 5) != 5)
    {
        if (g_n32.mux) xSemaphoreGiveRecursive(g_n32.mux);
        return KEY_READ_FAIL;
    }
    if (rsp[0] != 0x81 || rsp[1] != N32_REG_KEYS || rsp[2] != 1)
    {
        if (g_n32.mux) xSemaphoreGiveRecursive(g_n32.mux);
        return KEY_READ_FAIL;
    }
    if ((uint8_t)(rsp[0] + rsp[1] + rsp[2] + rsp[3]) != rsp[4])
    {
        if (g_n32.mux) xSemaphoreGiveRecursive(g_n32.mux);
        return KEY_READ_FAIL;
    }
    uint8_t v = rsp[3];
    if (g_n32.mux) xSemaphoreGiveRecursive(g_n32.mux);
    return v;
}

static void codecInit()
{
    if (!g_uartMux)
    {
        g_uartMux = xSemaphoreCreateRecursiveMutex();
        g_n32.mux = g_uartMux;
    }
    Serial1.begin(AUDIO_BAUD, SERIAL_8N1, AUDIO_RX_PIN, AUDIO_TX_PIN);
    // N32 是独立 MCU，上电自举需要时间，否则首条命令必丢
    delay(N32_BOOT_DELAY_MS);
    walkie::n32FlushIn(g_n32);
    Serial1.setTimeout(400);

    walkie::n32Control(g_n32, 0x00);  // 停麦
    walkie::n32SetVolume(g_n32, 6);   // 硬件音量拉满，音量走软件
    walkie::n32FlushIn(g_n32);
}

// ========== ESP-NOW ==========
static uint8_t txSeq = 0;
static uint8_t txVoiceSeq = 0;   // 语音独立序号，beacon 不占坑
// 闭环功率控制：收端测丢包率经 beacon 回传，发端三档自动切换。
// 高丢包+弱信号=距离远→升档；高丢包+强信号=近距饱和→降档。
walkie::Atomic<uint8_t> g_pwrLvl{1};            // 当前档（默认 13dBm）
static walkie::Atomic<uint8_t> g_fbLoss{0};            // 对端回传的丢包率%
static walkie::Atomic<int8_t> g_fbRssi{0};      // 对端收我的 RSSI
static walkie::Atomic<uint32_t> g_fbMs{0};             // 反馈时间戳
static walkie::Radio g_radio;
static walkie::Atomic<uint16_t> g_seqGap{0};    // 空口丢包数（序号缺口）
static walkie::LossWindow g_lossWin;   // 2s 窗口丢包率
static walkie::Atomic<uint8_t> g_lossPct{0};    // 最近窗口丢包率%（beacon 回传用）
static walkie::Atomic<int8_t> g_peerRssi{0};    // 最近语音包 RSSI
static walkie::Atomic<uint16_t> g_rxStall{0};   // 收包间隔>60ms 次数（发射侧断流）
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
            g_seqGap = 0; g_rxStall = 0; g_n32.underrun = 0; g_n32.playRestart = 0;
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


Mode g_mode = Mode::Boot;

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
            if (gapFrames == 0) g_n32.underrun++;   // 每次断供只计 1 次
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


int8_t g_chRssi[14];      // 各信道最强 AP 信号，0=未扫到
uint8_t g_chCount[14];    // 各信道 AP 数
bool g_scanned = false;

static void scanChannels();
walkie::Atomic<bool> g_scanBusy{false};
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
    // WiFi 启动的电源跌落/干扰会让 N32 的音频前端失效，
    // 之后命令照样 ACK 但不出声。这里等电源稳定后重建一次链路。
    walkie::n32Control(g_n32, 0x00);
    walkie::n32SetVolume(g_n32, 6);
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

// ---------- 按键状态（由 keyTask 轮询 N32 寄存器得到） ----------
// bit set = 按下（与厂商已烧录固件一致）
static walkie::Atomic<uint8_t> g_keys{0};
static walkie::Atomic<bool> g_keyReadFail{false};

// 逻辑键：A=PTT，B=配对/密码确认，L=音量减/密码位0，R=音量加/密码位1

// 按键轮询任务：读 N32 键寄存器。
// 与播放共用 UART 互斥锁，播放中每次读键约 0.5ms 总线占用（厂商同样做法）。
static void keyTask(void *)
{
    uint8_t stable = 0;
    uint8_t candidate = 0;
    uint8_t lastRaw = 0;
    uint8_t debounce = 0;

    for (;;)
    {
        uint8_t raw = n32ReadKeys();
        if (raw == KEY_READ_FAIL)
        {
            g_keyReadFail = true;
            vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
            continue;
        }
        g_keyReadFail = false;

#if KEY_PRESSED_IS_SET
        uint8_t pressed = raw & KEY_BITS_ALL;
#else
        uint8_t pressed = (uint8_t)(~raw) & KEY_BITS_ALL;
#endif
        if (pressed != lastRaw)
        {
            lastRaw = pressed;
            debounce = 0;
        }
        else if (++debounce >= 2)
        {
            candidate = pressed;
            debounce = 0;
        }
        stable = candidate;
        g_keys = stable;

        // 播放时放慢轮询，减少对音频流的干扰
        uint32_t period = (g_mode == Mode::Transmit) ? KEY_POLL_MS : KEY_POLL_MS * 2;
        vTaskDelay(pdMS_TO_TICKS(period));
    }
}

// ========== UI / 模式 ==========

static unsigned long lastButton = 0;
static unsigned long pairingStart = 0;
static unsigned long lastBeacon = 0;
static unsigned long lastBarRedraw = 0;
static unsigned long lastHomeCheck = 0;
static unsigned long lastPairDraw = 0;
static int lastShownTalker = -2;   // -2=未画过, -1=没人
static uint8_t lastDrawnVol = 0xFF;

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

void setup()
{
    Serial.begin(115200);

    // 背光：GPIO37 LEDC PWM，8bit，5kHz，50% 亮度（与厂商固件一致）
    ledcAttach(TFT_BL, 5000, 8);
    ledcWrite(TFT_BL, 128);

    displayInit();

    initIdentity();
    showBoot();
    delay(400);

    codecInit();
    initEspNow();

    // S3 板没有按键 GPIO，按键由 keyTask 轮询 N32 寄存器 0x00 获取

    enterHome();

    xTaskCreatePinnedToCore(keyTask, "keys", 3072, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(rxPlayTask, "rxplay", 4096, nullptr, 5, nullptr, 0);
    g_rxTaskRun = true;
    xTaskCreatePinnedToCore(txCaptureTask, "txcap", 4096, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(scanTask, "scan", 4096, nullptr, 1, nullptr, 0);
}

static constexpr size_t PLAY_CHUNK = 300;  // 100 个 8k 样本插值成 24k

void loop()
{
    uint32_t now = millis();
    uint8_t keysNow = g_keys.load();
    // 边沿检测：配置页/密码页一律"按一下=一次动作"，不做长按重复
    static uint8_t prevKeys = 0;
    uint8_t edgeDown = keysNow & (uint8_t)~prevKeys;
    uint8_t edgeUp = (uint8_t)~keysNow & prevKeys;
    prevKeys = keysNow;
    bool a = (keysNow & KEY_BIT_A) != 0;
    bool b = (keysNow & KEY_BIT_B) != 0;

    if (g_mode == Mode::Home)
    {
        if (edgeDown & KEY_BIT_A)
        {
            lastButton = now;
            enterTransmit();
        }
        else if (edgeDown & KEY_BIT_B)
        {
            lastButton = now;
            enterPairing();
        }
        else if (edgeDown & KEY_BIT_R)
        {
            if (gVolume < 9) gVolume++;
            saveVolume();
            gVolDisplayUntil = now + walkie::VOICE_RX_RECENT_MS;
            drawHomeStatus(now);
        }
        else if (edgeDown & KEY_BIT_L)
        {
            if (gVolume > 0) gVolume--;
            saveVolume();
            gVolDisplayUntil = now + walkie::VOICE_RX_RECENT_MS;
            drawHomeStatus(now);
        }
    }
    else if (g_mode == Mode::Pairing)
    {
        if (g_passEdit)
        {
            // 键位（用户定稿）：L=0 R=1 B=短按删一位/长按800ms清空 A=确定(保存并退回配置页)
            // 全部按键一次按下只触发一次（边沿检测），没有长按重复输入
            if ((edgeDown & KEY_BIT_R) && g_tmpLen < 16)
            {
                g_tmpBits = (g_tmpBits << 1) | 1;
                g_tmpLen++;
                drawPairing();
            }
            else if ((edgeDown & KEY_BIT_L) && g_tmpLen < 16)
            {
                g_tmpBits = (g_tmpBits << 1) | 0;
                g_tmpLen++;
                drawPairing();
            }
            else if (edgeDown & KEY_BIT_B)
            {
                g_bHeldMs = now;
                g_bCleared = false;
            }
            else if (b && g_bHeldMs && !g_bCleared && now - g_bHeldMs > 800 && g_tmpLen)
            {
                g_bCleared = true;      // 长按 800ms 全清，只触发一次
                g_tmpBits = 0; g_tmpLen = 0;
                drawPairing();
            }
            if (edgeUp & KEY_BIT_B)
            {
                if (!g_bCleared && g_bHeldMs && now - g_bHeldMs < 800)
                {
                    if (g_tmpLen) { g_tmpLen--; g_tmpBits >>= 1; drawPairing(); }
                }
                g_bHeldMs = 0;
                g_bCleared = false;
            }
            if (edgeDown & KEY_BIT_A)
            {
                lastButton = now;
                g_passBits = g_tmpBits;
                g_passLen = g_tmpLen;
                savePass();
                g_passEdit = false;   // 确定 → 回配置页
                drawPairing();
            }
        }
        else
        {
            if (edgeDown & KEY_BIT_B)
            {
                lastButton = now;
                g_passEdit = true;
                g_tmpBits = g_passBits;
                g_tmpLen = g_passLen;
                g_bHeldMs = 0; g_bCleared = false;
                drawPairing();
            }
            else if (edgeDown & KEY_BIT_A)
            {
                lastButton = now;
                enterHome();
            }
            else if (edgeDown & KEY_BIT_R)
            {
                applyChannel(gChannel >= 13 ? 1 : gChannel + 1);
                drawPairing();
            }
            else if (edgeDown & KEY_BIT_L)
            {
                applyChannel(gChannel <= 1 ? 13 : gChannel - 1);
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

    delay(1);
}
