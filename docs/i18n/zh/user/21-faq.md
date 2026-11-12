<!-- SPDX-License-Identifier: MIT -->

# 常见问题

**为什么允许商业使用？**

商业使用正是 40 美元 DMR 电台存在的原因。如果 DMR 全都只限业余，一台像样的电台仍要 500 美元。项目作者不从 RadioFoundry 赚钱，因此别人把它用于产品时没人受损。本项目不销售 DMR 固件。目标是做出人人都能使用的像样固件。

**为什么要重造轮子？**

RadioFoundry 用一个核心覆盖这些电台中的大多数，并采用允许任何人使用的许可证，因此对一款电台的工作会帮助到其他所有电台。

**为什么还要另一个电台固件？**

DMR 有许多低价电台很少实现的功能。它们对商业应用和业余使用都有用。

**我怎样才能帮忙？**

以任何力所能及的方式：

- **测试并报告。** 在你的电台上运行 RadioFoundry 并[报告问题](https://github.com/radiofoundry/radiofoundry/issues/new?template=bug_report.yml)。如果电台重启过，请附上**设置 > 高级 > 崩溃日志**中的记录（[设置](17-settings.md#crash-log)）。
- **帮助他人。** 在 Facebook 的[社区群组](https://www.facebook.com/groups/radiofoundry/)中回答问题、分享经验。
- **提出需求。**[请求功能](https://github.com/radiofoundry/radiofoundry/issues/new?template=feature_request.yml)，或[请求支持某款电台](https://github.com/radiofoundry/radiofoundry/issues/new?template=radio_request.yml)。
- **贡献。** 为代码或文档提交 pull request（[贡献指南](../dev/contributing.md)）。
- **捐赠或借出电台。** 这帮助最大，尤其是现在难以找到的老型号，因为每个移植都需要台架上的硬件。

**网页刷机工具或电台频率编辑器失败了，如何报告？**

在 radiofoundry.org 上，大多数错误会自动报告：信息会说明并提供参考号。在下方框中输入你当时在做什么并点击**发送详情**；开发者会审阅每一份报告并处理根本问题。要发送更多内容，或在报告关闭/被阻止时，请使用**报告问题**。

点击错误信息下方的**报告问题**，描述你在做什么，并下载报告。它是一个 zip，含页面执行的步骤、错误和浏览器信息。可通过电子邮件发给开发者、附到 GitHub issue，或在[社区群组](https://www.facebook.com/groups/radiofoundry/)中分享。如果问题是读取或写入电台频率，请勾选包含电台频率的选项：电台频率含你的呼号、DMR ID 和联系人，因此除非你添加，否则不会包含。开发者也可打开浏览器控制台（F12）：每个错误及其详情都会打印在那里。

**写入电台频率失败并提示“电台拒绝写入电台频率”，怎么办？**

如果电台刚从原厂固件转来，请再次选择**写入电台**：编辑器会提供**准备电台**，它会擦除 RadioFoundry 保存设置区域中的原厂固件数据。否则电台会拒绝每次写入，并在关机后忘记设置。若之后错误再次出现，请使用**报告问题**。

**为什么发行版不含编解码器？**

DMR 语音所需的 AMBE+2 编解码器属于 DVSI，RadioFoundry 无法分发它。你的电台原厂固件中已有它。rfoundry 在它已存在时保持不动，或从你电脑上的原厂固件文件中取出（[安装](02-installing.md)）。

**可以不带编解码器使用 RadioFoundry 吗？**

可以。FM 可用，DMR 呼叫会显示在屏幕上。DMR 数据（消息、呼叫提示、电台检查）和热点模式可用。DMR 语音关闭。

**RadioFoundry 会损坏我的电台吗？**

刷写只写入处理器的应用程序和编解码器扇区。引导程序和出厂校准绝不会被写入，因此你可以回到原厂（[安装](02-installing.md#going-back-to-the-stock-firmware)）。RadioFoundry 是早期软件。依赖它之前请先测试，并且只在你持有执照的地方发射。

**qdmr 能与 RadioFoundry 配合吗？**

可以，通过兼容层。qdmr 的开源固件驱动可读写电台频率（[为电台编程](04-programming.md)）。在 qdmr 驱动无法写入电台频率所在位置的电台，请使用电台频率编辑器（[电台参考](23-radios.md#programming)）。

**不安装任何东西也能为电台编程吗？**

可以。项目网站上的电台频率编辑器可在基于 Chromium 的浏览器（Chrome、Edge、Opera）中读写电台，并从 RadioID.net 构建 DMR ID 数据库（[电台频率编辑器](04-programming.md#the-codeplug-editor)）。

**可以用手机吗？**

Android 可以：Android 上的 Chrome 可运行网页安装器和电台频率编辑器。通过 USB OTG 转接头把电台的 USB 线或编程线插入手机。

**电台传输进行中，不要切换到其他应用或让屏幕超时。** Android 会降低后台页面的速度，备份或安装会因此耗时更长甚至失败。连接电台期间页面会保持屏幕常亮。

| 要点 | 原因 |
|---|---|
| FTDI 芯片的线缆和自带 USB 口的电台可用。其他 USB 串口线（CH340、CP210x、PL2303）尚不可用 | Android 上的 Chrome 只能通过 WebUSB 访问 USB 串口设备，页面为每种设备自带驱动 |
| 在 Chrome 列表中选择电台或线缆后，接受 Android 的“允许 Chrome 访问……”提示 | 否则页面无法打开设备 |
| 关闭插入电台或线缆时自动打开的应用，不要在 Android 的“打开方式”提示中选择它 | 占用线缆的应用会抢走电台的应答；页面随后会报告电台无应答 |
| 保持 Chrome 在前台且屏幕常亮（见上） | Android 会把后台页面降到大约每秒一步 |

iPhone 和 iPad：不行。iOS 没有 WebUSB 或 Web Serial。

**基础加密安全吗？**

不安全。基础加密用固定密钥置乱语音。它只能挡住随意收听者，仅此而已。
