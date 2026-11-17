#pragma once

// Shared N32 co-processor driver for all AlphaPi boards.
// Protocol (all boards): 0x90 <reg> <len> <payload...> <chk>  ->  0x91 <reg> <accepted>
// Devices differ only in UART instance, baud, pins, mic register/start payload,
// and in that One S additionally needs the 0x0f peripheral-bus init before playback.

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "walkie_codec.h"
#include "walkie_atomic.h"

namespace walkie
{

struct N32Driver
{
    HardwareSerial &uart;
    SemaphoreHandle_t mux = nullptr;   // recursive; if null, driver is unprotected
    uint8_t txBuf[260];

    // state
    bool playReady = false;
    uint32_t lastPlayMs = 0;

    // counters (UI/diag reads them)
    Atomic<uint32_t> playErr{0};
    Atomic<uint16_t> underrun{0};
    Atomic<uint16_t> playRestart{0};
    Atomic<uint32_t> playOk{0};

    bool needsBusInit = false;
    bool busInitOk    = false;

    uint8_t micReg   = 0x11;
    uint8_t micStart = 0x01;
    size_t  micSize  = 160;

    N32Driver(HardwareSerial &u) : uart(u) {}

    void lock()    { if (mux) xSemaphoreTakeRecursive(mux, portMAX_DELAY); }
    void unlock()  { if (mux) xSemaphoreGiveRecursive(mux); }
    void giveUp()  { if (mux) xSemaphoreGiveRecursive(mux); }
    void retake()  { if (mux) xSemaphoreTakeRecursive(mux, portMAX_DELAY); }
};

inline void n32FlushIn(N32Driver &d) { while (d.uart.available() > 0) d.uart.read(); }

inline bool n32WriteReg(N32Driver &d, uint8_t reg, const uint8_t *payload, size_t len,
                        uint8_t *accepted, uint16_t timeoutMs)
{
    size_t frameLen = buildWriteFrame(d.txBuf, sizeof(d.txBuf), reg, payload, len);
    if (frameLen == 0) return false;
    n32FlushIn(d);
    d.uart.write(d.txBuf, frameLen);

    uint8_t ack[3] = {0};
    d.uart.setTimeout(timeoutMs);
    if (d.uart.readBytes(ack, 3) != 3) return false;
    AckFrame a = parseAck(ack, 3);
    bool ok = (a.valid && a.reg == reg);
    if (accepted) *accepted = a.value;
    return ok;
}

// 0x0f 外设总线初始化（One S 必须；Zero/S3 无此需求）
inline bool n32BusInit(N32Driver &d)
{
    if (!d.needsBusInit) { d.busInitOk = true; return true; }
    uint8_t one = 1;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        n32FlushIn(d);
        size_t frameLen = buildWriteFrame(d.txBuf, sizeof(d.txBuf), REG_BUS_INIT, &one, 1);
        if (frameLen == 0) return false;
        d.uart.write(d.txBuf, frameLen);
        d.uart.setTimeout(200);
        uint8_t rsp[1];
        if (d.uart.readBytes(rsp, 1) >= 1) { d.busInitOk = true; return true; }
        n32FlushIn(d);
        delay(20);
    }
    return false;
}

inline bool n32Control(N32Driver &d, uint8_t payload)
{
    return n32WriteReg(d, REG_CTRL, &payload, 1, nullptr, 400);
}

inline bool n32SetVolume(N32Driver &d, uint8_t vol)
{
    if (vol > 6) vol = 6;
    return n32WriteReg(d, REG_VOLUME, &vol, 1, nullptr, 400);
}

inline bool n32StartMic(N32Driver &d) { d.playReady = false; return n32Control(d, 0x01); }
inline bool n32StopMic(N32Driver &d)  { d.playReady = false; return n32Control(d, 0x00); }

inline size_t n32ReadMicFrame(N32Driver &d, uint8_t *buf, size_t bufSize)
{
    n32FlushIn(d);
    d.txBuf[0] = 0x80; d.txBuf[1] = d.micReg; d.txBuf[2] = (uint8_t)d.micSize;
    d.uart.write(d.txBuf, 3);

    uint8_t head[3] = {0};
    d.uart.setTimeout(15);
    if (d.uart.readBytes(head, 3) != 3) return 0;
    if (head[0] != 0x81 || head[1] != d.micReg) return 0;

    uint8_t plen = head[2];
    if (plen == 0 || plen > bufSize) return 0;
    if (d.uart.readBytes(buf, plen + 1) != (size_t)(plen + 1)) return 0;

    uint8_t sum = head[0] + head[1] + head[2];
    for (size_t i = 0; i < plen; i++) sum += buf[i];
    if (sum != buf[plen]) return 0;
    return plen;
}

// N32 要求先收到 44 字节 WAV 头，声明 24000Hz / 8bit / mono
inline bool n32PlaybackStart(N32Driver &d)
{
    // 半双工：先停录音
    uint8_t stop = 0;
    n32WriteReg(d, REG_CTRL, &stop, 1, nullptr, 400);
    // 硬件音量拉满，音量走软件
    uint8_t hwvol = 6;
    n32WriteReg(d, REG_VOLUME, &hwvol, 1, nullptr, 400);

    // WiFi 射频启动瞬间电源跌落可能让 N32 复位丢失外设总线初始化状态，每次重建都重发
    if (!n32BusInit(d)) return false;

    constexpr uint32_t rate = 24000;
    const uint8_t wav[44] = {
        'R','I','F','F', 0xFF,0xFF,0xFF,0xFF,
        'W','A','V','E','f','m','t',' ',
        0x10,0x00,0x00,0x00,
        0x01,0x00,
        0x01,0x00,
        (uint8_t)rate,(uint8_t)(rate>>8),(uint8_t)(rate>>16),(uint8_t)(rate>>24),
        (uint8_t)rate,(uint8_t)(rate>>8),(uint8_t)(rate>>16),(uint8_t)(rate>>24),
        0x01,0x00,
        0x08,0x00,
        'd','a','t','a', 0xFF,0xFF,0xFF,0xFF,
    };
    // WAV 头是关键命令，失败重试（原厂注释：短超时会引发链路异常）
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++)
    {
        ok = n32WriteReg(d, REG_PLAY, wav, sizeof(wav), nullptr, 400);
        if (!ok) { n32FlushIn(d); delay(30); }
    }
    if (!ok) return false;
    d.playReady = true;
    d.lastPlayMs = millis();
    d.playRestart++;
    return true;
}

// 播放 PCM 分片
inline bool n32PlayPCM(N32Driver &d, const uint8_t *data, size_t len)
{
    d.lock();
    if (len == 0) { d.unlock(); return false; }
    // 静音超过 800ms 后 N32 可能退出播放态，需要重发 WAV 头
    if (d.playReady && millis() - d.lastPlayMs > 800) d.playReady = false;
    if (!d.playReady && !n32PlaybackStart(d)) { d.unlock(); return false; }

    size_t sent = 0;
    int stalled = 0;
    while (sent < len)
    {
        size_t n = len - sent;
        if (n > 160) n = 160;
        uint8_t accepted = 0;
        if (!n32WriteReg(d, REG_PLAY, data + sent, n, &accepted, 400))
        {
            d.playReady = false;
            d.playErr++;
            d.unlock();
            return false;
        }
        if (accepted == 0)
        {
            if (++stalled > 20) { d.playReady = false; d.unlock(); return false; }
            // 背压时让出 UART，否则 TX 侧读麦克风被饿死（厂商同样短等）
            d.giveUp();
            vTaskDelay(pdMS_TO_TICKS(5));
            d.retake();
            continue;
        }
        if (accepted > n) accepted = (uint8_t)n;
        sent += accepted;
        stalled = 0;
        d.playOk++;
        d.lastPlayMs = millis();
        // 每块之间让出总线，给按键/UI/TX 机会（厂商做法）
        d.giveUp();
        taskYIELD();
        d.retake();
    }
    d.unlock();
    return true;
}

} // namespace walkie
