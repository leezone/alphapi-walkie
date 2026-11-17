#pragma once

// AlphaPi Zero (ESP32-C3) walkie-talkie configuration

// --- Keys (INPUT_PULLDOWN in firmware, pressed = HIGH) ---
#define KEY_A 10
#define KEY_B 1
#define KEY_C 3

// --- Co-processor UART (HT, STEM-Radio verified) ---
#define CO_BAUD 460929
#define CO_RX 9
#define CO_TX 8
#define CO_RATE 24000 // 8-bit PCM playback rate consumed by co-processor

// --- Matrix ---
#define MTX_ADDR 0x08

// --- ESP-NOW / identity ---
#define CHANNEL_DEFAULT 1
#define CHANNEL_MIN 1
#define CHANNEL_MAX 13

// --- Timing (ms) ---
#define HT_BOOT_DELAY_MS 2000
#define PASS_CLEAR_HOLD_MS 10000
