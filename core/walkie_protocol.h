#pragma once

#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace walkie
{

static const uint8_t kMagic = 0x57;
static const uint8_t PKT_VOICE = 0x11;
static const uint8_t PKT_BEACON = 0x10;

// TX power levels in 0.25 dBm units: 8.5 / 13 / 17 dBm
static const int8_t kPwrDbm[3] = {34, 52, 68};

struct BeaconFeedback
{
    bool valid;
    uint8_t lossPct;
    int8_t rssi;
    int nameLen;
};

inline BeaconFeedback parseBeacon(const uint8_t *data, int len)
{
    BeaconFeedback fb{false, 0, 0, 0};
    if (!data || len < 5 || data[0] != kMagic || data[1] != PKT_BEACON)
        return fb;
    int nl = len - 5;
    if (nl < 0)
        nl = 0;
    if (nl > 11)
        nl = 11;
    fb.valid = true;
    fb.lossPct = data[len - 2];
    fb.rssi = (int8_t)data[len - 1];
    fb.nameLen = nl;
    return fb;
}

inline size_t buildBeacon(uint8_t *out, size_t outCap, uint8_t seq, const char *name,
                          uint8_t lossPct, int8_t rssi)
{
    size_t nl = 0;
    if (name)
    {
        nl = strlen(name);
        if (nl > 11)
            nl = 11;
    }
    size_t total = 5 + nl;
    if (!out || outCap < total)
        return 0;
    out[0] = kMagic;
    out[1] = PKT_BEACON;
    out[2] = seq;
    if (nl > 0)
        memcpy(out + 3, name, nl);
    out[3 + nl] = lossPct;
    out[4 + nl] = (uint8_t)rssi;
    return total;
}

// Compute next power level (0..2). Returns new level.
// hasFreshFeedback: peer feedback received recently.
// isTransmitting: currently in TX mode (stale feedback -> ramp up).
inline uint8_t nextPowerLevel(uint8_t current, bool hasFreshFeedback, uint8_t lossPct, int8_t rssi,
                              bool isTransmitting)
{
    uint8_t lvl = current;
    if (lvl > 2)
        lvl = 2;
    if (hasFreshFeedback)
    {
        if (lossPct > 10)
        {
            // high loss + strong signal = close-range saturation -> step down
            // high loss + weak signal = far range -> step up
            if (rssi > -55 && lvl > 0)
                lvl--;
            else if (rssi <= -55 && lvl < 2)
                lvl++;
        }
        else if (lossPct < 3 && rssi > -50 && lvl > 0)
        {
            lvl--; // very good link, probe lower power
        }
    }
    else if (isTransmitting)
    {
        if (lvl < 2)
            lvl++; // stale/no feedback while TX: ramp up
    }
    return lvl;
}

// Counters are touched by the ESP-NOW callback task and read by the UI loop,
// so they are atomic rather than plain volatile.
struct LossWindow
{
    std::atomic<uint32_t> recv{0};
    std::atomic<uint32_t> loss{0};
};

inline void lossWindowAdd(LossWindow &w, uint32_t recvDelta, uint32_t lossDelta)
{
    w.recv.fetch_add(recvDelta, std::memory_order_relaxed);
    w.loss.fetch_add(lossDelta, std::memory_order_relaxed);
}

inline void lossWindowReset(LossWindow &w)
{
    w.recv = 0;
    w.loss = 0;
}

inline uint8_t lossWindowPercent(const LossWindow &w)
{
    uint32_t tot = w.recv.load() + w.loss.load();
    if (tot == 0)
        return 0;
    return (uint8_t)((uint32_t)w.loss.load() * 100 / tot);
}

} // namespace walkie
