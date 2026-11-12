<!-- SPDX-License-Identifier: MIT -->

# 故障排查

## 安装

网页安装器的“选择症状”步骤会在屏幕上涵盖其中大部分内容。下表也适用于 `rfoundry` 命令行。

| 问题 | 原因与处理 |
|---|---|
| “找不到电台引导程序” | 电台未处于引导程序，或线缆只能充电。关闭电台。按住电台的引导程序按键（[电台参考](23-radios.md)中你电台的“安装”一节）并开机；屏幕保持黑屏。换一个 USB 口或线缆。Windows：可能缺少引导程序驱动（[步骤](02-installing.md#windows-the-bootloader-driver)） |
| “拒绝访问” | Linux：添加 rfoundry 打印的 udev 规则，然后拔插。Windows：用 Zadig 安装引导程序驱动（[步骤](02-installing.md#windows-the-bootloader-driver)） |
| “引导程序忙” | 另一个程序占用了它（CPS、带 USB 直通的虚拟机）。关闭它并重新插入 |
| 网页安装器提示电台端口或 USB 连接被占用 | 关闭占用电台的其他程序或浏览器标签（CPS、qdmr、热点主机、第二个安装器标签），然后重试。Windows：引导程序驱动（[步骤](02-installing.md#windows-the-bootloader-driver)）。Linux：你的用户需要有权访问该设备 |
| 网页安装器无法使用某个固件文件 | 它会说明原因：其他电台的文件、不是厂商文件、尚无适配配方的版本、下载损坏。[各提示的含义](02-installing.md#when-the-installer-cant-use-a-file) |
| 备份或安装中断后电台停留在更新模式，电源开关无效 | 在网页安装器中选择**电台停留在更新模式**，选择电台（若询问也选择线缆），它会重启电台到正常模式或说明方法（[详情](02-installing.md#if-something-goes-wrong)）。若加载器无应答，取出电池几秒 |
| 刷写后屏幕一直黑屏 | 电台仍在引导程序中。关机再开机，不要按任何键 |
| 每次开机都是白色背光屏和常亮红灯，电台始终不启动 | 由于应用程序缺失或不完整，电台卡在厂商更新模式。其他部分没有损坏。请用网页安装器重新安装（[电台参考](23-radios.md)中你电台的“安装”一节说明是否适用） |
| “准备电台失败”，或电台在编程线上无响应 | 把线缆插到底再试，或换用 FTDI、CP210x 或 PL2303 芯片的线缆。慢速线缆模式现在始终开启：没有单独的按钮。若仍失败，请选择**使用厂商更新模式**：开机时按住电台的更新模式按键（[电台参考](23-radios.md)中你电台的“安装”一节）。编解码器需要原厂固件文件或程序存储器备份（[安装](02-installing.md#if-something-goes-wrong)） |
| 刷写中途停止 | 把电台置于引导程序并再次运行同一命令。rfoundry 绝不擦除引导程序 |

## 命令行安装（进阶）

网页安装器会在屏幕上显示这些内容并引导修复。使用 `rfoundry` 安装时同样适用，包括安装器的[手动安装包](02-installing.md#install-from-the-command-line-with-a-kit)。

| 问题 | 原因与处理 |
|---|---|
| “未知原厂文件……不是受支持的编解码器来源” | rfoundry 按 SHA-256 识别原厂文件。请下载[安装](02-installing.md)中为你的电台指定的确切文件 |
| “……是宝锋/TYT 官方固件文件，不是 RadioFoundry 镜像” | 你把原厂文件当成了镜像。请提供 RadioFoundry 镜像（或 `--release`），并用 `--stock` 提供原厂文件 |
| “rfoundry 无法判断电台是否带有编解码器” | 用 RadioFoundry 启动电台并运行 `rfoundry probe`，或提供 `--stock` 让 rfoundry 写入编解码器 |
| “找不到 libusb”（从源码运行） | 安装 libusb（`brew install libusb` 或 `apt install libusb-1.0-0`），或使用自带它的 rfoundry 可执行文件 |

## 日常使用

| 问题 | 原因与处理 |
|---|---|
| DMR 呼叫显示但没有声音，“无编解码器：语音关闭” | 电台没有可用编解码器。见[安装，无编解码器](02-installing.md#no-codec-no-codec-voice-off) |
| DMR 呼叫显示“其他通话组：不播放” | 其通话组不是通话目标、不在收听列表中，也不是回复目标。按**监听**（数字监听），或在 qdmr 中把该通话组加入信道的接收组列表。鹦鹉模式播放你自己的呼叫（来源是你自己的 ID）时，与其他呼叫一样播放 |
| “已加密：不播放” | 该呼叫使用基础加密，而电台没有匹配密钥，或使用了电台无法播放的更强加密 |
| 发射时显示“电台频率中无 DMR ID” | 写入含你 DMR ID 的电台频率（[为电台编程](04-programming.md)） |
| “信道忙” | 有人在时隙上通话。等呼叫结束 |
| “中继台无应答” | 中继台未响应。检查频率、色码、时隙以及你与中继台的距离 |
| FM 没有声音 | 静噪关闭，或信道需要亚音。按住**监听**打开静噪。用 **\***、**3** 检查亚音 |
| 按键无作用且屏幕显示“按键已锁” | 长按 **\*** 解锁。检查**设置 > 加密 > 自动锁键** |
| “此处未使用” | 该键在此屏幕上没有功能 |
| “……：此构建不含” | 该功能在此构建中关闭，或尚未构建 |
| 电台卡在热点屏幕 | 某个主机打开了串口。按 **OK**，或停止主机程序。关闭**设置 > 连接 > 允许热点**可防止 |
| 区域或信道自行改变 | 扫描正在运行（状态行显示 SCAN）。在主屏幕按 **返回** |
| qdmr、rfoundry 或热点主机找不到电台串口 | USB 处于网络模式。在**工具 > USB 网络**中切回，或关闭**设置 > 连接 > 启动时 USB 网络**并重启。调试链路和 MMDVM 也会在 UDP 端口 3334 上应答（[USB 网络](15-usb-network.md#serial-tools-in-network-mode)） |
| 网络模式下电脑拿不到地址 | 电脑在 10 秒内未设置好网卡时，电台会回到串口。确认电脑出现新的网卡，然后重试切换 |

## 设置未保存

如果重启后设置恢复默认，且 qdmr 写入失败，说明电台在 RadioFoundry 保存设置的闪存区域中发现了其他固件的数据。RadioFoundry 不会擦除它，因此你不会意外丢失数据。电台在内存中使用默认值运行。

要把该区域交给 RadioFoundry（开发者，经调试链路）：

```
tools/rfoundry_debug.py storage info     # 显示 region=foreign
tools/rfoundry_debug.py storage claim --out ~/rfoundry-backup    # 保存该区域的副本，然后为 RadioFoundry 擦除
```

`storage claim` 会在擦除任何内容之前，把该区域的旧内容和校准副本保存到 `--out` 目录中的文件。它会拒绝保存在 git 工作树内。

有些设置目前按设计就是“不保留”：见[设置](17-settings.md)中的状态列。

## 诊断

**OK**、**6**、**7**（工具 > 诊断）显示用于台架检查的原始读数：按键、接收机电平、DMR 调制解调器的状态和计数器、电池、板子自身的线路。**\*** 翻页。**返回** 离开。

<img src="../ux/screens/160x120i5/diagnostics.png" width="200" alt="诊断页面">

开发者可通过 USB 用 `tools/rfoundry_debug.py diag` 和 `tools/rfoundry_debug.py view` 获得同样及更多信息。

## 获取帮助

请在 Facebook 的 [RadioFoundry 社区群组](https://www.facebook.com/groups/radiofoundry/)提问：其他用户和开发者会在那里回答问题、协助安装并发布消息。网页安装器会在每条错误信息下方链接该群组和 GitHub issues。

## 报告问题

请在[项目仓库](https://github.com/radiofoundry/radiofoundry/issues/new/choose)中提交 issue。请附上固件版本（**设置 > 我的电台 > 关于**）、电台型号、你做了什么、期望什么以及发生了什么。截图会有帮助：`tools/rfoundry_debug.py screenshot -o screen.png` 可通过 USB 保存一张。

如果电台自行重启，请查看**设置 > 高级 > 崩溃日志**（[设置](17-settings.md#crash-log)）并把其内容复制到 issue 中。开发者可用 `tools/rfoundry_debug.py crash` 获取完整记录。
