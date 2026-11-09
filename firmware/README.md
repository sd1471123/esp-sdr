# ESP32-C5 LCD 固件

为 ESP32-C5-DevKitC-1 + 8 针 ST7789 240×320 SPI 屏 + 5 键 + 有源蜂鸣器编译的
`esp-sdr` 固件（`CONFIG_ESP_SDR_LOCAL_UI=y`）。

面板原生 240×320，固件默认横屏使用（MADCTL `0x60`，坐标空间 320×240），
按 R 可切回竖屏 240×320。

构建时间：2026-11-10，ESP-IDF v6.1，target `esp32c5`。

## 文件

| 文件 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` | `0x0` | 941520 | 合并镜像，一次烧完 |
| `esp32c5-lcd-bootloader.bin` | `0x2000` | 22160 | 二级引导 |
| `esp32c5-lcd-partition-table.bin` | `0x8000` | 3072 | 分区表 |
| `esp32c5-lcd-app.bin` | `0x10000` | 875984 | 应用（占 1 MB 分区 84%） |

Flash 2 MB，DIO，80 MHz。

## 烧写

整片烧写（推荐）：

```bash
python -m esptool --chip esp32c5 -p COM5 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x0 esp32c5-lcd-full.bin
```

只更新应用（保留 NVS 里的设置与校准）：

```bash
python -m esptool --chip esp32c5 -p COM5 -b 460800 \
  write-flash 0x10000 esp32c5-lcd-app.bin
```

从源码重新构建：

```bash
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.esp32c5-lcd"
idf.py set-target esp32c5
idf.py build
```

## 接线

屏幕排针：`GND VCC SCL SDI RESET AO CS BL`，其中 `AO` 是 A0/DC，`SDI` 是 MOSI（单向写，无 MISO）。

| 屏幕 | ESP32-C5 | 位置 |
|---|---|---|
| GND | GND | — |
| VCC | 3V3 | 右排 1 |
| SCL | GPIO6 | 右排 7 |
| SDI | GPIO7 | 右排 8 |
| RESET | GPIO5 | 左排 9 |
| AO | GPIO4 | 左排 8 |
| CS | GPIO10 | 右排 11 |
| BL | GPIO23 | 左排 5 |

| 功能 | GPIO | 位置 |
|---|---|---|
| ← | GPIO0 | 右排 5 |
| → | GPIO1 | 右排 6 |
| B（频段） | GPIO8 | 右排 9 |
| M（模式） | GPIO9 | 右排 10 |
| R（旋转） | GPIO24 | 左排 4 |
| 蜂鸣器 | GPIO25 | 右排 13 |

按键对地、内部上拉，低电平有效；蜂鸣器经 NPN（S8050）驱动，基极串 1 kΩ。

引脚定义在 `main/local_ui/board_c5_lcd.h`，改线只需改这一处。

## 上电预期

背光先亮，接着是一段约 2 秒的开机自检：红、绿、蓝、白、黑满屏，再画白色
1 px 外框 + 16 px 黑白棋盘，最后清屏进入主界面。

自检就是给接线用的判据：

- 五块纯色没有杂点、外框四条边都贴边、棋盘格棱角分明 → 总线和窗口都对，
  主界面再花就是绘制逻辑的事。
- 纯色里出现彩色噪点或斜纹 → SPI 时钟太快或某根数据线虚接。把
  `main/local_ui/st7789.c` 里的 `LCD_SPI_HZ` 降到 10000000 再试。
- 外框偏移、缺一条边、棋盘被裁掉一部分 → 面板可见区有偏移，调
  `LCD_OFS_X` / `LCD_OFS_Y`（240×320 面板通常 0/0）。

自检确认没问题后，把 `LCD_SELFTEST` 改成 0 重新编译即可关掉。

自检结束后进入主界面：频谱 + 瀑布，顶栏显示频段与模式；按 ←/→ 走信道，
B 切 2.4G/5G，M 切 LIVE/SWEEP，R 转屏，← + → 同时按静音。
