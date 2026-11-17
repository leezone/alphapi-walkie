// AlphaPi One S 的 ST7735 面板定义（LovyanGFX 自定义面板）
//
// 这些参数是 2026-11-13 在真实硬件上逐项扫描确认的，不是猜的：
//   * MADCTL = 0x60 (MX | MV)  -> 横屏 160x128
//   * 色彩顺序 = RGB（MADCTL bit3 = 0）
//   * 反色 = INVOFF
//   * colstart / rowstart = 0 / 0
//   * 初始化 = 厂商极简序列（SLPOUT / COLMOD / MADCTL / DISPON）
//
// 重要 1：LovyanGFX 自带的 Panel_ST7735S 会发送 0xB1/0xC0-0xC4/0x13/gamma
//         等一整串命令，实测在这块面板上会导致「白屏」。所以必须覆盖
//         getInitCommands()，只保留厂商手册 §4.2 的 4 条命令。
//
// 重要 2：Panel_LCD::update_madctl() 会自动把方向位与
//         (_cfg.rgb_order ? MAD_RGB : MAD_BGR) 做 OR。
//         所以要得到 bit3=0（RGB 顺序），必须设 rgb_order = true，
//         并且 getMadCtl() 只返回方向位、不要自己 OR 色彩位。

#pragma once

#include <LovyanGFX.hpp>

namespace alphapi
{

// ST7735，横屏 160x128，参数已实机标定
struct Panel_AlphaPiST7735 : public lgfx::Panel_LCD
{
    Panel_AlphaPiST7735()
    {
        // 面板可视区（横屏）
        _cfg.panel_width = 160;
        _cfg.panel_height = 128;

        // 驱动显存原生是竖屏 128x160，开 MV 后按 160x128 访问
        _cfg.memory_width = 160;
        _cfg.memory_height = 128;

        // 实测无需偏移
        _cfg.offset_x = 0;
        _cfg.offset_y = 0;

        // 实测：RGB 顺序（对应 MADCTL bit3 = 0）
        _cfg.rgb_order = true;

        // 实测：不需要反色
        _cfg.invert = false;

        // 只写不读（本板未接 MISO）
        _cfg.readable = false;

        // ST7735 读取时的 dummy 周期（通用值）
        _cfg.dummy_read_pixel = 9;
    }

  protected:
    // 只发厂商手册 §4.2 的极简序列。
    // 多加任何电源/帧率/gamma 命令都会白屏（已实测）。
    //
    // 格式严格遵循 Panel_Device::command_list()：
    //   { 命令字节, 参数个数, [参数...], [延时ms] }
    // 每条命令**必须**带「参数个数」字节；无参数也要写 0。
    // 参数个数若带 CMD_INIT_DELAY(0x80) 位，则参数后紧跟一个延时字节。
    // 之前漏写无参数命令的 0，导致后续字节被当成参数个数、整表错乱 -> 白屏。
    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        static constexpr uint8_t init_cmds[] = {
            CMD_SWRESET, CMD_INIT_DELAY,
            150, // 软复位，等 150ms
            CMD_SLPOUT,  CMD_INIT_DELAY,
            120, // 退出睡眠，等 120ms
            CMD_COLMOD,  1,
            0x05, // 16bit RGB565
            CMD_MADCTL,  1,
            0x60,           // MX | MV -> 横屏 160x128
            CMD_INVOFF,  0, // 不反色（无参数，个数必须写 0）
            CMD_DISPON,  CMD_INIT_DELAY,
            120,              // 开显示，等 120ms
            0xFF,        0xFF // 结束标记
        };

        return listno == 0 ? init_cmds : nullptr;
    }

    // 只返回方向位；色彩位由 update_madctl() 用 rgb_order 追加。
    //
    // r 的低 3 位是旋转档：0=0度 1=90度 2=180度 3=270度，
    // bit3 是 lgfx 内部的 mirror 标志。
    uint8_t getMadCtl(uint8_t r) const override
    {
        switch (r & 0x07)
        {
        case 1:
            return MAD_MV; // 竖屏 128x160
        case 2:
            return MAD_MX | MAD_MY; // 横屏 180 度
        case 3:
            return MAD_MY | MAD_MV; // 竖屏 逆 90
        case 0:
        default:
            return MAD_MX | MAD_MV; // 横屏 160x128（默认）
        }
    }
}; // Panel_AlphaPiST7735

// LovyanGFX 需要一个把 bus + panel 组合起来的 LGFX 类
class LGFX_AlphaPi : public lgfx::LGFX_Device
{
    lgfx::Bus_SPI _bus;
    Panel_AlphaPiST7735 _panel;

  public:
    LGFX_AlphaPi()
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 20000000; // 实测稳定
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 37; // TFT_SCLK
            cfg.pin_mosi = 38; // TFT_MOSI
            cfg.pin_miso = -1; // 本板未接
            cfg.pin_dc = 36;   // TFT_DC
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        {
            auto cfg = _panel.config();
            cfg.pin_cs = 34;  // TFT_CS
            cfg.pin_rst = 35; // TFT_RST
            cfg.pin_busy = -1;
            _panel.config(cfg);
        }

        setPanel(&_panel);
    }
};

} // namespace alphapi
