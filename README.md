# AlphaPi Walkie

AlphaPi One S（ESP32-S2R2）对讲机固件：按住说话、松开发送，局域网内 ESP-NOW 直连，不走路由器、不需要蜂窝网络。

[![license](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)

## 硬件

| 项目 | 规格 |
|---|---|
| 开发板 | **AlphaPi One S** |
| 主控 | ESP32-S2R2（Xtensa LX7 单核 240MHz，WiFi，无蓝牙） |
| Flash / PSRAM | 8 MB Flash / 2 MB PSRAM |
| 屏幕 | ST7735，160×128 横屏，RGB565 |
| 音频 | N32 UART 协处理器，460800 baud，8-bit 无符号单声道 @ 24 kHz |
| 按键 | 前置 A / B / C / D 四键（高电平有效）：A=PTT，B=配对页入口，C/D=音量 −/+ |

## 功能

- **对讲**：A 按住说话、松开收听；收到的语音音频直达喇叭，不依赖路由器。
- **邻居发现**：设备周期广播心跳包（beacon）携带本机名，能看到同频道内所有邻居及谁在说话。
- **信道扫描**：配对页显示 13 个信道的占用情况（柱高=AP 数，红=强干扰），当前信道黄框标出。
- **闭环功率控制**：根据对端丢包率与接收信号自动调节发射功率三档（8.5 / 13 / 17 dBm），屏幕信号条实时显示当前档位。
- **接收丢帧补偿**：收到的语音攒够水位再起播，卡顿时用前一帧衰减补帧，不把欠载放大成空洞。
- **AES-128-CTR 加密**：按密码派生密钥（SHA-256），密码以位串输入，空=不加密。
- **全中文界面**：底层自绘中文字库（GB2312 一级字库 3756 字，HZK16），配对页显示邻居列表与信道占用图。

## 按键

| 页面 | 键位 | 作用 |
|---|---|---|
| 首页 | A（按住） | 发言（PTT），松开结束 |
| 首页 | B（单击） | 进入配对页 |
| 首页 | C / D（单击） | 音量 − / +（0..9） |
| 配对页 | A / B / C / D | A 返回首页 · B 进密码编辑 · C 频道 − · D 频道 + |
| 配对页（密码编辑） | A / B / C / D | A 保存 · B 清空编辑 · C 输入 0 · D 输入 1 |

所有按键均为上升沿触发，松手算一次，不重复响应。

## 从源码编译

实测环境：PlatformIO（`lolin_s2_mini` 板型）+ Arduino 框架，LovyanGFX 驱动 ST7735。

```bash
# 依赖：PlatformIO Core（首次会下载工具链 ~200MB）
cd alphapi-walkie
pio run
```

产物：`.pio/build/lolin_s2_mini/firmware.bin`

## 刷机

布局为 Arduino 默认 ota 布局（`app0@0x10000`，与 Zero / One S3 一致）：

```
0x1000  bootloader.bin      Arduino 二级 bootloader
0x8000  partition-table.bin Arduino 默认分区表（app0@0x10000）
0xe000  boot_app0.bin       otadata（factory → app0）
0x10000 alphapi-walkie.bin  本固件
```

命令行示例（esptool）：

```bash
python3 -m esptool --port /dev/ttyACM0 --chip esp32s2 write_flash -z \
  0x1000 bootloader.bin 0x8000 partition-table.bin 0xe000 boot_app0.bin 0x10000 alphapi-walkie.bin
```

## 仓库结构

```
alphapi-walkie/
├── platformio.ini    # 板型 / 框架 / 依赖
├── src/
│   ├── main.cpp      # 主程序
│   └── device/       # 设备专用：面板定义、配置、中文字库
├── core/             # 共享协议/功控/加密/音频等头文件（One S 与 Zero / One S3 复用）
└── dist/             # 打包好的刷机页（esp-web-tools）与分段固件
```

## 功能速览

- PTT 单向半双工语音，对讲范围内直连通信
- 邻居发现 + 信道占用图 + 当前信道高亮
- 三档功率闭环控制 + 信号强度指示
- 丢帧补偿 + 欠载补帧保护
- AES 加密的通话

## 许可

MIT（见 [LICENSE](LICENSE)）。
