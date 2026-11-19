<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# RadioFoundry 用户手册

本手册介绍如何安装 RadioFoundry 并在电台上使用它。各章描述 RadioFoundry 在每款电台上的行为。[电台参考](23-radios.md)
列出各电台之间的差异：按键、功率级别、电池、电源开关和显示屏。每个发行版还附带本手册的 PDF 版，由这些文件按文件名顺序生成。

从网站 [radiofoundry.org](https://radiofoundry.org) 开始。**网页安装器**用于安装、更新、备份和恢复电台。
**电台频率编辑器**用于编辑信道、区域和联系人。两者都在基于 Chromium 的浏览器中运行。各章把命令行工具作为替代方式列出。

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="DMR 信道上的主屏幕">
<img src="../ux/screens/160x120i5/quick_dmr.png" width="200" alt="快速设置">
<img src="../ux/screens/160x120i5/hub.png" width="200" alt="hub">

## 目录

先读哪一章，见[如何使用本手册](01-introduction.md#how-to-use-this-manual)。

### 入门

1. [简介](01-introduction.md)：RadioFoundry 提供什么、AMBE 编解码器、约定。
2. [安装](02-installing.md)：用网页安装器安装和更新、备份、回退。
3. [首次启动](03-first-boot.md)：内置信道和最初的几项检查。
4. [为电台编程](04-programming.md)：电台频率编辑器、qdmr、文件和 RadioID.net 联系人。

### 使用电台

5. [按键与导航](05-keys-and-navigation.md)：五条按键规则、查找、临时更改、按键锁。
6. [主屏幕](06-home-screen.md)：接收和发射时每个区域显示什么。
7. [信道、区域与 VFO](07-channels-zones-vfo.md)：包括 P25 和 NXDN 信道。
8. [FM](08-fm.md)：静噪、监听、亚音、带宽、VOX、双音多频。
9. [DMR](09-dmr.md)：通话目标、回复、最近、联系人、加密、漫游、集群系统。
10. [消息、呼叫提示与电台检查](10-messages-and-signaling.md)。
11. [紧急](11-emergency.md)。
12. [扫描与双守候](12-scan.md)。
13. [APRS](13-aprs.md)：FM 和 DMR 上的位置、信标、与电脑配合的 KISS 和 iGate。
14. [热点模式](14-hotspot.md)：MMDVMHost、单频中继、两台电台组成双工中继。
15. [USB 网络模式](15-usb-network.md)：DMR 承载 IP，以及电台向电脑提供的服务。
16. [电池与供电](16-battery-and-power.md)。

### 参考

17. [设置](17-settings.md)：每一项设置及其默认值。
18. [从原厂固件迁移](18-moving-from-stock.md)：把电台频率从制造商固件迁移过来。
20. [故障排查](20-troubleshooting.md)：症状与解决方法，包括安装器错误。
21. [常见问题](21-faq.md)。
22. [术语表](22-glossary.md)。
23. [电台参考](23-radios.md)：每款电台的按键、功率级别、电池、电源开关、显示屏和安装参数。

## 快速参考

按键以其功能命名。你的电台上哪个键承担哪项功能，见[按键角色](23-radios.md#key-roles)。

| 操作 | 按键 |
|---|---|
| 切换信道 | 信道旋钮，或 **上** / **下** |
| 切换区域 | **上一区域** / **下一区域**，或 **OK**、**4**，然后选择区域 |
| 打开快速设置 | **\*** |
| 打开菜单（hub） | **OK** |
| 处理最近来电 | **#** |
| 打开最近 | **OK**、**3** |
| 与另一个通话组或 ID 通话 | 长按 **#** |
| 选择通话对象 | **通话目标**（DMR），或 **\***、**8** |
| 监听 | 长按 **监听** |
| 开 / 关双守候 | 长按 **A/B**，或 **\***、**7** |
| 开 / 关扫描 | **OK**、**6**、**1** |
| 撤销临时更改 | 在主屏幕按 **返回** |
| 把更改保存到信道 | 长按 **OK** |
| 返回主屏幕 | 长按 **返回** |
| 锁定 / 解锁按键 | 长按 **\*** |

开发者文档见[开发者文档](../dev/README.md)。
