<!-- SPDX-License-Identifier: MIT -->
<!-- Generated from docs/glossary.json by tools/glossary.py. Edit the data file, not this page. -->
<!-- glossary: off -->

# 术语表

用户指南使用的术语。在网页版中，将鼠标悬停、聚焦或点按正文中的虚线术语即可看到其含义。开发者术语表（[dev/glossary.md](../dev/glossary.md)）包含协议和硬件术语。

## DMR

| 术语 | 含义 |
|---|---|
| <a id="t-all-call"></a>全呼（All call） | DMR 通话组 16777215，时隙上的每台电台都会听到。 |
| <a id="t-ambe-2"></a>AMBE+2（AMBE） | DVSI 的语音编解码器，DMR 使用。不属于 RadioFoundry。来自每台电台的原厂固件。[更多](../dev/codec.md) |
| <a id="t-call-alert"></a>呼叫提示（Call alert） | 使另一台电台响铃的 DMR 寻呼。 |
| <a id="t-color-code"></a>色码（CC） | DMR 信道使用的 0 到 15 的数字。电台只听到与其色码相同的呼叫。 |
| <a id="t-confirmed-data"></a>确认数据（Confirmed data） | 接收电台会确认的 DMR 分组数据。未到达的部分会重发。 |
| <a id="t-dmr"></a>DMR | Digital Mobile Radio，ETSI 数字无线电标准（TS 102 361）。 |
| <a id="t-hang-time"></a>挂机时间（Hang time） | 呼叫结束后，中继台或电台保持该呼叫通话组等待回复的时长。 |
| <a id="t-pi-header"></a>PI 头（PI header、PI headers、PI） | 宣告使用变化密钥加密的 DMR 头。RadioFoundry 绝不播放此类呼叫。 |
| <a id="t-private-call"></a>私密呼叫（Private call） | 发给一台电台的 DMR 呼叫。 |
| <a id="t-radio-check"></a>电台检查（Radio check） | 另一台电台会自动应答的 DMR 请求，用于表明它已开机并在范围内。 |
| <a id="t-sms"></a>SMS | 短消息服务：文本消息。在 DMR 上由短数据承载。 |
| <a id="t-talker-alias"></a>通话者别名（Talker alias） | 随 DMR 语音发送的短文本，通常是呼号和姓名。 |
| <a id="t-talkgroup"></a>通话组（TG、TGs） | DMR 组号。收听它的所有人都会听到发往它的呼叫。 |
| <a id="t-timeslot"></a>时隙（TS、TS1、TS2） | DMR 信道的两个时隙之一，TS1 和 TS2。 |

## 射频与硬件

| 术语 | 含义 |
|---|---|
| <a id="t-am"></a>AM | 幅度调制：语音改变载波强度。航空波段使用它。 |
| <a id="t-ani"></a>ANI | 自动号码识别：模拟呼叫中以音调发送的呼叫者 ID。 |
| <a id="t-bandwidth"></a>带宽（Bandwidth） | FM 信道的宽度：12.5 kHz（窄）或 25 kHz（宽）。 |
| <a id="t-ctcss"></a>CTCSS | 连续音调编码静噪系统：FM 上的亚音频音调，只有正确的信号才能打开静噪。 |
| <a id="t-dcs"></a>DCS | 数字编码静噪：FM 上的亚音频数字码，只有正确的信号才能打开静噪。 |
| <a id="t-dfu"></a>DFU | Device Firmware Upgrade：电台引导程序的 USB 协议。 |
| <a id="t-dtmf"></a>DTMF | 双音多频：键盘在音频中发送的按键音。 |
| <a id="t-dual-watch"></a>双守候（Dual watch） | 收听两个信道 A 和 B，并在选中的那个上通话。单接收机时电台在两者间交替。 |
| <a id="t-fm"></a>FM | 频率调制：模拟语音模式。语音改变载波频率。 |
| <a id="t-gps"></a>GPS | 全球定位系统：为电台提供位置的卫星接收机。 |
| <a id="t-lcd"></a>LCD | 液晶显示屏：电台的屏幕。 |
| <a id="t-led"></a>LED | 发光二极管：电台的状态灯。 |
| <a id="t-nmea"></a>NMEA | GPS 接收机报告位置和时间所用的文本格式。 |
| <a id="t-rf"></a>RF | 射频：空中的信号，相对于电台内部的音频或数据。 |
| <a id="t-rssi"></a>RSSI | 接收信号强度指示：接收信号有多强，单位 dBm。 |
| <a id="t-rx"></a>RX | 接收。 |
| <a id="t-sfr"></a>SFR | 单频中继。它把来自一个时隙的呼叫在同一频率的另一时隙上重发。MMDVM SFR 中由热点重发，由主机开启。独立 SFR 中，电台频率里设置的信道自行重发，或通过它通话。[更多](../spec/sfr.md) |
| <a id="t-split-duplex"></a>分离双工（Split duplex） | 同时在一个频率上发射、在另一个频率上接收。中继台如此，双工热点配置也如此。[更多](../spec/split-duplex.md) |
| <a id="t-squelch"></a>静噪（Squelch） | 接收信号必须超过、电台才会播放的电平。 |
| <a id="t-tot"></a>TOT | 发射超时：电台允许的最长发射时间。 |
| <a id="t-tx"></a>TX | 发射。 |
| <a id="t-uhf"></a>UHF | 特高频：300 MHz 到 3 GHz。业余 70 cm 频段属于 UHF。 |
| <a id="t-usb"></a>USB | 通用串行总线。电台用其进行调试链路、网络模式和固件更新。 |
| <a id="t-vfo"></a>VFO | 可变频率振荡器：手动调谐任何频率，而不是使用已存信道。 |
| <a id="t-vhf"></a>VHF | 甚高频：30 到 300 MHz。业余 2 m 频段属于 VHF。 |
| <a id="t-vox"></a>VOX | 声控发射。 |

## 厂商术语

| 术语 | 含义 |
|---|---|
| <a id="t-ars"></a>ARS | 自动注册服务：电台告诉在线状态服务器它已上线。 |
| <a id="t-basic-privacy"></a>基础加密（Basic Privacy、Enhanced privacy、Enhanced Privacy） | Motorola 和 Anytone 电台的固定语音置乱。不是加密。 |
| <a id="t-control-station"></a>控制站（Control station） | 电脑上的调度程序用来经空中访问车队的电台。 |
| <a id="t-cps"></a>CPS | 客户编程软件：写入电台频率的程序。RadioFoundry 使用 qdmr。[更多](04-programming.md) |
| <a id="t-lrrp"></a>LRRP（LIP） | 位置请求与响应协议：调度员向电台询问其位置。这是 Motorola 的位置格式；标准中为 LIP。 |
| <a id="t-mototrbo"></a>MOTOTRBO | Motorola 的 DMR 产品线。其扩展（ARS、TMS、LRRP、Capacity Plus）不在 ETSI 标准中。 |
| <a id="t-tms"></a>TMS | 文本消息服务：Motorola 在 DMR 上的文本消息格式。标准自身的短数据使用其他格式。 |
| <a id="t-tyt"></a>TYT | 中国的 DMR 电台制造商。 |

## 业余与网络

| 术语 | 含义 |
|---|---|
| <a id="t-aprs"></a>APRS | 自动分组报告系统：业余台站之间共享的位置和短消息。[更多](13-aprs.md) |
| <a id="t-brandmeister"></a>BrandMeister | 由互联服务器和通话组组成的全球业余 DMR 网络。 |
| <a id="t-cw-id"></a>CW 标识 | 以摩尔斯码发送的台站呼号。独立 SFR 中继信道在有通话后每隔几分钟以 FM 发送一次（设置 > 通话与收听 > SFR CW 标识）。[更多](14-hotspot.md) |
| <a id="t-dmr-id"></a>DMR ID（RadioID、DMR IDs） | 在 DMR 上标识你电台的号码。业余 ID 来自 RadioID.net。 |
| <a id="t-hotspot"></a>热点（Hotspot） | 通过互联网把附近电台接入 DMR 网络的小型 DMR 调制解调器。[更多](14-hotspot.md) |
| <a id="t-igate"></a>iGate | 在空中的 APRS 分组与 APRS-IS 互联网网络之间转发的台站。 |
| <a id="t-kiss"></a>KISS | 电脑上的 APRS 程序与电台调制解调器（TNC）之间的简单协议。 |
| <a id="t-mdns"></a>mDNS | 多播 DNS：通过 `.local` 名称在本地网络中找到设备。[更多](../spec/mdns.md) |
| <a id="t-mmdvm"></a>MMDVM | 多模数字语音调制解调器：MMDVMHost 使用的调制解调器协议。[更多](../spec/mmdvm-protocol.md) |
| <a id="t-rigctld"></a>rigctld | Hamlib 的网络 rig 控制协议，供日志和 SDR 程序使用。 |
| <a id="t-snmp"></a>SNMP | 简单网络管理协议：经网络读取设备状态。[更多](../spec/snmp.md) |
| <a id="t-tftp"></a>TFTP | 简单文件传输协议：在设备间复制文件的最小方式。[更多](../spec/tftp.md) |

## RadioFoundry 与固件

| 术语 | 含义 |
|---|---|
| <a id="t-codeplug"></a>电台频率（Codeplug） | 电台的编程数据：信道、区域、联系人、通话组列表、DMR ID。[更多](../dev/cps-and-codeplug.md) |
| <a id="t-contact"></a>联系人（Contact） | 电台频率中的一个人：名称和 DMR ID。 |
| <a id="t-csv"></a>CSV | 逗号分隔值：以纯文本表示表格，编程软件导出时使用。 |
| <a id="t-digital-monitor"></a>数字监听（Digital monitor） | 播放时隙上的所有通话组（混杂模式；旧版 RadioFoundry 中称为“hear all”）。 |
| <a id="t-id"></a>ID | 标识符：用于区分某事物的号码或名称。 |
| <a id="t-listen-list"></a>收听列表（Listen list、RX group list） | 信道收听的通话组。电台频率中称为接收组列表。 |
| <a id="t-monitor"></a>监听（Monitor） | 按住某键时打开静噪（FM）或收听所有通话组（DMR）。 |
| <a id="t-ok"></a>OK | 用于选择或确认的按键。 |
| <a id="t-one-shot"></a>一次性（One-shot） | 仅用于一次呼叫及其回复窗口的通话目标。 |
| <a id="t-pc"></a>PC | 个人电脑。 |
| <a id="t-ptt"></a>PTT | 按讲发射：用于发射的按键。 |
| <a id="t-qdmr"></a>qdmr | 面向 DMR 电台的免费开源电台频率编辑器。[更多](04-programming.md) |
| <a id="t-radiofoundry"></a>RadioFoundry | RadioFoundry，即本固件。 |
| <a id="t-recent"></a>最近（Recent） | RadioFoundry 记录听到的台站和发起呼叫的列表。 |
| <a id="t-reply-window"></a>回复窗口（Reply window） | 呼叫结束后 PTT 可应答该呼叫的时间。 |
| <a id="t-stock-firmware"></a>原厂固件（Stock firmware） | 电台出厂时附带的固件，在 RadioFoundry 之前。每款 RadioFoundry 电台都从其取得语音编解码器。 |
| <a id="t-talk-target"></a>通话目标（Talk target） | PTT 的去向：通话组、个人或全呼。 |
| <a id="t-temporary-change"></a>临时更改（Temporary change） | 持续到切换信道或在主屏幕按返回的更改。长按 OK 可保留。 |
| <a id="t-ui"></a>UI | 用户界面：屏幕、按键和菜单。 |
| <a id="t-yaml"></a>YAML | 一种文本文件格式。qdmr 以 YAML 保存其电台频率。 |
| <a id="t-zone"></a>区域（Zone） | 带名称的信道列表。 |
