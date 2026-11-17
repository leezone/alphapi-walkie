#pragma once

#include <stdint.h>
#include <string.h>

namespace walkie
{

static const int kMaxPeers = 8;
static const int kPeerNameLen = 12; // including null

struct PeerEntry
{
    bool used = false;
    uint8_t mac[6] = {0};
    char name[kPeerNameLen] = {0};
    uint32_t lastSeen = 0;
    uint32_t lastVoice = 0;
};

inline int peerIndexByMac(PeerEntry *peers, int count, const uint8_t *mac)
{
    if (!peers || !mac)
        return -1;
    for (int i = 0; i < count; i++)
        if (peers[i].used && memcmp(peers[i].mac, mac, 6) == 0)
            return i;
    return -1;
}

// Update/touch peer. Returns true if a new or rejoined peer was seen.
inline bool peerTouch(PeerEntry *peers, int count, const uint8_t *mac, const char *name,
                      uint32_t now, bool isVoice, uint32_t peerTimeoutMs, const char *defaultName)
{
    if (!peers || !mac)
        return false;
    int i = peerIndexByMac(peers, count, mac);
    bool wasStale = false;
    if (i < 0)
    {
        for (int k = 0; k < count; k++)
        {
            if (!peers[k].used)
            {
                i = k;
                peers[k].used = true;
                memcpy(peers[k].mac, mac, 6);
                const char *dn = defaultName ? defaultName : "";
                strncpy(peers[k].name, dn, kPeerNameLen - 1);
                peers[k].name[kPeerNameLen - 1] = 0;
                peers[k].lastSeen = 0;
                peers[k].lastVoice = 0;
                wasStale = true;
                break;
            }
        }
        if (i < 0)
            return false; // table full
    }
    else if ((uint32_t)(now - peers[i].lastSeen) > peerTimeoutMs)
    {
        wasStale = true; // rejoined after timeout
    }

    if (name && name[0])
    {
        strncpy(peers[i].name, name, kPeerNameLen - 1);
        peers[i].name[kPeerNameLen - 1] = 0;
    }
    peers[i].lastSeen = now;
    if (isVoice)
        peers[i].lastVoice = now;
    return wasStale;
}

inline int findTalker(const PeerEntry *peers, int count, uint32_t now, uint32_t talkTimeoutMs)
{
    if (!peers)
        return -1;
    for (int i = 0; i < count; i++)
        if (peers[i].used && (uint32_t)(now - peers[i].lastVoice) < talkTimeoutMs)
            return i;
    return -1;
}

// Prune timed-out peers. Returns true if any entry changed.
inline bool prunePeers(PeerEntry *peers, int count, uint32_t now, uint32_t peerTimeoutMs)
{
    if (!peers)
        return false;
    bool changed = false;
    for (int i = 0; i < count; i++)
    {
        if (peers[i].used && (uint32_t)(now - peers[i].lastSeen) > peerTimeoutMs)
        {
            peers[i].used = false;
            changed = true;
        }
    }
    return changed;
}

} // namespace walkie
