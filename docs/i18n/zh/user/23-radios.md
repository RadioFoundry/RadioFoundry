<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# 电台参考

本手册的其余部分描述 RadioFoundry 在每款电台上的功能。本章列出各电台之间的差异：哪个实体按键承担哪项功能、功率级别、电池、电源开关、显示屏，
以及电台缺少什么。哪款电台具有某项功能，见[功能矩阵](../feature-matrix.md)。

<!-- generated: tools/boards.py docs radios; edit boards/*/board.toml [status] -->
| Radio | Also sold as | What works |
|---|---|---|
| [Baofeng DM-32UV](#baofeng-dm-32uv) |  | FM and DMR voice both ways, CTCSS/DCS, APRS both ways, GPS data, the hotspot and each split duplex half work on air. Open: FM deviation at 12.5 kHz and an outdoor GPS fix. |
| [Baofeng DM-1701](#baofeng-dm-1701) | Retevis RT84 | FM, DMR voice and data, text messages, alerts, emergency, scan, the hotspot, single frequency repeating, each split duplex half and the USB network services work on air. Open: standalone SFR and a two-radio split duplex pair. |
| [Baofeng DM-1801 Pro](#baofeng-dm-1801-pro) | Baofeng DM-1801 | FM with CTCSS/DCS, DMR voice with the radio's own codec, DMR data and text, the hotspot, single frequency repeating, the PPP network services and each split duplex half work on air. Open: VHF transmit and VOX. |
| [Radioddity DB25-D PRO](#radioddity-db25-d-pro) | Kydera CDR-300UV | FM and DMR voice both ways, the hotspot, the DMR VFO, the PPP network services, cable programming and settings storage work on the radio. The older FD6818 batches run the same image, untested on a radio. Open: the Retevis RT73. |
| [TYT MD-380](#tyt-md-380) | TYT MD-390, Retevis RT3 | FM with CTCSS both ways, DMR voice and SMS in simplex and through a repeater or hotspot on either slot, and codeplug and ID database programming work on a UHF radio. Open: DCS, the tone deviation and the untested VHF image. |
| [TYT MD-UV380](#tyt-md-uv380) | TYT MD-UV390, Retevis RT3S, TYT MD-UV390 Plus 10 W | Built on the UV380 family code, with the MD-UV380's keys, display, power tables and GPS. Run on radios by community testers. |
| [TYT MD-9600](#tyt-md-9600) | Retevis RT90 | One image serves every board revision. On a V1-V3 radio, FM and DMR on both bands, DMR voice and data, text messages, roaming, scan, dual watch, APRS, the hotspot, each split duplex half and the USB network services work on air, with every power level checked on a wattmeter on both bands. V4A/V5 boards are detected; on older boards the radio asks for the revision once. Open: V4 and V4A/V5 boards have not run on a radio, and SFR loses inbound voice frames. |
| [Retevis RT10](#retevis-rt10) |  | Boots from the boot ROM with the host link, stub entry, the watchdog and the crash log. FM receive with CTCSS/DCS decode, FM transmit, DMR transmit and receive with the radio's own codec, P25 (conventional, and trunked transmit) and NXDN at 4800 bit/s on stored channels, and APRS transmit work on the radio. |
<!-- end generated -->

[计划中的电台](#planned-radios)列出尚未发布的电台。

## 按键功能

本手册按按键承担的功能为其命名。每款电台把这些功能放在自己的按键上：

| 本手册中的名称 | 功能（短按 / 长按） | DM-1701 | MD-380 | MD-UV380 | DM-32UV | DB25-D PRO | DM-1801 Pro |
|---|---|---|---|---|---|---|---|
| **OK** | 打开或选择 / 保存、设为永久 | 绿色菜单键 | 绿色菜单键 | 绿色菜单键 | 绿色菜单 / OK 键 | Menu / OK 键，或按下旋钮 | 菜单键 |
| **返回** | 返回上一级 / 主屏幕 | 红色返回键 | 红色返回键 | 红色返回键 | 红色返回键 | B/E 键 | 返回键 |
| **上**、**下** | 移动焦点 / 重复 | 方向键 | 方向键 | 方向键 | 上、下键 | 无：转动旋钮 | 上、下键 |
| **\*** | 快速设置 / 锁定或解锁按键 | **\*** | **\*** | **\*** | **\*** | P1 | **\*** |
| **#** | 操作 / 通话目标选择器 | **#** | **#** | **#** | **#** | P2 | **#** |
| **0** 到 **9** | 数字键 | 数字键 | 数字键 | 数字键 | 数字键 | 无 | 数字键 |
| **监听** | FM：按住时打开静噪。DMR：数字监听开 / 关 | SK1 | SK1 | SK1 | SK1 | P3 | SK1 |
| **通话目标** | 通话目标选择器 | 长按 **#** | 长按 **#** | 长按 **#** | 长按 **#** | 长按 **P2** | 长按 **#** |
| **A/B** | 双守候：另一个信道 / 双守候开或关 | SK2 | SK2 | SK2 | SK2 | TONE/T.SEL | A/B 键、SK2 |
| **功率级别** | 下一个功率级别 / 紧急报警（按住 1.5 秒） | SK3 | 无 | 无 | 无 | 无 | 无 |
| **上一区域**、**下一区域** | 在主屏幕上切换区域 | P1、P2 | 无 | 无 | P1、P2 | 无 | 左、右 |
| **VFO** | 在信道和 VFO 之间切换 / 把信道复制到 VFO | 无 | 无 | 无 | 无 | 无 | VFO/MR 键 |
| **音量** | 打开音量编辑器。仅限没有音量旋钮的电台 | 无：用旋钮 | 无：用旋钮 | 无：用旋钮 | 无：用旋钮 | VOL/SQL | 无：用旋钮 |
| **PTT** | 发射 | PTT | PTT | PTT | PTT | 手咪上的 PTT | PTT |
| 信道旋钮 | 每一格一步 | 信道旋钮 | 信道开关（16 档） | 信道旋钮 | 信道旋钮 | 前面板旋钮 | 无：上和下 |

这些是默认设置。在**设置 > 按键**中，你可以为菜单不需要的每个按键分配其他功能（[设置](17-settings.md#keys-key-8)）：

| 电台 | 可设置的按键 |
|---|---|
| DM-1701 | SK1、SK2、SK3、P1、P2 |
| MD-380 | SK1、SK2 |
| MD-UV380 | SK1、SK2 |
| DM-32UV | SK1、SK2，以及主屏幕上的 P1、P2 |
| DB25-D PRO | P3、TONE/T.SEL、手咪的 P4 和 P5 |
| DM-1801 Pro | SK1、SK2、SOS、A/B、VFO/MR |

**OK**、**返回**、方向键、数字键、**\***、**#**、**PTT** 和音量保留其功能。在 DM-32UV 上，方向键印为 P1 和 P2：你为它们设置的功能只在主屏幕上有效，
在菜单中它们仍是左和右。在 DB25-D PRO 上，固定按键包括 P1、P2 以及手咪的 P6 和 P7，它们在那里是 **\*** 和 **#**。

屏幕底部的提示使用这些名称。知道某个按键自己名称的电台会改为显示该名称（MD-9600 显示“P1 Quick”，而不是“\* Quick”）。没有数字键的电台不显示需要数字键的提示。

在没有某项功能按键的电台上，请使用菜单：

| 功能 | 没有该按键时 |
|---|---|
| 功率级别 | **\***、**1**（快速设置，功率） |
| 更改区域 | **OK**、**4**（区域），然后选择一个区域 |
| 通话目标 | **\***、**8** |
| 监听 | **\***、**5** 打开它，直到你关闭 |
| 紧急报警 | 在**设置 > 按键**中把它分配给侧键的长按 |

## 各电台上的双守候

[双守候](12-scan.md#dual-watch)在 A、B 两个信道上收听。每款受支持的电台都只有一个接收机，因此双守候在 A 和 B 之间交替。
电台在一个信道上收听时，可能错过另一个信道上的呼叫。

| 电台 | 开启 | 切换 A / B |
|---|---|---|
| DM-1701 | **\***、**7**，或长按 **SK2** | **SK2**（短按），或 **#**、**1** |
| MD-380 | **\***、**7**，或长按 **SK2** | **SK2**（短按），或 **#**、**1** |
| MD-UV380 | **\***、**7**，或长按 **SK2** | **SK2**（短按），或 **#**、**1** |
| DM-32UV | **\***、**7**，或长按 **SK2** | **SK2**（短按），或**左** / **右** |
| DB25-D PRO | 长按 **TONE/T.SEL** | **TONE/T.SEL**（短按） |
| DM-1801 Pro | 长按 **A/B** 键、**\***、**7**，或长按 **SK2** | **A/B** 键或 **SK2**（短按），或**左** / **右** |

双守候关闭时，短按切换键会显示“请先开双守候”。在 DM-1801 Pro 上，区域在长按**左** / **右**上，或在区域菜单中。DB25-D PRO 的第二颗 BK4819 芯片是接收链路的本振，
不是第二个接收机。

A 和 B 可以是任何区域的任何信道，或 VFO（[核心如何实现](../spec/radio-features.md#21-dual-watch)）。

## 音量条

在 RadioFoundry 能读取音量旋钮的电台上，转动旋钮会在屏幕底部显示音量：一个 16 级的条和级数。在使用底部一行的屏幕上（例如快速设置），音量条在顶部。
停止转动 3 秒后音量条消失，其下方的屏幕照常继续（呼叫、菜单）。转动旋钮也会打开背光。在没有旋钮的电台上，改由**音量**键打开音量屏幕。
音量控制作用在电台芯片之后的电台无法知道音量级别，因此不显示任何内容。

<img src="../ux/screens/160x128/volume_bar.png" width="200" alt="转动旋钮后主屏幕底部的音量条">

## 从电脑设置音量

`rfoundry_debug.py volume` 打印电台使用的音量及其来源。`rfoundry_debug.py volume 12` 设置为 16 级中的第 12 级（0 为静音），
`rfoundry_debug.py volume knob` 把控制交还给旋钮。经 DM-32UV 的串口线时，加上 `--port /dev/ttyUSB1` 或你线缆的端口。效果取决于电台：

| 电台 | 从电脑设置音量 |
|---|---|
| DM-1701 | 可以。电台读取旋钮并自行设置音频增益，因此电脑设置的音量取代旋钮的音量，直到你发送 `knob` 或重启电台。该音量不保存 |
| MD-380 | 不可以。旋钮直接作用于音频通路，电台应答“not supported” |
| MD-UV380 | 可以，与 DM-1701 相同（相同的音频通路） |
| DM-32UV | 不可以。旋钮直接作用于音频功放，电台应答“not supported” |
| DB25-D PRO | 可以。没有旋钮：音量就是电台自己的音量设置，因此电脑更改该设置，电台会保存它，就像你在电台上设置的一样。`knob` 把它恢复为默认值（第 8 级） |

## 编程

[电台频率编辑器](04-programming.md#the-codeplug-editor)可为本章中的每款电台编程。qdmr 需要端口上的电台以及其驱动知道的地址。

| 电台 | 线缆 | qdmr |
|---|---|---|
| DM-1701 | USB | 电台类型 8（另一种屏幕版本为 10）。qdmr 的 UV380 系列驱动 |
| MD-380 | USB | 电台类型 7 |
| DM-32UV | 编程线（无 USB） | 不能直接编程。RadioFoundry 把电台频率保存在电台数据闪存的 7 MB 处，因此 qdmr 驱动使用的地址落在原厂固件的区域中，RadioFoundry 拒绝写入那里。在 qdmr 中编辑电台频率，另存为 YAML，然后用 `dm32_migrate.py restore --yaml FILE` 写入（[在 DM-32UV 上从原厂固件迁移](#moving-from-the-original-firmware-on-the-dm-32uv)），它会加上偏移。电台报告电台类型 6（MD-UV380） |
| DB25-D PRO | 编程线，K1 插孔（[经编程线为 DB25-D PRO 编程](#programming-the-db25-d-pro-over-the-cable)） | 报告类型 6。电台频率位于与 DM-1701 相同的地址，因此 qdmr 的 MD-UV380 驱动可能可用 |
| DM-1801 Pro | 编程线 | 未列出 |

### 来自原厂固件的电台频率文件

打开这些文件时，电台频率编辑器会转换它们（[文件](04-programming.md#files)）。

| 电台 | 它能打开的文件 | 需先转换 |
|---|---|---|
| DM-1701、RT84 | 网页安装器的备份、电台数据闪存的转储 | 制造商的 MD-UV380 系列 CPS 文件（`.rdt`）：`dmrconf decode --manufacturer --radio dm1701 FILE.rdt FILE.yaml`，然后打开该 YAML |
| MD-380、MD-390、RT3 | 网页安装器的备份、电台数据闪存的转储、制造商的 CPS 文件（`.rdt`） | |
| DM-32UV | 在原厂固件上做的网页安装器备份，其中的 `data-memory.bin` 或 `original-channels.bin`（前 84 个信道） | 超过 84 个信道或离线时：`tools/backup_codeplug.py BACKUP.zip -o DIR`，然后打开 `DIR/<name>-ported.yaml`（[从网页安装器备份](#from-a-web-installer-backup)） |

## Baofeng DM-1701

也以 Retevis RT84 的名称销售。一款 136-174 和 400-480 MHz 的手台，带键盘、信道旋钮、带电源开关的音量旋钮、三个侧键和彩色屏幕。

### 在 DM-1701 上安装

按照[安装](02-installing.md)操作，使用以下值：

| 项目 | DM-1701 |
|---|---|
| 原厂固件文件 | Baofeng 的 `DM-1701(CSV)-V02.06.bin` 或 `DM-1701(noCSV)-V02.06.bin`（V02.06），或 Radioddity 的 V02.01、V02.02 或 V02.03 文件（`Firmware-DM-1701(CSV)-V02.01.bin`、`DM-1701(CSV)-V02.02.bin`、`DM-1701(CSV)-V02.03 Only for Radioddity.bin`、`DM-1701-V02.03 Only for Radioddity.bin`）。每个文件都提供引导程序密钥和语音编解码器。V02.06 的存档位于 github.com/daniel5600/BAOFENG-DM1701-Original-Firmware |
| MD-9600 文件 | 任何 TYT MD-9600 V16.45 或 V06.09 文件（`MD9600-CSV(V4)-V16.45.bin` 等）都提供语音编解码器，即 OpenGD77 系列安装留在电台上的那个编解码器。它不提供引导程序密钥，安装器也从不把它作为电台的固件写回。还需选择一个 DM-1701 文件，或先读取电台（**其他开源固件**，它会在电台中找到该编解码器） |
| `--stock-installed` | 电台运行的版本，例如 `"V02.06 CSV"` |
| 引导程序按键 | 同时按住 **PTT** 和 **SK1**，用音量旋钮开机 |
| 引导程序 USB 设备（Zadig） | “Digital Radio in USB mode”（0483:DF11），[驱动步骤](02-installing.md#windows-the-bootloader-driver) |
| 电路板名称 | `dm1701` |
| 回到原厂固件 | 原厂固件存档中的 Baofeng 固件升级工具。它写入应用程序以及原厂固件保存在 SPI 闪存中的资源区 |

### DM-1701 按键

| 按键 | 位置 | 功能 |
|---|---|---|
| 绿色菜单键 | 键盘 | **OK** |
| 红色返回键 | 键盘 | **返回** |
| 方向键 | 键盘 | **上**、**下** |
| P1、P2 | 前面板的两个功能键 | **上一区域**、**下一区域** |
| SK1 | 顶部侧键，在 **PTT** 上方 | **监听**。开机时与 **PTT** 一起按住，启动引导程序 |
| SK2 | **PTT** 下方的侧键 | **A/B**：短按在双守候中切换 A / B，长按打开或关闭双守候。通话目标选择器在长按 **#** 上 |
| SK3 | 最下方的侧键 | **功率级别**。短按：下一个功率级别。按住 1.5 秒：紧急报警。不含紧急报警的构建在长按时显示“语音提示：本版本无” |
| **PTT** | | **PTT** |
| 信道旋钮 | | 信道旋钮 |

侧键从上到下依次为 SK1、**PTT**、SK2 和 SK3。

### DM-1701 功率级别

五个级别：100 mW、500 mW、1 W、2.5 W 和 5 W。这些是标称值，使用电台的出厂校准。

| 更改方式 | 级别 |
|---|---|
| 电台频率中没有设置功率的信道 | 1 W |
| **\***、**1** | 100 mW、1 W、5 W |
| 主屏幕上的 SK3 | 全部五个，每按一次一级 |

### DM-1701 电池

2 节锂离子电池组。百分比跟随静置电压：每节 4.10 V 为满，3.20 V 为空。

| 级别 | 低于 |
|---|---|
| 低 | 6.2 V |
| 严重 | 6.0 V |

### DM-1701 电源开关

电源开关在音量旋钮上。顺时针转过“咔嗒”位置开机，逆时针转到“咔嗒”位置关机。开关切断电池检测线，因此电台把“关”视为低于 5.5 V 的读数。
它完成当前工作、保存，然后切断自己的电源。深度放电的电池读数相同，电台也会关机。

如果旋钮仍处于开启位置时电台无法切断自己的电源（自动关机），它会在 2 秒后重新启动，并显示“无法关闭 / 请用旋钮关机”。

### DM-1701 显示屏

160 x 128 彩色屏幕，顶部 8 行隐藏：可见 160 x 120。本手册中的截图就是这个尺寸。

### DM-1701 缺少的功能

- 出厂没有 GPS。LRRP 位置请求得到“无定位”。APRS 位置来自电脑或固定位置。如果你自己加装 GPS 模块，把其 NMEA 输出接到 PA10（STM32 第 69 脚），
  如有电源使能，接到 PA9。电台在启动时找到该模块，并只在**设置 > 位置 > GPS** 打开时为其供电。
- APRS 接收需要静噪打开：电台能听到强到足以打开静噪的分组，听不到更弱的。
- 发布镜像中没有 APRS iGate（把听到的分组转发到 APRS-IS）：闪存放不下。
- 没有振动马达：镜像中没有**设置 > 声音 > 振动**。
- 作为[独立 SFR](14-hotspot.md#standalone-sfr) 用户，电台同一时间只能听到呼叫的一份副本。**单频中继接收**的“第一份副本”和“双路接收”只在呼叫之间切换到另一份副本，
  从不混合副本。
- 在热点模式中，MMDVM SFR 在原呼叫之后约 210 ms 重发整个呼叫，而不是 90 ms。

## TYT MD-380

包括 TYT MD-380、MD-390 和 Retevis RT3。要连同原厂固件中的电台频率一起安装，见[在 MD-380 上从原厂固件迁移](#moving-from-the-original-firmware-on-the-md-380)。

> **检查你的频率。**有些 MD-380 的收发频率偏高 2 到 2.5 kHz，足以使 MMDVM 热点始终无法解码它们。用外部参考（频率计、综测仪或热点的 MMDVMCal BER 测试）
> 检查你的电台。然后用**设置 > 高级 > 频率微调**校正：以相反符号输入误差（在 448.7 MHz 偏高 2 kHz：-4.5 ppm）。经过该校正，这样的电台在热点上解码的 BER 低于 0.1%。

### 在 MD-380 上安装

按照[安装](02-installing.md)操作，使用以下值。

| 项目 | MD-380 |
|---|---|
| 原厂固件文件 | 制造商的 `MD-380-D2.32(AD).bin`（D002.032），或 TYT 的 `TYT-72310-11-MD380-D35.14.bin`（D35.14）或 `TYT-72310-11-MD390-S35.14.bin`（S35.14）。它提供引导程序密钥和语音编解码器。md380.org/firmware/orig/ 上的存档有每个版本 |
| 引导程序按键 | 按住 **PTT** 和其正上方的侧键，然后开机 |
| 镜像 | 每个频段一个：UHF 和 VHF。rfoundry 接受 `--option BAND=vhf` |
| 引导程序 USB 设备（Zadig） | “Digital Radio in USB mode”（0483:DF11），[驱动步骤](02-installing.md#windows-the-bootloader-driver) |
| 电路板名称 | `md380` |

### 在 MD-380 上从原厂固件迁移

[从原厂固件迁移](18-moving-from-stock.md)描述了各步骤。MD-380 的值：

| 项目 | MD-380 |
|---|---|
| 迁移程序 | `tools/md380_migrate.py` |
| qdmr | 0.12 或更高版本 |
| Python 软件包 | `pip install pyusb pyserial`，以及 libusb：macOS `brew install libusb`，Linux `apt install libusb-1.0-0` |
| 镜像 | `rfoundry_md380_codec.bin`（含编解码器），或 `rfoundry_md380.bin` 加 `--stock "MD-380-D2.32(AD).bin"`，后者会合并编解码器。使用适合你电台频段的镜像 |
| 原厂固件文件 | `MD-380-D2.32(AD).bin`（D2.32）提供引导程序密钥和语音编解码器。D2.30 只提供密钥。密钥文件（`--key`）也可以。TYT 的下载页面（tyt888.com/download.html）以 Windows 程序的形式提供更新，`.bin` 在其中；md380.org/firmware/orig/ 以普通文件提供每个版本。网页安装器也直接接受 TYT 的下载 zip |
| 更新模式按键 | 关闭电台。按住 **PTT** 和上方侧键，然后开机。屏幕保持黑屏 |
| 用于 `--from` 的电台频率文件 | TYT CPS 的 `.rdt`（262,709 字节）、dmrconf 的 `.dfu`、qdmr 的 `.yaml` |
| 工作目录 | `~/RadioFoundry-MD380-migration/<date-time>/` |
| 回到原厂 | `python3 md380_migrate.py flash --image "MD-380-D2.32(AD).bin"`。然后用 TYT CPS 或 qdmr（`dmrconf write --radio md390`）写入你的 `backup-stock.rdt` 或 `backup-stock.dfu` |

```sh
python3 md380_migrate.py all --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin" --dry-run
```

要自己构建镜像（[构建](../dev/building.md)），UHF 运行 `make BOARD=md380`（`build/md380/rfoundry_md380.bin`），VHF 运行
`make BOARD=md380 BAND=vhf`（`build/md380-vhf/rfoundry_md380.bin`）。`codecs/` 中有 `MD-380-D2.32(AD).bin` 时，构建还会写出 `rfoundry_md380_codec.bin`。

- 原厂固件最多可容纳 250 个区域和 250 个接收组列表。
- 回到原厂后，如果屏幕保持黑屏且 LED 一直闪烁，说明电台仍在引导程序中。关机再开机。如果这没有作用，取下电池再装回。程序看到这种情况时会告诉你。
- Windows：要安装，用 Zadig 把引导程序（0483:DF11）绑定到 WinUSB（[步骤](02-installing.md#windows-the-bootloader-driver)）。这会替换 TYT CPS
  对同一 USB ID 使用的驱动，因此请先运行备份步骤。
- Linux：如果访问被拒绝，程序会打印 0483:DF11 的 udev 规则。

### MD-380 按键

| 按键 | 功能 |
|---|---|
| 绿色菜单键 | **OK** |
| 红色返回键 | **返回** |
| 方向键 | **上**、**下** |
| SK1 | **监听** |
| SK2 | **A/B** |
| **PTT** | **PTT** |
| 信道开关 | 信道旋钮。一个 16 档开关，不是无限旋钮：RadioFoundry 把每次档位变化转换为步进 |

MD-380 没有 **P1**、**P2** 或 **SK3**，因此没有**功率级别**键、区域键和紧急报警键。用 **OK**、**4** 更改区域，用 **\***、**1** 更改功率。
开机时按住 **PTT** 和其正上方的侧键，引导程序启动。

### MD-380 频段与功率级别

每台电台一个频段：UHF（400-480 MHz）或 VHF（136-174 MHz）。每个频段一个镜像，镜像拒绝另一个频段。

五个功率级别：100 mW、500 mW、1 W、2.5 W 和 5 W。1 W 和 5 W 来自出厂校准，电台由它们设置其他级别。电台频率中没有设置功率的信道使用 1 W。
**\***、**1** 依次切换 100 mW、1 W 和 5 W。没有有效的出厂校准时，电台拒绝发射。

### MD-380 电池

2 节锂离子电池组，百分比曲线和阈值与 DM-1701 相同：低于 6.2 V 为低，低于 6.0 V 为严重。

### MD-380 电源开关

电源开关在音量旋钮上。它直接切换电源，因此 RadioFoundry 看不到旋钮关闭。RadioFoundry 自行关机时（自动关机），它释放其电源锁存。

### MD-380 显示屏

160 x 128 彩色屏幕，所有行都可见。

### MD-380 缺少的功能

- 含 `CONFIG_SUBTONE=n` 的构建（[构建开关](../build-config.md)）在任何载波上都会打开带亚音的信道，并且不发送亚音。
- FM 上没有结束提示音、1750 Hz 突发音、双音多频（DTMF）或 APRS：发射音通路尚未编写。经 DMR 的 APRS 位置可用。
- 没有 APRS 接收。
- 没有单频中继（SFR）：电台的 DMR 芯片（HR_C5000）无法发送 SFR 所需的内容。没有分离双工中继：尚未为该电台编写。热点模式已包含。
- 音频互联（USRP，[USB 网络](15-usb-network.md#linking-audio-allstarlink-and-dvswitch)）只传输 DMR：来自处理器的 FM 发射通路尚未编写。
- 只有“G”型号有 GPS。电台在启动时侦听 GPS 模块 5 秒，如果什么也没听到，就隐藏 GPS 功能。听到后，只在**设置 > 位置 > GPS** 打开时为模块供电，
  该设置立即生效。
- 没有振动马达：镜像中没有**设置 > 声音 > 振动**。
- 一些早期的 VHF 电台使用 1 MB 而不是 16 MB 的存储芯片。RadioFoundry 与其他电台一样在其上保存设置和电台频率，但 DMR ID 数据库仅限于能放下的部分。

## TYT MD-UV380

包括 TYT MD-UV380、MD-UV390、Retevis RT3S 和 MD-UV390 Plus 10 W，带或不带 GPS。它采用 DM-1701 的电路板设计。镜像由 DM-1701 的代码构建，
使用本电台的按键、显示屏、功率表和 GPS。

### 在 MD-UV380 上安装

按照[安装](02-installing.md)操作，使用以下值。

| 项目 | MD-UV380 |
|---|---|
| 原厂固件文件 | 任何受支持的 TYT 或 Retevis 文件：18.11、18.16、19.06、19.26、19.74、219.60 或 219.74。它提供引导程序密钥和语音编解码器。所有文件中的编解码器相同，因此任一文件都可用于这些电台中的任一款 |
| 引导程序按键 | 按住 **PTT** 和其正上方的侧键（SK1），然后开机 |
| 镜像 | 两个：5 W（MD-UV380、MD-UV390 和 RT3S）和 10 W（MD-UV390 Plus）。rfoundry 接受 `--option POWER=10w`。**按电台或其包装盒上的标签选择。**两个版本互换了发射机的频段选择和背光引脚，电台无法区分它们，因此没有任何东西能发现镜像选错 |
| 引导程序 USB 设备（Zadig） | “Digital Radio in USB mode”（0483:DF11），[驱动步骤](02-installing.md#windows-the-bootloader-driver) |
| 电路板名称 | `mduv380` |

### MD-UV380 按键

| 按键 | 功能 |
|---|---|
| 绿色菜单键 | **OK** |
| 红色返回键 | **返回** |
| 方向键 | **上**、**下** |
| SK1（上方侧键） | **监听** |
| SK2（下方侧键） | **A/B** |
| **PTT** | **PTT** |
| 信道旋钮 | 信道旋钮 |

MD-UV380 没有 **P1**、**P2**、**SK3** 或顶键，因此没有**功率级别**键、区域键和紧急报警键。用 **OK**、**4** 更改区域，用 **\***、**1** 更改功率。

### MD-UV380 功率级别

九个级别：50 mW、250 mW、500 mW、750 mW、1 W、2 W、3 W、4 W 和 5 W。在 MD-UV390 Plus 10 W 镜像上，最高四个级别为 2、3、5 和 10 W。
电台频率中没有设置功率的信道使用 1 W。没有有效的出厂校准时，电台拒绝发射。

### MD-UV380 电池

2 节锂离子电池组，使用 DM-1701 的百分比曲线和阈值：低于 6.2 V 为低，低于 6.0 V 为严重。电压刻度是 MD-UV380 自己的。

### MD-UV380 电源开关

与 DM-1701 相同：开关在音量旋钮上，RadioFoundry 通过电池检测线看到它关闭。

### MD-UV380 显示屏

160 x 128 彩色屏幕，所有行都可见。

### MD-UV380 缺少的功能

- 没有振动马达：镜像中没有**设置 > 声音 > 振动**。
- 只有 G 型号有 GPS。电台在启动时为模块供电 5 秒，如果什么也没听到，就隐藏 GPS 功能并再次关闭模块。听到后，只在**设置 > 位置 > GPS** 打开时为模块供电，
  该设置立即生效。

## Baofeng DM-32UV

一款 136-174 和 400-480 MHz 的手台，带键盘、左右方向键、信道旋钮和彩色屏幕。广播接收覆盖 65 到 108 MHz（[航空频段与调频广播](07-channels-zones-vfo.md#air-band-and-fm-broadcast)）。
作为[独立 SFR](14-hotspot.md#standalone-sfr) 中继台，它在 90 ms 后重发呼叫，并发送其 CW ID。要连同原厂固件中的电台频率一起安装，
见[在 DM-32UV 上从原厂固件迁移](#moving-from-the-original-firmware-on-the-dm-32uv)。

### 在 DM-32UV 上安装

按照[安装](02-installing.md)操作，使用以下值。

| 项目 | DM-32UV |
|---|---|
| 原厂固件文件 | 制造商的 `DM32.01.01.049.bin` |
| 线缆 | USB 串口编程线。电台本身没有 USB |
| 更新模式 | 按住 **PTT** 和 **SK1**，然后开机。绿色 LED 点亮 |
| 线缆无法准备电台时 | **使用工厂更新模式**：上述更新模式，使用原厂固件文件提供编解码器。约需 2.5 分钟，电台会自行重启 |
| 电路板名称 | `dm32` |

### 在 DM-32UV 上从原厂固件迁移

[从原厂固件迁移](18-moving-from-stock.md)描述了各步骤。DM-32UV 的值：

| 项目 | DM-32UV |
|---|---|
| 迁移程序 | `tools/dm32_migrate.py` |
| qdmr | 0.15 或更高版本 |
| Python 软件包 | `pip install pyserial` |
| 镜像 | `rfoundry_dm32_codec.bin`，已合并编解码器。不含编解码器的镜像可配合 `--codec-from DM32.01.01.049.bin` 使用 |
| 线缆 | USB 串口编程线（CH340、FTDI 或 CP210x）。原厂固件、引导程序和 RadioFoundry 都使用它。插入一根线缆时，程序会找到它。否则，加上 `--port /dev/ttyUSB0`（Linux）、`--port /dev/cu.usbserial-XXXX`（macOS）或 `--port COM5`（Windows） |
| 更新模式按键 | 关闭电台。按住 **PTT** 和 **SK1**（**PTT** 正下方的侧键），然后开机。绿色 LED 点亮。电台先擦除其旧固件，最多需要 15 秒 |
| 额外步骤 3b | 数据闪存备份：程序把原厂固件的电台频率和校准区（800 KB，约 2 分钟）读入工作目录。`--flash-backup full` 保存全部 16 MB（约 30 分钟），`--flash-backup none` 跳过 |
| 用于 `--from` 的电台频率文件 | `.dfu` 文件（`dmrconf read --radio dm32uv`）或用 qdmr 保存的 `.yaml` 文件。不支持 Baofeng CPS 的文件：用 qdmr 读取电台并从那里保存电台频率。`import --from FILE` 在没有电台的情况下转换并检查文件 |
| 转换报告 | `conversion-report.txt` 列出转换前后的数量、会被缩短的名称、136-174 和 400-480 MHz 以外的信道，以及 RadioFoundry 不使用的电台频率部分。漫游、APRS 设置、加密密钥和原厂固件的联系人数据库也不会迁移 |
| ID 数据库 | 电台约可容纳 300,000 条记录。列表更长时，程序保留与你的 ID 最接近的记录 |
| 工作目录 | `~/RadioFoundry-DM32-migration/<date-time>/` |
| 回到原厂 | `python3 serial_loader.py --stock DM32.01.01.049.bin` |

```sh
python3 dm32_migrate.py all --image rfoundry_dm32_codec.bin --dry-run
```

RadioFoundry 从不写入引导程序，因此更新模式始终可用。它也从不写入数据闪存 7 MB 以下的部分，原厂电台频率、出厂校准和原厂联系人数据库都在那里。
回到原厂固件时，电台会带着你原来的电台频率回来。如果没有，用 qdmr 写入 `backup-stock.dfu`（`dmrconf write --radio dm32uv`）。
RadioFoundry 自己的数据保留在 7 MB 及以上，原厂固件以后可能会重用该空间。

要安装镜像而不触碰电台频率，使用 `serial_loader.py`：

```sh
python3 serial_loader.py --model dm32 rfoundry_dm32_codec.bin   # a raw image (a _BFUV32.bin file needs no --model)
python3 serial_loader.py --list-ports                            # the cables it sees
```

- 如果安装后电台无法启动，取下电池再装回。更新模式仍然可用。
- Windows：先安装线缆的驱动（CH340 或 FTDI）。

### 从网页安装器备份

从原厂固件安装结束时，网页安装器会自行带过你的电台频率。没有 qdmr 时，[电台频率编辑器](04-programming.md#the-codeplug-editor)和工具页面可以从数据闪存读取结果中转换原厂电台频率：
在原厂固件上做的网页安装器备份 `.zip`，或其中的 `data-memory.bin` 或 `original-channels.bin`。它们带过信道及其发射联系人、联系人、接收组列表、区域、你的 DMR ID 和开机文字。
功率、带宽、亚音、时隙和色码随每个信道一起带过。静噪、发射超时和扫描列表保持 RadioFoundry 的默认值，转换器只读取前 84 个信道。
qdmr 0.15 不读取原厂固件的发射联系人，因此要保留它们，请使用这种方式。

`tools/backup_codeplug.py BACKUP.zip -o DIR` 通过 qdmr 的 `dmrconf`（0.15 或更高版本）离线从同一备份中取出原厂电台频率。它读取每个信道，并补回单独使用 dmrconf 时丢失的发射联系人：

- `<name>-stock.yaml` 和 `.dfu`：qdmr 读取的原厂电台频率。
- `<name>-ported.yaml`、`.dfu` 和 `.bin`：移植到 RadioFoundry 的电台频率。在电台频率编辑器中打开该 YAML，或用 `cps_client.py write` 写入 `.bin`。
- `<name>-rfoundry.yaml`：备份中含有 RadioFoundry 自己的电台频率时，即该电台频率。

### DM-32UV 按键

| 按键 | 功能 |
|---|---|
| 绿色菜单 / OK 键 | **OK** |
| 红色返回键 | **返回** |
| 上、下 | **上**、**下** |
| P1、P2（四向键的左和右） | 在主屏幕上短按或长按：**上一区域**、**下一区域**，或**设置 > 按键**为它们设置的功能。在菜单中为左和右 |
| SK1（**PTT** 下方的侧键） | **监听**。开机时与 **PTT** 一起按住，启动引导程序 |
| SK2 | **A/B** |
| **PTT** | **PTT** |
| 信道旋钮 | 信道旋钮 |

默认情况下，DM-32UV 没有**功率级别**键和紧急报警键。在**设置 > 按键**中把这些功能分配给 P1 或 P2（紧急报警放在长按上），或用 **\***、**1** 更改功率。

### DM-32UV 功率级别

五个级别，来自电台的出厂校准。屏幕把它们命名为 L-、L、M、M+ 和 H。电台在 H 时的额定功率为 5 W（VHF）和 4 W（UHF）。电台频率中没有设置功率的信道使用 L。
**\***、**1** 依次切换 L-、L 和 H。

### DM-32UV 电池

百分比从 6.4 V（0%）到 8.2 V（100%）线性变化。没有电压阈值：只有百分比警告（**设置 > 省电 > 低电量提示**）适用，低电量从不停止发射。

### DM-32UV 电源开关

电源开关在旋钮上。电台看到它关闭（连续三次读数），保存，然后切断自己的电源。音量控制直接作用于音频功放。RadioFoundry 不读取它，也不能从电脑设置它。

### DM-32UV 显示屏

240 x 320 彩色屏幕。RadioFoundry 使用其完整分辨率，以平滑（抗锯齿）的粗体文字显示，比其他 240 x 320 电台大一号。

### DM-32UV 缺少的功能

- 在较新的射频部分上（诊断行以 `rf B` 结尾），没有航空频段 AM 接收：AM 信道以 FM 播放。
- 没有 USB。串口在编程线上：热点模式、调试链路和网络（PPP，[USB 网络](15-usb-network.md#network-over-the-programming-cable-ppp)）都在那里运行。
  USB 网络模式不可用。
- GPS 模块必须在启动后 5 秒内应答。之后，只在**设置 > 位置 > GPS** 打开时为其供电，该设置立即生效。
- APRS 收听期间，APRS 频率上的 FM 经另一条音频通路播放，听起来可能不同。
- 没有 VOX：读取麦克风电平的代码尚未为该电台编写。
- 音频互联（USRP）只传输 DMR：来自处理器的 FM 发射通路尚未编写。
- 发射时没有提示音：电台的提示音通路会把它们发到空中。发射结束时仍在播放的提示音，从那时起才能听到。DMR 通话许可音是一声短促提示音，
  在电台发射前播放，此时屏幕显示 ADMITTED（[DMR](09-dmr.md#the-talk-permit-tone)）。
- 作为独立 SFR 中继台时，它重发的语音比原始语音带有更多误码，但仍能播放。
- 以 dBm 表示的信号强度未经校准：强信号读数偏高。
- 没有振动提醒：RadioFoundry 不在该电台上驱动振动马达，镜像中也没有**设置 > 声音 > 振动**。

## Radioddity DB25-D PRO

一款 136-174 和 400-480 MHz 的车台，带前面板旋钮、几个前面板按键和手咪。只能用[电台编程工具](#installing-the-db25-d-pro)安装。

**哪种 DB25-D。**打开电台的 **Version** 屏幕。版本类似 `909E.DBB.EARSAB.020`。`909E.` 之后的字母表示硬件。

| 代码 | 硬件 | RadioFoundry |
|---|---|---|
| DBB | DB25-D PRO：两颗 BK4819（2024 年起） | 受支持 |
| D64 | FD6818 收发器，0x7813 本振（2021 到 2023 年） | 同一镜像 |
| D66 | FD6818 收发器和本振（2021 到 2023 年） | 同一镜像 |
| D6T | FD6818 收发器，AT1141 本振（2021 到 2023 年） | 同一镜像 |

一个镜像可在每个批次上运行：它在启动时读取电台具有哪些射频芯片。**工具 > 诊断**显示它们（`rf` 行）。如果你把它安装在 D64、D66 或 D6T 上，
请报告哪些可用、哪些不可用（[报告问题](20-troubleshooting.md#reporting-a-problem)）。

电台编程工具在写入前从电台读取该代码，在只有工厂更新模式应答时会向你询问。在早于 2025-01-21 构建的原厂固件上，备份可能失败。工具提供三个选择：
仍然尝试备份（不向电台的闪存写入任何内容；如果失败，关机再开机）、先更新原厂固件，或不备份直接安装。**备份**提供同样的尝试。
你可以把这样做出的备份随报告一起发给本项目，让下一个版本认识该固件。**备份**适用于每个硬件版本，包括 RadioFoundry 不能运行的版本：它只读取电台。

### 安装 DB25-D PRO

网站上的电台编程工具通过编程线（K1 插孔）安装和更新这款电台。没有 rfoundry 途径。

| 电台运行 | 电台编程工具做什么 |
|---|---|
| 原厂固件（909E.DBB.EARSAB.018 或 .020） | 通过原厂固件的诊断控制台把电台切换到更新模式：约 4 分钟，无需按键，也不向电台存储器写入任何内容。然后备份整个 2 MB 程序存储器（约一分钟），安装，并重启电台，与运行 RadioFoundry 的电台相同。版本为 020 或 2024-05-09 及以后的 018 构建之一时，语音编解码器来自备份。对于其他任何版本，备份仍然完整：工具会说明这一点，并要求提供制造商的 018 文件，其编解码器适用于每个版本 |
| 原厂固件的其他版本，或无法启动的电台 | 通过原厂加载程序的工厂更新模式安装：关闭电台，按住 P1 并按电源键。红色 LED 点亮，显示屏保持空白。这种方式不做备份。加载程序本身从不被写入 |
| RadioFoundry | 通过调试链路把控制交给 RAM 中的加载程序（无需按键，无需断电重启），只写入更改的扇区，然后重启电台。程序存储器的备份是可选的 |
| 不应答的 RadioFoundry | **开机时恢复**：第一阶段在每次开机时应答半秒，电台编程工具在此时加载加载程序 |

每次开机时（没有按住 P1）显示白色背光屏幕和常亮红色 LED 的电台，因其应用程序缺失或不完整而停留在工厂更新模式。它不需要按键：工具向加载程序提一个只读问题，
说明电台处于更新模式，然后安装。查明这一点不会写入任何内容。

线缆上没有任何应答时，故障排除提供**在工厂更新模式下重新安装**，恢复也以同样方式通过工厂更新模式写回备份。通过控制台或 RadioFoundry 恢复是正常途径。
两者都不应答时，工厂更新模式是退路。

电台编程工具的备份不包含电台 NAND 存储器中的信道和联系人。用制造商的编程软件或 `tools/cps_client.py` 备份它们
（[经编程线为 DB25-D PRO 编程](#programming-the-db25-d-pro-over-the-cable)）。

### DB25-D PRO 按键

RadioFoundry 不读取手咪的数字键，因此电台没有数字键。

| 按键 | 功能 |
|---|---|
| Menu / OK，或按下旋钮 | **OK** |
| B/E | **返回** |
| P1 | **\*** |
| P2 | **#** |
| P3 | **监听** |
| TONE/T.SEL | **A/B** |
| VOL/SQL | **音量** |
| 手咪上的 **PTT** | **PTT** |
| 前面板旋钮 | 信道旋钮，在列表中为**上** / **下** |
| 电源键 | 按下开机。按住 2 秒关机。按住 P1 的同时按下它启动引导程序（红色 LED 点亮，屏幕保持空白） |

电台在前面板插孔中检测到手咪后，手咪的 **PTT** 才起作用。电台没有手咪也能运行但不发射，之后插入的手咪从那时起可用。如果你在电台开机时拔下手咪，
电台会把它读作按住的 **PTT**：它会一直发射，直到[发射超时](08-fm.md#transmit-timeout)，或在发射超时为关时，直到你关机。拔下手咪之前请先关闭电台。

### DB25-D PRO 功率与供电

五个功率级别。屏幕把它们命名为 L--、L-、L、M 和 H。L 和 H（标称 20 W）来自出厂校准，电台由它们设置其他级别。电台频率中没有设置功率的信道使用 L。
在 RadioFoundry 能读取其功率校准之前，电台拒绝发射。

电台由 13.8 V 电源供电，没有电池。状态行不显示电池图标或百分比，工具中没有电池页面，**设置 > 省电**中没有省电或低电量提示。
RadioFoundry 不读取电源电压，因此没有任何相关警告。

### DB25-D PRO 显示屏

1.77" 彩色屏幕，160 x 128。

没有音量旋钮。按 **VOL/SQL**，然后转动前面板旋钮（或按**上**和**下**），把音量设为 0（静音）到 16。要关闭音量屏幕，按 **OK** 或**返回**，或等待 3 秒。
屏幕以大号数字显示级数，下方是 16 段的条。该级别也在**设置 > 声音 > 音量**中。RadioFoundry 在接收芯片的音频输出处设置并保留它。
以前有 10 级的构建保存的值会被沿用（第 5 级变为 8，第 10 级变为 16）。

### 经编程线为 DB25-D PRO 编程

通过侧面 K1 插孔中的编程线，经 USB 串口适配器（115200 波特）读写电台频率（信道、区域、联系人、电台名称和 DMR ID）。电台没有 USB 端口。
电台频率位于与 DM-1701 相同的地址，因此 `tools/cps_client.py` 无需偏移，qdmr 的 MD-UV380 驱动也可能可用（电台报告类型 6）。该工具需要 `pyserial`。
示例使用 `/dev/ttyUSB1`：

```
python3 tools/cps_client.py --port /dev/ttyUSB1 info                  # type 6, build date, flash size
python3 tools/cps_client.py --port /dev/ttyUSB1 read backup.bin       # about 20 s
python3 tools/cps_client.py --port /dev/ttyUSB1 write backup.bin      # then the radio restarts
python3 tools/cps_client.py --port /dev/ttyUSB1 verify backup.bin     # "match"
```

先做备份。没有存储任何内容的电台显示内置的单工 FM 信道，读取返回空白（0xFF）字节。要把测试信道放到电台上，读取电台频率，把它们加入文件，然后写回：

```
python3 tools/cps_client.py --port /dev/ttyUSB1 read cp.bin
python3 tools/cps_client.py test-channels cp.bin --rx 438.2500 --tg 91 --cc 1 --ts 1 --id 9991234 --call Q0TST
python3 tools/cps_client.py --port /dev/ttyUSB1 write cp.bin
python3 tools/cps_client.py --port /dev/ttyUSB1 verify cp.bin
```

`test-channels` 把信道 1 设为 FM 信道（12.5 kHz，单工，无亚音），信道 2 设为同一频率上的 DMR 信道（色码 1，时隙 1，通话目标为组 91），
区域 1 包含这两个信道。它替换这些信道、第一个联系人和第一个区域，文件中的其他内容保持不变。DMR 信道需要含 `CONFIG_DMR=y`（默认）的构建。
没有它时，电台只列出其 FM 信道。

`verify` 比较工具传输的区域。写入以一步替换整个 4 KB 扇区：断电或拔线会使每个扇区保持原样或与文件一致，绝不会一半一半。编程工具从不写入电台自己的设置或出厂校准。
电台的存储中含有另一个固件的数据时（诊断页面上的 `nst foreign`），电台拒绝写入。

### DB25-D PRO 缺少的功能

- GPS 模块必须在启动后 5 秒内应答。如果应答，电台就有 GPS（APRS 位置来源和位置设置）。否则，GPS 功能被隐藏。只在**设置 > 位置 > GPS** 打开时为模块供电。
- 没有分离双工热点角色。
- 没有 APRS 接收或 iGate：电台不向核心提供 FM 接收音频。
- 没有 VOX：读取麦克风电平的代码尚未为该电台编写。
- DMR 语音需要来自受支持的原厂固件文件的编解码器。
- RadioFoundry 把设置保存在电台存储器中原本未使用的部分。如果它在那里发现不是自己写入的数据，就不动这些数据，也不保存设置（诊断页面显示 `nst foreign`）。

## Baofeng DM-1801 Pro

一款 136-174 和 400-470 MHz 的手台，带键盘、方向键和 128 x 64 单色屏幕。广播接收覆盖 65 到 108 MHz（[航空频段与调频广播](07-channels-zones-vfo.md#air-band-and-fm-broadcast)）。

通过电台自己的更新模式安装，因此原厂加载程序保留，原厂固件也可以用同样方式写回。关闭电台，按住 **PTT** 和 **SK1** 开机，然后经编程线运行
`tools/serial_loader.py --model dm1801pro IMAGE`。线缆的插头会关闭内置扬声器和麦克风。要听电台或讲话，请拔下它。

**设置 > 通话与收听 > 麦克风增益**在这款电台上设置 FM 和 DMR 的麦克风电平。

`rfoundry flash IMAGE --board dm1801pro` 经线缆做同样的事，并自行选择进入方式：

| 电台运行 | rfoundry |
|---|---|
| 以 `CONFIG_STUB_ENTRY=y` 构建的 RadioFoundry | 从 RadioFoundry 进入加载程序（屏幕显示“PC 加载器已连接”），只写入更改的部分，然后重启电台。无需按键。电台中的编解码器保留 |
| 其他任何情况 | 要求你让电台进入更新模式（开机时按住 **PTT** 和 **SK1**）。更新模式会擦除编解码器，因此镜像必须带有编解码器（构建生成的 `rfoundry_dm1801pro_codec.bin`），或用 `--stock` 提供该电台的程序闪存读取结果（`stub/stubtool.py PORT --progmem FILE`） |

`--route stub` 或 `--route bootloader` 手动选择进入方式，`--dry-run` 显示计划而不写入。

在网页安装器中，线缆无法准备电台时，**使用工厂更新模式**通过同一更新模式（开机时按住 **PTT** 和 **SK1**）安装。这款电台没有原厂固件文件，
因此编解码器来自其程序存储器备份（`program-memory.bin`，或安装器在首次安装前保存的备份 .zip），或来自浏览器保留的编解码器。

### DM-1801 Pro 按键

| 按键 | 功能 |
|---|---|
| Menu | **OK** |
| Back | **返回** |
| 上、下 | **上**、**下** |
| 左、右 | 在编辑器中为左和右。长按：**上一区域**、**下一区域** |
| VFO/MR | 信道或 VFO。长按：把信道复制到 VFO |
| A/B | 短按：A / B（双守候）。长按：双守候开或关 |
| **\*** | **\*** |
| **#** | **#** |
| SK1 | **监听** |
| SK2 | **A/B** |
| SOS（顶部） | 长按：紧急报警，在含紧急报警的构建中 |
| **PTT** | **PTT** |

### DM-1801 Pro 功率与显示屏

五个功率级别。屏幕把它们命名为 L--、L-、L、M 和 H。L、M 和 H 是出厂校准的低、中、高，电台由 L 设置 L 以下的两个级别。电台频率中没有设置功率的信道使用 L。
在 RadioFoundry 能读取其出厂校准之前，电台拒绝发射。

屏幕为单色，背光只有开和关。收到消息、呼叫提示和紧急报警时电台振动（**设置 > 声音 > 振动**）。

### DM-1801 Pro 缺少的功能

- 没有 GPS：电台没有 GPS 接收机。

## Retevis RT10

一款 902-928 MHz 的手台，带 16 档信道旋钮、两个侧键，没有显示屏。

### 在 RT10 上安装

电台没有自己的引导程序。进入方式是每次开机时编程线上芯片的启动窗口。

| 项目 | RT10 |
|---|---|
| 线缆 | 配件插孔上的电台编程线，带 FTDI 芯片。一根在其他电台上可用的 CH340 线缆在 RT10 上没有得到应答 |
| 按键 | 无：在工具提示时开启电台 |
| 备份 | 安装会替换整个原厂映像。只有事先做的完整程序闪存备份才能把它恢复 |
| 电路板名称 | `rt10` |

### RT10 的信道旋钮

旋钮按其指向的位置选择信道：位置 1 到 16 是该区域的前 16 个信道，按区域中的顺序排列。电台开机时和区域更改时也是如此。
超出较短区域末尾的位置不改变任何内容。

### RT10 功率级别

两个级别，名为 **L** 和 **H**：电台自己的出厂校准，每 10 MHz 一档。没有有效的校准时，电台拒绝发射。

### RT10 的 LED 红绿快速闪烁时

线缆信号线卡在低电平。在该信号线释放之前，电台不发射：关机再开机。如果电台启动时你按住 **PTT** 超过 2 秒，LED 会显示同样的闪烁，直到你松开按键。

编程线本身可能把信号线拉低。这时只要插着该线缆就会闪烁，拔出后停止，并且电台收不到来自电脑的任何内容。请换一根线缆
（[安装](02-installing.md#if-something-goes-wrong)）。

### RT10 缺少的功能

- 经编程线的网络（PPP）及其服务。电脑和电台同时发送时，线缆的信号线会锁死，只有关闭电台才能解除，因此 RT10 构建中 PPP 默认关闭。
  自定义构建可以打开它，但需自担该风险。
- 热点模式。电脑上的热点软件也会随时发送，因此可能以同样方式锁死信号线。RT10 构建中热点默认关闭。自定义构建可以打开它，但需自担该风险。
- GPS。电台没有接收机，因此 RT10 构建中 DMR 位置报告和 LRRP 位置应答默认关闭。
- 经校准的电池刻度。
- 没有屏幕，因此没有 VFO、双守候或 DTMF。
- 没有 VOX、音频互联（USRP）或分离双工中继：尚未为该电台编写。

## TYT MD-9600

包括 TYT MD-9600 和 Retevis RT90，一款车台。一个镜像适用于每个电路板版本。只有在你能回到原厂固件时才安装它。

电台能自行检测 V4A/V5 电路板。它无法区分 V1-V3 电路板和 V4，因此在这些电路板上会询问一次：主屏幕显示“请设置: Board revision”，
在你设置**设置 > 高级 > Hardware > Board revision** 之前，电台不发射。在此期间可以接收。安装器能读取原厂固件的版本时会替你设置
（V06.xx 为 V1-V3，V16.xx 为 V4）。否则，它们会在安装后立即询问。如果不确定，选择 V1-V3。如果电台随后在你预期能听到的信道上什么也听不到，选择 V4。
版本的更改从下次启动起生效（“请重启电台”）。

### MD-9600 语音编解码器

DMR 语音需要电台自己的编解码器，代码库和发布镜像都不包含它。安装器从该电台的原厂固件文件中取得它，该电路板自己的 `codecs/` 文件夹也同样可用
（[codecs/README.md](../../codecs/README.md)）。安装器按 SHA-256 识别文件，因此任何文件名都可以。它接受以下文件：

| 电台 | 原厂固件文件 |
|---|---|
| MD-9600 V1-V3 电路板 | TYT `MD9600-CSV-V06.09.bin` 以及其他 06.09、06.24 和 06.03 文件；较旧的 `MD9600-REC-D03.28.bin`、`-D03.29.bin` 和 `-D04.03.bin` |
| MD-9600 V4 | TYT `MD9600-CSV(V4)-V16.45.bin` 以及 16.24 和 16.45 文件 |
| MD-9600 V5 和 Retevis RT90 | 电台出厂所带的文件：TYT `MD9600-CSV(2571V5)-V26.45.bin` 和其他 26.45 文件、`MD9600-REC(2571V4)-D26.57.bin`，或 Retevis `RT90-CSV(2571V5)-V26.41.bin`、`RT90-REC(2571V5)-D26.38.bin`、`RT90-CSV-GPS(2571V5)-P26.41.bin` 和 `RT90-GPS-REC(2571V5)-S26.42.bin`，或 TYT `MD9600-AES-GPS-REC(2571V5)-S126.61.bin` |

同一版本的 CSV、CSV-GPS、REC 和 GPS-REC 变体都可用。

### MD-9600 按键

| 按键 | 功能 |
|---|---|
| Enter | **OK** |
| Back | **返回** |
| 上、下 | **上**、**下** |
| P1 | **Quick** |
| P2 | **Context** |
| P3 | **监听** |
| P4 | **通话目标** |
| 橙色键 | 用户键 3 |
| 信道旋钮 | 信道旋钮 |
| **PTT** | **PTT**（在手咪上） |

手咪的键盘提供数字、**\***（**Quick**）和 **#**（**Context**）。其 A/B 键在信道和 VFO 之间切换，长按它把信道复制到 VFO。

### MD-9600 频段与功率级别

每台电台都有两个频段：VHF（136-174 MHz）和 UHF（400-480 MHz）。五个功率级别，接假负载测量：UHF 100 mW、1 W、5 W、25 W 和 45 W；
VHF 250 mW、1 W、5 W、25 W 和 50 W。最高级别因频段而异，屏幕显示信道所在频段的级别。电台由车辆的 13.8 V 电源供电，因此显示电源电压而不是电池电量，
也从不发出低电量警告。

### MD-9600 显示屏

128 x 64 单色屏幕。

### MD-9600 缺少的功能

- 没有航空频段或广播接收机，也没有振动马达。

## 计划中的电台

这些电台尚未发布。正在进行的移植在其自己的分支上。[电台调查](../spec/radios.md)列出每款电台的已知信息及其涵盖的换牌型号。

<!-- generated: tools/boards.py docs planned; edit the Port cells of docs/spec/radios.md -->
| Radio | Status |
|---|---|
| Anytone AT-D868UV / AT-D878UV / AT-D878UVII Plus, BTECH DMR-6X2, Alinco DJ-MD5, Radioddity GD-AT10G | In progress ([#291](https://github.com/radiofoundry/radiofoundry/pull/291)). A new platform (GD32F30x) and a new DMR chip (SCT3258, which runs the vocoder itself), so it starts the Anytone family. |
| Radioddity GD-77, TYT MD-760, GD-77S, Baofeng DM-1801 v1 / DM-860, RD-5R | In progress ([#111](https://github.com/radiofoundry/radiofoundry/pull/111)). A new platform: a different MCU (NXP MK22FN512), the same HR_C6000 and AT1846S, a 128x64 mono LCD. A DM-1801 bought new today is not v1, and the look-alike DR-1801 is unrelated. |
| Radioddity GD-88 | In progress ([#132](https://github.com/radiofoundry/radiofoundry/pull/132)). Dual receiver on the HR_C7000 platform; it can reuse much of the DM-32UV port. |
| Baofeng DM-36 | In progress ([#607](https://github.com/radiofoundry/radiofoundry/pull/607)). Built on the DM-32UV's platform: FM, the display, keys and programming run; DMR voice on air and power levels open. |
| Anytone AT-D578UV (mobile) | In progress ([#291](https://github.com/radiofoundry/radiofoundry/pull/291)). The 878's chips in a mobile, with a mid-run MCU change: two platforms. |
| Radioddity DB40-D, Kydera CDR-700UV (mobile) | Planned. Hardware needed. Two HR_C7000-class processors (A and B), like the GD-88, so it follows the GD-88 port. |
| TYT MD-2017 / Retevis RT82 | Planned. The DM-1701's STM32 and HR_C6000 pair with two receivers, so it can reuse the STM32F4 platform. The hardware can receive two channels at once. |
| BTECH DMR-6X2 PRO | Planned. Built on the AT-D878UVII Plus, so it can follow the Anytone port. |
| BridgeCom Maverick | Planned. Sold as an upgraded AT-D878UV, so it likely follows the Anytone port. |
<!-- end generated -->
