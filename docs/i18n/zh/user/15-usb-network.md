<!-- SPDX-License-Identifier: MIT -->

# USB 网络模式

在 USB 网络模式中，电台是电脑的 USB 网卡。它通过 DMR 分组数据把 IPv4 分组路由到其他电台，并向电脑提供状态页、rig 控制、KISS、TFTP 和 SNMP。它位于有 USB 的电台镜像中。经 DMR 到另一台电台的 IP 仅在主机测试中验证。各项服务在各电台上的检查情况见[电台参考](23-radios.md)。不含该功能的构建（`CONFIG_DMR_IP_GATEWAY=n`）会在**工具 > USB 网络**中显示“USB 网络：此构建不含”。

## 把电台切换到网络模式

除非**启动时 USB 网络**开启（见下文），电台启动时是 USB 串口。要切换，请打开**工具 > USB 网络**（**OK**、**6**、**4**）。页面说明 USB 当前模式以及会有哪些变化。聚焦“USB 切到网络”并按 **OK**。电台提示“USB 现为网卡”并返回主屏幕，状态行显示 IP。电脑设置好网卡且 IP 可通后，其旁会出现一个向上和一个向下箭头。USB 作为网卡期间，qdmr、热点主机和串口调试链路都无法访问电台。

要回到串口，再次打开**工具 > USB 网络**并选择“USB 切到串口”（“USB 已恢复为串口”），或重启电台。工具页中该行显示当前模式（串口或网络）。

开发者也可通过调试链路切换：

```
tools/rfoundry_debug.py radio usb network
```

电台会短暂从 USB 消失，然后作为网卡回来（USB ID 1FC9:0096）。Linux、macOS、Windows 10 和 11 使用内置驱动。若电脑在 10 秒内未设置好网卡，电台会回到串口。

## 以网络模式启动

**设置 > 连接 > 启动时 USB 网络**使电台以网卡启动。若电脑在插入后 10 秒内未设置好网卡，电台会重新变为串口，直到重启。这样你总能访问它来关闭该设置。电台会等待电脑。在远离电脑时开机，之后再插入，它仍是网卡。

## 地址

电脑通过 DHCP 从电台取得地址：

| 谁 | 地址 |
|---|---|
| 你的电脑 | 12.x.y.z，由你的 DMR ID 得出（例如 ID 3101234 为 12.47.82.50） |
| 电台 | 你电脑地址翻转最低位（12.47.82.51） |
| 另一台电台 | 由其 DMR ID 得出的 12.x.y.z |
| 一个通话组 | 由通话组编号得出的 225.x.y.z |

发往另一台电台或通话组地址的分组会在当前 DMR 信道上经空中发出。来自其他电台的分组到达网卡。DMR 分组数据远慢于任何有线网络。2 秒内无法发出的分组会被丢弃。[控制站模式](#control-station-for-dispatch-software)的固定地址会替换前两行。

## 网络模式下的串口工具

调试链路、MMDVM 热点协议和 CPS 协议仍可通过电台地址的 UDP 端口 3334 访问：

```
tools/rfoundry_debug.py --port udp:12.47.82.51 version
```

MMDVMHost 以同样方式连接（`Protocol=udp`、`ModemAddress=<电台地址>`、`ModemPort=3334`）。qdmr 只按串口 USB ID 查找电台，因此请在串口模式下为电台编程。

## 按名称查找电台

电台会在网卡上应答多播 DNS（Bonjour、Avahi）。其名称为 `rfoundry-<DMR ID>.local`，例如 `rfoundry-3101234.local`。它会广播自己的服务。Bonjour 服务浏览器会按你的呼号、电台型号和 DMR ID 列出它，例如“N0CALL XR-100UV (3101234)”：

```
ping rfoundry-3101234.local
dns-sd -B _rfoundry-debug._udp            # macOS；Linux 上为 avahi-browse -r _rfoundry-debug._udp
tools/rfoundry_debug.py --port udp:rfoundry-3101234.local version
```

| 服务 | 端口 | 内容 |
|---|---|---|
| `_rfoundry-debug._udp` | 3334 | 经 UDP 的调试链路（以及其他串口协议） |
| `_mmdvm._udp` | 3334 | 供 MMDVMHost 使用的 MMDVM 调制解调器 |
| `_tftp._udp` | 69 | 经 TFTP 的电台频率（见下文） |
| `_snmp._udp` | 161 | SNMP 代理（见下文） |
| `_http._tcp` | 80 | 状态页 |
| `_kiss-tnc._tcp` | 8001 | KISS over TCP |
| `_rigctld._tcp` | 4532 | Hamlib rig 控制 |

每项的 TXT 记录都包含板型、DMR ID、呼号和固件版本。

## 电台上的服务

电台在自身地址的这些 TCP 端口上应答，仅经 USB 线，绝不走空中。

| 端口 | 服务 |
|---|---|
| 80 | 电台前面板。在浏览器中打开 `http://<电台地址>/`。它显示电台及其实时屏幕和 LED，以及信道、频率、信号和电池。移动台上还会画出其手咪。点击按键或使用键盘。侧键和顶键在电台边缘外也有带标签的按钮，信道旋钮上方的箭头每次点击转一格（按住持续转动）。键盘按键：数字、`*` 和 `#`；方向键；Enter 选择（绿色键或 MENU）；Esc 或 Backspace 返回（红色键或 BACK）；`[` 和 `]`（或 Page Up/Page Down）转动信道旋钮；按住 Space 通话。页面在图片旁列出你的电台具有的键盘按键。**收听**播放电台接收到的内容（在有网络音频的电台，[音频互联](#linking-audio-allstarlink-and-dvswitch)）；经编程线（PPP，见下文）时页面没有“收听”，因为线缆对音频来说太慢。按键只能从插入电台的那台电脑操作；同一时间只有一个页面控制电台，其他页面只显示状态。程序可读取 `/status`（JSON）以及调试链路的各段 `/api/view?section=core` |
| 8001 | 供 APRS 程序（APRSdroid、Xastir、YAAC）使用的 KISS over TCP。把程序的 TNC 设为“KISS over TCP”、电台地址、端口 8001。先在工具 > APRS 中开启 **USB 上的 KISS**。电台只发送来自你自己呼号的分组。在无法接收 APRS 的电台上（[电台参考](23-radios.md)），程序只能看到自己发送的内容 |
| 4532 | 供日志、SDR 和卫星程序使用的 Hamlib rig 控制。选择“Hamlib NET rigctl”，地址为电台地址、端口 4532，或运行 `rigctl -m 2 -r <电台地址>:4532 f`。程序可读取和设置频率、读取模式和 S 表，以及按下发射。设置信道频率会切换到 VFO。发射只能从你的电脑进行，每次按键最多 3 分钟 |

### 电台自身连接 APRS-IS

电台可以自己连接 APRS-IS 服务器并把你的位置发送到那里。你的电脑必须把电台的流量转发到互联网：

```
# macOS（en0：你的互联网连接）
sudo sysctl -w net.inet.ip.forwarding=1
echo "nat on en0 from 12.47.82.51 to any -> (en0)" | sudo pfctl -ef -
# Linux（eth0：你的互联网连接，usb0：电台）
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 12.47.82.51/32 -o eth0 -j MASQUERADE
sudo iptables -A FORWARD -i usb0 -o eth0 -j ACCEPT
sudo iptables -A FORWARD -i eth0 -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT
```

请用你电台的地址代替 12.47.82.51。然后给电台一个服务器的 IP 地址。电台无法解析名称。`dig +short rotate.aprs2.net` 可给出一些地址。你还可以给出它接收的消息和分组的范围（公里）：

```
curl "http://12.47.82.51/api/aprsis?server=44.31.12.34:14580&range=50"
curl "http://12.47.82.51/api/aprsis"              # 状态
curl "http://12.47.82.51/api/aprsis?server=off"   # 停止
```

电台用其 APRS 呼号和由呼号计算出的通行码登录。当 **USB 上的 KISS** 和“发送到 APRS-IS”开启时（`tools/rfoundry_debug.py aprs set to_is 1`），你的信标会发往 APRS-IS。

## 音频互联：AllStarLink 与 DVSwitch

电台在 UDP 端口 34001 上使用 USRP 语音协议，即 AllStarLink 的 USRP 信道驱动和 DVSwitch Analog_Bridge 所用的协议。电脑上的节点会听到电台接收的内容并通过它说话，如同一个射频链路：

- AllStarLink：在节点 `rpt.conf` 段中设置 `rxchannel = usrp/<电台地址>:34001:32001`。
- Analog_Bridge：`txPort = 34001`、`rxPort = 32001`，目的地为电台地址。

电台接收到信号时（静噪打开，或正在播放 DMR 呼叫），会把音频发送到电脑的 UDP 端口 32001，并以静噪作为按下标志。程序按下时，电台在当前信道上发射其音频：FM 上经电台自身的发射音频（适用其亚音和频偏限制），DMR 上作为发往信道联系人的呼叫。程序松开、停止发送或 3 分钟后，电台停止发射。接收音频跟随音量旋钮。`tools/usrp_client.py` 是一个小型测试程序（`tools/usrp_client.py <电台地址> tone 1000 3` 发送 3 秒音调）。

哪些电台能以这种方式发射 FM 见[电台参考](23-radios.md)；其他电台上 USRP 只承载 DMR。

## 经网络编程

电台在其地址上运行 TFTP 服务器。任何 TFTP 客户端都可用：

```
curl tftp://rfoundry-3101234.local/codeplug.bin -o backup.bin       # 读取电台频率
curl -T edited.bin tftp://rfoundry-3101234.local/codeplug.bin        # 写入它
curl tftp://rfoundry-3101234.local/info.txt                          # 板型、固件、ID、地址
```

`codeplug.bin` 与 `tools/cps_client.py read` 生成的 716,384 字节镜像相同。任一来源的文件都可用任一方式写入。写入在存储前会先检查。电台先检查大小，再确认文件是电台频率。文件错误、传输中途停止或传输中拔线都会让电台频率保持原样。存储新电台频率时断电，会在下次开机时完成。电台与使用 CPS 时一样保留自己的快捷键，并在写入后立即重新读取电台频率。

DMR ID 数据库（为不是联系人的 ID 显示名称）有自己的文件：

```
curl tftp://rfoundry-3101234.local/iddb.bin -o iddb.bin             # 读取 ID 数据库
curl -T iddb.bin tftp://rfoundry-3101234.local/iddb.bin              # 写入它
```

`iddb.bin` 只包含数据库。新数据库开始到达时，电台会丢弃旧数据库；整个文件到达后重新显示名称。传输中断会在下次尝试前没有数据库，绝不会留下损坏的数据库。这不需要暂存区，因此在无法写入 `codeplug.bin` 的电台上也可用。

电台频率编辑器用**保存 > 用于 USB 网络**保存的 `codeplug.bin`，在电台频率含 ID 数据库时也会携带该数据库。只有当客户端发送文件大小时（如 `curl` 所做），电台才会从中取用数据库。否则，以及使用任何较旧的 `codeplug.bin` 时，电台保留现有数据库。电台频率部分按上述方式写入。

要停止经网络的写入（`codeplug.bin` 和 `iddb.bin`），请关闭**设置 > 连接 > TFTP 写入电台频率**。读取仍然可用。

## 用 SNMP 监视电台

电台在其地址的端口 161 上应答 SNMP 2c 版请求（为 flash 空间省去 SNMP 的构建除外）。任何 SNMP 管理器都可用。仓库中有一个小的：

```
tools/rfoundry_snmp.py walk 12.47.82.51                 # 电台报告的所有内容
tools/rfoundry_snmp.py get 12.47.82.51 rfoundryBatteryVoltage.0 rfoundryChannelName.0
tools/rfoundry_snmp.py traps                            # 事件实时到达（需要端口 162：Linux 上为 root）
```

电台报告其身份（DMR ID、电台名、型号、固件）、电池、信道、频率、功率、正在进行的 DMR 呼叫和最后听到的呼叫、消息和数据计数器、ARS 状态以及 USB 网络计数器。这些对象在 `docs/spec/RFOUNDRY-MIB.txt` 中描述。把它载入你的管理器即可看到名称而不是数字。

USB 作为网卡时，电台还会向你的电脑发送事件（端口 162）：开机、呼叫开始和结束、收到文本消息、紧急、ARS 变化和低电量。

读团体名为 `public`。更改设置需要写团体名，在你设置之前处于关闭状态：

```
tools/rfoundry_debug.py snmp write MySecret             # 经串口，在切换到网络模式之前
tools/rfoundry_snmp.py -c MySecret set 12.47.82.51 sysName.0 s:N0CALL
tools/rfoundry_debug.py snmp status                     # 代理的设置与计数器
```

其他电台可在空中向其 12.x.y.z 地址查询本台（除非你允许写入，否则只读）。空中事件只发给你指定的管理器电台（`tools/rfoundry_debug.py snmp set rf_manager <DMR ID>`）。除紧急外，每分钟最多一次。没有加密。SNMP 团体名以明文传输。

## 经编程线的网络（PPP）

没有 USB 的电台（[电台参考](23-radios.md)）可通过编程线和 PPP 获得同样的网络。电台没有 USB 且运行 DMR 时，其镜像会包含 PPP。其他构建需要 `CONFIG_NET_PPP=y`。在电脑上以 root 运行以下命令。pppd 随 macOS 和大多数 Linux 系统提供。

```
sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                 # Linux
sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX       # macOS
```

pppd 启动时电台会自行切换到 PPP。它给电脑的地址与 USB 网络模式相同。上述服务工作方式相同，只是更慢（115200 波特时约 10 KB/s）。前面板页面可用，但没有“收听”按钮；在大尺寸彩色屏上，其屏幕更新可能需要几秒。`sudo tools/rfoundry_ppp.sh --stop` 挂断，线缆恢复为编程和调试链路。详情见 `docs/spec/ppp.md`。

## 供调度软件使用的控制站

调度软件（TRBOnet、SmartPTT 等）可把电台用作控制站。电脑随后经电台收发车队的消息、位置和注册。

1. 开启**启动时 USB 网络**并重启电台，或在**工具 > USB 网络**中切换。
2. 开启**PC 应答数据**。电脑连接期间，发往你电台 DMR ID 的文本消息、位置请求和注册应答只交给电脑。电台不再自行应答，不把消息放入收件箱，也不向在线状态服务器注册。它仍会确认要求确认的数据分组，仍会应答呼叫提示和电台检查。拔下电脑后，电台恢复自行应答。
3. 若软件要求固定地址，请把 **USB 地址**设为 192.168.10.x。此时电台为 192.168.10.1，电脑获得 192.168.10.2。拔插线缆以便电脑取得新地址。

在调度软件中设置：

| 项目 | 值 |
|---|---|
| 控制站地址 | 电台的地址：192.168.10.1，或由你的 DMR ID 翻转最低位得出的 12.x.y.z |
| 电台网络 | 12.0.0.0/8 |
| 通话组网络 | 225.0.0.0/8 |
| DMR ID | 你电台的 DMR ID |

电脑发送的内容会以你电台的 DMR ID 经空中发出。每台电脑只能有一台电台使用固定地址。

## 更短的分组与确认送达

发往其他电台的 UDP 分组使用压缩头（ETSI 的“UDP/IP 头压缩”）。头为 5 到 9 字节，而不是 28 字节。端口在 4000-4099 的分组保留完整头，因为 MOTOTRBO 文本、位置和在线状态服务器在那里监听。每个分组可节省一到两个数据块（空中 60–120 ms）。无法读取它的电台很少。为它们，或为压缩这些端口：

```
tools/rfoundry_debug.py data ip-settings off        # 始终使用完整头
tools/rfoundry_debug.py data ip-settings all        # 压缩每个 UDP 分组
```

发往电台的分组也可作为确认数据发送。对方电台确认每个分组，电台重发未到达的部分。默认总共尝试 4 次。最多可指定两个目的地（DMR ID）或 UDP 端口：

```
tools/rfoundry_debug.py data ip-settings auto rules id=3101234 port=161 tries=4
```

通话组分组绝不确认。发往不应答电台的分组会每次都发送后才丢弃，因此只在有帮助时启用确认。设置保存在电台中。

## 分组大小

网络链路接受最大 586 字节的分组。电台通过 DHCP 把该大小告诉电脑，并通过 PPP 作为其 MRU（584）告知；该大小的每个分组都能在一次空中传输中完成：最多 558 字节 UDP 数据（PPP 上为 556）。电脑上无需设置。

满尺寸分组会占用信道约 3 秒（发送确认时约 4 秒）。小分组更快，在弱信道上更可能送达：丢失一个突发就丢失整个分组。

## 经网关电台访问其他网络

默认情况下，电台只承载代表 DMR ID 的地址：电台为 12.x.y.z，通话组为 225.x.y.z。从电脑发往其他任何地址的分组都会被丢弃。

若你车队中的另一台电台是某个网络的网关（例如调度电脑上的 MOTOTRBO 控制站），请把该电台的 DMR ID 告诉本台：

```
tools/rfoundry_debug.py data ip-settings auto rules gateway=3100500
tools/rfoundry_debug.py data ip-settings auto rules gateway=0      # 再次关闭
```

该命令会一次性设置所有 IP 设置，因此请重复你使用的任何 `id=`、`port=` 或 `tries=`。设置保存在电台中。

然后在电脑上为该网络添加一条经电台地址的路由。例如，在 Linux 上以 `usb0` 为电台：

```
sudo ip route add 10.20.0.0/16 via 12.0.39.20 dev usb0     # 电台地址（见上文“地址”）
```

发往该网络的分组会经空中送到网关电台，分组中带有真正的目的地址。它们始终使用完整头，绝不使用压缩头。每个分组的生存时间（TTL）减一，TTL 为零的分组会被丢弃。电台把它收到的 TTL 传给电脑，因此困在两台网关电台之间循环的分组会耗尽。应答以发送者的真实地址回到你电台的 DMR ID，电台再把它们交给电脑。
