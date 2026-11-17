#pragma once

// AlphaPi One (ESP32-S3, 8MB Flash + 2MB PSRAM) walkie-talkie configuration.
// Hardware facts taken from the vendor "AlphaPi One 独立 WiFi 收音机" developer
// manual (measured on real hardware).

// --- Display: ST7789 1.8" 160x128, SPI2 ---
#define TFT_SCLK 41
#define TFT_MOSI 42
#define TFT_MISO 47
#define TFT_CS 38
#define TFT_DC 40
#define TFT_RST 39
#define TFT_BL 37

#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 128

// --- N32 co-processor UART (same wiring as One S, different baud) ---
#define AUDIO_BAUD 460929
#define AUDIO_TX_PIN 3
#define AUDIO_RX_PIN 0

// --- Keys: no ESP32 GPIOs on this board; all keys live in N32 register 0x00.
// Bit set = pressed (runtime-verified in the vendor radio firmware).
#define N32_REG_KEYS 0x00
#define KEY_BIT_A 0x04
#define KEY_BIT_B 0x08
#define KEY_BIT_L 0x02
#define KEY_BIT_R 0x01
#define KEY_BITS_ALL (KEY_BIT_A | KEY_BIT_B | KEY_BIT_L | KEY_BIT_R)
#define KEY_READ_FAIL 0xFF

// 按键极性：厂商手册写"低电平有效"，但厂商已烧录固件按"置位=按下"取掩码。
// 实机若相反，把这里改成 0 即可，无需改逻辑。
#define KEY_PRESSED_IS_SET 1

// --- ESP-NOW / identity ---
#define CHANNEL_DEFAULT 1
#define CHANNEL_MIN 1
#define CHANNEL_MAX 13

// --- Timing (ms) ---
#define N32_BOOT_DELAY_MS 2000
#define KEY_POLL_MS 40
#define DIAG_INTERVAL_MS 1000
