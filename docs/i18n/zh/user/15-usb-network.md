<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# USB 网络模式

在 USB 网络模式中，电台是电脑的 USB 网卡。它通过 DMR 分组数据把 IPv4 分组路由到其他电台，并向电脑提供状态页面、电台控制、KISS、
TFTP 和 SNMP。有 USB 的电台的镜像包含此功能。哪款电台有哪些服务，见[功能矩阵](../feature-matrix.md)。不含此功能的构建
（`CONFIG_DMR_IP_GATEWAY=n`）在**工具 > USB 网络**中显示“USB 网络：本版本无”。

链路、地址和各项服务如何工作，见 [USB 网络](../spec/usb-network.md)。

## 把电台切换到网络模式

电台启动时是 USB 串口，除非**启动时 USB 网络**为开（[以网络模式启动](#starting-in-network-mode)）。

1. 打开**工具 > USB 网络**（**OK**、**6**、**4**）。页面说明 USB 当前处于哪种模式，以及会有什么变化。
2. 选中“USB 切到网络”并按 **OK**。电台显示“USB 现为网卡”并返回主屏幕，状态行显示 IP。电脑设置好网卡、IP 可以传输后，
   旁边出现一个上箭头和一个下箭头。

USB 作为网卡期间，qdmr、热点主机以及经串口的调试链路无法连接电台。

要回到串口，再次打开**工具 > USB 网络**并选择“USB 切到串口”（“USB 已恢复为串口”），或重启电台。**工具**中的这一行显示当前模式（串口或网络）。

开发者也可以通过调试链路切换：

```sh
tools/rfoundry_debug.py radio usb network
```

电台短暂从 USB 断开，然后作为网卡重新出现（USB ID 1FC9:0096）。Linux、macOS 以及 Windows 10 和 11 使用其自带驱动。
如果电脑在 10 秒内没有设置好网卡，电台回到串口模式。

## 以网络模式启动

**设置 > 连接 > 启动时 USB 网络**使电台以网卡启动。如果插入后 10 秒内电脑没有设置好网卡，电台在重启前重新成为串口。
这样你总能连上它并关闭该设置。电台会等待电脑：远离电脑时开机的电台，之后插入时仍是网卡。

## 地址

电脑通过 DHCP 从电台获取地址：

| 对象 | 地址 |
|---|---|
| 你的电脑 | 12.x.y.z，由你的 DMR ID 得出（例如 ID 9991234 为 12.152.116.66） |
| 电台 | 你的电脑地址，最后一位取反（12.152.116.67） |
| 另一台电台 | 12.x.y.z，由其 DMR ID 得出 |
| 通话组 | 225.x.y.z，由通话组号码得出 |

发往另一台电台或通话组地址的分组在当前 DMR 信道上经空中发出。来自其他电台的分组到达网卡。DMR 分组数据比任何有线网络都慢得多。
2 秒内无法发出的分组会被丢弃。[控制站模式](#control-station-for-dispatch-software)的固定地址取代前两行。

## 网络模式下的串口工具

调试链路、MMDVM 热点协议和 CPS 协议仍可经电台地址的 UDP 端口 3334 访问：

```sh
tools/rfoundry_debug.py --port udp:12.152.116.67 version
```

MMDVMHost 以同样方式连接（`Protocol=udp`、`ModemAddress=<radio address>`、`ModemPort=3334`）。qdmr 只按串口 USB ID 查找电台，
因此请在串口模式下为电台编程。

项目自己的工具无需地址，也无需在电台上做任何更改：

| 工具 | 网络模式下 |
|---|---|
| `tools/rfoundry_debug.py`、`tools/cps_client.py`、`rfoundry.py` | 没有 `--port` 且没有插入串口电台时，它们在网络链路上找到电台并使用它 |
| `stub/stubtool.py --rfoundry-enter --write-app ...`（无端口） | 更新固件并重启电台，电台以网络模式回来 |
| 网页安装器和电台频率编辑器 | 它们请求电台在操作期间成为串口。有些浏览器这样做时不显示任何内容。否则，按**将电台切换为 USB 串口**：一个标签页在电台自己的页面上打开（`http://12.255.255.254/`，每台电台在其 USB 链路上的地址），然后再关闭。然后连接。电台重启时回到网络模式 |

如果该按钮无法连接电台（标签页显示错误，因为电脑没有设置好网卡，或把 12.x 地址发往别处），请在电台上切换：**工具 > USB 网络**。

## 按名称查找电台

电台在网卡上响应多播 DNS（Bonjour、Avahi）。其名称为 `rfoundry-<DMR ID>.local`，例如 `rfoundry-9991234.local`。
Bonjour 服务浏览器按你的呼号、电台型号和你的 DMR ID 列出电台，例如“Q0TST XR-100UV (9991234)”：

```sh
ping rfoundry-9991234.local
dns-sd -B _rfoundry-debug._udp            # macOS; avahi-browse -r _rfoundry-debug._udp on Linux
tools/rfoundry_debug.py --port udp:rfoundry-9991234.local version
```

电台通告的服务见 [mDNS](../spec/mdns.md)。

## 电台上的服务

电台在自己的地址上响应以下 TCP 端口，只经 USB 线，从不经空中。详情见 [TCP 服务](../spec/tcp-services.md)。

| 端口 | 服务 |
|---|---|
| 80 | 电台的前面板。在浏览器中打开 `http://<radio address>/`。它显示电台及其实时屏幕和 LED，以及信道、频率、信号和电池。在车台上，页面还画出其手咪。点击按键或使用键盘。侧键和顶键在电台边缘外还有带标签的按钮，信道旋钮上方的箭头每点击一次转动一步（按住持续转动）。键盘按键：数字、`*` 和 `#`；方向键；Enter 为确认（绿键或 MENU）；Esc 或 Backspace 为返回（红键或 BACK）；`[` 和 `]`（或 Page Up 和 Page Down）转动信道旋钮；按住空格键讲话。页面在图片旁列出你的电台具有的键盘按键。在支持网络音频的电台上（[音频互联](#linking-audio-allstarlink-and-dvswitch)），**Listen** 播放电台接收的内容。经编程线（[PPP](#network-over-the-programming-cable-ppp)）时，页面没有 **Listen**，因为编程线太慢，无法传输音频。按键只在电台所插的电脑上有效。同一时间只有一个页面控制电台；其他页面只显示状态。程序可以读取 `/status`（JSON），以及 `/api/view?section=core` 中调试链路的各部分 |
| 8001 | 供 APRS 程序（APRSdroid、Xastir、YAAC）使用的 TCP 上的 KISS。把程序的 TNC 设为“KISS over TCP”、电台地址和端口 8001。先在**工具 > APRS** 中打开 **USB KISS**。电台只发送来自你自己呼号的分组。在不能接收 APRS 的电台上（[电台参考](23-radios.md)），程序只看到它们自己发送的内容 |
| 4532 | 供日志、SDR 和卫星程序使用的 Hamlib 电台控制。选择“Hamlib NET rigctl”，填入电台地址和端口 4532，或运行 `rigctl -m 2 -r <radio address>:4532 f`。程序可以读取和设置频率，读取模式和 S 表，并控制发射。设置信道的频率会切换到 VFO。发射只在你的电脑上有效，每次最多 3 分钟 |

### 从电台连接 APRS-IS

电台可以自己连接到 APRS-IS 服务器，并把你的位置发送到那里。

1. 让你的电脑把电台的流量转发到互联网：

   ```sh
   # macOS (en0: your internet connection)
   sudo sysctl -w net.inet.ip.forwarding=1
   echo "nat on en0 from 12.152.116.67 to any -> (en0)" | sudo pfctl -ef -
   # Linux (eth0: your internet connection, usb0: the radio)
   sudo sysctl -w net.ipv4.ip_forward=1
   sudo iptables -t nat -A POSTROUTING -s 12.152.116.67/32 -o eth0 -j MASQUERADE
   sudo iptables -A FORWARD -i usb0 -o eth0 -j ACCEPT
   sudo iptables -A FORWARD -i eth0 -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT
   ```

   用你的电台地址替换 12.152.116.67。
2. 给电台一个服务器的 IP 地址。电台不能查询名称；`dig +short rotate.aprs2.net` 会给出一些地址。你还可以为它接收的消息和分组指定一个以 km 计的范围：

   ```sh
   curl "http://12.152.116.67/api/aprsis?server=44.31.12.34:14580&range=50"
   curl "http://12.152.116.67/api/aprsis"              # the state
   curl "http://12.152.116.67/api/aprsis?server=off"   # stop
   ```

电台以其 APRS 呼号和由此计算出的密码登录。**USB KISS** 和“to APRS-IS”都打开时（`tools/rfoundry_debug.py aprs set to_is 1`），
你的信标发往 APRS-IS。

## 音频互联：AllStarLink 与 DVSwitch

电台在 UDP 端口 34001 上使用 USRP 语音协议，即 AllStarLink 的 USRP 信道驱动和 DVSwitch Analog_Bridge 使用的协议。
你电脑上的节点听到电台接收的内容，并通过它讲话，就像一条射频链路：

- AllStarLink：在节点的 `rpt.conf` 部分中写 `rxchannel = usrp/<radio address>:34001:32001`。
- Analog_Bridge：`txPort = 34001`、`rxPort = 32001`，目的地址为电台的地址。

电台接收到信号时（静噪打开，或正在播放 DMR 呼叫），它把音频发送到你电脑的 UDP 端口 32001，并以静噪作为 keyup 标志。
程序 keyup 时，电台在当前信道上发射其音频：在 FM 上经电台自己的发射音频通路（其亚音和频偏限制适用），在 DMR 上作为对信道联系人的呼叫。
程序取消 keyup、停止发送或 3 分钟后，电台停止发射。接收的音频跟随音量旋钮。`tools/usrp_client.py` 是一个小测试程序
（`tools/usrp_client.py <radio address> tone 1000 3` 发射一个音调 3 秒）。

哪些电台可以这样发射 FM，见[功能矩阵](../feature-matrix.md)。在其他电台上，USRP 只传输 DMR。

## 经网络编程

电台在其地址上运行 TFTP 服务器。任何 TFTP 客户端都可以：

```sh
curl tftp://rfoundry-9991234.local/codeplug.bin -o backup.bin       # read the codeplug
curl -T edited.bin tftp://rfoundry-9991234.local/codeplug.bin        # write it
curl tftp://rfoundry-9991234.local/info.txt                          # board, firmware, IDs, addresses
```

`codeplug.bin` 与 `tools/cps_client.py read` 生成的映像相同，因此任一方式得到的文件都可以用任一方式写入。电台在存储任何内容前先检查写入：
先检查大小，再检查文件是否为电台频率。错误的文件、中途停止的传输或传输中拔出的线缆都会使电台频率保持原样。如果电台存储新电台频率时断电，
电台会在下次开机时完成。与 CPS 一样，电台保留自己的快捷键，并在写入后立即重新读取电台频率。

DMR ID 数据库（为不是联系人的 ID 显示的名称）有自己的文件：

```sh
curl tftp://rfoundry-9991234.local/iddb.bin -o iddb.bin             # read the ID database
curl -T iddb.bin tftp://rfoundry-9991234.local/iddb.bin              # write it
```

`iddb.bin` 只包含数据库。新数据库开始到达时，电台丢弃旧数据库，整个文件到齐后再次显示名称。中途停止的传输会使电台在下次尝试前没有数据库，
但绝不会留下损坏的数据库。这不需要暂存区，因此在无法写入 `codeplug.bin` 的电台上也可用。

电台频率编辑器用**保存 > 用于 USB 网络（codeplug.bin）**保存的 `codeplug.bin`，在电台频率含有 ID 数据库时也带有它。只有客户端发送文件大小时
（`curl` 会这样做），电台才从中取用数据库。否则，以及对任何较旧的 `codeplug.bin`，电台保留它已有的数据库。电台频率部分按前述方式写入。

要阻止经网络写入（`codeplug.bin` 和 `iddb.bin`），关闭**设置 > 连接 > TFTP 写入电台频率**。你仍可读取。

详情见 [TFTP](../spec/tftp.md)。

## 用 SNMP 监视电台

电台在其地址上响应 SNMP 第 2c 版请求，除非构建为节省闪存空间而省去 SNMP。任何 SNMP 管理程序都可以。代码库中有一个小程序：

```sh
tools/rfoundry_snmp.py walk 12.152.116.67               # everything the radio reports
tools/rfoundry_snmp.py get 12.152.116.67 rfoundryBatteryVoltage.0 rfoundryChannelName.0
tools/rfoundry_snmp.py traps                            # events as they happen (needs port 162: root on Linux)
```

电台报告其身份、电池、信道、频率、功率、呼叫、消息和数据计数、ARS 状态以及 USB 网络计数。它还把呼叫、消息、紧急报警和低电量等事件发送到你的电脑。
要看到名称而不是数字，把 `docs/spec/RFOUNDRY-MIB.txt` 加载到你的管理程序中。

读团体名是 `public`。更改设置需要写团体名，在你设置之前它是关闭的：

```sh
tools/rfoundry_debug.py snmp write MySecret             # over the serial port, before switching to network mode
tools/rfoundry_snmp.py -c MySecret set 12.152.116.67 sysName.0 s:Q0TST
tools/rfoundry_debug.py snmp status                     # the agent's settings and counters
```

没有任何加密：SNMP 团体名以明文传输。对象、事件以及经空中来自其他电台的查询，见 [SNMP](../spec/snmp.md)。

## 经编程线的网络（PPP）

没有 USB 的电台（[电台参考](23-radios.md)）可以用 PPP 经编程线获得同样的网络。电台没有 USB 且运行 DMR 时，其镜像包含 PPP。
其他构建需要 `CONFIG_NET_PPP=y`。在电脑上以 root 运行以下命令。macOS 和大多数 Linux 系统自带 pppd。

```sh
sudo tools/rfoundry_ppp.sh /dev/ttyUSB0                 # Linux
sudo tools/rfoundry_ppp.sh /dev/cu.usbserial-XXXX       # macOS
```

pppd 启动时，电台自行切换到 PPP，并给电脑与 USB 网络模式下相同的地址。各项服务工作方式相同，只是更慢（115200 波特时约 10 KB/s）。
前面板页面没有 **Listen** 按钮也能工作，在大尺寸彩色屏幕上其屏幕最多需要几秒才更新。要挂断，运行 `sudo tools/rfoundry_ppp.sh --stop`；
编程线回到编程和调试链路。详情见 [PPP](../spec/ppp.md)。

## 供调度软件使用的控制站

调度软件（TRBOnet、SmartPTT 等）可以把电台用作其控制站。电脑随后通过电台收发整个车队的消息、位置和登记。

1. 打开**启动时 USB 网络**并重启电台，或在**工具 > USB 网络**中切换。
2. 打开 **PC 应答数据**。电脑连接期间，发往你电台 DMR ID 的短信、位置请求和登记应答只发往电脑。电台不再自己应答，不把消息放入收件箱，
   也不向在线状态服务器登记。它仍确认要求确认的数据分组，仍应答呼叫提示和电台检查。拔下电脑后，电台重新自己应答。
3. 如果软件需要固定地址，把 **USB 地址**设为 192.168.10.x。电台随后为 192.168.10.1，电脑得到 192.168.10.2。拔下线缆再插上，
   让电脑获取新地址。

在调度软件中设置以下值：

| 项目 | 值 |
|---|---|
| 控制站地址 | 电台的地址：192.168.10.1，或由你的 DMR ID 得出、最后一位取反的 12.x.y.z |
| 电台网络 | 12.0.0.0/8 |
| 通话组网络 | 225.0.0.0/8 |
| DMR ID | 你电台的 DMR ID |

电脑发送的内容从你电台的 DMR ID 经空中发出。每台电脑只有一台电台可以使用固定地址。

## 确认送达

发往电台的分组可以作为确认数据发送。另一台电台确认每个分组，电台重发未到达的部分。默认总共尝试 4 次。最多指定两个目的地（DMR ID）或 UDP 端口：

```sh
tools/rfoundry_debug.py data ip-settings auto rules id=9991234 port=161 tries=4
```

通话组分组从不确认。发往不应答电台的分组在丢弃前每次尝试都会发送，因此只在有帮助的地方使用确认。电台会保存这些设置。

电台还会缩短发往其他电台的 UDP 分组的报头，并让每个分组在空中只占一次发射。满长分组占用信道约 3 秒（确认发送时约 4 秒）。
小分组更快，在弱信道上更容易送达，因为丢失一个突发就会丢失整个分组。电脑上无需任何设置。报头压缩和分组大小见 [USB 网络](../spec/usb-network.md)。

## 经网关电台访问其他网络

默认情况下，电台只传输代表 DMR ID 的地址：电台为 12.x.y.z，通话组为 225.x.y.z。它丢弃电脑发往任何其他地址的分组。

如果你车队中的另一台电台是某个网络的网关（例如调度电脑上的一台商用控制站），在以下任一处为电台填入那台电台的 DMR ID：

- **设置 > 连接 > 数据服务 > IP 网关**（0 为关）。
- [电台频率编辑器](04-programming.md)。
- 调试链路：

  ```sh
  tools/rfoundry_debug.py data ip-settings auto rules gateway=9990500
  tools/rfoundry_debug.py data ip-settings auto rules gateway=0      # off again
  ```

  该命令一次设置所有 IP 设置，因此请重复你使用的任何 `id=`、`port=` 或 `tries=`。电台会保存该设置。

然后在电脑上为该网络添加一条经电台地址的路由。例如，在 Linux 上电台为 `usb0` 时：

```sh
sudo ip route add 10.20.0.0/16 via 12.152.116.67 dev usb0   # the radio's address (see Addresses)
```

发往该网络的分组经空中到达网关电台，分组中带有真实的目的地址。应答以发送方的真实地址返回到你电台的 DMR ID，电台把它们传给电脑。
网关如何处理报头和生存时间，见 [USB 网络](../spec/usb-network.md)。
