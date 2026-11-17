#pragma once

// Shared ESP-NOW transport for AlphaPi walkie firmwares.
// Device code only provides the receive callback and the channel/power policy.

#include <atomic>
#include <stdint.h>
#include <string.h>

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace walkie
{

inline const uint8_t *broadcastAddr()
{
    static const uint8_t addr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return addr;
}

struct Radio
{
    uint8_t channel = 1;
    std::atomic<uint16_t> txFail{0}; // esp_now_send() returned non-OK
    std::atomic<uint16_t> txNack{0}; // send callback reported failure (queue overflow)

    bool init(uint8_t ch, int8_t txPowerQuarterDbm, esp_now_recv_cb_t recvCb)
    {
        channel = ch;
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        // Must disable power save: in PS mode the STA sleeps periodically and
        // broadcast frames get dropped ("talk works, then suddenly dies").
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_max_tx_power(txPowerQuarterDbm);
        if (esp_now_init() != ESP_OK)
            return false;
        esp_now_register_recv_cb(recvCb);
        esp_now_register_send_cb(radioSendCb);
        activeRadio() = this;
        return addBroadcastPeer();
    }

    bool addBroadcastPeer()
    {
        esp_now_peer_info_t peerInfo{};
        memcpy(peerInfo.peer_addr, broadcastAddr(), 6);
        peerInfo.channel = channel;
        peerInfo.encrypt = false;
        return esp_now_add_peer(&peerInfo) == ESP_OK;
    }

    bool removeBroadcastPeer()
    {
        return esp_now_del_peer(broadcastAddr()) == ESP_OK;
    }

    // Switch channel and re-register the broadcast peer (peer is channel bound).
    bool setChannel(uint8_t ch)
    {
        channel = ch;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        removeBroadcastPeer();
        return addBroadcastPeer();
    }

    void setTxPower(int8_t txPowerQuarterDbm)
    {
        esp_wifi_set_max_tx_power(txPowerQuarterDbm);
    }

    bool send(const uint8_t *data, int len)
    {
        if (!data || len <= 0)
            return false;
        if (esp_now_send(broadcastAddr(), data, len) != ESP_OK)
        {
            txFail = (uint16_t)(txFail.load() + 1);
            return false;
        }
        return true;
    }

    static Radio *&activeRadio()
    {
        static Radio *p = nullptr;
        return p;
    }

    static void radioSendCb(const wifi_tx_info_t *info, esp_now_send_status_t st)
    {
        Radio *r = activeRadio();
        if (r && st != ESP_NOW_SEND_SUCCESS)
            r->txNack = (uint16_t)(r->txNack.load() + 1);
    }
};

} // namespace walkie
