#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include "walkie_protocol.h"

namespace walkie
{

static const uint8_t kEncMarker = 0xA5;
static const int kVoiceChunkMax = 244; // max raw audio bytes per ESP-NOW voice packet

struct CryptoState
{
    bool enabled = false;
    uint8_t key[16] = {0};
    mbedtls_aes_context tx = {};
    mbedtls_aes_context rx = {};
    bool inited = false;
};

inline void cryptoInit(CryptoState &s)
{
    if (s.inited)
        return;
    mbedtls_aes_init(&s.tx);
    mbedtls_aes_init(&s.rx);
    s.inited = true;
}

inline void cryptoFree(CryptoState &s)
{
    if (!s.inited)
        return;
    mbedtls_aes_free(&s.tx);
    mbedtls_aes_free(&s.rx);
    s.inited = false;
    s.enabled = false;
    memset(s.key, 0, sizeof(s.key));
}

inline void derivePasswordKey(CryptoState &s, uint16_t passBits, uint8_t passLen)
{
    cryptoInit(s);
    s.enabled = (passLen > 0);
    if (!s.enabled)
        return;

    char seed[32];
    snprintf(seed, sizeof(seed), "walkie:%u:%04X", (unsigned)passLen, (unsigned)passBits);
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t *)seed, strlen(seed), hash, 0);
    memcpy(s.key, hash, 16);
    mbedtls_aes_setkey_enc(&s.tx, s.key, 128);
    mbedtls_aes_setkey_enc(&s.rx, s.key, 128);
}

// AES-128-CTR (ECB keystream), nonce[0]=seq, nonce[1..2]=block counter.
inline void cryptPayload(CryptoState &s, uint8_t *buf, size_t len, uint8_t seq, bool tx)
{
    if (!s.enabled || !buf || len == 0)
        return;
    mbedtls_aes_context *ctx = tx ? &s.tx : &s.rx;
    uint8_t nonce[16] = {0};
    nonce[0] = seq;
    uint8_t stream[16];
    size_t off = 0;
    uint32_t ctr = 0;
    while (off < len)
    {
        nonce[1] = (uint8_t)(ctr >> 8);
        nonce[2] = (uint8_t)(ctr & 0xFF);
        mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_ENCRYPT, nonce, stream);
        size_t n = len - off;
        if (n > 16)
            n = 16;
        for (size_t i = 0; i < n; i++)
            buf[off + i] ^= stream[i];
        off += n;
        ctr++;
    }
}

// Build one voice packet into pkt (cap >= 3 + chunk). Returns packet length.
inline int buildVoicePacket(CryptoState &s, uint8_t *pkt, int pktCap, uint8_t seq,
                            const uint8_t *audio, int chunk)
{
    if (!pkt || !audio || chunk <= 0)
        return 0;
    if (chunk > kVoiceChunkMax)
        chunk = kVoiceChunkMax;
    int pktLen = 3 + chunk;
    if (pktCap < pktLen)
        return 0;
    pkt[0] = kMagic;
    pkt[1] = PKT_VOICE;
    pkt[2] = seq;
    if (s.enabled)
    {
        if (chunk < 1)
            return 0;
        pkt[3] = kEncMarker;
        memcpy(pkt + 4, audio, chunk - 1);
        cryptPayload(s, pkt + 3, chunk, seq, true);
    }
    else
    {
        memcpy(pkt + 3, audio, chunk);
    }
    return pktLen;
}

struct VoiceDecoded
{
    const uint8_t *payload = nullptr;
    int len = 0;
    bool badKey = false;
};

// Decode voice packet body (data points to full packet, len is full packet len).
// decBuf must have room for at least (len - 3) bytes. Returns decoded view into decBuf.
inline VoiceDecoded decodeVoicePacket(CryptoState &s, uint8_t *decBuf, int decCap,
                                      const uint8_t *data, int len, uint8_t seq)
{
    VoiceDecoded out{};
    if (!data || !decBuf || len < 4)
        return out;
    int plen = len - 3;
    if (plen <= 0 || plen > decCap)
        return out;
    memcpy(decBuf, data + 3, plen);
    cryptPayload(s, decBuf, plen, seq, false);
    if (s.enabled)
    {
        if (decBuf[0] != kEncMarker)
        {
            out.badKey = true;
            return out;
        }
        out.payload = decBuf + 1;
        out.len = plen - 1;
    }
    else
    {
        out.payload = decBuf;
        out.len = plen;
    }
    return out;
}

} // namespace walkie
