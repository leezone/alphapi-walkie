#pragma once

#include <stdint.h>

namespace walkie
{

// Tracks voice packet sequence continuity for one peer.
// Call onVoicePacket() for every received voice packet.
struct VoiceRxTracker
{
    uint8_t lastSeq = 0;
    bool haveSeq = false;
    uint32_t lastVoiceMs = 0;

    struct Event
    {
        bool newSession = false; // gap since last packet exceeded reset window
        uint16_t gap = 0;        // number of lost packets (sequence gap)
        bool stalled = false;    // inter-arrival gap in stall window (sender starved)
    };

    Event onVoicePacket(uint8_t seq, uint32_t now, uint32_t gapResetMs, uint32_t stallMinMs,
                        uint32_t stallMaxMs)
    {
        Event ev{};
        if (haveSeq && now - lastVoiceMs >= gapResetMs)
        {
            ev.newSession = true;
            haveSeq = false;
        }
        if (haveSeq && (uint8_t)(seq - lastSeq) != 1)
        {
            ev.gap = (uint16_t)(uint8_t)(seq - lastSeq - 1);
        }
        if (haveSeq && now - lastVoiceMs > stallMinMs && now - lastVoiceMs < stallMaxMs)
        {
            ev.stalled = true;
        }
        lastSeq = seq;
        haveSeq = true;
        lastVoiceMs = now;
        return ev;
    }
};

} // namespace walkie
