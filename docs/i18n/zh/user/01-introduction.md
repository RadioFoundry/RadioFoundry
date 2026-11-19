<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# 简介

RadioFoundry 是面向低价数字电台的自由多模固件，采用 MIT 许可，支持 DMR、P25、NXDN 和 FM。它替换电台的原厂固件。
一个与机型无关的核心提供数字语音协议栈、用户界面、电台频率和主机工具。每款电台在其下增加一层很薄的板级支持。

本手册描述 RadioFoundry 在每款电台上的行为。各电台之间的差异（按键、功率级别、电池、电源开关、显示屏和缺少的硬件）
见[电台参考](23-radios.md)。哪款电台有哪项功能，见[功能矩阵](../feature-matrix.md)。截图为 160 x 120。
其他电台显示相同的屏幕，只是按各自的显示屏布局。

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="DMR 信道上的主屏幕">

## RadioFoundry 提供什么

- DMR 语音：组呼和个呼、通话者别名、基础加密、通话中的位置（ETSI GPS Info）。
- DMR 数据：文本消息、呼叫提示、电台检查、ARS 注册和 LRRP 位置应答。
- DMR 紧急：告警、带热麦的紧急呼叫，以及他人宣告紧急时的提醒。
- 每个区域内的 DMR 中继台漫游。
- FM：CTCSS 和 DCS 亚音、静噪、监听、12.5 和 25 kHz 带宽、VOX 和双音多频。
- VFO：手动调谐频率，支持 FM 或 DMR。具备相应硬件的电台还可接收航空波段 AM 和调频广播。
- 扫描一个区域或 VFO 频段，以及两个信道的双守候。
- APRS：FM 上的位置、状态和消息，DMR 上的位置，以及 KISS TNC。
- 热点模式：电台作为 MMDVM 调制解调器，供 MMDVMHost 和 Pi-Star 类主机使用，并带单频中继模式（MMDVM SFR）。
- USB 网络模式：电台作为 USB 网卡，并通过 DMR 路由 IP。在这条链路上，它提供状态页、rig 控制、KISS over TCP、
  TFTP 电台频率访问、SNMP 和 mDNS，还可作为调度软件的控制站。
- 在浏览器中用电台频率编辑器或用 qdmr 编程，以及由 RadioID.net 构建的 DMR ID 数据库。

## 如何使用本手册

| 目的 | 阅读 |
|---|---|
| 把 RadioFoundry 装到电台上 | [安装](02-installing.md)，然后[首次启动](03-first-boot.md) |
| 把电台频率从原厂固件迁移过来 | [从原厂固件迁移](18-moving-from-stock.md) |
| 载入信道、区域和联系人 | [为电台编程](04-programming.md) |
| 熟悉按键和屏幕 | [按键与导航](05-keys-and-navigation.md)、[主屏幕](06-home-screen.md) |
| 使用某项功能 | 从[信道、区域与 VFO](07-channels-zones-vfo.md)到[电池与供电](16-battery-and-power.md)的各章 |
| 查找某项设置 | [设置](17-settings.md) |
| 解决问题 | [故障排查](20-troubleshooting.md)、[常见问题](21-faq.md) |
| 查找你的电台的按键和参数 | [电台参考](23-radios.md) |

## AMBE 编解码器

DMR 语音需要 DVSI 专有的 AMBE+2 声码器。RadioFoundry 从不附带它。每款电台运行其原厂固件中的编解码器。
安装器保留电台中已有的编解码器，或从电脑上的原厂固件文件中取出并写入电台。详情见[安装](02-installing.md)。

没有编解码器时，电台可用于 FM、显示 DMR 呼叫并处理 DMR 数据，但不播放也不发送 DMR 语音。

## 本手册的约定

按键以其承担的功能命名。每款电台把这些功能分配到自己的按键上。每款电台的对照表见[按键角色](23-radios.md#key-roles)。

按键名称包括 **OK**、**返回**、**上**、**下**、**\***、**#**、**0** 到 **9**、**监听**、**通话目标**、**功率级别**、
**上一区域**、**下一区域** 和 **PTT**。电台上没有其中某个键时，可通过菜单完成该功能。

| 术语 | 含义 |
|---|---|
| 按 | 短按 |
| 长按 | 按住按键至少 0.6 秒。这是长按时间，可在[设置](17-settings.md#keys-key-8)中更改 |
| **设置 > 显示 > 亮度** | 菜单路径：打开设置，再打开显示，再打开亮度 |
| “已保存到信道” | 屏幕文字，按电台的显示引用 |
