<!-- SPDX-License-Identifier: MIT -->

# 从原厂 DM-32UV 固件迁移

本章适用于运行宝锋原厂固件的宝锋 DM-32UV。一个程序 `dm32_migrate.py` 通过编程线完成整个迁移：

1. 从电台读取你的电台频率：信道、区域、联系人、接收组列表、你的 DMR ID。
2. 通过电台自带的引导程序（更新模式）刷写 RadioFoundry。
3. 写回电台频率，以及 DMR ID（呼号）数据库。

网页安装器和电台频率编辑器分两步完成同样的事（[安装](02-installing.md)）。使用本程序可一次运行完成全部。电台上已验证的内容见[电台参考](23-radios.md#baofeng-dm-32uv)，[bring-up 检查表](../testing/dm32-bringup.md)列出尚未完成的项目。请保留宝锋的固件文件（`DM32.01.01.049.bin`），以便回到原厂。

RadioFoundry 绝不触碰引导程序，因此更新模式始终可用。它也绝不触碰 7 MB 以下的数据闪存。原厂电台频率、出厂校准和原厂联系人数据库都在那里。回到原厂时，你的旧电台频率会原样恢复。

## 你需要什么

| 项目 | 说明 |
|---|---|
| Python 3.8 或更新 | macOS、Linux 和 Windows。仅用文件测试过 |
| qdmr 0.15 或更新 | 提供读取原厂电台并转换电台频率的 `dmrconf` 命令。macOS：`brew install qdmr`。Linux：发行版的 `qdmr` 包。Windows：dm3mat.darc.de/qdmr 的安装程序 |
| `pip install pyserial` | 用于线缆 |
| RadioFoundry 镜像 | 已合并编解码器的 `rfoundry_dm32_codec.bin`，来自测试包或你自己的构建（[bring-up 检查表](../testing/dm32-bringup.md)第 0 步）。不含编解码器的镜像可配合 `--codec-from DM32.01.01.049.bin` 使用 |
| 编程线 | DM-32 的 USB 串口线（CH340、FTDI 或 CP210x）。电台本身没有 USB：原厂 CPS、引导程序和 RadioFoundry 都使用这条线 |

测试包（`dm32_migrate.py package`）是一个文件夹，含脚本、辅助文件、镜像、`README.txt` 和 `requirements.txt`。

## 运行

先查看每一步。此操作只读文件，不触碰任何东西：

```
python3 dm32_migrate.py all --image rfoundry_dm32_codec.bin --dry-run
```

然后实际运行。插入一条 USB 串口线时，程序会自动找到它。否则请加 `--port /dev/ttyUSB0`（Linux）、`--port /dev/cu.usbserial-XXXX`（macOS）或 `--port COM5`（Windows）：

```
python3 dm32_migrate.py all --image rfoundry_dm32_codec.bin
```

程序在需要你在电台旁操作的每一步之前询问：

| 步骤 | 你要做 | 程序会做 |
|---|---|---|
| 0. 检查 | 无 | 检查 dmrconf、pyserial、线缆和镜像。若有缺失，会在触碰电台前停止 |
| 1. 备份 | 正常开机，插好线 | 读取电台频率（`backup-stock.dfu`、`backup-stock.yaml`），为 RadioFoundry 转换，并显示哪些能放下、哪些不能（`conversion-report.txt`） |
| 2. 刷写 | 关机。按住 **PTT** 和 **SK1**（PTT 下方的侧键），开机。绿色 LED 点亮 | 再次询问，然后写入 RadioFoundry。电台会先擦除旧固件，最多需要 15 秒 |
| 3. 设置区 | 等待 RadioFoundry 主屏幕 | 确认电台运行 RadioFoundry。若原厂数据位于 RadioFoundry 保存设置的位置，它会询问、把该区域保存到文件并交给 RadioFoundry |
| 3b. 数据闪存备份 | 无 | 把原厂电台频率和校准区域（800 KB，约 2 分钟）读入工作目录。`--flash-backup full` 保存全部 16 MB（约 30 分钟），`none` 跳过 |
| 4. 电台频率 | 无 | 保存 RadioFoundry 电台频率区域中的内容，写入电台频率，等待重启，读回并比较 |
| 5. ID 数据库 | 无 | 写入 DMR ID 数据库。较大的数据库经线缆需要许多分钟 |

它把所有内容保存在 `~/RadioFoundry-DM32-migration/<日期时间>/`，绝不删除这些文件。某步失败时会停止并说明原因。若失败发生在刷写之后，它会打印继续执行的命令：

```
python3 dm32_migrate.py restore --workdir ~/RadioFoundry-DM32-migration/20261011-101500
```

## 从文件使用你的电台频率

`--from` 接受用 qdmr 从原厂电台保存的电台频率，可以是 `.dfu`（`dmrconf read --radio dm32uv`）或 `.yaml`。`dm32_migrate.py import --from FILE` 只转换并检查，无需电台。此处不支持宝锋自带 CPS 的文件。请用 qdmr 读取电台并从那里保存电台频率。

网页安装器在从原厂固件安装结束时自行迁移这些内容。没有 qdmr 时，[电台频率编辑器](04-programming.md#the-codeplug-editor)和工具页可直接从数据闪存读取来转换原厂电台频率：原厂固件上由网页安装器取得的备份 `.zip`，或其中的 `data-memory.bin` 或 `original-channels.bin`。它们会迁移带发射联系人的信道、联系人、接收组列表、区域、你的 DMR ID 和启动文字。功率、带宽、亚音、时隙和色码随每个信道迁移。静噪、发射超时和扫描列表保持 RadioFoundry 的默认值，且前 84 个之后的信道尚未读取（[格式](../spec/dm32-stock-codeplug.md)）。qdmr 0.15 不读取原厂发射联系人，因此这是保留它们的方式。

## 容量

RadioFoundry 支持 1024 个信道、68 个区域（每区 80 个信道）、1024 个联系人和 76 个接收组列表，名称最多 16 个字符。原厂电台支持更多。若你的超出，程序会列出将被略去的内容（最后那些），并在刷写前询问。要自行选择，请在 qdmr 中打开 `backup-stock.yaml` 并删除不需要的内容。保存后，用 `--from` 指向保存的文件再次运行。

程序还会整理接收组列表：只含信道自身通话组的列表会被删除（RadioFoundry 总是收听信道通话的通话组），空列表、重复列表和未使用的列表也一样。每个信道保留其通话组。`--no-cleanup` 保持列表原样。

`conversion-report.txt` 列出所有变化：前后数量、被缩短的名称、136-174 和 400-480 MHz 之外的信道，以及 RadioFoundry 不使用的电台频率部分。这些部分不会迁移：原厂菜单、按键和显示设置、扫描列表、漫游、GPS 和 APRS 设置、加密密钥以及原厂联系人数据库。RadioFoundry 使用[为电台编程](04-programming.md)中列出的电台频率部分。

## DMR ID 数据库

默认情况下，dmrconf 下载公开的 DMR 用户数据库，并选取与你最接近的 ID（需要互联网）。电台可容纳约 300,000 条记录。列表更长时，工具保留与你的 ID 最接近的记录。`--users FILE` 改用 radioid.net 的 `user.csv`。选项：`--db-limit N`、`--id-prefix`、`--no-db`。

## 仅刷写

`serial_loader.py` 可刷写任意镜像而不触碰电台频率：

```
python3 serial_loader.py --model dm32 rfoundry_dm32_codec.bin   # 原始镜像（_BFUV32.bin 文件无需 --model）
python3 serial_loader.py --list-ports                       # 它看到的线缆
```

## 回到原厂

宝锋的固件文件通过同一个引导程序刷写：

```
python3 serial_loader.py --stock DM32.01.01.049.bin
```

原厂电台频率从未被覆盖，因此电台会带着它回来。若没有，请用 qdmr 写入 `backup-stock.dfu`（`dmrconf write --radio dm32uv`）。RadioFoundry 自己的数据保持在 7 MB 及以上，原厂固件以后可能会复用该空间。

## 限制与说明

- 若刷写后电台无法启动，请取出并重新装上电池。更新模式仍然可用。
- 程序绝不更改电台的校准。
- Linux：请把自己加入 `dialout` 组以使用线缆。
- Windows：请先安装线缆驱动（CH340 或 FTDI）。未测试。
