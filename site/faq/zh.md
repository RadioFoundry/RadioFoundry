<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- The FAQ in Chinese: as en.md, which says how to add an item -->

## 更新后电台变成了其他语言

按菜单键，然后按 7（设置）、2（显示），再按一次上键到最后一行“语言”。按 OK 并选择中文。网页安装程序中的“电台语言”决定电台启动时的语言，恢复出厂设置也会回到该语言。 [详情](../../docs/user/17-settings.md#display-key-2)

## 安装程序找不到我的电台

电台不在引导程序中，或者 USB 线只能充电。关闭电台，按住引导程序按键再开机；屏幕保持黑屏。换一根线或一个 USB 端口试试。在 Windows 上，引导程序可能需要安装驱动。 [详情](../../docs/user/20-troubleshooting.md#installing)

## 写频时，串口选择框不弹出或没有列出我的电台

关闭其他可能占用该端口的程序（写频软件、qdmr、热点程序、其他浏览器标签页）。确认电脑把该端口识别为串口（Windows 上在设备管理器中查看）。处于 USB 网络模式的电台不会列出：使用页面上的“将电台切换为 USB 串口”，电台会保持串口模式直到重启。使用 FTDI、CP210x 和正品 Prolific 芯片的线缆效果最好。CH340 线缆在备份和安装时可能不可靠，安装程序会对其给出警告。Prolific 的 Windows 驱动会禁用仿冒芯片，而网上出售的许多此类电台线缆使用的正是仿冒芯片。 [详情](../../docs/user/20-troubleshooting.md#everyday-use)

## 安装后屏幕一直是黑的

电台仍在引导程序中。不按任何键，关机后再开机。 [详情](../../docs/user/20-troubleshooting.md#installing)

## 电台卡在更新模式

在网页安装程序中选择“电台停留在更新模式”，然后选择电台。它会重启电台或告诉你怎么做。如果没有响应，取下电池几秒钟。 [详情](../../docs/user/02-installing.md#if-something-goes-wrong)

## 如果不喜欢 RadioFoundry，能改回去吗？

可以。安装程序在安装时会备份电台及其原始码表，“恢复”可以用厂商的固件文件装回原厂固件。请保留该文件的副本。RadioFoundry 从不擦除原厂引导程序，因此作为最后手段，你总能手动进入引导程序并装回原厂固件。 [详情](../../docs/user/02-installing.md#going-back-to-the-stock-firmware)

## 如何进入 VFO？

有三种方法：菜单、4（区域），VFO 是第一行；在 设置 > 按键 中把侧键设为 VFO/信道（或使用电台自带的 VFO/MR 键）；或在信道上按 * 再按 4（到 VFO），把该信道复制到 VFO。 [详情](../../docs/user/07-channels-zones-vfo.md#the-vfo)

## 在 DMR 上很难连上我的热点或某个中继

RadioFoundry 使用电台的原厂频率校准，而热点对频率非常敏感。如果电台频率略有偏差，在支持的电台上调整 设置 > 高级 > 频率微调。请先测量误差，最好用校准过的 SDR、频率计或热点的 BER 测试，然后以相反的符号输入。 [详情](../../docs/user/17-settings.md#advanced-last-row)

## RadioFoundry 支持数据模式吗？

支持。除模拟分组（APRS 和 KISS TNC）外，RadioFoundry 还支持原生 DMR 数据：与商用 DMR 电台兼容的短信，以及使用 ETSI 压缩 UDP 头的 IP 数据。免费的 ATAK 插件 takRF 只需写频线和一根便宜的 USB OTG 线连接手机或平板，几秒钟即可上线。 [详情](https://takrf.com/dmr)

## 使用 DMR 数据需要什么线缆？

只需电台的写频线。连接安卓手机或平板时，再加一根常见的 USB OTG 线。 [详情](https://takrf.com/dmr)

## DMR Tier III 集群无法使用

它已在真实的 RadioFoundryGateway Tier III 中继、双工热点和软件模拟器上测试过，协议实现也已与商用 Tier III 系统的数据包抓包进行比对和验证。请提交问题并附上详情：系统、控制信道和业务信道、码表中的系统设置和电台显示的内容，以及“支持”页面下载的支持信息。 [详情](../../docs/user/09-dmr.md#trunked-systems)

## RadioFoundry 会支持 P25 第二阶段吗？

不会。这些电台的射频收发器件无法发送或接收第二阶段的调制方式（H-CPM 和 H-DQPSK）。TETRA 也是如此。 [详情](../../docs/user/07-channels-zones-vfo.md#p25-trunked-channels)

## 只有 FM 的电台能用 DMR 或其他数字模式吗？

一般不能。它没有所需的芯片。 [详情](../../docs/user/23-radios.md)

## 能看到 DMR 呼叫，但没有声音

屏幕显示“无编解码器：语音关闭”：电台没有可用的编解码器。请用电台的原厂固件文件重新安装。 [详情](../../docs/user/02-installing.md#no-codec-no-codec-voice-off)

## 呼叫显示“其他通话组：不播放”

该通话组不是信道的发射目标，也不在收听列表中。按“监听”键，或将通话组加入信道的接收组列表。 [详情](../../docs/user/20-troubleshooting.md#everyday-use)

## 我的设置恢复成了默认值

电台在 RadioFoundry 保存设置的位置发现了其他固件的数据，不会自行擦除。 [详情](../../docs/user/20-troubleshooting.md#settings-are-not-kept)

## 电台自己重启了

查看 设置 > 高级 > 崩溃日志，并将其内容与支持信息一起附在报告中。 [详情](../../docs/user/17-settings.md#crash-log)
