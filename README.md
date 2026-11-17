# AlphaPi Walkie

AlphaPi 系列小板的对讲机固件：按住说话、松开发送，局域网内 ESP-NOW 直连，不走路由器、不需要蜂窝网络。适用于三款开发板：

- **AlphaPi One S**（`one-s/`，ESP32-S2，ST7735 160×128，四键）
- **AlphaPi Zero**（`zero/`，ESP32-C3，5×5 LED 点阵，三键）
- **AlphaPi One S3**（`one-s3/`，ESP32-S3，ST7789 160×128，四键）

三端共用 `core/` 里的协议、加密、功控、音频实现；设备差异（面板、按键、协处理器波特率、字体等）均在各自的 `src/device/` 下。

[![license](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)

## 功能

- **对讲**：A 按住说话、松开收听；收到的语音音频直达喇叭，不依赖路由器。
- **邻居发现**：设备周期广播心跳包（beacon）携带本机名，能看到同频道内所有邻居及谁在说话。
- **信道扫描**：配对页显示 13 个信道的占用情况（柱高=AP 数，红=强干扰），当前信道黄框标出。
- **闭环功率控制**：根据对端丢包率与接收信号自动调节发射功率三档（8.5 / 13 / 17 dBm），屏幕信号条实时显示当前档位。
- **接收丢帧补偿**：收到的语音攒够水位再起播，卡顿时用前一帧衰减补帧，不把欠载放大成空洞。
- **AES-128-CTR 加密**：按密码派生密钥（SHA-256），密码以位串输入，空=不加密。
- **全中文界面**（One S / One S3）：GB2312 一级字库 3756 字，HZK16。
- **5×5 点阵显示**（Zero）：形状+MAC 尾号滚动、信道号/密码视图/音量档，加密时锁字形排在机器名前。
- **上线提示音**：马里奥金币音。

## 按键

### One S / One S3（四键 A/B/C/D）

| 页面 | 键位 | 作用 |
|---|---|---|
| 首页 | A（按住） | 发言（PTT），松开结束 |
| 首页 | B（单击） | 进入配对页 |
| 首页 | C / D（单击） | 音量 − / +（0..9） |
| 配对页 | A / B / C / D | A 返回首页 · B 进密码编辑 · C 频道 − · D 频道 + |
| 配对页（密码编辑） | A / B / C / D | A 保存 · B 清空编辑 · C 输入 0 · D 输入 1 |

所有按键均为上升沿触发，松手算一次，不重复响应。

### Zero（三键 A/B/C）

| 键位 | 作用 |
|---|---|
| A（按住） | 发言（PTT）；A 先按住 = 音量模式，B/C 只调音量 |
| B（单击） | 先按住 = 信道模式（A/C 改信道）；单按 = 显示信道号 |
| C（单击） | 先按住 = 密码模式（A=位 0 / B=位 1）；单按 = 密码视图 |
| C（长按 ≥10s） | 清空密码 |

密码视图：先按 A 后按 B，进入 5×5 点阵密码显示——
- 无密码：全屏 X
- 有密码：位数 = 灯数，1 恒亮 / 0 半亮（闪烁）

## 从源码编译

环境：PlatformIO（对应板型）+ Arduino 框架。

```bash
# 依赖：PlatformIO Core（首次会下载工具链 ~200MB）
cd one-s && pio run          # One S
cd zero   && pio run          # Zero
cd one-s3 && pio run          # One S3
```

产物：`one-s/.pio/build/lolin_s2_mini/firmware.bin`（对应板型目录下的 `.pio/build/...`）

## 刷机

`dist/` 下是刷机页面与分段固件（esp-web-tools WebSerial）：

```
dist/walkie-one-s/index.html    One S 刷机页（按页面上的说明刷）
dist/walkie-zero/index.html     Zero 刷机页
dist/walkie-one-s3/index.html   One S3 刷机页
```

布局统一为 Arduino 默认 OTA 布局（`app0@0x10000`）：app 写 `0x10000`，`bootloader.bin` 写 `0x1000`（Zero 写 `0x0`），`partition-table.bin` 写 `0x8000`，`boot_app0.bin` 写 `0xe000`。

## 仓库结构

```
core/                   ← 三端共享：协议 / 加密 / 功控 / 音频 / NVS / 字体
one-s/                  ← AlphaPi One S（ESP32-S2, ST7735, 四键）
  platformio.ini
  src/main.cpp
  src/device/           设备专用：面板定义、配置、开机界面
zero/                   ← AlphaPi Zero（ESP32-C3, 5×5 点阵, 三键）
one-s3/                 ← AlphaPi One S3（ESP32-S3, ST7789, 四键）
dist/                   ← 各板刷机页 + 分段固件（bin 构建产物不入库）
```

## 许可

MIT（见 [LICENSE](LICENSE)）。
