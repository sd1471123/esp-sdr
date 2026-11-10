# ESP32-C5 + ST7789 本地频谱面板固件

不接电脑就能用的手持频谱仪：ESP32-C5-DevKitC-1 直接驱动一块 3.2 英寸
240×320 SPI 屏（ST7789 / GMT032-8P-SPI）、5 个按键和一个蜂鸣器，
实时显示 2.4 GHz / 5 GHz 的频谱、瀑布和信道活动度。

完整说明、接线表和按键表见仓库根目录的 `README.md`。

## 烧写

整片烧写（推荐，Flash 2 MB / DIO / 80 MHz）：

```bash
python -m esptool --chip esp32c5 -p COM6 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x0 esp32c5-lcd-full.bin
```

只更新应用（保留 NVS 里的频段、模式、横竖屏、静音和四套 dB 量程）：

```bash
python -m esptool --chip esp32c5 -p COM6 -b 460800 \
  write-flash 0x10000 esp32c5-lcd-app.bin
```

## 镜像说明

| 文件 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` | `0x0` | 945264 | 正式版合并镜像，SPI **8 MHz** |
| `esp32c5-lcd-app.bin` | `0x10000` | 879728 | 正式版应用 |
| `esp32c5-lcd-full-4mhz.bin` | `0x0` | 945264 | **4 MHz 回退镜像**，代码相同，长杜邦线上 8 MHz 出问题时用这个 |
| `esp32c5-lcd-app-4mhz.bin` | `0x10000` | 879728 | 4 MHz 回退应用 |
| `esp32c5-lcd-full-diag.bin` | `0x0` | 946960 | 调试版，UART0 115200 + UI 日志，与正式版同一份代码 |
| `esp32c5-lcd-app-diag.bin` | `0x10000` | 881424 | 调试版应用 |
| `esp32c5-lcd-pinprobe.bin` | `0x10000` | 136736 | 引脚探针，只烧应用：碰一下排针就用蜂鸣器报出 GPIO 号 |

## 这一版的要点

- 顶栏、频谱、瀑布全部改为**单窗口连续写**，顶栏用整张 2-bit 位图原子替换，
  消掉了原来一块块小窗口重写造成的闪烁。
- SPI 提到 **8 MHz + DMA**，一帧写屏时间从约 270 ms 降到约 135 ms。
- SWEEP 扫描提速：采集节流从 80 ms 降到 30 ms，瀑布绘制中每 16 行再插一个
  采集时隙，一帧可完成 3～4 步，5 GHz 全段明显快于之前的约 3.4 秒。
- 纵轴 dB 量程**没有自动量程**：2.4G / 5G × LIVE / SWEEP 四套独立上下限，
  写进 NVS，重启保留，只能用 B+←/→ 与 M+←/→ 组合键调整。
- SWEEP 频谱上新增当前信道的 **20 MHz 选框**，与底栏游标对齐。
