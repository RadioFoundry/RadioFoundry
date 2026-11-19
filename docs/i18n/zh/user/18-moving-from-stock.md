<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# 从原厂固件迁移

本章介绍如何把你的电台频率从制造商的固件带到 RadioFoundry：你的信道、区域、联系人、接收组列表和 DMR ID。

有两种方式：

- **网页安装器。**它备份电台，安装 RadioFoundry，然后转换电台频率并写回（[安装](02-installing.md#install-with-the-web-installer)中的**信道**步骤）。
  电台频率编辑器可以在之后转换备份（[文件](04-programming.md#files)）。除非你需要命令行，否则请使用这种方式。
- **迁移程序。**有些电台有一个程序，从命令行一次完成整个迁移，还会写入 DMR ID 数据库。[电台参考](23-radios.md)中你电台的章节列出了该程序、
  它需要什么，以及进入其更新模式的按键。

请保留你电台的制造商固件文件，以便回到原厂固件。

## 迁移程序做什么

1. 它从电台读取你的电台频率：信道、区域、联系人、接收组列表以及你的 DMR ID。
2. 它通过电台的引导程序（更新模式）安装 RadioFoundry。
3. 它写回电台频率，以及 DMR ID（呼号）数据库。

该程序从不更改电台的校准。

## 你需要什么

| 项目 | 说明 |
|---|---|
| Python 3.8 或更高版本 | |
| qdmr | 提供读取电台并转换电台频率的 `dmrconf` 命令。macOS：`brew install qdmr`。Linux：你的发行版的 `qdmr` 软件包。Windows：dm3mat.darc.de/qdmr 上的安装程序。你电台的章节会说明所需的版本 |
| Python 软件包 | 你电台的章节列出了它们，例如 `pip install pyserial` |
| RadioFoundry 镜像 | 来自发布页面或你自己的构建（[构建](../dev/building.md)）。已合并编解码器的镜像（`_codec.bin`）无需其他文件。对于不含编解码器的镜像，按你电台的章节所述提供原厂固件文件 |
| 编程线 | 电台的 USB 线，或没有 USB 的电台的编程线 |

测试者套件（`package`）是一个文件夹，含程序、其辅助文件、镜像、一个 `README.txt` 和一个 `requirements.txt`。

## 运行程序

示例中把程序称为 `MIGRATE.py`，把镜像称为 `IMAGE.bin`。请使用你电台的章节中的名称。

1. 先查看每个步骤。这只读取文件，不触碰任何东西：

   ```sh
   python3 MIGRATE.py all --image IMAGE.bin --dry-run
   ```

2. 正式运行：

   ```sh
   python3 MIGRATE.py all --image IMAGE.bin
   ```

在每个需要你操作电台的步骤之前，程序都会询问：

| 步骤 | 你做什么 | 程序做什么 |
|---|---|---|
| 0. 检查 | 无 | 检查 dmrconf、库、线缆和镜像。如果缺少什么，它在触碰电台之前停止 |
| 1. 备份 | 正常开启电台，插好线缆 | 读取电台频率（`backup-stock.dfu`、`backup-stock.yaml`），为 RadioFoundry 转换它，并显示能容纳哪些内容 |
| 2. 安装 | 关闭电台。按住你电台的更新模式按键并开机 | 再询问一次，然后写入 RadioFoundry |
| 3. 设置区 | 等待 RadioFoundry 主屏幕出现 | 检查电台是否运行 RadioFoundry。如果原厂固件的数据位于 RadioFoundry 保存设置的位置，它会询问，把该区域保存到文件，然后把该区域交给 RadioFoundry |
| 4. 电台频率 | 无 | 写入电台频率，等待重启，读回并比较 |
| 5. ID 数据库 | 无 | 写入 DMR ID 数据库。大的数据库需要几分钟 |

程序把所有内容保存在你主文件夹中的一个工作目录里，每次运行一个，并且从不删除这些文件。如果某个步骤失败，程序会停止并说明原因。
失败发生在安装之后时，它会打印继续执行的命令，例如：

```sh
python3 MIGRATE.py restore --workdir ~/<work directory>/<date-time>
```

## 从文件获取电台频率

如果你有电台频率文件，程序就不需要先读取电台。用 `--from` 提供它：

```sh
python3 MIGRATE.py all --from my-codeplug.dfu --image IMAGE.bin
```

程序接受 dmrconf 生成的 `.dfu` 文件，或 qdmr 生成的 `.yaml` 文件。你电台的章节列出了其他格式。要在没有电台的情况下转换并检查文件，
运行程序的 `backup --from FILE`（或在你电台的章节中列出时，运行 `import --from FILE`）。

## 能容纳什么

RadioFoundry 可容纳 1024 个信道、68 个各含 80 个信道的区域、1024 个联系人和 76 个接收组列表，名称最长 16 个字符。原厂固件可以容纳更多。
如果你的更多，程序会列出它将省去的内容（最后的那些），并在安装前询问。要自己选择，在 qdmr 中打开 `backup-stock.yaml` 并删除你不需要的内容。
保存它，然后对保存的文件用 `--from` 再次运行程序。

程序还会整理接收组列表。许多电台频率为每个信道设有一个只含该信道通话组的列表，因为原厂固件只收听信道列表中的通话组。
RadioFoundry 始终收听信道所通话的通话组，因此这些列表会被去掉，空的、重复的和未使用的列表也一样。每个信道都保留其通话组。
`--no-cleanup` 使列表保持原样。

RadioFoundry 使用[电台从电台频率中使用什么](04-programming.md#what-the-radio-uses-from-the-codeplug)中列出的电台频率部分。
扫描列表、GPS 系统以及原厂固件的菜单和按键设置不会迁移。

## DMR ID 数据库

程序以以下两种方式之一写入 RadioFoundry 的 DMR ID 数据库：

- 默认情况下，dmrconf 下载公开的 DMR 用户数据库，并挑选与你最接近的 ID。这需要互联网。
- `--users FILE` 改为使用用户 CSV 文件：RadioID.net 的 `user.csv`。程序会显示它读取的前几条记录。如果文件没有标题行且各列看起来不对，
  用 `--users-columns id,callsign,fname,surname,city,state,country` 给出列的顺序。

| 选项 | 作用 |
|---|---|
| `--db-limit N` | 最多写入 N 条记录 |
| `--id-prefix` | 按你所在国家的 ID 前缀挑选记录 |
| `--no-db` | 不写入数据库 |

## 回到原厂固件

制造商的固件文件通过同一个引导程序写入。你电台的章节给出了命令。然后用制造商的 CPS 或 qdmr 写入你的 `backup-stock.dfu` 或 `backup-stock.yaml`。

## 各操作系统的说明

- **Windows：**先安装引导程序或线缆的驱动程序。对于通过 USB 安装的电台，见 [Windows：引导程序驱动](02-installing.md#windows-the-bootloader-driver)。
- **Linux：**如果访问被拒绝，程序会打印需要添加的 udev 规则。要访问电台的串口或线缆，把自己加入 `dialout` 组。
- **macOS：**在有 USB 的电台上，其串口列出两次（`cu.usbmodem…` 和 `tty.usbmodem…`），`dmrconf` 无法在两者之间选择。
  程序按 USB ID 找到电台，并把 `cu.` 名称交给 `dmrconf`。`--port` 接受 `/dev/cu.usbmodem…` 或 `cu.usbmodem…`。连接两台电台时，
  程序会停止并要求提供 `--port`。`RFOUNDRY_PORT` 与 `--port` 作用相同。
