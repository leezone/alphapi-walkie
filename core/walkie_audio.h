#pragma once

// Shared audio helpers: 8 kHz voice -> 24 kHz PCM playback and gap filling.
//
// Wire format: 8-bit unsigned PCM (128 = silence) at 8 kHz.
// Playback format: 8-bit unsigned PCM at 24 kHz, so every sample is
// linearly interpolated into 3 output samples.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace walkie
{

static const int kSamplesPerFrame = 66; // 8.25 ms at 8 kHz
static const int kBytesPerFrame = 198;  // 66 * 3 at 24 kHz
static const int kUpScale = 3;          // 24k / 8k
static const uint8_t kSilence = 128;

// Convert one 8 kHz sample to 3 interpolated 24 kHz samples.
inline size_t upsample(int sample, int &prev, uint8_t volume, uint8_t *out, size_t outCap)
{
    if (prev < 0)
        prev = sample;
    size_t n = 0;
    for (int k = 1; k <= kUpScale; k++)
    {
        if (n >= outCap)
            break;
        int iv = prev + (sample - prev) * k / kUpScale;
        int v = iv * volume / 9 + kSilence;
        if (v < 0)
            v = 0;
        else if (v > 255)
            v = 255;
        out[n++] = (uint8_t)v;
    }
    prev = sample;
    return n;
}

// Build a "keep the link alive" frame from the last sample, attenuated to 3/4.
inline size_t gapFrame(int &prev, uint8_t volume, uint8_t *out, size_t outCap)
{
    int base = (prev < 0 ? 0 : prev) * 3 / 4;
    prev = base;
    int v = base * volume / 9 + kSilence;
    if (v < 0)
        v = 0;
    else if (v > 255)
        v = 255;
    size_t n = outCap < kBytesPerFrame ? outCap : (size_t)kBytesPerFrame;
    memset(out, (uint8_t)v, n);
    return n;
}

inline uint8_t amplitudeOf(int sample)
{
    int amp = sample < 0 ? -sample : sample;
    return (uint8_t)(amp > 255 ? 255 : amp);
}

} // namespace walkie
