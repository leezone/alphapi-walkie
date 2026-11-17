#pragma once

#include <stdint.h>

// Shared timing constants (identical for One S and Zero).
// Device-specific settings stay in each device's device/config.h.

namespace walkie
{

constexpr uint32_t LOSS_WINDOW_MS = 2000;
constexpr uint32_t BEACON_INTERVAL_MS = 2000;
constexpr uint32_t BEACON_FAST_MS = 500;
constexpr uint32_t VOICE_RX_RECENT_MS = 1500;
constexpr uint32_t PWR_ADJ_INTERVAL_MS = 1000;
constexpr uint32_t PWR_FB_FRESH_MS = 4000;
constexpr uint32_t PRUNE_INTERVAL_MS = 1000;
constexpr uint32_t VOICE_GAP_RESET_MS = 500;
constexpr uint32_t VOICE_STALL_MIN_MS = 60;
constexpr uint32_t VOICE_STALL_MAX_MS = 500;
constexpr uint32_t PEER_TIMEOUT_MS = 6000;
constexpr uint32_t TALK_TIMEOUT_MS = 800;

} // namespace walkie
