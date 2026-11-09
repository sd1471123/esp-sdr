# ESP32-C5 LCD 固件

为 ESP32-C5-DevKitC-1 + 8 针 ST7789 320×240 SPI 屏 + 5 键 + 有源蜂鸣器编译的
`esp-sdr` 固件（`CONFIG_ESP_SDR_LOCAL_UI=y`）。

构建时间：2026-11-09，ESP-IDF v6.1，target `esp32c5`。

## 文件

| 文件 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` | `0x0` | 941200 | 合并镜像，一次烧完 |
| `esp32c5-lcd-bootloader.bin` | `0x2000` | 22160 | 二级引导 |
| `esp32c5-lcd-partition-table.bin` | `0x8000` | 3072 | 分区表 |
| `esp32c5-lcd-app.bin` | `0x10000` | 875664 | 应用（占 1 MB 分区 84%） |

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

背光先亮，屏上出现频谱 + 瀑布，顶栏显示频段与模式；按 ←/→ 走信道，
B 切 2.4G/5G，M 切 LIVE/SWEEP，R 转屏，← + → 同时按静音。
