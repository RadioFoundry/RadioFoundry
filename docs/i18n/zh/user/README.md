<!-- SPDX-License-Identifier: MIT -->

# RadioFoundry 用户手册

如何在电台上安装并使用 RadioFoundry。各章描述 RadioFoundry 在每款电台上的行为。[电台参考](23-radios.md) 列出各机型之间的差异：按键、功率档位、电池、电源开关和显示屏，以及每款电台已经验证过的项目。每个发行版还附带由这些文件生成的 PDF 手册（`tools/build_manual.sh`，按文件名顺序）。

从网站 [radiofoundry.org](https://radiofoundry.org) 开始：**网页安装器**可以安装、更新、备份和恢复电台，**电台频率编辑器**可以编辑信道、区域和联系人。两者都在 Chromium 浏览器中运行。下文各章把命令行工具作为替代方式。

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="DMR 信道上的主屏幕">
<img src="../ux/screens/160x120i5/quick_dmr.png" width="200" alt="快速设置">
<img src="../ux/screens/160x120i5/hub.png" width="200" alt="hub">

## 阅读顺序

### 入门

1. [简介](01-introduction.md)：RadioFoundry 是什么、项目状态、约定
2. [安装](02-installing.md)：先讲网页安装器，再讲进阶的 rfoundry 命令行；更新、备份、回退
3. [首次启动](03-first-boot.md)：内置信道、最初的几项检查
4. [为电台编程](04-programming.md)：电台频率编辑器、qdmr、RadioID 联系人

### 使用电台

5. [按键与导航](05-keys-and-navigation.md)：五条按键规则、查找、临时更改、按键锁
6. [主屏幕](06-home-screen.md)：每个区域显示什么、接收、发射
7. [信道、区域与 VFO](07-channels-zones-vfo.md)
8. [FM](08-fm.md)：静噪、监听、亚音、带宽、VOX、双音多频
9. [DMR](09-dmr.md)：通话目标、回复、最近呼叫、联系人、时隙与色码、加密、漫游
10. [消息、呼叫提示与电台检查](10-messages-and-signaling.md)
11. [紧急](11-emergency.md)
12. [扫描与双守候](12-scan.md)
13. [APRS](13-aprs.md)：FM 与 DMR 上的位置、信标、与电脑配合的 KISS 和 iGate
14. [热点模式](14-hotspot.md)：MMDVMHost 与 Pi-Star、SFR、两台电台组成双工中继
15. [USB 网络模式](15-usb-network.md)：DMR 承载 IP、状态页、rig 控制、KISS、TFTP、SNMP、控制站
16. [电池与供电](16-battery-and-power.md)

### 参考

17. [设置](17-settings.md)：每一项设置、默认值和状态
20. [故障排查](20-troubleshooting.md)
21. [常见问题](21-faq.md)
22. [术语表](22-glossary.md)
23. [电台参考](23-radios.md)：每款电台的按键、功率档位、电池、电源开关、显示屏、安装参数、编程方式和状态

### 从原厂固件迁移

这些章节适用于仍运行厂商固件、且电台频率值得保留的电台。一个程序即可安装 RadioFoundry 并迁移电台频率。没有对应章节时，用网页安装器安装，并在电台频率编辑器中打开它的备份（[安装](02-installing.md#install-with-the-web-installer)）。

[电台参考](23-radios.md) 会从各电台的“安装”一节链接到对应章节。

## 快速参考

按键以其功能命名。[按键角色](23-radios.md#key-roles) 显示在你的电台上哪个键承担哪项功能。

| 操作 | 按键 |
|---|---|
| 切换信道 | 信道旋钮，或 **上** / **下** |
| 切换区域 | **上一区域** / **下一区域**，或 **OK**、**4**，选择区域 |
| 快速设置 | **\*** |
| 打开菜单（hub） | **OK** |
| 处理最近来电 | **#** |
| 最近呼叫 | **OK**、**3** |
| 与另一个通话组或 ID 通话 | 长按 **#** |
| 选择通话对象 | **通话目标**（DMR），或 **\***、**8** |
| 监听 | 长按 **监听** |
| 开 / 关双守候 | 长按 **通话目标**，或 **\***、**7** |
| 开 / 关扫描 | **OK**、**6**、**1** |
| 撤销临时更改 | 在主屏幕按 **返回** |
| 保存对信道的更改 | 长按 **OK** |
| 返回主屏幕 | 长按 **返回** |
| 锁定 / 解锁按键 | 长按 **\*** |

开发者：见[开发者文档](../dev/README.md)。
