#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Merge an HR_C7000 radio's own stock AMBE+2 codec into a RadioFoundry image, on your machine.

The radios differ only in where things are (BOARDS: the slot and the windows its linker script reserves; PROFILES: where
a stock file keeps the codec). The recipe, for the DM-32UV from docs/spec/codec-dm32.md ("Where the codec is",
"Recommended hosting") and for the DB25-D from docs/spec/hardware-db25d.md 8:
  1. check the stock file by SHA-256 (DM-32UV: Baofeng DM32.01.01.049.bin or DM32.00.01.046HRVocoder.bin, "BFUV32-V2"
     header; DB25-D PRO: a plain 909E.D6x.EARSAB.018 image of 2024-05-09 or later);
  2. carve the codec code window and the codec table window (file offset = address - 0x0300C000 + the file's header);
  3. check the code's first two words (0x152514D8, 0x74C776CD);
  4. write a new raw image: the built image from 0x0300C000 (the DB25-D's includes its first stage), 0xFF padding,
     then the codec slot at 0x030E8000: a 128-byte header (platform/hrc7000/codec_slot.h, codec_slot_t), the code, the
     tables and the relocation list. The code and tables are the stock bytes. At boot the firmware copies the code to
     the start of the board's codec window and moves the words the list names (the build's codec map, self_refs:
     docs/spec/codec-placement.md), and copies the tables to their stock modem SRAM address.

The codec is DVSI's and Baofeng's, not ours: it is never stored in this repository or in a release. This tool reads
your stock file and writes the merged image to a file you choose (by default next to the built image, in build/, which
git ignores). Wrap the result for the vendor's updater with tools/serial_loader.py --wrap.
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rfoundry"))
from rfoundry import boards, codecmap  # noqa: E402  (the board registry; the codec maps: which words of each build hold addresses)

APP_BASE = 0x0300C000
STOCK_HEADER = 0x100
MAGIC = b"BFUV32-V2"
SLOT_MAGIC = 0x43524444  # "DDRC"
SLOT_VERSION = 2   # the only one: code at the window's start, the listed words moved (platform/hrc7000/codec_slot.h)
REL_MAX = 64
HEADER_LEN = 128
CODEC_SIG = (0x152514D8, 0x74C776CD)

# Per radio, from the board registry (the __codec_* symbols of the board's linker script, its [flash.stub]): the codec
# slot in program flash, the codec window at the top of SRAM (the slot's code runs at its start; sized for the largest
# code among the board's maps), the modem SRAM table window (docs/spec/codec-placement.md), and what the web flasher
# needs beyond the codec (site/rfoundrysite/repo.py c7000_flash): the program flash size, whether the SPI0 chip select the
# data flash sits on has one (not on a radio with the SPI NAND there). The image starts at app_base.
BOARDS = {b.id: {"slot": b.codec_slot, "run": b.codec_run, "tables": b.codec_tables, "app_base": b.app_base,
                 "progmem_size": b.stub["progmem"] if b.stub else 0x100000,
                 "data_flash": not (b.stub and b.stub["nand"])}
          for b in boards.BOARDS.values() if b.codec_tool == "hrc7000_codec"}
PROGMEM_BASE = 0x03000000   # a program flash dump starts here

# Entry points, offsets from the codec start (codec-dm32.md "Entry points"); fec_init differs per version
ENTRIES = ("decode", "encode", "dec_init", "dec_set", "enc_init", "enc_set", "fec_init")

# The 049 entry points: every DM-32UV stock build from 032 to 049 carries the same codec code, byte for byte, except
# for its table pointers (the NRF and fanti builds' tables sit 8 bytes higher) and stock code that lies inside the
# window but isn't codec (codec-dm32.md "Stock versions")
ENTRIES_049 = {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
               "enc_set": 0x2E4, "fec_init": 0x1BF8}

PROFILES = {
    "03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a": {
        "name": "DM32.01.01.049.bin", "board": "dm32", "tag": 49, "size": 869688,
        "code": (0x030C687C, 0x14600, 0x00016CDC),       # flash address, length, run address
        "tables": (0x03072628, 0x2F78, 0x18000EA8),
        "code_sha256": "355ca9061979eceb2991826da9d39c95b4a9d40466245923d541a6556ceeafd1",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1BF8},
    },
    "f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887": {
        "name": "DM32.00.01.046HRVocoder.bin", "board": "dm32", "tag": 46, "size": 890516,
        "code": (0x030CB13C, 0x140DC, 0x0001B838),
        "tables": (0x030723A8, 0x2F78, 0x18000E48),
        "code_sha256": "02743cf4b3d252b8764fe083fb808b00ab42f0f0ab76935e62ca59fd5c5e64fb",
        "tables_sha256": "cd5a4d270f555cdbd586bca8de89e4e845789ed7aa2a2bb7414b6c3a270ef803",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1DCC},
    },
    "5ee2cb549792d5596fa50bb826613cde212e38f58b10b0084fdfd7b6032ec44d": {
        "name": "DM32.01.01.032.bin", "board": "dm32", "tag": 32, "size": 872708,
        "code": (0x030C74B4, 0x14600, 0x00016CDC),
        "tables": (0x03071908, 0x2F78, 0x18000EA8),
        "code_sha256": "cc17ee0bf3ee45d16912e44351a3434fc1ebcbc6e22ef3e42463e394624bb164",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "249bcd25a021bb5e384633232c3a2da5fb5cf0881d08fc4ab573faaf3a1fb28a": {
        "name": "DM32.01.01.037.bin", "board": "dm32", "tag": 37, "size": 867280,
        "code": (0x030C5F08, 0x14600, 0x00016CDC),
        "tables": (0x03071D68, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "101dd348b11b21ba67274d40e1b2cc8248863e442c7da0471bc9f7db09f61f6b": {
        "name": "DM32.01.01.039.bin", "board": "dm32", "tag": 39, "size": 868572,
        "code": (0x030C6414, 0x14600, 0x00016CDC),
        "tables": (0x03072228, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "016962d82b926e489e729abee46b13f97d223f8e97e07fe3b8bcba1dd1e37058": {
        "name": "DM32.01.01.040.bin", "board": "dm32", "tag": 40, "size": 868588,
        "code": (0x030C6424, 0x14600, 0x00016CDC),
        "tables": (0x03072238, 0x2F78, 0x18000EA8),
        "code_sha256": "6b1c46cb4bfefbbe9fe5d2d47d43624a4854fa4d52d5db29b75a5c968d68a065",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "ab21bc8dedb225a3e48c72e07af029fd0c453f00018f3b83a062fd0b9a3e3049": {
        "name": "DM32.01.01.046.bin", "board": "dm32", "tag": 46, "size": 868984,
        "code": (0x030C65B0, 0x14600, 0x00016CDC),
        "tables": (0x03072378, 0x2F78, 0x18000EA8),
        "code_sha256": "9720ec0c923ac32bc909d06a0d4c414c6a1e80c3eaa6e9c5b5df0d0bf5ff5665",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "7c045763c5b98c85836e932d9b4612f2c97fe67f5a4893ff564ecff27cfe1005": {
        "name": "DM32.01.01.047.bin", "board": "dm32", "tag": 47, "size": 869748,
        "code": (0x030C68B8, 0x14600, 0x00016CDC),
        "tables": (0x03072658, 0x2F78, 0x18000EA8),
        "code_sha256": "355ca9061979eceb2991826da9d39c95b4a9d40466245923d541a6556ceeafd1",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "fda860febfcf1a234eed7fa73272112891074aac83746e4f8dfe224a2a700f8f": {
        "name": "DM32.01.L01.048.bin", "board": "dm32", "tag": 48, "size": 860416,
        "code": (0x030C4444, 0x14600, 0x00016CDC),
        "tables": (0x03070468, 0x2F78, 0x18000EA8),
        "code_sha256": "cf275cd01ef8ba09c2873009ac5923dd5d2bc2384f6ceaa8b658b9d46d4478e2",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "1930cbe7c8ae173badff868f5e6f1f754f8601956965696d1de4ce48433cbc93": {
        "name": "DM32.01.02.046.bin", "board": "dm32", "tag": 46, "size": 868952,
        "code": (0x030C6590, 0x14600, 0x00016CDC),
        "tables": (0x03072358, 0x2F78, 0x18000EA8),
        "code_sha256": "9720ec0c923ac32bc909d06a0d4c414c6a1e80c3eaa6e9c5b5df0d0bf5ff5665",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "aff6b4dc09dcce45847173defba26e94157cc628af86826e21e5af227bdc52dd": {
        "name": "DM32.01.01.049 NRF build", "board": "dm32", "tag": 49, "size": 873964,
        "code": (0x030C7914, 0x14600, 0x00016CDC),
        "tables": (0x03072E80, 0x2F78, 0x18000EB0),
        "code_sha256": "cd0820a18df0b53bf011907a02d5b649bfbccf030c3d0432f4e363077d67a2c0",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    "a8a5fc80e7116e8e8c9926f9c5e6d56a72689d70e09db3f0fec57b3a01ec4a31": {
        "name": "DM32.01.01.049 fanti build", "board": "dm32", "tag": 49, "size": 874028,
        "code": (0x030C7954, 0x14600, 0x00016CDC),
        "tables": (0x03072EC0, 0x2F78, 0x18000EB0),
        "code_sha256": "cd0820a18df0b53bf011907a02d5b649bfbccf030c3d0432f4e363077d67a2c0",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": ENTRIES_049,
    },
    # A whole program flash dump (1 MB from 0x03000000, e.g. stub/stubtool.py --progmem) holds the radio's own data
    # besides the firmware, so it's known by the SHA-256 of the code window (the key) and of the table window
    # (docs/spec/codec-dm1801pro.md 1)
    "8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42": {
        "name": "DM-1801 Pro program flash (049 codec)", "board": "dm1801pro", "kind": "progmem", "tag": 49,
        "size": 0x100000,
        "code": (0x030A22F0, 0x14600, 0x00016CDC),
        "tables": (0x03068770, 0x2F78, 0x18000EF0),
        "code_sha256": "8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42",
        "tables_sha256": "10684c33e3cc8238376e455fd96b1c15a863a864420dbbaa4589803c4eb0b0e1",
        "entries": {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                    "enc_set": 0x2E4, "fec_init": 0x1BF8},
    },
}

# DB25-D PRO stock images: plain, linked at 0x0300C000, no container (hardware-db25d.md 8). Code: 0x12B20 bytes at the
# signature, run at 0x1CEB0 (the stock start-up copies the RAM block from 0x030B4DA8-class addresses to 0x10000, which puts
# the codec at 0x1CEB0 in all three). Tables: one 0x2D50 window (two object groups, 4 bytes apart) in the modem SRAM
# image, run at 0x180014AC + the build's shift. fec_init is +0x1DCC in all three
_DB25D_ENTRIES = {"decode": 0x0, "encode": 0x260, "dec_init": 0xB4, "dec_set": 0x84, "enc_init": 0x314,
                  "enc_set": 0x2E4, "fec_init": 0x1DCC}
PROFILES.update({
    "341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b": {
        "name": "DB25-D PRO 909E.Dx.EARSAB.018_2025-01-21.bin", "board": "db25d", "tag": 2501, "size": 846700,
        "hdr": 0, "magic": None,
        "code_sha256": "c8152ea4f4eba93c5d83a6e30953c1872923aa67639399836906a5849db9e54e",
        "code": (0x030C1C58, 0x12B20, 0x0001CEB0),
        "tables": (0x0307A8EC, 0x2D50, 0x180014AC),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _DB25D_ENTRIES,
    },
    "f2a4b214720b971b58cdadcc53d44695e1462b75296afc41f2a0b53a704d36b4": {
        "name": "DB25-D PRO 909E.Dx.EARSAB.018_2024-09-23.bin", "board": "db25d", "tag": 2409, "size": 845900,
        "hdr": 0, "magic": None,
        "code_sha256": "a2346ca4b122622476f437f9c031df70cace737b760bee614758cd5a4805b3cd",
        "code": (0x030C192C, 0x12B20, 0x0001CEB0),
        "tables": (0x0307A614, 0x2D50, 0x180014B4),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _DB25D_ENTRIES,
    },
    "9e199f54cd6545d153d39542ca5be3ac1076ee215eb93ca630a83cbdbbff4606": {
        "name": "DB25-D 909E.D6x.EARSAB.018 2024-05-09 (db25d-v2024-05-09.bin)", "board": "db25d", "tag": 2405,
        "size": 843944, "hdr": 0, "magic": None,
        "code": (0x030C1130, 0x12B20, 0x0001CEB0),
        "tables": (0x0307A150, 0x2D50, 0x180014B0),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _DB25D_ENTRIES,
    },
    # 020: a different compile of the same codec (other function order, run at 0x180B8, its own entry offsets; the tables
    # are byte for byte 018's, at 0x18001610). No vendor file is published, so it is known by its code window in a program
    # flash read, as the DM-1801 Pro's (hardware-db25d.md 8)
    "a9e67fb713b56c89b6571ee01b6d49fd605fb723ee2668d0b6afc09962bd8b90": {
        "name": "DB25-D PRO 909E.D6x.EARSAB.020 (program flash read)", "board": "db25d", "kind": "progmem", "tag": 20,
        "size": 0x100000,
        "code": (0x030C142C, 0x1315C, 0x000180B8),
        "tables": (0x0307E760, 0x2D50, 0x18001610),
        "code_sha256": "a9e67fb713b56c89b6571ee01b6d49fd605fb723ee2668d0b6afc09962bd8b90",
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": {"decode": 0x0, "encode": 0x294, "dec_init": 0xE8, "dec_set": 0xA4, "enc_init": 0x400,
                    "enc_set": 0x32C, "fec_init": 0x1CE4},
    },
})
# GD-88: two HR_C7000 SoCs, each with its own plain image linked at 0x0300C000 (hardware-gd88.md 1, 8). A is the main
# SoC (board gd88), B the second transceiver (board gd88b); each carries its own copy of the codec, so each board merges
# from its own file. 2023-03 and later: the DB25-D 018 code (0x12B20 bytes, the same entry offsets), run at 0x1795C
# (2023) or 0x1CEB0 (2024 on). 2022-10: the DB25-D 020 compile (0x131E4 bytes, its entry offsets). The table window is
# the DB25-D's, found by content; in A the last 47 bytes after the tables hold other data, so A's window hash differs
_GD88_ENTRIES = _DB25D_ENTRIES
_GD88_2210_ENTRIES = {"decode": 0x0, "encode": 0x294, "dec_init": 0xE8, "dec_set": 0xA4, "enc_init": 0x400,
                      "enc_set": 0x32C, "fec_init": 0x1EB8}
PROFILES.update({
    "6b907bd067fc50893427f73159b3815cde37e407a738ab652df8fefef344e238": {
        "name": "GD-88 A B49E.D64.EHRSAB.007 (2022-10-27)", "board": "gd88", "tag": 2210, "size": 776908, "hdr": 0, "magic": None,
        "code_sha256": "0bb4d143ec5b5c404aff4d0c8a64aca648d139138feca0ac5b51de89be65e249",
        "code": (0x030AF450, 0x131E4, 0x0001795C),
        "tables": (0x03076954, 0x2D50, 0x18001204),
        "tables_sha256": "2819cf8418f9a466c804c2ef471b8c66ae2811e17fa444cce5fccacdec60766e",
        "entries": _GD88_2210_ENTRIES,
    },
    "3e9cd8a65a36887485718e960539244ba700bca21f1059171fe58794056fb971": {
        "name": "GD-88 B B49C.D64.EHRNAB.005 (2022-10-28)", "board": "gd88b", "tag": 2210, "size": 394368, "hdr": 0, "magic": None,
        "code_sha256": "7a0ab47d59626ee65a705458ff831b7c4c14e70e9b414e6d25e30bdd0de9ea0d",
        "code": (0x030520F8, 0x131E4, 0x0001795C),
        "tables": (0x03042074, 0x2D50, 0x18000B94),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _GD88_2210_ENTRIES,
    },
    "6949edbb2743549aa12c55bc16f513306ed823f77e8c3efe996f5f1f9b05653c": {
        "name": "GD-88 B B49C.D64.EHRNAB.008 (2023-03-22)", "board": "gd88b", "tag": 2303, "size": 393556, "hdr": 0, "magic": None,
        "code_sha256": "9a3f267e46132466dfd64bb3cb66f6c6feec7015cdb717c396b17c910918f5a0",
        "code": (0x030526D8, 0x12B20, 0x0001795C),
        "tables": (0x030421F0, 0x2D50, 0x18000B40),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _GD88_ENTRIES,
    },
    "10e9533cef29797f0c3610565f61d61aaea641d5112d928c714655ce77e6b220": {
        "name": "GD-88 A B49E.D64.EOPSAB.013 (2023-03-27)", "board": "gd88", "tag": 2303, "size": 810636, "hdr": 0, "magic": None,
        "code_sha256": "b78e268376854154fbc47c5078fd969fd47494b80bc373f31393cc9d7ce3d58b",
        "code": (0x030B8120, 0x12B20, 0x0001795C),
        "tables": (0x0307BE28, 0x2D50, 0x18001228),
        "tables_sha256": "2819cf8418f9a466c804c2ef471b8c66ae2811e17fa444cce5fccacdec60766e",
        "entries": _GD88_ENTRIES,
    },
    "79b14687ef286bca90b139726bdbb7686fa6932c628f8679b4d8ffe5212be594": {
        "name": "GD-88 B B49C.D64.EHRNAB.010 (2024-01-15)", "board": "gd88b", "tag": 2401, "size": 417292, "hdr": 0, "magic": None,
        "code_sha256": "0f3f75b17ca289324ebc93e2f2d97a007596e4e1d94780512fc8ad499f80e30a",
        "code": (0x03058FC4, 0x12B20, 0x0001CEB0),
        "tables": (0x03043920, 0x2D50, 0x18000DB0),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _GD88_ENTRIES,
    },
    "3b2ac6666ff0798c9c0f43f3963fe56c5483ac3347561c7fada0a4d24502dbcc": {
        "name": "GD-88 A B49E.D64.EOPSAB.017 (2024-05-10)", "board": "gd88", "tag": 2405, "size": 836200, "hdr": 0, "magic": None,
        "code_sha256": "e8742d965e784c51a03d716ce86bbd5344c0c26242220c158c5aa0d5083f1798",
        "code": (0x030BF140, 0x12B20, 0x0001CEB0),
        "tables": (0x0307DC28, 0x2D50, 0x180014A8),
        "tables_sha256": "2819cf8418f9a466c804c2ef471b8c66ae2811e17fa444cce5fccacdec60766e",
        "entries": _GD88_ENTRIES,
    },
    "94115bd990aba9dcf0fe27a5a8f911298dd914f3afe662b688a2e25fefcf625d": {
        "name": "GD-88 A B49E.D64.EOPSAB.018 (2024-06-07)", "board": "gd88", "tag": 2406, "size": 836244, "hdr": 0, "magic": None,
        "code_sha256": "dcc527f101709df5b17e3ad8af3c8ec189f0aede38530c00a3bc78e3adb634bd",
        "code": (0x030BF16C, 0x12B20, 0x0001CEB0),
        "tables": (0x0307DC3C, 0x2D50, 0x180014AC),
        "tables_sha256": "2819cf8418f9a466c804c2ef471b8c66ae2811e17fa444cce5fccacdec60766e",
        "entries": _GD88_ENTRIES,
    },
    "7cfd711d2fe2e2a7c885728fcc4c9f4a66514c3a1a4dadf7e615b28a3ec4c7c9": {
        "name": "GD-88 B B49C.D64.EHRNAB.012 (2025-01-17)", "board": "gd88b", "tag": 2501, "size": 418000, "hdr": 0, "magic": None,
        "code_sha256": "c072499bdf8f64e1aff5bab65f3a10def0905a50153e614f732621cc558319dc",
        "code": (0x03059304, 0x12B20, 0x0001CEB0),
        "tables": (0x03043A90, 0x2D50, 0x18000DC0),
        "tables_sha256": "e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e",
        "entries": _GD88_ENTRIES,
    },
    "2c408b492b1449f71cf9e513a4f25498fdabcbab2ce207062ec1969a46c102df": {
        "name": "GD-88 A B49E.D64.EOPSAB.021 (2025-01-19)", "board": "gd88", "tag": 2501, "size": 843696, "hdr": 0, "magic": None,
        "code_sha256": "5c8fe6fa8e404d05fb4f301e0ea3a8079ead5be61671c0676cdbe1d12363fab0",
        "code": (0x030C0EFC, 0x12B20, 0x0001CEB0),
        "tables": (0x0307F064, 0x2D50, 0x180015D4),
        "tables_sha256": "2819cf8418f9a466c804c2ef471b8c66ae2811e17fa444cce5fccacdec60766e",
        "entries": _GD88_ENTRIES,
    },
})
# CRC-32 of the first 64 bytes of every table window above: a cheap first test when locate() looks for the tables in a
# program flash dump, before the SHA-256. Not a codec byte, only a fingerprint
TABLES_HEAD_CRC = 0x11475389
PROGMEM_SIZE = 0x100000

EPILOG = """\
examples:
  %(prog)s --stock DM32.01.01.049.bin
  %(prog)s --stock DM32.01.01.049.bin --image build/dm32/rfoundry_dm32.bin -o build/dm32/rfoundry_dm32_codec.bin
  %(prog)s --self-test

then:  tools/serial_loader.py --model dm32 build/dm32/rfoundry_dm32_codec.bin --wrap build/dm32/rfoundry_dm32_codec_BFUV32.bin
       and load that file with the Baofeng updater (docs/testing/dm32-bringup.md)
"""


class ExtractError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_offset(addr, hdr=STOCK_HEADER):
    return addr - APP_BASE + hdr


def carve(stock, prof):
    """The code and table windows of a stock file, by the profile's kind."""
    windows = []
    for key in ("code", "tables"):
        addr, length, _ = prof[key]
        hdr = 0 if prof.get("kind") == "progmem" else prof.get("hdr", STOCK_HEADER)
        off = addr - PROGMEM_BASE if prof.get("kind") == "progmem" else file_offset(addr, hdr)
        if off < hdr or off + length > len(stock):
            raise ExtractError("%s window outside the file" % key)
        windows.append(stock[off:off + length])
    return windows


def locate(dump, profiles=None, board=None):
    """A known codec anywhere in a program flash dump (1 MB from 0x03000000). Each stock version links the codec to its
    own flash addresses (DM-32 047 and 049 hold the same code, 0x3C bytes apart), so it is found by content: the code
    starts with the signature words and the tables by TABLES_HEAD_CRC, both 4-byte aligned, and each window must match
    a profile's SHA-256. Returns the profile with the addresses found (kind "progmem"), else None."""
    profiles = PROFILES if profiles is None else profiles
    profiles = [p for p in profiles.values() if board in (None, p["board"]) and "code_sha256" in p]
    if len(dump) != PROGMEM_SIZE or not profiles:
        return None
    sig = struct.pack("<2I", *CODEC_SIG)
    heads = None
    at = dump.find(sig)
    while at >= 0:
        # stock versions with the same codec bytes (037, 039, 040) are told apart by where they put it
        for p in sorted(profiles, key=lambda p: p["code"][0] != PROGMEM_BASE + at):
            clen, tlen = p["code"][1], p["tables"][1]
            if at % 4 or sha256(dump[at:at + clen]) != p["code_sha256"]:
                continue
            want = p.get("tables_head_crc", TABLES_HEAD_CRC)
            if heads is None:
                heads = {}
                for i in range(0, len(dump) - 64, 4):
                    heads.setdefault(zlib.crc32(dump[i:i + 64]), []).append(i)
            for t in heads.get(want, []):
                if sha256(dump[t:t + tlen]) == p["tables_sha256"]:
                    return dict(p, kind="progmem", size=PROGMEM_SIZE, code=(PROGMEM_BASE + at, clen, p["code"][2]),
                                tables=(PROGMEM_BASE + t, tlen, p["tables"][2]))
        at = dump.find(sig, at + 1)
    return None


def identify(stock, profiles=None, board=None):
    """The profile a stock file matches (whole-file SHA-256, or a program flash dump by its windows), else None."""
    profiles = PROFILES if profiles is None else profiles
    mine = {h: p for h, p in profiles.items() if board in (None, p["board"])}
    prof = mine.get(sha256(stock))
    if prof is not None and prof.get("kind") != "progmem":
        return prof
    return locate(stock, mine, board)


def parse_slot(slot):
    """The header of a codec slot read back from a radio, checked: magic, version, windows inside the slot and the CRC
    of code, tables and the relocation list. Returns a dict of its fields, else None (an empty or damaged slot, or a
    version 1 slot from before the code moved)."""
    if len(slot) < HEADER_LEN:
        return None
    f = struct.unpack_from("<8I7I4I", slot, 0)
    magic, version, code_off, code_len, run, tab_off, tab_len, tab_ram = f[:8]
    if magic != SLOT_MAGIC or version != SLOT_VERSION:
        return None
    if code_off < HEADER_LEN or code_off + code_len > len(slot) or tab_off + tab_len > len(slot) or not code_len:
        return None
    code, tables = slot[code_off:code_off + code_len], slot[tab_off:tab_off + tab_len]
    crc = zlib.crc32(tables, zlib.crc32(code))
    rel_off, rel_count = f[17], f[18]
    if rel_count > REL_MAX or rel_off % 4 or rel_off + 4 * rel_count > len(slot):
        return None
    rels = list(struct.unpack_from("<%dI" % rel_count, slot, rel_off))
    crc = zlib.crc32(slot[rel_off:rel_off + 4 * rel_count], crc)
    end = max(tab_off + tab_len, rel_off + 4 * rel_count)
    if crc & 0xFFFFFFFF != f[16] or struct.unpack_from("<2I", code, 0) != CODEC_SIG:
        return None
    return {"version": version, "run": run, "tables_ram": tab_ram, "tag": f[15], "crc": f[16], "length": end,
            "relocations": rels, "code_sha256": sha256(code), "tables_sha256": sha256(tables)}


def extract(stock, profiles=None, board=None):
    """Check the stock file and return (profile, code bytes, table bytes)."""
    profiles = PROFILES if profiles is None else profiles
    profiles = {h: p for h, p in profiles.items() if board in (None, p["board"])}
    digest = sha256(stock)
    prof = identify(stock, profiles, board)
    if prof is None:
        raise ExtractError("unsupported stock file (sha256 %s); supported: %s"
                           % (digest, ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    magic = prof.get("magic", MAGIC)
    if prof.get("kind") != "progmem" and magic is not None and not stock.startswith(magic):
        raise ExtractError("not a BFUV32-V2 firmware file")
    code, tables = carve(stock, prof)
    if struct.unpack_from("<2I", code, 0) != CODEC_SIG:
        raise ExtractError("codec signature words not found at the code window")
    return prof, code, tables


def codec_map(code, maps=None):
    """The codec map of this build (found by the code's SHA-256), or None"""
    found = codecmap.by_code_sha(codecmap.load_maps() if maps is None else maps, sha256(code))
    return found[0] if found else None


def slot_bytes(prof, code, tables, maps=None):
    """The codec slot contents: header, code, tables (4-byte aligned), the relocation list from the build's codec
    map (the code runs at the start of the board's window). A build without a map is refused."""
    lay = BOARDS[prof["board"]]
    code_off = HEADER_LEN
    tab_off = code_off + ((len(code) + 3) & ~3)
    run = prof["code"][2]
    tab_ram = prof["tables"][2]
    if not (lay["tables"][0] <= tab_ram and tab_ram + len(tables) <= lay["tables"][1]):
        raise ExtractError("table address outside the reserved modem SRAM window")
    m = codec_map(code, maps)
    if m is None:
        raise ExtractError("no codec map for this build (code sha256 %s...): its code can't be moved to the board's "
                           "codec window" % sha256(code)[:8])
    if m["code"]["run"] != run or m["code"]["len"] != len(code):
        raise ExtractError("the codec map of %s doesn't match its profile" % prof["name"])
    if len(code) > lay["run"][1] - lay["run"][0]:
        raise ExtractError("the codec (%d bytes) is bigger than the board's codec window" % len(code))
    if (lay["run"][0] - run) % 4:
        raise ExtractError("the codec window would change the code's word alignment")
    codecmap.move_code(code, m, lay["run"][0])     # checks every listed word against the code
    rels = codecmap.relocations(m)
    rel_off = tab_off + ((len(tables) + 3) & ~3)
    rel_bytes = struct.pack("<%dI" % len(rels), *rels)
    crc = zlib.crc32(rel_bytes, zlib.crc32(tables, zlib.crc32(code)))
    entries = [prof["entries"][k] for k in ENTRIES]
    header = struct.pack("<8I7I4I", SLOT_MAGIC, SLOT_VERSION, code_off, len(code), run, tab_off, len(tables), tab_ram,
                         *entries, prof["tag"], crc & 0xFFFFFFFF, rel_off, len(rels))
    header += b"\0" * (HEADER_LEN - len(header))
    body = header + code + b"\xff" * (tab_off - code_off - len(code)) + tables
    body += b"\xff" * (rel_off - len(body)) + rel_bytes
    if len(body) > lay["slot"][1] - lay["slot"][0]:
        raise ExtractError("codec does not fit the slot")
    return body


def merge(image, prof, code, tables, maps=None):
    lay = BOARDS[prof["board"]]
    slot_off = lay["slot"][0] - lay["app_base"]
    if len(image) > slot_off:
        raise ExtractError("the application image (%d bytes) runs into the codec slot at +0x%X" % (len(image), slot_off))
    return image + b"\xff" * (slot_off - len(image)) + slot_bytes(prof, code, tables, maps)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check_output_path(path):
    """Refuse to write a merged image anywhere git could pick it up, outside build/."""
    root = repo_root()
    full = os.path.abspath(path)
    if full.startswith(root + os.sep) and not full.startswith(os.path.join(root, "build") + os.sep):
        raise ExtractError("write the merged image under build/ (ignored by git), not %s" % path)


def synthetic_map(prof, code, refs=()):
    """A codec map for a synthetic codec (the self-test's random bytes): {profile id: map}"""
    pid = "test-%s-%s" % (prof["board"], prof["tag"])
    return {pid: codecmap.normalize({"format": codecmap.FORMAT, "profile": pid, "board": prof["board"], "isa": "csky",
                                     "code": {"flash": prof["code"][0], "len": len(code), "run": prof["code"][2],
                                              "sha256": sha256(code)},
                                     "entries": dict(prof["entries"]),
                                     "self_refs": [{"off": o, "value": v} for o, v in refs]})}


def self_test():
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL", msg)
            ok = False

    real = PROFILES["03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a"]
    # A synthetic stock file laid out like the real one: header, zeros, code and tables at their file offsets
    size = real["size"]
    stock = bytearray(size)
    stock[:len(MAGIC)] = MAGIC
    stock[0xFD:0x100] = b"END"
    caddr, clen, _ = real["code"]
    taddr, tlen, _ = real["tables"]
    code = bytearray(os.urandom(clen))
    struct.pack_into("<2I", code, 0, *CODEC_SIG)
    tables = os.urandom(tlen)
    stock[file_offset(caddr):file_offset(caddr) + clen] = code
    stock[file_offset(taddr):file_offset(taddr) + tlen] = tables
    stock = bytes(stock)
    profiles = {sha256(stock): dict(real)}
    p, c, t = extract(stock, profiles)
    check(c == bytes(code) and t == tables, "windows carved at the right offsets")
    image = b"\x11" * 1000
    maps = synthetic_map(p, c)
    try:
        merge(image, p, c, t, {})
        check(False, "a build without a codec map is refused")
    except ExtractError:
        pass
    out = merge(image, p, c, t, maps)
    slot_base, slot_end = BOARDS["dm32"]["slot"]
    slot = out[slot_base - APP_BASE:]
    check(out[:1000] == image and out[1000] == 0xFF, "image then padding")
    f = struct.unpack_from("<8I7I2I", slot, 0)
    check(f[0] == SLOT_MAGIC and f[1] == SLOT_VERSION, "header magic and version 2")
    check(f[2] == HEADER_LEN and f[3] == clen and f[4] == 0x16CDC, "code window, its stock run address")
    check(f[6] == tlen and f[7] == 0x18000EA8, "table window")
    check(f[8:15] == (0, 0x260, 0xB4, 0x84, 0x314, 0x2E4, 0x1BF8), "entry points")
    check(f[15] == 49, "stock tag")
    check(slot[f[2]:f[2] + clen] == bytes(code) and slot[f[5]:f[5] + tlen] == tables, "code and tables in the slot")
    check(f[16] == zlib.crc32(tables, zlib.crc32(bytes(code))) & 0xFFFFFFFF, "CRC of code then tables")
    check(len(slot) <= slot_end - slot_base, "fits the slot")

    def expect_error(fn, msg):
        try:
            fn()
        except ExtractError:
            return
        check(False, msg)

    expect_error(lambda: extract(stock[:-1] + b"\1", profiles), "other file: unsupported sha256")
    expect_error(lambda: extract(stock, profiles, board="other"), "another radio's stock file")

    # A program flash dump (DM-1801 Pro): known by its windows, whatever else the dump holds
    pm = PROFILES["8f7641b69a5cdfc946507ec48ddb67febe91d964520d5522bebd707f363f1a42"]
    dump = bytearray(os.urandom(pm["size"]))
    pcode = bytearray(os.urandom(pm["code"][1]))
    struct.pack_into("<2I", pcode, 0, *CODEC_SIG)
    ptab = os.urandom(pm["tables"][1])
    co, to = pm["code"][0] - PROGMEM_BASE, pm["tables"][0] - PROGMEM_BASE
    dump[co:co + len(pcode)] = pcode
    dump[to:to + len(ptab)] = ptab
    pprof = dict(pm, code_sha256=sha256(bytes(pcode)), tables_sha256=sha256(ptab), tables_head_crc=zlib.crc32(ptab[:64]))
    pprofiles = {sha256(bytes(pcode)): pprof}
    q, qc, qt = extract(bytes(dump), pprofiles, board="dm1801pro")
    check(qc == bytes(pcode) and qt == ptab, "program flash dump: windows carved at their addresses")
    dump2 = bytearray(dump)
    dump2[0x100] ^= 0xFF    # the radio's own data elsewhere in the dump doesn't matter
    check(identify(bytes(dump2), pprofiles, board="dm1801pro")["code"][0] == pm["code"][0], "program flash dump: other bytes ignored")
    moved = bytearray(os.urandom(pm["size"]))    # another stock version: the same codec at other addresses
    moved[0xC1234 & ~3:(0xC1234 & ~3) + len(pcode)] = pcode
    moved[0x70010:0x70010 + len(ptab)] = ptab
    found = locate(bytes(moved), pprofiles, board="dm1801pro")
    check(found and found["code"][0] == PROGMEM_BASE + (0xC1234 & ~3) and found["tables"][0] == PROGMEM_BASE + 0x70010,
          "program flash dump: the codec found wherever the stock version put it")
    check(locate(bytes(moved[:-1]), pprofiles, board="dm1801pro") is None, "locate wants a whole program flash")
    pmaps = synthetic_map(found, bytes(pcode))
    slot = merge(b"\x22" * 64, found, *carve(bytes(moved), found), pmaps)[BOARDS["dm1801pro"]["slot"][0] - APP_BASE:]
    ps = parse_slot(slot)
    check(ps and ps["code_sha256"] == pprof["code_sha256"] and ps["tag"] == 49, "parse_slot reads a slot back")
    broken = bytearray(slot)
    broken[HEADER_LEN + 500] ^= 1
    check(parse_slot(bytes(broken)) is None and parse_slot(b"\xff" * 4096) is None, "parse_slot refuses a damaged or empty slot")
    qs = merge(b"\x22" * 64, q, qc, qt, pmaps)[BOARDS["dm1801pro"]["slot"][0] - APP_BASE:]
    g = struct.unpack_from("<8I7I2I", qs, 0)
    check(g[4] == 0x16CDC and g[7] == 0x18000EF0 and g[14] == 0x1BF8, "program flash dump: slot header")
    dump2[co + 100] ^= 1
    expect_error(lambda: extract(bytes(dump2), pprofiles, board="dm1801pro"), "program flash dump: changed code")
    expect_error(lambda: extract(bytes(dump), pprofiles, board="dm32"), "program flash dump: other radio")
    bad = bytearray(stock)
    bad[file_offset(caddr)] ^= 1
    bad = bytes(bad)
    expect_error(lambda: extract(bad, {sha256(bad): dict(real)}), "signature check")
    expect_error(lambda: merge(b"\0" * (slot_base - APP_BASE + 1), p, c, t, maps), "image too big")
    expect_error(lambda: check_output_path(os.path.join(repo_root(), "codec.bin")), "output outside build/")

    # The DB25-D: a plain image (no header, no magic), the first stage inside the merged image's low part
    d25 = PROFILES["341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b"]
    plain = bytearray(d25["size"])
    caddr, clen, crun = d25["code"]
    taddr, tlen, trun = d25["tables"]
    code25 = bytearray(os.urandom(clen))
    struct.pack_into("<2I", code25, 0, *CODEC_SIG)
    tables25 = os.urandom(tlen)
    plain[file_offset(caddr, 0):file_offset(caddr, 0) + clen] = code25
    plain[file_offset(taddr, 0):file_offset(taddr, 0) + tlen] = tables25
    plain = bytes(plain)
    p25, c25, t25 = extract(plain, {sha256(plain): dict(d25)}, board="db25d")
    check(c25 == bytes(code25) and t25 == tables25, "DB25-D windows carved from a plain image")
    out25 = merge(b"\x22" * 5000, p25, c25, t25, synthetic_map(p25, c25))
    slot25 = out25[BOARDS["db25d"]["slot"][0] - APP_BASE:]
    f25 = struct.unpack_from("<8I7I2I", slot25, 0)
    check(f25[4] == 0x1CEB0 and f25[7] == 0x180014AC and f25[3] == 0x12B20 and f25[6] == 0x2D50, "DB25-D windows")
    check(f25[8:15] == (0, 0x260, 0xB4, 0x84, 0x314, 0x2E4, 0x1DCC) and f25[15] == 2501, "DB25-D entry points and tag")
    check(len(slot25) <= BOARDS["db25d"]["slot"][1] - BOARDS["db25d"]["slot"][0], "DB25-D slot fits")
    for key, prof in PROFILES.items():
        lay = BOARDS[prof["board"]]
        c0, cl, cr = prof["code"]
        t0, tl, tr = prof["tables"]
        check(cl <= lay["run"][1] - lay["run"][0], "%s: code fits the codec window" % prof["name"])
        check(lay["tables"][0] <= tr and tr + tl <= lay["tables"][1], "%s: tables inside the table window" % prof["name"])
    expect_error(lambda: extract(plain, {sha256(plain): dict(d25)}, board="dm32"), "DB25-D file refused for the DM-32")

    # The DB25-D's 020: no vendor file, found in a program flash read by its windows; another run address and entries
    p20 = next(p for p in PROFILES.values() if p["board"] == "db25d" and p["tag"] == 20)
    (c0, cl, cr), (t0, tl, tr) = p20["code"], p20["tables"]
    code20 = bytearray(os.urandom(cl))
    struct.pack_into("<2I", code20, 0, *CODEC_SIG)
    struct.pack_into("<I", code20, 0x1BC0, cr + 0x2B14)		# a word that holds an address in the code
    code20 = bytes(code20)
    dump = bytearray(b"\xFF" * PROGMEM_SIZE)
    dump[c0 - PROGMEM_BASE:c0 - PROGMEM_BASE + cl] = code20
    dump[t0 - PROGMEM_BASE:t0 - PROGMEM_BASE + tl] = tables25
    syn20 = {sha256(code20): dict(p20, code_sha256=sha256(code20), tables_sha256=sha256(tables25),
                                  tables_head_crc=zlib.crc32(tables25[:64]))}
    q20, k20, u20 = extract(bytes(dump), syn20, board="db25d")
    check(q20["tag"] == 20 and k20 == code20 and u20 == tables25, "DB25-D 020 found in a program flash read")
    s20 = slot_bytes(q20, k20, u20, synthetic_map(q20, k20, ((0x1BC0, cr + 0x2B14),)))
    ps20 = parse_slot(s20)
    check(ps20 and ps20["version"] == SLOT_VERSION and ps20["run"] == 0x180B8 and ps20["relocations"] == [0x1BC0],
          "DB25-D 020: a slot from its stock address")
    f20 = struct.unpack_from("<8I7I2I", s20, 0)
    check(f20[8:15] == (0, 0x294, 0xE8, 0xA4, 0x400, 0x32C, 0x1CE4) and f20[7] == 0x18001610, "DB25-D 020 entries and tables")
    check((BOARDS["db25d"]["run"][0] - 0x180B8) % 4 == 0 and (BOARDS["db25d"]["run"][0] - 0x1CEB0) % 4 == 0,
          "DB25-D window keeps both builds' word alignment")

    # A build with a codec map runs at the window's start with its listed words moved
    slot2, windows, moved_crc, _ = test_slot()
    ps2 = parse_slot(slot2)
    check(ps2 and ps2["version"] == SLOT_VERSION and ps2["relocations"] == [0x40, 0x84, 0x1000], "version 2 slot read back")
    f2 = struct.unpack_from("<8I7I4I", slot2, 0)
    check(f2[4] == 0x1CEB0, "version 2 keeps the stock run address in the header")
    broken2 = bytearray(slot2)
    broken2[f2[17]] ^= 4
    check(parse_slot(bytes(broken2)) is None, "the relocation list is covered by the CRC")
    old = bytearray(slot2)
    struct.pack_into("<I", old, 4, 1)
    check(parse_slot(bytes(old)) is None, "a version 1 slot (from before the code moved) is refused")
    print("hrc7000_codec self-test: %s" % ("all passed" if ok else "FAILED"))
    return ok


def test_slot():
    """A version 2 slot for a synthetic DB25-D codec (random bytes, the signature, three planted address words) and
    a synthetic map, the window it was made for, the CRC-32 of the code once moved there, and the map. tests/host
    hrc7000CodecSlotTest reads it (--test-slot) to check that the firmware's slot code agrees with this writer."""
    import random
    rng = random.Random(25)
    prof = dict(PROFILES["341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b"])
    clen, run = prof["code"][1], prof["code"][2]
    code = bytearray(rng.randrange(256) for _ in range(clen))
    struct.pack_into("<2I", code, 0, *CODEC_SIG)
    refs = [(0x40, run + 0x100), (0x84, run), (0x1000, run + clen - 4)]
    for off, v in refs:
        struct.pack_into("<I", code, off, v)
    code = bytes(code)
    tables = bytes(rng.randrange(256) for _ in range(prof["tables"][1]))
    m = codecmap.normalize({"format": codecmap.FORMAT, "profile": "test-db25d", "board": "db25d", "isa": "csky",
                            "code": {"flash": prof["code"][0], "len": clen, "run": run, "sha256": sha256(code)},
                            "entries": dict(prof["entries"]),
                            "self_refs": [{"off": o, "value": v} for o, v in refs]})
    lay = BOARDS["db25d"]
    slot = slot_bytes(prof, code, tables, {"test-db25d": m})
    windows = struct.pack("<4I", lay["run"][0], lay["run"][1], *lay["tables"])
    return slot, windows, zlib.crc32(codecmap.move_code(code, m, lay["run"][0])) & 0xFFFFFFFF, m


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", choices=sorted(BOARDS), help="the radio (default: the one the stock file is for)")
    ap.add_argument("--stock", help="your radio's stock firmware file (DM-32UV: 049 or 046; DM-1801 Pro: a program "
                    "flash dump, stub/stubtool.py --progmem; DB25-D PRO: 018, 2024-05-09 or later, or a program flash read of a "
                    "radio with 020; GD-88: the package's A file for gd88, its B file for gd88b, or a program flash read of "
                    "that SoC)")
    ap.add_argument("--image", help="the built raw application image (default: build/<board>/rfoundry_<board>.bin)")
    ap.add_argument("-o", "--output", help="merged image (default: <image>_codec.bin)")
    ap.add_argument("--self-test", action="store_true", help="run the built-in checks on synthetic data")
    ap.add_argument("--test-slot", metavar="FILE", help="write a synthetic version 2 slot, its windows and the moved "
                    "code's CRC (for tests/host/hrc7000CodecSlotTest)")
    args = ap.parse_args()
    if args.self_test:
        sys.exit(0 if self_test() else 1)
    if args.test_slot:
        slot, windows, crc, _ = test_slot()
        with open(args.test_slot, "wb") as f:
            f.write(slot + windows + struct.pack("<I", crc))
        return
    if not args.stock:
        ap.error("--stock is required")
    try:
        stock = open(args.stock, "rb").read()
        prof = identify(stock, board=args.board)
        board = args.board or (prof["board"] if prof else None)
        if board is None:
            raise ExtractError("unsupported stock file (sha256 %s): give --board to see what that radio takes"
                               % sha256(stock))
        args.image = args.image or os.path.join(repo_root(), "build", board, "rfoundry_%s.bin" % board)
        out = args.output or os.path.splitext(args.image)[0] + "_codec.bin"
        check_output_path(out)
        prof, code, tables = extract(stock, board=board)
        merged = merge(open(args.image, "rb").read(), prof, code, tables)
    except (ExtractError, OSError) as e:
        sys.exit("hrc7000_codec: %s" % e)
    with open(out, "wb") as f:
        f.write(merged)
    print("hrc7000_codec: %s codec (%d + %d bytes) merged into %s" % (prof["name"], len(code), len(tables), out))


if __name__ == "__main__":
    main()
