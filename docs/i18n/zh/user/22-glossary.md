<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- Generated from docs/glossary.json by tools/glossary.py. Edit the data file, not this page. -->
<!-- glossary: off -->

# 术语表

用户指南使用的术语。在网页版中，将鼠标悬停、聚焦或点按正文中带虚线的术语即可看到其含义。开发者术语表（[dev/glossary.md](../dev/glossary.md)）包含协议和硬件术语。

## DMR

| 术语 | 含义 |
|---|---|
| <a id="t-all-call"></a>All call | DMR 通话组 16777215，时隙上的每台电台都会听到。 |
| <a id="t-ambe-2"></a>AMBE+2 (AMBE) | DMR 使用的 DVSI 语音编解码器。它不属于 RadioFoundry：RadioFoundry 从每款电台的原厂固件文件中取得它。[更多](../dev/codec.md) |
| <a id="t-call-alert"></a>Call alert | 使另一台电台响铃的 DMR 寻呼。 |
| <a id="t-color-code"></a>Color code (CC) | DMR 信道使用的 0 到 15 的数字。电台只听到带有其信道色码的呼叫。 |
| <a id="t-confirmed-data"></a>Confirmed data | 由接收电台确认的 DMR 分组数据。发送方重发未到达的部分。 |
| <a id="t-dmr"></a>DMR | Digital Mobile Radio（数字移动无线电），ETSI 数字无线电标准（TS 102 361）。 |
| <a id="t-hang-time"></a>Hang time | 呼叫结束后，中继台或电台保持该呼叫的通话组以待回复的时长。 |
| <a id="t-pi-header"></a>PI header (PI headers, PI) | 宣告以变化的密钥加密的 DMR 报头。RadioFoundry 从不播放此类呼叫。 |
| <a id="t-private-call"></a>Private call | 发给一台电台的 DMR 呼叫（个呼）。 |
| <a id="t-radio-check"></a>Radio check | 另一台电台自动应答的 DMR 请求，用于表明它已开机并在范围内。 |
| <a id="t-sms"></a>SMS | 短消息服务：短信。在 DMR 上由短数据承载。 |
| <a id="t-talker-alias"></a>Talker alias | 随 DMR 语音发送的短文本，通常是呼号和姓名。 |
| <a id="t-talkgroup"></a>Talkgroup (TG, TGs) | DMR 组号。收听它的所有人都会听到发往它的呼叫。 |
| <a id="t-timeslot"></a>Timeslot (TS, TS1, TS2) | DMR 信道的两个时隙之一，TS1 和 TS2。 |

## 射频与硬件

| 术语 | 含义 |
|---|---|
| <a id="t-am"></a>AM | 调幅：语音改变载波的强度。航空频段使用它。 |
| <a id="t-ani"></a>ANI | 自动号码识别：在模拟呼叫中以音调发送的呼叫者 ID。 |
| <a id="t-bandwidth"></a>Bandwidth | FM 信道的带宽：12.5 kHz（窄带）或 25 kHz（宽带）。 |
| <a id="t-ctcss"></a>CTCSS | 连续音调编码静噪系统：FM 上的亚音频音调，只为正确的信号打开静噪。 |
| <a id="t-dcs"></a>DCS | 数字编码静噪：FM 上的亚音频数字码，只为正确的信号打开静噪。 |
| <a id="t-dfu"></a>DFU | Device Firmware Upgrade（设备固件升级）：电台引导程序的 USB 协议。 |
| <a id="t-dtmf"></a>DTMF | 双音多频：键盘在音频中发送的按键音。 |
| <a id="t-dual-watch"></a>Dual watch | 在 A、B 两个信道上收听，并在选中的信道上讲话。只有一个接收机时，电台在两者之间交替。 |
| <a id="t-fm"></a>FM | 调频：模拟语音模式。语音改变载波的频率。 |
| <a id="t-gps"></a>GPS | 全球定位系统：为电台提供位置的卫星接收机。 |
| <a id="t-lcd"></a>LCD | 液晶显示屏：电台的屏幕。 |
| <a id="t-led"></a>LED | 发光二极管：电台的状态指示灯。 |
| <a id="t-nmea"></a>NMEA | GPS 接收机用来报告位置和时间的文本格式。 |
| <a id="t-rf"></a>RF | 射频：空中的信号，区别于电台内部的音频或数据。 |
| <a id="t-rssi"></a>RSSI | 接收信号强度指示：接收到的信号有多强，以 dBm 计。 |
| <a id="t-rx"></a>RX | 接收。 |
| <a id="t-sfr"></a>SFR | 单频中继：一个时隙上的呼叫在同一频率的另一个时隙上重发。在 MMDVM SFR 中，由热点重发，由其主机开启。在独立 SFR 中，在电台频率中设置的信道自行重发，或通过这样的中继台通话。[更多](../spec/sfr.md) |
| <a id="t-split-duplex"></a>Split duplex | 同时在一个频率上发射、在另一个频率上接收。中继台这样做，双工热点配置也是如此。[更多](../spec/split-duplex.md) |
| <a id="t-squelch"></a>Squelch | 接收信号必须超过才会被电台播放的电平。 |
| <a id="t-tot"></a>TOT | 发射超时：电台允许的最长发射时间。 |
| <a id="t-tx"></a>TX | 发射。 |
| <a id="t-uhf"></a>UHF | 特高频：300 MHz 到 3 GHz。业余 70 cm 频段属于 UHF。 |
| <a id="t-usb"></a>USB | 通用串行总线。电台用它进行调试链路、网络模式和固件更新。 |
| <a id="t-vfo"></a>VFO | 可变频率振荡器：手动调谐任意频率，而不是使用存储的信道。 |
| <a id="t-vhf"></a>VHF | 甚高频：30 到 300 MHz。业余 2 m 频段属于 VHF。 |
| <a id="t-vox"></a>VOX | 声控发射：你说话时电台发射。 |

## 厂商术语

| 术语 | 含义 |
|---|---|
| <a id="t-ars"></a>ARS | 自动登记服务：电台告诉在线状态服务器它已上线。 |
| <a id="t-basic-privacy"></a>Basic privacy (Basic Privacy, Enhanced privacy, Enhanced Privacy) | 许多商用 DMR 电台提供的固定语音置乱。它不是加密。 |
| <a id="t-control-station"></a>Control station | 电脑上的调度程序用来经空中联系车队的电台。 |
| <a id="t-cps"></a>CPS | 客户编程软件：写入电台频率的程序。使用 RadioFoundry 时，请使用电台频率编辑器或 qdmr。[更多](04-programming.md) |
| <a id="t-lrrp"></a>LRRP (LIP) | 位置请求与响应协议：调度员向电台询问其位置。一种商用位置格式；标准中有 LIP。 |
| <a id="t-mototrbo"></a>MOTOTRBO | 一个商用 DMR 产品线。其扩展（ARS、TMS、LRRP、Capacity Plus）不在 ETSI 标准中。 |
| <a id="t-tms"></a>TMS | 短信服务：DMR 上的一种商用短信格式。标准自身的短数据使用其他格式。 |
| <a id="t-tyt"></a>TYT | 一家中国 DMR 电台制造商。 |

## 业余与网络

| 术语 | 含义 |
|---|---|
| <a id="t-aprs"></a>APRS | 自动分组报告系统：在业余台站之间共享的位置和短消息。[更多](13-aprs.md) |
| <a id="t-brandmeister"></a>BrandMeister | 由互联的服务器和通话组组成的全球业余 DMR 网络。 |
| <a id="t-cw-id"></a>CW ID | 以摩尔斯码发送的台站呼号。独立 SFR 中继信道在有通话后每隔几分钟以 FM 发送一次（设置 > 通话与收听 > 单频中继 CW ID）。[更多](14-hotspot.md) |
| <a id="t-dmr-id"></a>DMR ID (RadioID, DMR IDs) | 在 DMR 上识别你电台的号码。业余 ID 来自 RadioID.net。 |
| <a id="t-hotspot"></a>Hotspot | 经互联网把附近电台接入 DMR 网络的小型 DMR 调制解调器。[更多](14-hotspot.md) |
| <a id="t-igate"></a>iGate | 在空中和 APRS-IS 互联网网络之间转发 APRS 分组的台站。 |
| <a id="t-kiss"></a>KISS | 电脑上的 APRS 程序与电台调制解调器（TNC）之间的简单协议。 |
| <a id="t-mdns"></a>mDNS | 多播 DNS：在本地网络上按 `.local` 名称查找设备。[更多](../spec/mdns.md) |
| <a id="t-mmdvm"></a>MMDVM | Multi-Mode Digital Voice Modem（多模数字语音调制解调器）：MMDVMHost 使用的调制解调器协议。[更多](../spec/mmdvm-protocol.md) |
| <a id="t-rigctld"></a>rigctld | Hamlib 的网络电台控制协议，供日志和 SDR 程序使用。 |
| <a id="t-snmp"></a>SNMP | 简单网络管理协议：经网络读取设备的状态。[更多](../spec/snmp.md) |
| <a id="t-tftp"></a>TFTP | 简单文件传输协议：向设备复制文件或从设备复制文件的最简方式。[更多](../spec/tftp.md) |

## RadioFoundry 与固件

| 术语 | 含义 |
|---|---|
| <a id="t-codeplug"></a>Codeplug | 电台的编程数据（电台频率）：信道、区域、联系人、通话组列表和 DMR ID。[更多](04-programming.md) |
| <a id="t-contact"></a>Contact | 电台频率中的一个人：名称和 DMR ID。 |
| <a id="t-csv"></a>CSV | 逗号分隔值：以纯文本表示的表格，编程软件以此导出。 |
| <a id="t-digital-monitor"></a>Digital monitor | 播放时隙上的所有通话组（混听模式；旧版 RadioFoundry 构建中称为“hear all”）。 |
| <a id="t-id"></a>ID | 标识符：挑出某一事物的号码或名称。 |
| <a id="t-listen-list"></a>Listen list (RX group list) | 信道收听的通话组。电台频率中称为接收组列表。 |
| <a id="t-monitor"></a>Monitor | 按住按键期间打开静噪（FM），或收听所有通话组（DMR）。 |
| <a id="t-ok"></a>OK | 选择或确认的按键。 |
| <a id="t-one-shot"></a>One-shot | 只用于一次呼叫及其回复窗口的通话目标。 |
| <a id="t-pc"></a>PC | 个人电脑。 |
| <a id="t-ptt"></a>PTT | 按键通话：发射的按键。 |
| <a id="t-qdmr"></a>qdmr | 一款免费、开源的 DMR 电台频率编辑器。[更多](04-programming.md) |
| <a id="t-radiofoundry"></a>RadioFoundry | 本固件。 |
| <a id="t-recent"></a>Recent | 听到的台站和发起的呼叫列表（最近）。 |
| <a id="t-reply-window"></a>Reply window | 呼叫结束后，PTT 回复该呼叫的时间段（回复窗口）。 |
| <a id="t-stock-firmware"></a>Stock firmware | 电台在使用 RadioFoundry 之前出厂时所带的固件（原厂固件）。RadioFoundry 从其文件中取得语音编解码器。 |
| <a id="t-talk-target"></a>Talk target | PTT 的去向（通话目标）：通话组、个人或全呼。 |
| <a id="t-temporary-change"></a>Temporary change | 持续到信道更改或你在主屏幕上按返回为止的更改。要保留它，长按 OK。 |
| <a id="t-ui"></a>UI | 用户界面：屏幕、按键和菜单。 |
| <a id="t-yaml"></a>YAML | 一种文本文件格式。qdmr 以 YAML 存储其电台频率。 |
| <a id="t-zone"></a>Zone | 命名的信道列表（区域）。 |
