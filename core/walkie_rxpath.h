#pragma once

// AlphaPi walkie 共享的 ESP-NOW 收包路径。
// 三块板的 onDataRecv 逐字节一致，这里收敛成一份：
//   * 语音包：序号跟踪/断流检测/丢包窗口/AES 解密/环形缓冲
//   * 心跳包：邻居表更新 + 对端功率闭环反馈（RSSI/丢包率）
// main.cpp 只负责模块编排；Radio 初始化、N32 音频链路、按键、UI 仍归各板。

#include <atomic>
#include <stdint.h>
#include <string.h>

#include <esp_now.h>

#include "walkie_protocol.h"
#include "walkie_crypto.h"
#include "walkie_peer.h"
#include "walkie_ring.h"
#include "walkie_rx.h"
#include "walkie_timing.h"
#include "walkie_atomic.h"
#include "walkie_n32.h"
#include "walkie_radio.h"

namespace walkie
{

constexpr size_t kAudioRingSize = 4096;

// 每个固件一份实例（main.cpp 定义），收包路径所有状态都挂在这里
struct RxPath
{
    // 新邻居默认名（邻居表占位）
    const char *defaultPeerName;

    explicit RxPath(const char *defaultPeerName = "同伴") : defaultPeerName(defaultPeerName) {}

    // 邻居表
    PeerEntry peers[kMaxPeers] = {};
    Atomic<bool> peersDirty{false};
    Atomic<bool> peerJoined{false};   // 有新邻居/重连，主循环响提示音

    // 接收音频环形缓冲（解耦 RX 回调与播放任务）
    uint8_t audioBuf[kAudioRingSize] = {};
    Atomic<size_t> rxWritePos{0};
    Atomic<size_t> rxReadPos{0};

    // 接收统计 / 闭环功率反馈
    VoiceRxTracker rxTracker;
    LossWindow lossWin;
    Atomic<uint16_t> seqGap{0};      // 空口丢包数（序号缺口）
    Atomic<uint16_t> rxStall{0};     // 收包间隔>阈值次数（发射侧断流）
    Atomic<uint16_t> rxVoice{0};     // 收到的语音包数
    Atomic<uint16_t> rxBeacon{0};    // 收到的心跳包数
    Atomic<uint16_t> rxBadKey{0};    // AES 标记校验失败的语音包数
    Atomic<uint16_t> rxOver{0};      // 环形缓冲溢出丢弃字节数
    Atomic<uint8_t> lossPct{0};      // 最近窗口丢包率%（beacon 回传用）
    Atomic<int8_t> peerRssi{0};      // 最近语音包 RSSI
    Atomic<uint8_t> fbLoss{0};       // 对端回传的丢包率%
    Atomic<int8_t> fbRssi{0};        // 对端收我的 RSSI
    Atomic<uint32_t> fbMs{0};        // 反馈时间戳
    Atomic<uint32_t> lastVoiceRxMs{0}; // 最近收到语音包时刻（beacon 快报用）
};

// 邻居表操作（名称与 walkie::peerTouch 等对应，额外维护 dirty/joined 标志）
inline void peerTouch(RxPath &rx, const uint8_t *mac, const char *name, uint32_t now, bool isVoice)
{
    bool joined = walkie::peerTouch(rx.peers, walkie::kMaxPeers, mac, name, now, isVoice,
                                    walkie::PEER_TIMEOUT_MS, rx.defaultPeerName);
    rx.peersDirty = true;
    if (joined) rx.peerJoined = true;
}
inline int findTalker(RxPath &rx, uint32_t now)
{
    return walkie::findTalker(rx.peers, walkie::kMaxPeers, now, walkie::TALK_TIMEOUT_MS);
}
inline void prunePeers(RxPath &rx, uint32_t now)
{
    if (walkie::prunePeers(rx.peers, walkie::kMaxPeers, now, walkie::PEER_TIMEOUT_MS))
        rx.peersDirty = true;
}

// 收包入口（在 Radio 注册的回调中调用）
inline void handleEspNowPacket(RxPath &rx, CryptoState &crypto, N32Driver &n32,
                               const esp_now_recv_info_t *info, const uint8_t *data, int len,
                               uint32_t now)
{
    if (!data || len < 4 || len > 250) return;
    if (data[0] != walkie::kMagic) return;

    if (data[1] == walkie::PKT_VOICE)
    {
        uint8_t seq = data[2];
        walkie::VoiceRxTracker::Event ev =
            rx.rxTracker.onVoicePacket(seq, now, walkie::VOICE_GAP_RESET_MS, walkie::VOICE_STALL_MIN_MS,
                                       walkie::VOICE_STALL_MAX_MS);
        if (ev.newSession)
        {
            // 新一句开始：清零会话计数，读数只反映当前这句
            rx.seqGap = 0; rx.rxStall = 0; n32.underrun = 0; n32.playRestart = 0;
        }
        if (ev.gap)
        {
            rx.seqGap += ev.gap;   // 空口丢包
            walkie::lossWindowAdd(rx.lossWin, 0, ev.gap);
        }
        walkie::lossWindowAdd(rx.lossWin, 1, 0);
        if (ev.stalled) rx.rxStall++;   // 发射侧断流
        rx.lastVoiceRxMs = now;
        rx.rxVoice++;
        rx.peerRssi = (info && info->rx_ctrl) ? (int8_t)info->rx_ctrl->rssi : 0;
        peerTouch(rx, info ? info->src_addr : nullptr, nullptr, now, true);
        uint8_t dec[250];
        walkie::VoiceDecoded vd = walkie::decodeVoicePacket(crypto, dec, sizeof(dec), data, len, seq);
        if (vd.badKey) { rx.rxBadKey++; return; }
        if (!vd.payload || vd.len <= 0) return;
        for (int i = 0; i < vd.len; i++)
        {
            if (!walkie::ringPush(rx.audioBuf, sizeof(rx.audioBuf), rx.rxWritePos, rx.rxReadPos, vd.payload[i]))
            { rx.rxOver++; break; }
        }
    }
    else if (data[1] == walkie::PKT_BEACON)
    {
        rx.rxBeacon++;
        walkie::BeaconFeedback fb = walkie::parseBeacon(data, len);
        if (fb.valid)
        {
            char nm[12] = {0};
            memcpy(nm, data + 3, fb.nameLen);
            nm[fb.nameLen] = 0;
            if (info) peerTouch(rx, info->src_addr, nm, now, false);
            rx.fbLoss = fb.lossPct;
            rx.fbRssi = fb.rssi;
            rx.fbMs = now;
        }
    }
}

// 2s 丢包窗口结算：每 loop 调用，内部按 LOSS_WINDOW_MS 节流
inline void settleLossWindow(RxPath &rx, uint32_t now, uint32_t &lastSettleMs)
{
    if (now - lastSettleMs <= walkie::LOSS_WINDOW_MS) return;
    lastSettleMs = now;
    rx.lossPct = walkie::lossWindowPercent(rx.lossWin);
    walkie::lossWindowReset(rx.lossWin);
}

// 闭环功率调整：每 loop 调用，内部按 PWR_ADJ_INTERVAL_MS 节流
inline void autoPowerAdjust(RxPath &rx, Radio &radio, Atomic<uint8_t> &pwrLvl,
                            bool transmitting, uint32_t now, uint32_t &lastAdjMs)
{
    if (now - lastAdjMs <= walkie::PWR_ADJ_INTERVAL_MS) return;
    lastAdjMs = now;
    bool fresh = (rx.fbMs != 0 && now - rx.fbMs < walkie::PWR_FB_FRESH_MS);
    uint8_t lvl = walkie::nextPowerLevel(pwrLvl, fresh, rx.fbLoss, rx.fbRssi, transmitting);
    if (lvl != pwrLvl)
    {
        pwrLvl = lvl;
        radio.setTxPower(walkie::kPwrDbm[lvl]);
    }
}

// 邻居保活剪枝：每 loop 调用，内部按 PRUNE_INTERVAL_MS 节流
inline void prunePeersThrottled(RxPath &rx, uint32_t now, uint32_t &lastPruneMs)
{
    if (now - lastPruneMs <= walkie::PRUNE_INTERVAL_MS) return;
    lastPruneMs = now;
    prunePeers(rx, now);
}

// 发信标：seq 由调用方持有（voice 用独立序号，beacon 不占坑）
inline void sendBeacon(RxPath &rx, Radio &radio, const char *name, uint8_t &seq)
{
    uint8_t b[20];
    size_t n = walkie::buildBeacon(b, sizeof(b), seq++, name, rx.lossPct, rx.peerRssi);
    if (n > 0) radio.send(b, (int)n);
}

// 信标快报间隔：最近收到语音时 500ms，平时常规间隔
inline uint32_t beaconIntervalMs(const RxPath &rx, uint32_t now)
{
    return (now - rx.lastVoiceRxMs < walkie::VOICE_RX_RECENT_MS) ? walkie::BEACON_FAST_MS : walkie::BEACON_INTERVAL_MS;
}

} // namespace walkie
