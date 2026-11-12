# ESP32-C5 + ST7789 本地频谱面板固件

不接电脑就能用的手持频谱仪：ESP32-C5-DevKitC-1 直接驱动一块 3.2 英寸
240×320 SPI 屏（ST7789 / GMT032-8P-SPI）、5 个按键和一个蜂鸣器，
实时显示 2.4 GHz / 5 GHz 的频谱、瀑布和信道活动度。

完整说明、接线表和按键表见仓库根目录的 `README.md`。

## 烧写

> ⚠️ **整片烧录会清空设置。** `esp32c5-lcd-full.bin` 是从 `0x0` 起的连续镜像，
> 中间要用 `0xFF` 填满，而 NVS 就在 `0x9000–0xF000`，所以静音、频段、模式、
> 四套 dB 量程都会丢。日常更新请用下面的分段命令。

日常更新（保留设置，Flash 2 MB / DIO / 80 MHz）：

```bash
python -m esptool --chip esp32c5 -p COM6 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x2000 esp32c5-lcd-bootloader.bin \
  0x8000 esp32c5-lcd-partition-table.bin \
  0x10000 esp32c5-lcd-app.bin
```

首次烧写 / 救砖（会清设置）：

```bash
python -m esptool --chip esp32c5 -p COM6 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x0 esp32c5-lcd-full.bin
```

## 镜像说明

| 文件 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` | `0x0` | 943536 | 正式版合并镜像，SPI **8 MHz** |
| `esp32c5-lcd-app.bin` | `0x10000` | 878000 | 正式版应用 |
| `esp32c5-lcd-full-4mhz.bin` | `0x0` | 943536 | **4 MHz 回退镜像**，代码相同，长杜邦线上 8 MHz 出问题时用这个 |
| `esp32c5-lcd-app-4mhz.bin` | `0x10000` | 878000 | 4 MHz 回退应用 |
| `esp32c5-lcd-full-diag.bin` | `0x0` | 946960 | 调试版，UART0 115200 + UI 日志，与正式版同一份代码 |
| `esp32c5-lcd-app-diag.bin` | `0x10000` | 881424 | 调试版应用 |
| `esp32c5-lcd-pinprobe.bin` | `0x10000` | 136736 | 引脚探针，只烧应用：碰一下排针就用蜂鸣器报出 GPIO 号 |

## 这一版的要点

- 顶栏、频谱、瀑布全部改为**单窗口连续写**，顶栏用整张 2-bit 位图原子替换，
  消掉了原来一块块小窗口重写造成的闪烁。
- SPI 提到 **8 MHz + DMA**，一帧写屏时间从约 270 ms 降到约 135 ms。
- SWEEP 大幅提速：5 GHz 全段 38 步约 **0.9 秒**（实测 24 ms/步，原来约 3.4 秒）。
  关键是换频点的 DC 重校准从"每步都做"改成"每 8 步一次"，采集节流降到 5 ms，
  并把频谱平均从 `powf`/`log10f` 改成 dB 域整数运算、排序改用计数排序。
- 静音（← + → 同时按）与解除静音都会立即写入 NVS，重启后保持。
- 纵轴 dB 量程**没有自动量程**：2.4G / 5G × LIVE / SWEEP 四套独立上下限，
  写进 NVS，重启保留，只能用 B+←/→ 与 M+←/→ 组合键调整。
- SWEEP 频谱上新增当前信道的 **20 MHz 选框**，与底栏游标对齐。
- 5 GHz 里没有信道号的频段（RADAR / SAT / ISM）也画同样的活动度 / 检出率
  两条，标签写成用途英文名：整词放得下写整词（`SAT`），放不下退回首字母
  （`RADAR` → `R`），宽度不足 7 px 的段（5730–5735 ISM）只保留条。
