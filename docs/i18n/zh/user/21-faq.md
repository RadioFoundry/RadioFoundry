<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# 常见问题

症状及其解决方法，见[故障排除](20-troubleshooting.md)。

## 关于本项目

### 为什么允许商业使用？

商业使用正是 40 美元 DMR 电台存在的原因。如果 DMR 的一切都只限业余，一台像样的电台仍要 500 美元。项目作者不从 RadioFoundry 赚钱，
因此别人把它用于产品时没有人受损。本项目不销售 DMR 固件。目标是做出人人都能使用的像样固件。

### 为什么要重造轮子？

RadioFoundry 用一个核心覆盖这些电台中的大多数，并采用允许任何人使用的许可证，因此在一款电台上的工作会帮助其他所有电台。

### 为什么还要另一个电台固件？

DMR 有许多低价电台很少实现的功能。它们对商业应用和业余使用都有用。

### 我怎样才能帮忙？

以任何力所能及的方式：

- **测试并报告。**在你的电台上运行 RadioFoundry，并
  [报告出错的地方](https://github.com/radiofoundry/radiofoundry/issues/new?template=bug_report.yml)。如果电台重启过，
  请附上**设置 > 高级 > 崩溃日志**中的崩溃日志（[设置](17-settings.md#crash-log)）。
- **帮助他人。**在 Facebook 上的[社区小组](https://www.facebook.com/groups/radiofoundry/)中回答问题，分享可行的做法。
- **提出请求。**请求你希望支持的
  [功能](https://github.com/radiofoundry/radiofoundry/issues/new?template=feature_request.yml)或
  [电台](https://github.com/radiofoundry/radiofoundry/issues/new?template=radio_request.yml)。
- **贡献。**为代码或文档提交 pull request（[贡献指南](../dev/contributing.md)）。
- **捐赠或借出电台。**这帮助最大，尤其是难以找到的老型号，因为每个移植都需要台架上的硬件。

### 我可以在自己的项目中使用 RadioFoundry 吗？

可以，包括商业和闭源产品。许可证只有一个条件：在你复制的代码中以及随你发布的固件保留我们的版权和许可证声明。我们也希望你为项目另起名称。
如果你添加了功能，我们很乐意把它收入这里，署你的名字，并在硬件能运行它的每款电台上使用。发起[讨论](https://github.com/radiofoundry/radiofoundry/discussions)
或提交 pull request。详情见[在你的项目中使用 RadioFoundry](../../FORKING.md)。

### 本项目是 AI 生成的吗？

不是。本项目是多位贡献者合计数千小时人工投入的成果，用于电台拆解、逆向工程、软件架构和测试。AI 作为结对编程的伙伴，加快对更多电台的支持，
使本项目的范围得以实现并可持续。

## 安装与编解码器

### 为什么发行版不含编解码器？

DMR 语音需要 DVSI 的 AMBE+2 编解码器，而 RadioFoundry 无法分发它。你的电台在其原厂固件中已有该编解码器。安装器在编解码器已存在时保持不动，
或在你自己的电脑上从你电台的原厂固件文件中取出它。详情见[安装](02-installing.md)。

### 可以不带编解码器使用 RadioFoundry 吗？

可以。FM 可用，DMR 呼叫会显示在屏幕上。DMR 数据（消息、呼叫提示、电台检查）和热点模式可用。DMR 语音关闭。

### RadioFoundry 会损坏我的电台吗？

刷写只写入处理器的应用程序和编解码器扇区。安装器从不写入引导程序或出厂校准，因此你可以回到原厂固件
（[安装](02-installing.md#going-back-to-the-original-firmware)）。依赖 RadioFoundry 之前请先测试，并且只在你持有执照的地方发射。

### 如何报告网页安装器或电台频率编辑器的问题？

见[报告问题](20-troubleshooting.md#reporting-a-problem)。

## 编程

### qdmr 能与 RadioFoundry 配合使用吗？

可以，通过兼容层。qdmr 的开源固件驱动读写电台频率（[为电台编程](04-programming.md)）。在电台频率位于 qdmr 驱动无法写入之处的电台上，
使用电台频率编辑器（[电台参考](23-radios.md#programming)）。

### 不安装任何东西也能为电台编程吗？

可以。项目网站上的电台频率编辑器在基于 Chromium 的浏览器（Chrome、Edge、Opera）中读写电台，并从 RadioID.net 构建 DMR ID 数据库。
详情见[电台频率编辑器](04-programming.md#the-codeplug-editor)。

### 可以用手机吗？

Android 可以：Android 上的 Chrome 运行网页安装器和电台频率编辑器。通过 USB OTG 转接头把电台的 USB 线或编程线插入手机。

**电台传输进行中，不要切换到其他应用，也不要让屏幕超时。**Android 会降低后台页面的速度，因此备份或安装耗时会长得多，并且可能失败。
电台连接期间，页面保持屏幕常亮。

| 要点 | 原因 |
|---|---|
| 带 FTDI 芯片的线缆和自带 USB 端口的电台可用。其他 USB 串口线（CH340、CP210x、PL2303）不可用 | Android 上的 Chrome 只能通过 WebUSB 访问 USB 串口设备，页面为每种设备自带驱动 |
| 在 Chrome 的列表中选择电台或线缆后，接受 Android 的“允许 Chrome 访问……”提示 | 否则页面无法打开设备 |
| 关闭插入电台或线缆时打开的任何应用，也不要在 Android 的“打开方式”提示中选择它 | 占用线缆的应用会拿走电台的应答，页面随后会报告电台没有应答 |
| 保持 Chrome 在前台且屏幕常亮 | Android 会把后台页面降到大约每秒一步 |

iPhone 和 iPad 不行：iOS 没有 WebUSB 或 Web Serial。

## 加密

### 基本加密安全吗？

不安全。基本加密用固定密钥置乱语音。它只能挡住随意收听的人，仅此而已。
