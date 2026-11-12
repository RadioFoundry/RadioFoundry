<!-- SPDX-License-Identifier: MIT -->

# 从原厂 MD-380 固件迁移

本章适用于运行 TYT 原厂固件的 TYT MD-380、MD-390 或 Retevis RT3。一个程序 `md380_migrate.py` 完成整个迁移：

1. 从电台读取你的电台频率：信道、区域、联系人、接收组列表、你的 DMR ID。
2. 刷写 RadioFoundry。
3. 写回电台频率，以及 DMR ID（呼号）数据库。

网页安装器做同样的事：备份电台、安装 RadioFoundry，然后转换电台频率并写回（[安装](02-installing.md)）。电台频率编辑器可稍后转换备份（[为电台编程](04-programming.md#files)）。要从命令行连同 ID 数据库一起完成，请使用本程序。电台上已验证的内容见[电台参考](23-radios.md#tyt-md-380)。请保留原厂固件文件，以便用 TYT 的升级工具回到原厂。

## 你需要什么

| 项目 | 说明 |
|---|---|
| Python 3.8 或更新 | macOS 和 Linux 仅用文件测试过，Windows 未测试 |
| qdmr 0.12 或更新 | 提供转换电台频率的 `dmrconf` 命令。macOS：`brew install qdmr`。Linux：发行版的 `qdmr` 包。Windows：dm3mat.darc.de/qdmr 的安装程序 |
| `pip install pyusb pyserial` 和 libusb | libusb：macOS `brew install libusb`，Linux `apt install libusb-1.0-0` |
| 适用于你频段的 RadioFoundry 镜像 | `rfoundry_md380_codec.bin`（含编解码器），或配合 `--stock` 的 `rfoundry_md380.bin`（工具会合并编解码器）。UHF 电台用 UHF 镜像，VHF 电台用 VHF 镜像。见[获取镜像](#getting-the-image) |
| TYT 的 `MD-380-D2.32(AD).bin`，或引导程序密钥文件 | 工具从原厂文件推导引导程序密钥并校验。密钥文件（`--key`）也可用 |
| 编程线 | 电台的 USB 线 |

原厂文件从哪里获取：TYT 下载页（[tyt888.com/download.html](https://www.tyt888.com/download.html)）把固件更新打包成 Windows 程序，`.bin` 在其中。[md380.org/firmware/orig](https://md380.org/firmware/orig/) 的归档以普通文件形式提供所有已知 MD-380 发行版。请使用 **D2.32**（`MD-380-D2.32(AD).bin`）：它同时提供引导程序密钥和语音编解码器。D2.30 只提供密钥。网页刷机工具也直接接受 TYT 的下载 zip，并按 SHA-256 校验每个文件。

测试包（`md380_migrate.py package`）是一个文件夹，含脚本、辅助文件、镜像、`README.txt` 和 `requirements.txt`。

## 获取镜像

发行页面提供各频段的镜像。自行构建（[构建](../dev/building.md)）：

```
make BOARD=md380                                         # UHF：build/md380/rfoundry_md380.bin
make BOARD=md380 BAND=vhf                                # VHF：build/md380-vhf/rfoundry_md380.bin
```

把 TYT 的 `MD-380-D2.32(AD).bin` 放入 `codecs/` 后，构建还会写出合并了编解码器的 `rfoundry_md380_codec.bin`。

## 运行

先查看每一步。此操作只读文件，不触碰任何东西：

```
python3 md380_migrate.py all --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin" --dry-run
```

然后实际运行：

```
python3 md380_migrate.py all --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

程序在需要你在电台旁操作的每一步之前询问：

| 步骤 | 你要做 | 程序会做 |
|---|---|---|
| 0. 检查 | 无 | 检查 dmrconf、USB 库、镜像和密钥。若有缺失，会在触碰电台前停止 |
| 1. 备份 | 正常开机，插好线 | 读取电台频率（`backup-stock.dfu`、`backup-stock.yaml`），为 RadioFoundry 转换并显示前后数量 |
| 2. 刷写 | 关机。按住 **PTT** 和**上侧键**，开机。屏幕保持黑屏 | 显示 USB 设备并再次询问，然后写入 RadioFoundry |
| 3. 设置区 | 等待 RadioFoundry 主屏幕 | 确认电台运行 RadioFoundry。原厂固件的数据位于 RadioFoundry 保存设置的位置，因此它会询问、把该区域保存到文件，并交给 RadioFoundry |
| 4. 电台频率 | 无 | 写入电台频率，等待重启，读回并比较 |
| 5. ID 数据库 | 无 | 写入 DMR ID 数据库。较大的数据库需要几分钟 |

它把所有内容保存在 `~/RadioFoundry-MD380-migration/<日期时间>/`，绝不删除这些文件。某步失败时会停止并说明原因。若失败发生在刷写之后，它会打印继续执行的命令：

```
python3 md380_migrate.py restore --workdir ~/RadioFoundry-MD380-migration/20261011-101500
```

## 从文件使用你的电台频率

如果你已有电台频率文件，程序无需先读取电台。用 `--from` 提供：

| 文件 | 来源 |
|---|---|
| `.rdt`（262,709 字节） | TYT CPS |
| `.dfu` | dmrconf |
| `.yaml` | qdmr |

```
python3 md380_migrate.py all --from my-codeplug.rdt --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

`md380_migrate.py backup --from FILE` 只转换并检查，无需电台。

## 容量

RadioFoundry 支持 1024 个信道、68 个区域（每区 80 个信道）、1024 个联系人和 76 个接收组列表。原厂 MD-380 最多支持 250 个区域和 250 个接收组列表。若你的超出，程序会列出将被略去的内容（最后那些），并在刷写前询问。要自行选择，请在 qdmr 中打开 `backup-stock.yaml` 并删除不需要的内容。保存后，用 `--from` 指向保存的文件再次运行。

程序还会整理接收组列表。TYT CPS 只收听信道列表中的通话组，因此许多电台频率为每个信道建一个只含该信道通话组的列表。RadioFoundry 总是收听信道通话的通话组，因此这些列表会被删除，空列表、重复列表和未使用的列表也一样。每个信道保留其通话组。`--no-cleanup` 保持列表原样。

RadioFoundry 使用[为电台编程](04-programming.md)中列出的电台频率部分。扫描列表、GPS 系统以及 TYT 的菜单和按键设置不会迁移。

## DMR ID 数据库

程序以两种方式之一写入 RadioFoundry 的 DMR ID 数据库：

- 默认情况下，dmrconf 下载公开的 DMR 用户数据库，并选取与你最接近的 ID（需要互联网）。
- `--users FILE` 改用用户 CSV：radioid.net 的 `user.csv`。程序会显示它读取的前几条记录。若文件没有表头且列看起来不对，请用 `--users-columns id,callsign,fname,surname,city,state,country` 给出顺序。

| 选项 | 作用 |
|---|---|
| `--db-limit N` | 最多 N 条记录 |
| `--id-prefix` | 按你所在国家的 ID 前缀选取记录 |
| `--no-db` | 不写数据库 |

## 回到原厂

TYT 的升级文件通过同一个引导程序刷写：

```
python3 md380_migrate.py flash --image "MD-380-D2.32(AD).bin"
```

然后用 TYT CPS 或 qdmr（`dmrconf write --radio md390`）写入你的 `backup-stock.rdt` 或 `backup-stock.dfu`。

## 限制与说明

- 一些早期 VHF 电台只有 1 MB 的小闪存。RadioFoundry 可在其上运行，但不保存电台频率或设置。程序会在刷写后停止并告知你。若需要你的信道，请回到原厂。
- 程序绝不更改电台的校准。
- Windows（未测试）：刷写前需用 Zadig 把引导程序（0483:DF11）绑定到 WinUSB（[步骤](02-installing.md#windows-the-bootloader-driver)）。这会替换 TYT CPS 用于同一 USB ID 的驱动，因此请先运行备份步骤。
- Linux：若访问被拒绝，程序会打印 0483:DF11 的 udev 规则。电台运行 RadioFoundry 后，请把自己加入 `dialout` 组以访问串口。
