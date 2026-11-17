#pragma once

// Shared co-processor frame protocol (0x90 write / 0x91 ack).
// One S (N32) and Zero (HT) speak the same framing; only the UART instance differs.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace walkie
{

static const uint8_t kFrameWrite = 0x90;
static const uint8_t kFrameAck = 0x91;
static const size_t kMaxPayload = 200;

// Register map (identical on One S N32 and Zero HT)
static const uint8_t REG_CTRL = 0x10;     // control: 0x00 stop / 0x01 mic on
static const uint8_t REG_VOLUME = 0x14;   // hardware volume 0..6
static const uint8_t REG_PLAY = 0x15;     // playback data
static const uint8_t REG_BUS_INIT = 0x0f; // co-processor peripheral bus init

inline uint8_t checksumOf(const uint8_t *buf, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++)
        s += buf[i];
    return s;
}

// Build a write frame into out. Returns frame length, or 0 if it does not fit.
inline size_t buildWriteFrame(uint8_t *out, size_t outCap, uint8_t reg, const uint8_t *payload,
                              size_t len)
{
    if (!out || len > kMaxPayload || outCap < len + 4)
        return 0;
    out[0] = kFrameWrite;
    out[1] = reg;
    out[2] = (uint8_t)len;
    if (len > 0 && payload)
        memcpy(out + 3, payload, len);
    out[len + 3] = checksumOf(out, len + 3);
    return len + 4;
}

struct AckFrame
{
    bool valid = false;
    uint8_t reg = 0;
    uint8_t value = 0;
};

// Parse a 3-byte ack: 0x91 <reg> <value>
inline AckFrame parseAck(const uint8_t *ack, size_t len)
{
    AckFrame a;
    if (!ack || len < 3)
        return a;
    if (ack[0] != kFrameAck)
        return a;
    a.valid = true;
    a.reg = ack[1];
    a.value = ack[2];
    return a;
}

} // namespace walkie
