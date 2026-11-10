# ESP32-C5 LCD 固件

为 ESP32-C5-DevKitC-1 + 8 针 3.2" 240×320 SPI 屏（GMT032-8P-SPI，**ST7789** 控制器）
+ 5 键 + 有源蜂鸣器编译的 `esp-sdr` 固件（`CONFIG_ESP_SDR_LOCAL_UI=y`）。

面板原生 240×320，固件默认横屏使用（MADCTL `0x60`，坐标空间 320×240），
按 R 可切回竖屏 240×320。主界面为深色：顶栏（频段 / 模式 / 电平）、频谱 +
峰值保持、瀑布、底部信道条。

构建时间：2026-11-10，ESP-IDF v6.1，target `esp32c5`。

## 两套固件，先选一套

| 镜像 | host 串口 | UI 日志 | 用在哪 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` / `esp32c5-lcd-app.bin` | UART0 **2 Mbaud** | 关 | 正式版，可接上位机 burst 协议 |
| `esp32c5-lcd-full-diag.bin` / `esp32c5-lcd-app-diag.bin` | UART0 **115200** | 开 | 调试版，CH340 转接板上能看 `UI tick ...` 电平日志 |

两者只差 `CONFIG_ESP_SDR_UART_BAUD`。`ui.c` 里的 `UI_TRACE` 会跟着这个宏自动开关：115200 时往 UART0 打日志，
2 Mbaud 时静默，否则 UI 文本会混进 host 协议流。USB-Serial-JTAG 口不受影响。

`*-diag.bin` 是 13:25 的调试期快照，**不含** 14:13 新增的 20 MHz 选中选框
（见下）；需要带选框的诊断版请按下面的诊断构建步骤重编一次。

## 文件

| 文件 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `esp32c5-lcd-full.bin` | `0x0` | 944736 | **正式版**合并镜像，一次烧完 |
| `esp32c5-lcd-app.bin` | `0x10000` | 879200 | 正式版应用（占 1 MB 分区 84%） |
| `esp32c5-lcd-full-diag.bin` | `0x0` | 945792 | 诊断版合并镜像（115200 + UI 日志） |
| `esp32c5-lcd-app-diag.bin` | `0x10000` | 877648 | 诊断版应用 |
| `esp32c5-lcd-pinprobe.bin` | `0x10000` | 136736 | 引脚探针，只烧应用，见下 |
| `esp32c5-lcd-bootloader.bin` | `0x2000` | 22160 | 二级引导 |
| `esp32c5-lcd-partition-table.bin` | `0x8000` | 3072 | 分区表 |
| `esp32c5-lcd-full-ili9341.bin` | `0x0` | 941872 | 早期 ILI9341 试验镜像，仅留档，勿用 |

Flash 2 MB，DIO，80 MHz。`full` 镜像由 `merge_full.py` 把三段按偏移拼成单文件。

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

## 从源码构建

正式版（默认 host 2 Mbaud）：

```bash
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.esp32c5-lcd"
bash build.sh set-target esp32c5
bash build.sh build
cp build/esp_sdr.bin firmware/esp32c5-lcd-app.bin
python merge_full.py
```

诊断版：在 `SDKCONFIG_DEFAULTS` 末尾再加一个
`;sdkconfig.defaults.esp32c5-lcd-diag`，重新 `set-target` + `build`，产物另存为
`*-diag.bin`。`build.sh` / `_idfwrap.py` 只是剥掉本机 shell 注入的 `MSYSTEM`，
否则 IDF 6.x 拒绝启动。

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
GPIO13/14 是 USB D-/D+，GPIO11/12 是 UART0 控制台，**不要**拿来接屏或按键。

引脚定义在 `main/local_ui/board_c5_lcd.h`，改线只需改这一处。

## 背光电流（务必看）

GMT032 模块的背光是 6 颗 LED，工作电流约 **90–95 mA**，远超单个 GPIO 的
承受能力（≤40 mA，推荐 20 mA）。**BL 不要直连 GPIO23**，二选一：

- 应急：BL 直接接 3V3，常亮，切断 GPIO23 那根线；
- 正规：S8050 低边开关，GPIO23 → 1 kΩ → 基极，发射极 GND，集电极接 BL。

## 驱动上踩过的坑（改代码前先看）

- **SPI 4 MHz + DMA**：`LCD_SPI_HZ=4000000`、`SPI_DMA_CH_AUTO` 必须成对。这组
  15 cm 杜邦线上 8 MHz 无 DMA 会丢字节（纯白 / 一根竖线 / GRAM 雪花随机出现）。
  只降速不换 DMA 也可能不起图。
- **CS 手动拉**：`spics_io_num=-1`，命令 / 参数 / 像素在同一 CS 窗口内发完，
  前后各留 2 µs 建立时间。
- **颜色取反**：这块玻璃对 `INVON/INVOFF` 的表现和预期相反，靠命令调不出深色
  背景。驱动保留厂商的 `0x21`，但每个像素在发送前用 `LCD_PIX(v) = v ^ 0xffff`
  软件取反，两次反转抵消。
- **`LCD_SELFTEST=0`**：出厂自检（纯色 + 棋盘）已关，上电直接进主界面。需要排查
  接线时把它改成 1 重新编译。
- **瀑布 64 行环形缓冲**：120 行会吃光内部 RAM，`xTaskCreate` 直接返回 -1，
  表现为按键切换模式后红蓝屏重启。
- **频谱是 2-bit 位图，只有 4 个颜色**：为消除曲线闪烁，`render_spectrum()`
  把整块频谱区合成一张 `mask` 再由 `lcd_draw_bitmap2()` 一次刷出。调色板只有
  背景 / 网格 / 曲线 / 峰值四色，所以设计稿里半透明的效果一律做不了。曾经
  传过索引 4 给 `mask_put()`（内部是 `value & 3`），被截成 0，把峰值带下的
  网格擦成纯黑；现在峰值带改用网格色填充，选框也只画边线。要还原半透明填充
  必须把 mask 扩到 4-bit（内存 11.2 → 22.4 KB）。
- **瀑布一次开窗、整块连续写**：原先每行都 `lcd_set_window()` + `lcd_pixels()`，
  102 次独立 CS 窗口，面板扫描会插在写行之间；LIVE 数据每帧全变时表现为一条
  从上往下走的接缝。现在 `lcd_set_window()` 只调一次，逐行用
  `lcd_pixels_cont()`、最后 `lcd_pixels_end()`，CS 全程保持低。同时把环形缓冲
  索引在帧首冻结（`base = wf_next`），并去掉瀑布绘制途中的 `maybe_acquire()`，
  避免一帧里上半屏旧数据、下半屏新数据。
- **帧周期 300 ms**：一帧约 134 KB / 4 MHz ≈ 270 ms，旧的 120 ms 周期让
  `render()` 一返回就开下一帧，屏幕永远没有静止的一刻。SPI 屏没有 VSYNC 和
  双缓冲，tearing 只能减轻不能根除；采集仍插在频谱阶段、帧首和帧尾，SWEEP
  的 80 ms 步速不变。

`main/local_ui/st7789.c` 顶部还留着两个接线排错开关：

| 宏 | 作用 |
|---|---|
| `LCD_SWAP_SCL_SDI` | 1 = 按 SCL→GPIO7、SDI→GPIO6 驱动 |
| `LCD_SWAP_DC_RST` | 1 = 按 RES→GPIO4、AO→GPIO5 驱动 |

## 操作

←/→ 走信道，B 切 2.4G / 5G，M 切 LIVE / SWEEP，R 转屏，← + → 同时按静音。
SWEEP 全段约 3.4 s（每步 77–95 ms）；渲染期间会插空采样按键和射频，
所以长按时仍可切模式。底栏 2.4G 按 1–13 等分格子，5G 按真实中心频率
（36–64 / 100–144 / 149–177）贴边铺满，非等分处留空档。

SWEEP（以及连接上位机）时频谱上会用琥珀色边线框出当前信道的 **20 MHz 范围**，
两条竖边 + 上下亮边，x 坐标由 `strip_x()` 计算，与底栏选中游标严格对齐。设计稿
是半透明青色填充 + 描边，固件受 4 色调色板限制只保留边线；LIVE 模式不画框，
改为中心虚线。5G 全段 725 MHz 时这个框只有约 9 px 宽，2.4G 全段 80 MHz 时约 80 px，
是真实频率比例，不是画错了。

## 引脚探针（查丝印、查按键接线）

`esp32c5-lcd-pinprobe.bin` 是不启动射频、不初始化屏幕的独立小工程
（`CONFIG_ESP_SDR_PIN_PROBE`）。把所有排针 GPIO 设成上拉输入，拿一根 GND 线
去碰任意针，蜂鸣器把这个针的 **GPIO 号**报出来。探测表已剔除
13/14（USB）和 11/12（console）。

只烧这一支，不碰 bootloader 和分区表：

```bash
python -m esptool --chip esp32c5 -p COM8 -b 460800 \
  write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m \
  0x10000 esp32c5-lcd-pinprobe.bin
```

退回正常固件：重新烧 `esp32c5-lcd-full.bin` 即可。

**报号规则**：400 ms 长音开头 → 十位（每声 160 ms）→ 停 600 ms → 个位，
末尾单独一声 700 ms 长音表示 0。开机先响 3 声短音表示活着；什么都没碰就
自己报号，说明那些脚上电时已被短到 GND。

| 听到 | 是 |
|---|---|
| 2 声、停、4 声 | GPIO24 |
| 6 声（中间不停） | GPIO6 |
| 1 声、停、一声长音 | GPIO10 |
| 一声长音 | GPIO0 |
