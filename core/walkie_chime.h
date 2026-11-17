#pragma once

// Shared notification chime (Mario coin).
// The co-processor has an inherent startup latency (~100ms) before audio is
// actually output, so the first N milliseconds of a tone can be swallowed.
// Always emit a leading silence gap so the first audible note is intact.

#include <stdint.h>
#include <string.h>

namespace walkie
{

using PlayPcmFn = bool (*)(const uint8_t *pcm, size_t len);

constexpr uint32_t kChimeRate = 24000;
constexpr uint32_t kChimeLeadMs = 150;  // covers co-processor startup latency
constexpr uint32_t kChimeFreqB5 = 988;
constexpr uint32_t kChimeFreqE6 = 1319;

inline bool playCoinChime(PlayPcmFn play)
{
    constexpr uint32_t leadMs = kChimeLeadMs;
    constexpr uint32_t b5Ms = 110;
    constexpr uint32_t gapMs = 25;
    constexpr uint32_t e6Ms = 740;

    uint8_t buf[200];
    uint32_t total = kChimeRate * (leadMs + b5Ms + gapMs + e6Ms) / 1000;
    uint32_t phase = 0;
    uint32_t halfB5 = kChimeRate / (2u * kChimeFreqB5);
    uint32_t halfE6 = kChimeRate / (2u * kChimeFreqE6);

    for (uint32_t done = 0; done < total; )
    {
        size_t n = (total - done) > sizeof(buf) ? sizeof(buf) : (size_t)(total - done);
        for (size_t i = 0; i < n; i++)
        {
            uint32_t idx = done + (uint32_t)i;
            uint32_t ms = idx * 1000 / kChimeRate;
            int amp = 0;
            uint32_t half = halfB5;
            if (ms < leadMs) amp = 0;                        // 静音引导，等协控就绪
            else if (ms < leadMs + b5Ms) { amp = 8; half = halfB5; }
            else if (ms < leadMs + b5Ms + gapMs) amp = 0;    // 两音间隙
            else
            {
                uint32_t step = (ms - leadMs - b5Ms - gapMs) / 60;
                amp = 7 - (int)step;
                if (amp < 1) amp = 1;
                half = halfE6;
            }
            phase++;
            buf[i] = ((phase / half) & 1u) ? (uint8_t)(128 + amp) : (uint8_t)(128 - amp);
        }
        if (!play(buf, n)) return false;
        done += n;
    }
    return true;
}

} // namespace walkie
