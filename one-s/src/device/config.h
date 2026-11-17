#pragma once

// AlphaPi One S (ESP32-S2, lolin_s2_mini) walkie-talkie configuration

// --- Display: ST7735 160x128 ---
#define TFT_MOSI 38
#define TFT_SCLK 37
#define TFT_CS 34
#define TFT_DC 36
#define TFT_RST 35
#define TFT_BL 33

#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 128

// --- Keys (INPUT_PULLDOWN in firmware, pressed = HIGH) ---
#define KEY_A 13 // PTT
#define KEY_B 12
#define KEY_C 11
#define KEY_D 10

// --- N32 codec UART ---
#define AUDIO_BAUD 460800
#define AUDIO_TX_PIN 3
#define AUDIO_RX_PIN 0

// --- I2C (N32 peripheral bus scan / init context) ---
#define I2C_SDA_PIN 39
#define I2C_SCL_PIN 40
#define I2C_FREQ_HZ 100000

// --- ESP-NOW / identity ---
#define CHANNEL_DEFAULT 1
#define CHANNEL_MIN 1
#define CHANNEL_MAX 13

// --- Timing (ms) ---
#define N32_BOOT_DELAY_MS 2000
#define DIAG_INTERVAL_MS 1000
