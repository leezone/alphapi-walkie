#pragma once

// Shared NVS persistence for AlphaPi walkie firmwares.
// Both devices use the same namespace and keys so a device can be re-flashed
// without losing settings (and so the two stay interchangeable).

#include <Preferences.h>
#include <stdint.h>

namespace walkie
{

static const char kNvsNamespace[] = "walkie";
static const char kKeyName[] = "name";
static const char kKeyChannel[] = "ch";
static const char kKeyVolume[] = "vol";
static const char kKeyPassBits[] = "pass";
static const char kKeyPassLen[] = "passlen";
static const uint8_t kMaxPassLen = 16;
static const uint8_t kDefaultVolume = 6;
static const uint8_t kMaxVolume = 9;

struct StoredSettings
{
    String name;
    uint8_t channel = 1;
    uint8_t volume = kDefaultVolume;
    uint16_t passBits = 0;
    uint8_t passLen = 0;
};

inline StoredSettings loadSettings(uint8_t channelDefault, uint8_t channelMin, uint8_t channelMax)
{
    StoredSettings s;
    Preferences p;
    p.begin(kNvsNamespace, false);
    s.name = p.getString(kKeyName, "");
    s.channel = p.getUChar(kKeyChannel, channelDefault);
    if (s.channel < channelMin || s.channel > channelMax)
        s.channel = channelDefault;
    s.volume = p.getUChar(kKeyVolume, kDefaultVolume);
    if (s.volume > kMaxVolume)
        s.volume = kDefaultVolume;
    if (s.volume == 0)
        s.volume = kDefaultVolume; // legacy 0..6 semantics meant silence
    s.passBits = p.getUShort(kKeyPassBits, 0);
    s.passLen = p.getUChar(kKeyPassLen, 0);
    if (s.passLen > kMaxPassLen)
    {
        s.passLen = 0;
        s.passBits = 0;
    }
    p.end();
    return s;
}

inline void saveName(const String &name)
{
    Preferences p;
    p.begin(kNvsNamespace, false);
    p.putString(kKeyName, name);
    p.end();
}

inline void saveChannel(uint8_t ch)
{
    Preferences p;
    p.begin(kNvsNamespace, false);
    p.putUChar(kKeyChannel, ch);
    p.end();
}

inline void saveVolume(uint8_t vol)
{
    Preferences p;
    p.begin(kNvsNamespace, false);
    p.putUChar(kKeyVolume, vol);
    p.end();
}

inline void savePassword(uint16_t passBits, uint8_t passLen)
{
    Preferences p;
    p.begin(kNvsNamespace, false);
    p.putUShort(kKeyPassBits, passBits);
    p.putUChar(kKeyPassLen, passLen);
    p.end();
}

} // namespace walkie
