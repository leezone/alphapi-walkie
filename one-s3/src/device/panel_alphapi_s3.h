#pragma once

// AlphaPi One (ESP32-S3) ST7789 1.8" 160x128 panel definition for LovyanGFX.
//
// Hardware facts (vendor radio firmware, measured):
//   * ST7789, logical landscape 160x128, MADCTL = 0x60 (swap_xy + mirror y)
//   * SPI2: SCK 41 / MOSI 42 / MISO 47 / CS 38 / DC 40 / RST 39 / BL 37
//   * Backlight on GPIO37 (LEDC PWM, 8 bit, 5 kHz, 50 % default)
//
// The minimal init sequence below mirrors the vendor panel behaviour: only
// reset / sleep-out / colour-mode / madctl / display-on. Extra power, gamma
// or frame-rate commands are known to upset panels of this family.

#include <LovyanGFX.hpp>

#include "config.h"

namespace alphapi {

struct Panel_AlphaPiST7789 : public lgfx::Panel_LCD {
  Panel_AlphaPiST7789() {
    _cfg.panel_width = 160;
    _cfg.panel_height = 128;

    _cfg.memory_width = 160;
    _cfg.memory_height = 128;

    _cfg.offset_x = 0;
    _cfg.offset_y = 0;

    // MADCTL bit3 = 0 -> RGB order
    _cfg.rgb_order = true;
    _cfg.invert = false;
    _cfg.readable = true; // MISO 47 is wired on this board
    _cfg.dummy_read_pixel = 8;
    _cfg.dummy_read_bits = 8;
  }

protected:
  const uint8_t *getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t init_cmds[] = {
        CMD_SWRESET,
        CMD_INIT_DELAY,
        150,
        CMD_SLPOUT,
        CMD_INIT_DELAY,
        120,
        CMD_COLMOD,
        1,
        0x55, // 16 bit RGB565
        CMD_MADCTL,
        1,
        0x60, // swap_xy + mirror -> 160x128 landscape
        CMD_INVOFF,
        0,
        CMD_DISPON,
        CMD_INIT_DELAY,
        120,
        0xFF,
        0xFF};

    return listno == 0 ? init_cmds : nullptr;
  }

  // Orientation bits only; update_madctl() appends the colour bit.
  uint8_t getMadCtl(uint8_t r) const override {
    switch (r & 0x07) {
    case 1:
      return MAD_MV;
    case 2:
      return MAD_MX | MAD_MY;
    case 3:
      return MAD_MY | MAD_MV;
    case 0:
    default:
      return MAD_MX | MAD_MV;
    }
  }
};

class LGFX_AlphaPiOne : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  Panel_AlphaPiST7789 _panel;

public:
  LGFX_AlphaPiOne() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 31250000; // vendor uses pclk = 31.25 MHz
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_dc = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = TFT_RST;
      cfg.pin_busy = -1;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

} // namespace alphapi
