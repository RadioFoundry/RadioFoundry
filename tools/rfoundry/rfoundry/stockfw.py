# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""TYT/Baofeng stock firmware files as a library: container, decryption, codec extraction and merge, key derivation.

Recipes from docs/spec/codec-dm1701.md ("Container and encryption", "Extraction recipe (V02.06 CSV)") and
docs/spec/codec-md380.md ("Extraction recipe"). Used by rfoundry (merge from a stock file) and by
tools/codec_extract.py (the command line merge tool).

The codec is DVSI's and the radio maker's, not ours: nothing here stores it; it is read from the user's file on the
user's machine and only ever written to a file or a radio the user chooses.
"""
import collections
import hashlib
import itertools
import struct
import zlib

from . import codecmap

FLASH_BASE = 0x08000000      # internal flash: the bootloader, then the application from APP_BASE
APP_BASE = 0x0800C000
FLASH_END = 0x08100000
KEY_LEN = 1024
HEADER_LEN = 0x100
MAGIC = b"OutSecurityBin"
CODEC_SIG = 0x4FF0E92D
# The descriptor the merge writes at the end of sector 11 (platform/stm32f4/codec_desc.h; docs/spec/codec-placement.md)
DESC_ADDR = 0x080FFF80
DESC_LEN = 0x80
DESC_MAGIC = 0x43534544      # "DESC"
DESC_VERSION = 1
DESC_WINDOWS = 5

# DM-1701 bootloader key identity (docs/spec/hardware-dm1701.md 15.3); the key bytes are never stored here
DM1701_KEY_SHA256 = "d983ce95fbc19fc78b913d9afc0381e3ba4ffedb480798865292d05a2fb3deff"
# MD-380 / MD-390 / RT3 (docs/spec/hardware-md380.md 14.2, as tools/stm32_dfu.py's md380 model)
MD380_KEY_SHA256 = "5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd"
# MD-UV380 / MD-UV390 / RT3S (docs/spec/hardware-mduv380.md 11, as tools/stm32_dfu.py's mduv380 model)
MDUV380_KEY_SHA256 = "739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991"
# TYT MD-9600 (docs/spec/codec-md9600.md, "Container and encryption"): its files are codec sources for the DM-1701 only
MD9600_KEY_SHA256 = "c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae"
# The id the firmware reports for a placed codec, one per radio: every build the radio's firmware accepts (each by its
# descriptor tag) reports it, so a radio keeps its codec across updates whichever build it came from. That holds going
# forward only: a release older than the one that added a build's profile doesn't know its tag and finds no codec, so
# going back to it needs the codec merged again from a file it supports. Each is named after the first build
# supported; boards.py requires one id per board, and the site and release manifests compare it
DM1701_CODEC = "dm1701-v0206-csv-s11"
MDUV380_CODEC = "mduv380-1926-s11"
MD380_CODEC = "md380-d2032-s11"
# The count word of the single-segment container form (the MD-380 D002.032 file: one segment, count 0xFFFFFFFF)
SINGLE_SEGMENT = 0xFFFFFFFF

# One profile per supported stock file, keyed by its SHA-256 (codec-dm1701.md and codec-md380.md, "Stock files" and
# "Codec in the stock image"). Window = (flash address, length, SHA-256 of the decrypted bytes): "code" (its first word
# is CODEC_SIG), "tables", and "extra" for profiles whose codec has further small table windows. "codec_profile" is
# the id the firmware reports (debug link VERSION codec=) and the release manifests carry; "board" the radio it is
# for; "key_sha256" the identity of the key the file is encrypted with, which is that radio's bootloader key unless
# "file_radio" names another radio the file is for (a codec source only, never the key). "placement"
# (docs/spec/codec-placement.md): where the merge puts the code ("code"), how much of the table window it keeps
# ("tables_len": the codec's own data, from the codec map; the tables stay at their stock address) or where the
# tables move to ("tables": the window is the codec's own data and moves as one block, codecmap.move_window) and the
# descriptor's tag; "placed": the CRC-32 and SHA-256 of the code and tables as placed (the code's self references and
# the islands' calls into it rewritten for the new address, and with "tables" every reference into the tables too),
# which the descriptor carries and the firmware checks, and the CRC-32 of each "extra" window (written unchanged).
PROFILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": {
        "name": "DM-1701(CSV)-V02.06.bin",
        "version": "V02.06 CSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x08074714, 0x1021C, "1d8d04603b288e102f70252c1dd489600dfde656eccea6264e7bf4fac413f0cd"),
        "tables": (0x080D19F0, 0x12850, "b99f14e72bf72555946c1f04e8a1636652e6f62843baaf7a135787879e028894"),
        # The code goes right after the codec's data (which ends at 0x080E3A72) in sector 11, word aligned
        # (boards/dm1701/memory.ld, boards/dm1701/vocoder_dm1701.c); sectors 3-9 are the application's
        "placement": {"code": 0x080E3A74, "tables_len": 0x12082, "tag": 0x0206},
        "placed": {"code": (0x485BFC13, "d88c93ba5c3eac043558d05b182fe824a25aa373f84c48a341b369c0cc942e8c"),
                   "tables": (0xC1EEFA8B, "9ed939d23dd7b20f633d084f4713c35277d0227821070c551990caf8df409215")},
    },
    # DM-1701 V02.01, V02.02 and V02.03 CSV: byte-identical code and table windows, 0x40 below V02.06's (codec-dm1701.md
    # "Firmware files"), so one codec map (dm1701-v0203-csv) and one placement: the tables at their stock address cut
    # to the codec's data (which ends at 0x080E3A32), the code right after them, word aligned. The three files put the
    # same bytes at the same addresses, so they share one descriptor tag
    "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713": {
        "name": "Firmware-DM-1701(CSV)-V02.01.bin",
        "version": "V02.01 CSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x080746D4, 0x1021C, "2ff7b7a886b26c182d8dd883c09b8b7e5c8d5bd7331a5f8f482ecd65a86a4cc9"),
        "tables": (0x080D19B0, 0x12850, "ab3cfb492b9740ab9edbaf3505674ee390f21d7539c746dcf910c03854f1379b"),
        "placement": {"code": 0x080E3A34, "tables_len": 0x12082, "tag": 0x0203},
        "placed": {"code": (0x99B02204, "a24f6b793b2d725401cdeaad7bba728f0ed01e45a6f2cc89e5fe726757ce1531"),
                   "tables": (0x811EC3C0, "a6045020e11c0d2c9cccff08074d5b259c6023dc9d3e968da13e3a91f813fe35")},
    },
    "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7": {
        "name": "DM-1701(CSV)-V02.02.bin",
        "version": "V02.02 CSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x080746D4, 0x1021C, "2ff7b7a886b26c182d8dd883c09b8b7e5c8d5bd7331a5f8f482ecd65a86a4cc9"),
        "tables": (0x080D19B0, 0x12850, "ab3cfb492b9740ab9edbaf3505674ee390f21d7539c746dcf910c03854f1379b"),
        "placement": {"code": 0x080E3A34, "tables_len": 0x12082, "tag": 0x0203},
        "placed": {"code": (0x99B02204, "a24f6b793b2d725401cdeaad7bba728f0ed01e45a6f2cc89e5fe726757ce1531"),
                   "tables": (0x811EC3C0, "a6045020e11c0d2c9cccff08074d5b259c6023dc9d3e968da13e3a91f813fe35")},
    },
    "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948": {
        "name": "DM-1701(CSV)-V02.03 Only for Radioddity.bin",
        "version": "V02.03 CSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1272320,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xD9400)],
        "code": (0x080746D4, 0x1021C, "2ff7b7a886b26c182d8dd883c09b8b7e5c8d5bd7331a5f8f482ecd65a86a4cc9"),
        "tables": (0x080D19B0, 0x12850, "ab3cfb492b9740ab9edbaf3505674ee390f21d7539c746dcf910c03854f1379b"),
        "placement": {"code": 0x080E3A34, "tables_len": 0x12082, "tag": 0x0203},
        "placed": {"code": (0x99B02204, "a24f6b793b2d725401cdeaad7bba728f0ed01e45a6f2cc89e5fe726757ce1531"),
                   "tables": (0x811EC3C0, "a6045020e11c0d2c9cccff08074d5b259c6023dc9d3e968da13e3a91f813fe35")},
    },
    # DM-1701 V02.06 and V02.03 noCSV: a larger application (0xDC000), the codec 0x29DC (code) and 0x2DA4 (tables)
    # above the CSV file's of the same version, its code 0x1C bytes shorter (codec-dm1701.md "Firmware files"). Each
    # has its own map and placement: the tables at their stock address cut to the codec's data, the code right after
    # them, word aligned. Tag bit 16 marks noCSV; V02.03 noCSV is V02.06 noCSV 0x40 lower, as for CSV
    "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9": {
        "name": "DM-1701(noCSV)-V02.06.bin",
        "version": "V02.06 noCSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1283584,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDC000)],
        "code": (0x080770F0, 0x10200, "c34e45885ccd422d57f0f009b5bcc2eb741fc3c765a57c2016e0ce07b0870566"),
        "tables": (0x080D4794, 0x12850, "de6e8494159b7f089cfd97b5724ddde57c99b4480fc61249c41ac3cad1683acf"),
        "placement": {"code": 0x080E6828, "tables_len": 0x12092, "tag": 0x10206},
        "placed": {"code": (0xAF82F7D0, "1493b162933e3877d926e1154ac85ee8e784de4ceb4ccf1762df616bd025fe30"),
                   "tables": (0x3EDE91FF, "a5297282ec421fa155ff9319430f0e98be61b5c9dd89a920a461fc1ff2c723f9")},
    },
    "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a": {
        "name": "DM-1701-V02.03 Only for Radioddity.bin",
        "version": "V02.03 noCSV",
        "board": "dm1701",
        "key_sha256": DM1701_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1283584,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDC000)],
        "code": (0x080770B0, 0x10200, "4fccfdda199e239464bc7a2622af20e99eb7885bcf8bffa2df1e4bc0f756a3ce"),
        "tables": (0x080D4754, 0x12850, "b5a73a199e91a6d684d9d1bfe46261a6f6aeb0fe65a907c781b809d0425f537a"),
        "placement": {"code": 0x080E67E8, "tables_len": 0x12092, "tag": 0x10203},
        "placed": {"code": (0x96264D90, "238692b07a37e97964e72bd9c0569aaf4555ff877ab8307fa4cd32da3375e96f"),
                   "tables": (0x217E610A, "32c75144218f0e95541bfc375d1330f248ed66cccf09a02fe2a00e4e92628f2e")},
    },
    # TYT MD-9600 files as codec sources for the DM-1701 (codec-dm1701.md "MD-9600 files"; their codec: codec-md9600.md).
    # An open-firmware install on the DM-1701 starts from the MD-9600 V4 firmware, so its radios carry this codec. It is
    # the DM-1701's codec in function (the same entries, state and calls), linked elsewhere. These files give the codec
    # only: they are encrypted with the MD-9600's key ("file_radio" names the radio they are for), never the
    # DM-1701's bootloader key. The table window is the codec's own data (the map's cut) and moves with the code
    # ("placement" "tables"; codecmap.move_window): the tables to 0x080D0000 plus their stock offset in a 4 KB page,
    # the code to the page after them at its own stock page offset. Tag: 0x96 in the top byte, the variant (0 CSV,
    # 1 CSV-GPS, 2 REC, 3 GPS-REC) in the next, the version in BCD below
    "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532": {
        "name": "MD9600-CSV(V4)-V16.45.bin",
        "version": "MD-9600 V16.45 CSV",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1099264,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB2C00)],
        "code": (0x08075C98, 0xF214, "4bcfec3e1113d17bb53bee1543039894db00c00c32d1b7dfd258f224043f52cd"),
        "tables": (0x080AC2BC, 0x11036, "a101749f26c6df7d784130c45570e25abc5c88e02bee6f2510beb0a6c3ad4dfa"),
        "placement": {"code": 0x080E2C98, "tables": 0x080D02BC, "tag": 0x96001645},
        "placed": {"code": (0x07D97C60, "c7b27f64e8af74601fe2e21e530a1eacfdf61e7a174e0839ec2313b131a6e39e"),
                   "tables": (0xD4FB8040, "1e4af02d48b8753a52d18ed139832638fded1f0707797a417c96588d36d8da9c")},
    },
    "7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f": {
        "name": "MD9600-CSV-GPS(V4)-P16.45.bin",
        "version": "MD-9600 P16.45 CSV-GPS",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1105408,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB4400)],
        "code": (0x08077434, 0xF238, "70bbb7beaa426dbefa080af083c51b263945818f935533c619aad9675a5f91b9"),
        "tables": (0x080ADA7C, 0x110B6, "b29494898a4a96c5333c57d0681ed76b571e0aaeb460a8208479a0d700d2a225"),
        "placement": {"code": 0x080E2434, "tables": 0x080D0A7C, "tag": 0x96011645},
        "placed": {"code": (0xDB9C6525, "4dd26d6754499fd23e6646c04190971c237c9ca9ddf1cbac0654113fb6aa12f9"),
                   "tables": (0x53687A8C, "9545849603782fad13bb6467eafbeeb0003b987187ceb89a36c9d9362c15ed98")},
    },
    "d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c": {
        "name": "MD9600-REC(V4)-D16.45.bin",
        "version": "MD-9600 D16.45 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1108480,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB5000)],
        "code": (0x08077E38, 0xF234, "c870d2b5b9bf77e139544de24331f4cac2ca439bb0e9e24cc92b3a87a3f6e2c5"),
        "tables": (0x080AE820, 0x11046, "e1baec7419b42d6f420d64c639a5b92d2a6e3b7410a5648b17acaeddd93c71e3"),
        "placement": {"code": 0x080E2E38, "tables": 0x080D0820, "tag": 0x96021645},
        "placed": {"code": (0x5C29A9B4, "430f6bd7aac566b7570e1c0ceb498914835d52cb4cbd90634b0663cda9ff5337"),
                   "tables": (0x407C8912, "de5cf1bb053d5bd514901a2e0daf29f52bb856ae32d790e769000f1537e1e88d")},
    },
    "3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff": {
        "name": "MD9600-GPS-REC(V4)-S16.45.bin",
        "version": "MD-9600 S16.45 GPS-REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1114624,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB6800)],
        "code": (0x08079630, 0xF1FC, "5a7f1d52b1aac95abf7f2f79f3db58cd35188f5bc0b35374f10f74aa17db2c1a"),
        "tables": (0x080AFFE0, 0x110C6, "2172ecdd60f3217a28b1549d935c8870051f01d39f7ed8b7264712a14d9b9d31"),
        "placement": {"code": 0x080E3630, "tables": 0x080D0FE0, "tag": 0x96031645},
        "placed": {"code": (0x7E5C1170, "6cbc0ba6461d0604a9389c60257d6b0012cf3d8df96858fc9ff484f80ea11656"),
                   "tables": (0x2E4B09C6, "43b741257138fb1aa92c039df79ede3acf2f1a7d5a33cb33a2432e68bb33b2d6")},
    },
    "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09": {
        "name": "MD9600-CSV-V06.09.bin",
        "version": "MD-9600 V06.09 CSV",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1094144,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB1800)],
        "code": (0x08074B18, 0xF214, "c729480a47c7444c03eff1ec6b176a76d071f72b64d2f0fe3965e517c85a1215"),
        "tables": (0x080AB13C, 0x10FE6, "0b431c14ef7e32fe8a0915976c250f127a757ed4cc5af2bc7f867ae261c6f1ca"),
        "placement": {"code": 0x080E2B18, "tables": 0x080D013C, "tag": 0x96000609},
        "placed": {"code": (0x51334F80, "a5477fe55360f3c17af52d84c051d5f4d2ebbdb8d48ecb6be4896e059647fd4e"),
                   "tables": (0x028B006A, "5767b06b023d726747c76b40de73bf5e9ea028223a224eb03862890dda7d07f2")},
    },
    "9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5": {
        "name": "MD9600-CSV-GPS-P06.09.bin",
        "version": "MD-9600 P06.09 CSV-GPS",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1100288,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB3000)],
        "code": (0x080760B8, 0xF234, "1836c0870ef9333d022c495fbaf2d989f64f6a26dd5d762d8c730cce82e300b6"),
        "tables": (0x080AC6FC, 0x11066, "f2b8bda5a65f2da7bba0d525347dd1896242f3ead77db03b9af44ba971a12b9e"),
        "placement": {"code": 0x080E20B8, "tables": 0x080D06FC, "tag": 0x96010609},
        "placed": {"code": (0x7C7A0876, "073882891ef1e478bc25c4795ec753efb499287fec99f06b4443e2e7ffe8d2b3"),
                   "tables": (0x9E0642E5, "595983acfbe27e378e082bdb0e27f47ed8db9c4c61a512219825ea6d65dd157c")},
    },
    "7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f": {
        "name": "MD9600-REC-D06.09.bin",
        "version": "MD-9600 D06.09 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1103360,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB3C00)],
        "code": (0x08076C9C, 0xF210, "75a937300cda4cc1802453e7352686511ca427888b4cc95732f45894b3e14a50"),
        "tables": (0x080AD660, 0x10FF6, "d61403ecd01634b9054e41f32c32085db2b3e0f2e998b83a102fd7a77f55431f"),
        "placement": {"code": 0x080E2C9C, "tables": 0x080D0660, "tag": 0x96020609},
        "placed": {"code": (0x53A7AF3B, "b128d37ea14fbb54e88843eae18fdef33dd912e02f8177d9efaf4da0f28e5358"),
                   "tables": (0x8EFB3BDF, "eb2dd965b483f6a2ec382016ec2eaafa76651cc7b8b70c6261d308227e3fa953")},
    },
    "4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7": {
        "name": "MD9600-GPS-REC-S06.09.bin",
        "version": "MD-9600 S06.09 GPS-REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1109504,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB5400)],
        "code": (0x08078280, 0xF22C, "a3d768224197cdd4f72db9024149e38f1c33a965ce44bca33a1904bb9e6c82b5"),
        "tables": (0x080AEC60, 0x11076, "2fe79fad2e2688e78ce892d52121874ba623df8f2ec73f44e4740413f356dfa3"),
        "placement": {"code": 0x080E2280, "tables": 0x080D0C60, "tag": 0x96030609},
        "placed": {"code": (0xB54871EE, "18387dc3dd41d93697f5f2d9221fea5e97889de79c99007c2281511865c9cff2"),
                   "tables": (0x4758C9EE, "439b8b855b1c53fe3c8c03bd988df7491c516aa191a06eb4929f27bbc8215070")},
    },
    "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f": {
        "name": "MD9600-CSV(2571V5)-V26.45.bin",
        "version": "MD-9600 V26.45 CSV",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1096192,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB2000)],
        "code": (0x0807537C, 0xF22E, "c437494bb0f9f4e58cb2c4a1c039185330a54ff293b54fdaff5c7d4159b30ad7"),
        "tables": (0x080AB9BC, 0x11036, "b7017c2346c41c2bcaac3ecceb037263952557ee6a9fe8094d5a9caffe6e8c4b"),
        "placement": {"code": 0x080E237C, "tables": 0x080D09BC, "tag": 0x96002645},
        "placed": {"code": (0x0095F077, "0c0425cf300f3ca533ec30ed2967f6da95d7df7c5c5dab48cc519a233ebfcc8b"),
                   "tables": (0xDA6E4D8D, "551ced06c100d978f43ddc2c8bdc2d7b62312086499b142ffc7890e1b1b4ae66")},
    },
    "194526d60f796cc0f7339d14c1efeedc267731d108fd66a96876b459408de0ba": {
        "name": "MD9600-CSV-V06.24.bin",
        "version": "MD-9600 V06.24 CSV",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1094144,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB1800)],
        "code": (0x08074C78, 0xF232, "cc3372d87629dfca98a22d0227301011084189fdec8cf073eb3c18fb8412d3a6"),
        "tables": (0x080AB2BC, 0x10FE6, "3e19a511cb77dd2d1c8ac6a09d215f1dd862470dce99718645b43821c8a7d6fe"),
        "placement": {"code": 0x080E2C78, "tables": 0x080D02BC, "tag": 0x96000624},
        "placed": {"code": (0xF977B4D6, "4f8104ed87a857ffd498ef460b3f8c95bd32237797a0f0f39d3895f0c8129681"),
                   "tables": (0x337667F2, "b6439d6a5007389361e396936550ca7dabe3767b709f5cc2ea98efb929c60043")},
    },
    "1eafdb774fd71cb7bc3334d06617ed42264ee25df1c73f61744f6b21742bbd20": {
        "name": "MD9600-CSV-GPS-P06.24.bin",
        "version": "MD-9600 P06.24 CSV-GPS",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1100288,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB3000)],
        "code": (0x080763CC, 0xF21E, "403b3df9ddcd5d732999d7c2c65a74f4db35bcf4d71d76dd0747df172a90a5d8"),
        "tables": (0x080AC9FC, 0x11066, "be95f990663f73c1c703c68e0393121b8f78a8266027295b20cfd7f824b4dfa7"),
        "placement": {"code": 0x080E23CC, "tables": 0x080D09FC, "tag": 0x96010624},
        "placed": {"code": (0x25094D99, "1261a200286f8b8a738d16290b13dd033d30b4ea613b60137b5cbc6b005b0919"),
                   "tables": (0x2D8040A6, "679df53dbe2949e15ca9da2b09e9468759eafd971412416886a59c96fff4bc4a")},
    },
    "ec44929e855442ad248f1a5c171a467922d6cb5742fc35f759db269c645e22ab": {
        "name": "MD9600-GPS-REC-S06.24.bin",
        "version": "MD-9600 S06.24 GPS-REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1110528,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB5800)],
        "code": (0x08078594, 0xF216, "16fbf16c889e12bede6ea2c0501321881489d9c2dc5a90998d181d7afcccd41e"),
        "tables": (0x080AEF60, 0x11076, "10db31bf95d1a0cb40593d8d35da14a0ae5d59bb35ae513047553c647fdb2a6a"),
        "placement": {"code": 0x080E2594, "tables": 0x080D0F60, "tag": 0x96030624},
        "placed": {"code": (0x69B987A1, "6c74bc8bdc7bf22c9cc4899f21899f83035839600dc9f6dea81cf912502d17ac"),
                   "tables": (0x49D18C19, "312c74e0b66485160f1e494109c5d15fe090bbc2e85be7f18d0e60ee566205fe")},
    },
    "6643261e62b9778a5df43b1e5c3428d6d308bf54b34478513df3e08f6f4dbb63": {
        "name": "MD9600-REC-D06.24.bin",
        "version": "MD-9600 D06.24 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1104384,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB4000)],
        "code": (0x08076DFC, 0xF22E, "c5b5123caa9b045cc63d3aeb61aa4c53b8daa2001ede7745102960820fcaedf7"),
        "tables": (0x080AD7E0, 0x10FF6, "8db73635d11b0216bb59ab8a099e547948c522a9bbf526e95cfe607fb6309c5c"),
        "placement": {"code": 0x080E2DFC, "tables": 0x080D07E0, "tag": 0x96020624},
        "placed": {"code": (0x05F34587, "0a1f5aa925925eaad2684a4e2ec81ccea70fbdef6f89a2bb0da92f266e715925"),
                   "tables": (0xDDBD5036, "5b2b5bf3a58b352fa88ab8a72ec9959f0adae8cf791bec76f3856d77aeed9a5c")},
    },
    "98302fbc26fa0db93bd7ecbd29a410ac9fbb42cbd7d6ce15d54ce085fefff329": {
        "name": "MD9600-CSV(V4)-V16.24.bin",
        "version": "MD-9600 V16.24 CSV",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1095168,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB1C00)],
        "code": (0x08074FB0, 0xF23A, "f68bd78b26ce3ef2ed9547703569460b6a1e77406bd9122f8c2981e4b0565438"),
        "tables": (0x080AB5FC, 0x10FE6, "df2ca5d596c3ee6266342b8ea66f73f74fc910ff5e9a00cec439d136491766c7"),
        "placement": {"code": 0x080E2FB0, "tables": 0x080D05FC, "tag": 0x96001624},
        "placed": {"code": (0xE413CEF1, "8c8faf47b297edc39ca4ed60bf7de99517cfeb9a62ed58c83e4d2a8180049fb4"),
                   "tables": (0x8C4CD107, "7d1b0b1bd373bf31a6b65dba3aea5c63c185009fbe29858319128682b6d6cf3e")},
    },
    "e86b9e9775758548dd2d01334cfacc96c640fbf843aa6c5fa009b4bc7598e98d": {
        "name": "MD9600-CSV-GPS(V4)-P16.24.bin",
        "version": "MD-9600 P16.24 CSV-GPS",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1101312,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB3400)],
        "code": (0x08076704, 0xF226, "bd0d774b118b10f68f13fbf11002f42951abf9c94973a5dc48e8e349790b6349"),
        "tables": (0x080ACD3C, 0x11066, "545966b60d6b5f34d2fa4248826126dd0a74dbfb67e26c5ba58274e29156598c"),
        "placement": {"code": 0x080E2704, "tables": 0x080D0D3C, "tag": 0x96011624},
        "placed": {"code": (0xB2F7AB22, "60d274186249d99e6a36c7fd3807024a7a9c18449bfd6b6f8cd70db5ff37d589"),
                   "tables": (0x1BA5D665, "3ffdba16d7b7d996e357ece42787b5f90f10559a7e39abd7c1b732231615b869")},
    },
    "4e1c5c081d5bdf4bef8f205668fd7381391187d8b07150df7e99275b030b811b": {
        "name": "MD9600-GPS-REC(V4)-S16.24.bin",
        "version": "MD-9600 S16.24 GPS-REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1111552,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB5C00)],
        "code": (0x080788CC, 0xF21E, "181da1f9656b7851d94cacda6b6b5e59c9d3355e3456d3b3fbb36ebb83a7f7e3"),
        "tables": (0x080AF2A0, 0x11076, "1af5f6df7ec08475330f28a543bc6164acb74e249b0c3c10326b661046b9ea26"),
        "placement": {"code": 0x080E28CC, "tables": 0x080D02A0, "tag": 0x96031624},
        "placed": {"code": (0x111C3DB7, "8d577e8f9ee77cf7e529c5d7fdb771a9b765453246bccf4d87eb16e5176ecfda"),
                   "tables": (0xFC643CC9, "703c07af1d149356b0fa3b64e8c468a156b181ff791a124f1bf323073ffe97ba")},
    },
    "e21c72e30716fb43e4d15798012aec15e964b9ec8c1a534ede5d0b0fe73f3acb": {
        "name": "MD9600-REC(V4)-D16.24.bin",
        "version": "MD-9600 D16.24 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1105408,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB4400)],
        "code": (0x08077134, 0xF236, "d7f73d858f2f91e8bcebf1b3a67f2164e3e9128ef1ea926faa1f26afe010804a"),
        "tables": (0x080ADB20, 0x10FF6, "c989388d09c824d424c6820d72fac1fb726b432a982b4404aa609190c61bac37"),
        "placement": {"code": 0x080E2134, "tables": 0x080D0B20, "tag": 0x96021624},
        "placed": {"code": (0x4FD7EB93, "9724607b40a9094c40dc7cba5e7ba88c1ed43a0192e12ecff7f6591e0482f303"),
                   "tables": (0x82E6D100, "fa428d33f9a2906ca937cc726ab9de01da9269a390c8ed81c4c28fc6cf854ae5")},
    },
    "02e366687f122ed752581344db8316666d21782d9353334518b69237a81e74dc": {
        "name": "MD9600-CSV-GPS(2571V5)-P26.45.bin",
        "version": "MD-9600 P26.45 CSV-GPS",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1102336,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB3800)],
        "code": (0x08076B18, 0xF212, "72dd6322a68709db0e2841fff1cb4f6def3f6f3f912e3f16271a35125d907a4a"),
        "tables": (0x080AD13C, 0x110B6, "b6c9bf6433f9ea38622173293140b72bbe8589ee3acd5064026f54f7fe65e7de"),
        "placement": {"code": 0x080E2B18, "tables": 0x080D013C, "tag": 0x96012645},
        "placed": {"code": (0x757367A4, "136ea25d61b86aacefcf6005bd2ad3588dc7cbb4a3b87444f5699b48afdea872"),
                   "tables": (0x230044AB, "ee082f6d97ca003782e7e6e60eb29b391e097fcba2dee100fa2204d9857c6bf9")},
    },
    "6df34152e9786530d61032ab71f1962cf194aca0b99cf0a54e3802fd2cd4ac72": {
        "name": "MD9600-GPS-REC(2571V5)-S26.45.bin",
        "version": "MD-9600 S26.45 GPS-REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1112576,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB6000)],
        "code": (0x08078D14, 0xF216, "7789393c8053c61ac8021c2f79b9b961d77693c3433c0e5d4e65bdf86d8276ab"),
        "tables": (0x080AF6E0, 0x110C6, "17ad382ff8e8649e60dfeb81ca5118075dacde0bf63400a5a5c64b4b5b8e3bb5"),
        "placement": {"code": 0x080E2D14, "tables": 0x080D06E0, "tag": 0x96032645},
        "placed": {"code": (0xD772D25B, "43b691748d5e614b82d10bda4dc7ecb34f8d1da1474a14a9ac2c607cea5837dd"),
                   "tables": (0x960E46C6, "4e798dd7ebd528764de1bd261a9728047e1ae75cc0b686fe2cdf1d0b96a8db92")},
    },
    "baee36b0fd852dfc1fb14952242a1231dbfa4fabc37cba57c61f1b8d3e0d830d": {
        "name": "MD9600-REC(2571V5)-D26.45.bin",
        "version": "MD-9600 D26.45 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1106432,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB4800)],
        "code": (0x0807751C, 0xF20E, "8974385cad3549428d9dad0685d1961535e730856073cbf3083b55e182eb2f96"),
        "tables": (0x080ADEE0, 0x11046, "fa652e605a9c8c941ff4aca30862ca4efe9e5154e959129126cad0192d016c37"),
        "placement": {"code": 0x080E251C, "tables": 0x080D0EE0, "tag": 0x96022645},
        "placed": {"code": (0x3CE9E9D7, "0853c68fa8028a60e247cb7ef706af8d301bdc88780a6f6a6780ba246ffd86fd"),
                   "tables": (0x5D4334F8, "4c808deb414e00058d80c70a8954911e9800622946e4e6cd407b683939464bf3")},
    },
    "2522008737d7845e43ab47a307d38498ee783c788a377fac855dff94dea53af0": {
        "name": "MD9600-REC(2571V4)-D26.57.bin",
        "version": "MD-9600 D26.57 REC",
        "board": "dm1701",
        "file_radio": "MD-9600",
        "key_sha256": MD9600_KEY_SHA256,
        "codec_profile": DM1701_CODEC,
        "size": 1107456,
        "segments": [(0x00060000, 0x59800), (0x0800C000, 0xB4C00)],
        "code": (0x08077C3C, 0xF1EE, "c43426f088eb61bb8c8aa96356326431a0958340aa44ad9451741ed9c93419d2"),
        "tables": (0x080AE5E0, 0x11032, "877866109452231406875d9c1d39a1bca355fa2e491af691f1f530bde5f2a74e"),
        "placement": {"code": 0x080E2C3C, "tables": 0x080D05E0, "tag": 0x96022657},
        "placed": {"code": (0x24D32768, "7d7cd7931aff9096595f0ceab4290bf6c7eb7af83b2731506c4c599b9d02aeed"),
                   "tables": (0x782C12BE, "3aae21aa7aa4c42313a87e9ef3655c744a39390c14f5041022049af1783c24d6")},
    },
    # TYT MD-380 "Firmware 2.32" (D002.032), single-segment container. Stock keeps the code in sectors 6 and 7
    "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43": {
        "name": "MD-380-D2.32(AD).bin",
        "version": "D002.032",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 0xF2E00,
        "segments": [(0x0800C000, 0xF2C00)],
        "code": (0x08050C44, 0x10884, "00d9565dc459c47119690e47c35daf9ae1a89c13bb3513d966390e0835b0c0ad"),
        "tables": (0x0808FC64, 0xAFF8, "9c7b66b08a6d397565c70551df9097870757f8de719b8d31525eba72e1db4840"),
        "extra": [
            (0x080D0FC8, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9698, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9E74, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD490, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        # The code goes to the start of sector 11, below the small windows there (boards/md380/memory.ld,
        # boards/md380/vocoder_md380.c); the tables stay in sector 8, so sectors 3-7 are the application's
        "placement": {"code": 0x080E0000, "tag": 0x2032},
        "placed": {"code": (0x067E6A77, "d5c68973b33d5709b6d1df719b80841b77f1835d3bef5b61c6c682147da6bebc"),
                   "tables": (0x2A5C9EE1, "8c704a9606ec19fbe4af275de4fbd1ded64e2ab644e0115ec34bff86cb3d21a9"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    # TYT MD-UV380 / MD-UV390 19.26, the four builds (codec-mduv380.md). One codec, placed as the DM-1701's: the tables
    # at their stock address cut to the codec's data, the code right after them in sector 11, word aligned
    # (boards/mduv380/vocoder_mduv380.c). Each build keeps both at its own addresses, so each has its own tag
    "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e": {
        "name": "MD-UV380(CSV)-V19.26.bin",
        "version": "V19.26 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1303040,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE0C00)],
        "code": (0x0807B830, 0xF4BA, "5de390664c831412f37ae9ec1f8aadaf77d93112bded424e5bfc9d6b77a64aa1"),
        "tables": (0x080D8C94, 0x12850, "6ea640d8da93215d6b3c9d7ce8db3663c32743a83c9478b1e9997e0905edc53c"),
        # the codec's data ends at 0x080EB2FE; the code leaves 22470 bytes below the descriptor
        "placement": {"code": 0x080EB300, "tables_len": 0x1266A, "tag": 0x00561926},
        "placed": {"code": (0x10AC40E0, "1b7a30bb218c85a7ad626217d26a9158283c9e87393167acaf984e986e25a5d6"),
                   "tables": (0xCE661968, "fd9447e5d97289fee35e734ba10651be8cbb9505ed3bea24d81f15e45f040724")},
    },
    "7dec4b751e8a0f44cf7d54d0ebc3b439a9f49899eadd7c8ff14d2804edbe8ac2": {
        "name": "MD-UV380(REC)-D19.26.bin",
        "version": "D19.26 REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1315328,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE3C00)],
        "code": (0x0807E3C0, 0xF4AA, "7e885b7f6f96bd433a310c7b1bea4ae9db55a71d74bcc102e8684bf0b1e88e96"),
        "tables": (0x080DBC1C, 0x12850, "96331ad1617b965e2b59a633767bfcc505c4840f3924e8fb82d5e522ff699599"),
        # the codec's data ends at 0x080EE28E; the code leaves 10310 bytes below the descriptor
        "placement": {"code": 0x080EE290, "tables_len": 0x12672, "tag": 0x00441926},
        "placed": {"code": (0x3367A6D0, "608d2fef64ebcd6ed2ef6d5491572808641fa8505675023004b059450e87d7a0"),
                   "tables": (0x7E080ADE, "32c841f3e768dc1dd148810b01ae6754c7f8cf0cca14889e1eef160779d586b4")},
    },
    "84331dbc9bccf5021122b715dcb77d7589dba0745694bf3a249aae6e7c383ef1": {
        "name": "MD-UV390(CSV-GPS)-P19.26.bin",
        "version": "P19.26 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1312256,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE3000)],
        "code": (0x0807DBF0, 0xF4BA, "bb8035df0a200c827b65128a3f24358fafd1c10601626bd54888595173bb6ae8"),
        "tables": (0x080DB054, 0x12850, "1f19c01eb5c0e9bcf10f2aeb7bfaee68580b19f4933448d3742d5aef2e6aadea"),
        # the codec's data ends at 0x080ED75E; the code leaves 13158 bytes below the descriptor
        "placement": {"code": 0x080ED760, "tables_len": 0x1270A, "tag": 0x00501926},
        "placed": {"code": (0x0AEE6C4A, "d7cc254326d9f4a0c2d7f7702cb2c0bdf8cec95d4ba1ac083a059d103524034a"),
                   "tables": (0x06DB6E3C, "e251afc5bc00609a05f266d29111adfefe934f77151a7b40a974f93ce0147546")},
    },
    "77923d67aa75a07f7601a1c53ae1d5f5af37bd4367d52a50c8aeff89b834eceb": {
        "name": "MD-UV390(GPS-REC)-S19.26.bin",
        "version": "S19.26 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1324544,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE6000)],
        "code": (0x080807B4, 0xF4B6, "96c018bc0956b86a7ac4a90713eb53fb457cebb9f2584b927d785fefa2085613"),
        "tables": (0x080DE01C, 0x12850, "08adbdac3d3c95e2b5e2e895e77e045aee15a7839933d328727310b517a854f1"),
        # the codec's data ends at 0x080F072E; the code leaves 922 bytes below the descriptor
        "placement": {"code": 0x080F0730, "tables_len": 0x12712, "tag": 0x00531926},
        "placed": {"code": (0x11E43041, "c9b49028c2ef65fb4a216ec244e60b96ef8e8584b91421c4a4fb7afba0f2fc6c"),
                   "tables": (0xDED111EF, "b699e0277eb179426a00b763cdff96a8753593f9cad1cf4a6aa7b67568eef66a")},
    },
    # MD-UV380 / MD-UV390 18.11 and 18.16, RT3S 19.06, TYT 19.74 (5 W and 10 W), and the AES 255 line (RT3S 219.60,
    # TYT 219.74). The same codec, placed the same way; the RT3S 18.11 and 18.16 files are TYT's, byte for byte. Where
    # the code doesn't fit between the codec's data and the descriptor, it goes to the start of sector 10, before the
    # tables (boards/mduv380/memory.ld leaves sectors 10 and 11 to the codec)
    "fb1897b7123dc6a039a5266972c5bb6f563f574d3769bd1cbdc7bb02b5039cc7": {
        "name": "MD-UV380(CSV)-V18.11.bin",
        "version": "V18.11 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1289728,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDD800)],
        "code": (0x0807884C, 0xF6DE, "bc9af04eec52ab3b8f222e81d63e52c106f98d60f66d7010ea2e6c807894c46f"),
        "tables": (0x080D5B34, 0x12850, "f4547d849bb694b8e86fb3a55c4320a6945e458f1b3a133b1111bd6dc34684e5"),
        # the codec's data ends at 0x080E7D22; the code leaves 35710 bytes below the descriptor
        "placement": {"code": 0x080E7D24, "tables_len": 0x121EE, "tag": 0x00561811},
        "placed": {"code": (0x0B1FFBD7, "43e0c3f6987490d29e070694f799f400fed4361ce4bbffbaaf24a6aa8b2a9fd7"),
                   "tables": (0x28A30A27, "0b0558626f01daece414ddf129a99ac0aa7ad5a375f07664f69fc1b95ac2f75b")},
    },
    "78650258f65fef33d78fad65678bedd74e44b75e924466829c9a360ce8915f39": {
        "name": "MD-UV380(REC)-D18.11.bin",
        "version": "D18.11 REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1300992,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE0400)],
        "code": (0x0807B2D8, 0xF6D2, "42ab9d8090c3f630ccd38658bd4e7ee4b22a981cb2fb77f1980aff9a068af5b9"),
        "tables": (0x080D8998, 0x12850, "029aecd4f281be17f56362aa570935da57e5868a43deca1d6b3c33d8c0270b29"),
        # the codec's data ends at 0x080EAB8E; the code leaves 23838 bytes below the descriptor
        "placement": {"code": 0x080EAB90, "tables_len": 0x121F6, "tag": 0x00441811},
        "placed": {"code": (0x8DB9F023, "1157277e7244b0a15b316b73cc4d9f43bb21cc68d4e95a1cd580d80f48b67ca9"),
                   "tables": (0x6249AF48, "480f5ac44c0b89b3ce2017199e1ae1636c93c451cd7d88c6a5b64450a4f5abcb")},
    },
    "e31ecd122f77ccd34e563193b004e8621ff5422676521503521f64f1045f73a9": {
        "name": "MD-UV390(CSV-GPS)-P18.11.bin",
        "version": "P18.11 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1296896,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDF400)],
        "code": (0x0807A388, 0xF6E2, "2ed2ea20323649a8463c76e03a42772e490ead803c854f73541626fe7f2b03f0"),
        "tables": (0x080D7674, 0x12850, "c586fe9ab586eb2cf92d0f65064fc9d83bcc690b58846ad9824246cdd99b359a"),
        # the codec's data ends at 0x080E9902; the code leaves 28570 bytes below the descriptor
        "placement": {"code": 0x080E9904, "tables_len": 0x1228E, "tag": 0x00501811},
        "placed": {"code": (0x229BC3B4, "c6286416b4a5dd9f5fc5b2624c26b1a82bf610e18b3d37ecc8094645364d1d25"),
                   "tables": (0x1AC8D7F3, "9958aa46084b79f4a1bc8514c03fd7d220771a19bbdf34e383bc4f02b4422a02")},
    },
    "fbb307fd14577e48e1fb6edb3395d4d684baf12dd9fed66280ad76d12e0e04c2": {
        "name": "MD-UV390(GPS-REC)-S18.11.bin",
        "version": "S18.11 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1308160,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE2000)],
        "code": (0x0807CE28, 0xF6C2, "cc51722f43551ab7dd1dd7e8815ac27a8a0dff586cb3efda9b3ccd276f38abd9"),
        "tables": (0x080DA4D8, 0x12850, "adf44ada74eed62ef90de73a702775b62dd8e97fce17b06ffb80e4a57a66b01a"),
        # the codec's data ends at 0x080EC76E; the code leaves 16718 bytes below the descriptor
        "placement": {"code": 0x080EC770, "tables_len": 0x12296, "tag": 0x00531811},
        "placed": {"code": (0x21EBA180, "01dc99f45c121245aa20de81abdb2a2dbeedb9fb9c90b24f4a4f0bf12442c024"),
                   "tables": (0x87CA8BC7, "8d674501496c54bcbfe1ff01256ef27a0f285a7b690b279a76e1e7ba2cd4dda7")},
    },
    "6c2e43558290076e86671cb2d25bd18c90cbeb08e02a1b68e534c0ab4fe808bb": {
        "name": "MD-UV390(CSV-GPS)-P18.16.bin",
        "version": "P18.16 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1300992,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE0400)],
        "code": (0x0807B4F4, 0xF6F6, "0862316ff1dfbc3ae12143796379edafa1b8c8c4131b4842de05dd29bcacbc77"),
        "tables": (0x080D87F4, 0x12850, "801d435cb5cb1a0a18b5468f7d2b58e3e1efd4fe11cc99811bf08b8fc9a314ff"),
        # the codec's data ends at 0x080EAA82; the code leaves 24070 bytes below the descriptor
        "placement": {"code": 0x080EAA84, "tables_len": 0x1228E, "tag": 0x00501816},
        "placed": {"code": (0x9C51AF50, "58b9eca18b7c22c530acbffd9f3d2a1280409a0542e820cf5b63cc77637864e7"),
                   "tables": (0xD772CFAD, "8cccdcdf76790a22b71ccb0db2a4e254d910a2cbc8a8e26ef61e061e58f1dcde")},
    },
    "23c14a9c30b2b626021c39159a8bb68a2403f4eda28fd8101a0264d022fb06da": {
        "name": "MD-UV390(GPS-REC)-S18.16.bin",
        "version": "S18.16 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1313280,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE3400)],
        "code": (0x0807DF58, 0xF6D2, "5b3e6520620a97c45e648a9707c147417f29063902e54aff2a90787e074507ed"),
        "tables": (0x080DB618, 0x12850, "be4549cded7e7acf2d522ef208a69e70fd372dbf1494592c618ddb4d519861d6"),
        # the codec's data ends at 0x080ED8AE; the code leaves 12286 bytes below the descriptor
        "placement": {"code": 0x080ED8B0, "tables_len": 0x12296, "tag": 0x00531816},
        "placed": {"code": (0x1986B8FF, "c122ad619f5ab622b5425bc785d38e71b218c2c900e523bf68bb18dd67b8a9a4"),
                   "tables": (0x3D1F111C, "b064d33818a724d47b3269f04efa3164ce3e3ee54ccd81e29b247705b04fc1ac")},
    },
    "195847c34a037f1c7ca8cc1c69bc9bfd12c2fada22fb1f74a86189281096129d": {
        "name": "MD-UV380(CSV)-V18.03.bin",
        "version": "V18.03 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1284608,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDC400)],
        "code": (0x0807770C, 0xF6DE, "072275d43680ff3455c57dec0c0a5f7f3c76c703546389de58cb60600fc7a5f0"),
        "tables": (0x080D49F4, 0x12850, "83be3c3ee0a31334ffb18ca67052b3bcc81db93858885a2c94fac610b91b43a7"),
        "placement": {"code": 0x080E6B28, "tables_len": 0x12132, "tag": 0x00561803},
        "placed": {"code": (0xFB39A876, "2a9c7c96c8d5a9bdd3c2edeec8ea304905e72e13d7c10f6ebc7e702e5d29d8da"),
                   "tables": (0xB84C8BA8, "6ef5734dfcbae46e919bf67cd0a92d4e0e024767f680c1affc186708fb77a8ab")},
    },
    "6f103808b733260903e03b24de375b7f5c046b8cb3f50551e3b665573221f0cd": {
        "name": "MD-UV380(REC)-D18.03.bin",
        "version": "D18.03 REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1296896,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDF400)],
        "code": (0x0807A1AC, 0xF6BE, "990f7d92bd79c1d6224b2998f12d72d2827683d75f6c2244fce433d5e429532b"),
        "tables": (0x080D7858, 0x12850, "0c5765312fa2a2c5b37e3e36848c7548836d814cbcac901d36afecf531efe971"),
        "placement": {"code": 0x080E9994, "tables_len": 0x1213A, "tag": 0x00441803},
        "placed": {"code": (0xAB2D790F, "49c51082c691675a604defe2aa2d5ec51c0c128e2a1be685b7ca67abd06b9447"),
                   "tables": (0x901D342F, "9fb1baa8a31bebbee1e7a1dd55ccebb4926ebe3de564e1755045e41a81103681")},
    },
    "7b17e39185a89ada84e5f5e46389c8d9af62a54a4ed0b6096b7e5a6e0df9f9ea": {
        "name": "MD-UV390(CSV-GPS)-P18.03.bin",
        "version": "P18.03 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1291776,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDE000)],
        "code": (0x08079220, 0xF70A, "ec90d35c59d697f4ad1c220a25566b50ff8be70e794a3eeb62b869b1abb9fe36"),
        "tables": (0x080D6534, 0x12850, "cba61d1bae859e193e655f8e21ae3e97e0b572584701a832b3d7da9e9b6137d1"),
        "placement": {"code": 0x080E8708, "tables_len": 0x121D2, "tag": 0x00501803},
        "placed": {"code": (0xBF2AB13E, "78b968f2a37ff22816f068639baa06558c92016d8c4501bcd55ef5048936e961"),
                   "tables": (0x15D2D1B0, "acafc376bae6cfd83eb88af3595e8871e2a3e373c96956a9b3a16621fe8960e6")},
    },
    "f816ee4092a0b9e77d6a23bec49508ba5f190c2dc255ac6cc869dfdb67dfb2d3": {
        "name": "MD-UV390(GPS-REC)-S18.03.bin",
        "version": "S18.03 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1304064,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE1000)],
        "code": (0x0807BCDC, 0xF6CE, "1fd15cee5af9a21496e5f7383bf544e7a10497d5e6bf73487677d9368b1ff0b0"),
        "tables": (0x080D9398, 0x12850, "27724f1f0053eb235881ead5062fc988855bf5e9e3b9c465ebda62121677c54b"),
        "placement": {"code": 0x080EB584, "tables_len": 0x121EA, "tag": 0x00531803},
        "placed": {"code": (0x46C1D218, "ff390c87e283292ffaa5e431e9e9a4d7b77e481c51da8ade4b927b0f18cd0417"),
                   "tables": (0x7D1A9F28, "aee0d52cf0ae3ccdd1552256bac33537a3977c201f2d169b315244b9d15256cc")},
    },
    "657cd4e6c86aa63428f1bc081ce0614b35db0607d34ba6b6065357a58a045c89": {
        "name": "RT3S(CSV)-V19.06.bin",
        "version": "V19.06 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1295872,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xDF000)],
        "code": (0x08079E14, 0xF6D6, "21a64c6e0b9b99507765e409f3c26ed28cfdaca4499a7d452c241b4fb88efe5f"),
        "tables": (0x080D70F4, 0x12850, "3a77386ecd001053ea661bea94fd6fe6d024f9482a999bcd8f131da8cb818b92"),
        # the codec's data ends at 0x080E946A; the code leaves 29758 bytes below the descriptor
        "placement": {"code": 0x080E946C, "tables_len": 0x12376, "tag": 0x00561906},
        "placed": {"code": (0x25117C5E, "488c56d79932d3a3495c5c49c398eaaeabf45cd7b68c7fe78df66ac78feec77c"),
                   "tables": (0x22393F1B, "40102395ff82b97be8acd46701b10c6bcc503a54d5221f9aa636086ec15b83bd")},
    },
    "9c58aca48de3d42b897852e424b0da1392f6e983d51ca69b1425b72a0a836ca1": {
        "name": "RT3S(CSV-GPS)-P19.06.bin",
        "version": "P19.06 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1305088,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE1400)],
        "code": (0x0807C1CC, 0xF6DE, "713e2ba7003f5229c78f0b04707ca4ec597ba8aca6953472a66247ec61905870"),
        "tables": (0x080D94B4, 0x12850, "c71415bda534d4872c595e8372554c2f9d83f64fbc385cc140e19797f321abfb"),
        # the codec's data ends at 0x080EB8CA; the code leaves 20438 bytes below the descriptor
        "placement": {"code": 0x080EB8CC, "tables_len": 0x12416, "tag": 0x00501906},
        "placed": {"code": (0xB18F0F57, "b85c5b649a031768796275056d96db8a911d9ff898c2bf5b92c4a5078b20e275"),
                   "tables": (0x02CB0AB8, "10e956193caccc9acaa75efc20c49a4003728029c3d22aa0e04e914ab1dec582")},
    },
    "a2f38eaf4b98379fee6dfc00cae916bbc550e26ba0dd04c5fb56199a9e62b7f8": {
        "name": "MD-UV390(CSV-GPS)-P19.33.bin",
        "version": "P19.33 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1315328,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE3C00)],
        "code": (0x0807E608, 0xF4A2, "5f17438d44f7cc0ef4a9a731a7f0d87893fcd96c096258debe87197f4aae18e1"),
        "tables": (0x080DBA54, 0x12850, "287764a8693d7a08383e15b7ecbf27150cb822fee8ec767b1b38ad597e7c219a"),
        "placement": {"code": 0x080EE17C, "tables_len": 0x12726, "tag": 0x00501933},
        "placed": {"code": (0xB2440AD5, "aa9a2d7dbc1172c28338d212fbfed5a7fb37e9fe990bad7c1cc6f604d3052c69"),
                   "tables": (0x7B71694C, "4cc0ec93689e131f0cafe3db8aec7404ebdd955a50ea5eb70351133005591893")},
    },
    "1e6a74b8cc62ffabd06dd83912f1bf0a7a7ef12d4bda63b961544c7e1393655a": {
        "name": "MD-UV380(CSV)-V19.74.bin",
        "version": "V19.74 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1309184,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE2400)],
        "code": (0x0807CEC4, 0xF4A6, "b2ef9546969f9c529ecc5ab06b31a9529a1af78939594e81e255c49f0dcf085d"),
        "tables": (0x080DA36C, 0x12850, "b05685290b253f9438d2b9ad39c95fae104019b3a59d36290e5e96191defb469"),
        # the codec's data ends at 0x080ECA9A; the code leaves 16446 bytes below the descriptor
        "placement": {"code": 0x080ECA9C, "tables_len": 0x1272E, "tag": 0x00561974},
        "placed": {"code": (0x54149419, "1709ba54483a3b8afa7f48a7bd6696810728663973a2046c735186c573dab4ee"),
                   "tables": (0x605799E9, "bd3063ba6d640dd801979977603890d6b5ccaf32c2912547f994c521a4854fac")},
    },
    "d3996d672d6878ca823880320e240883ea64208f121923cd2bb6fe51d2e50784": {
        "name": "MD-UV380(REC)-D19.74.bin",
        "version": "D19.74 REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1321472,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE5400)],
        "code": (0x0807FA70, 0xF4BA, "0d9b2aad9150af6f3a583d123d69d0ee5c1074bf0282be4fc351a87344581256"),
        "tables": (0x080DD330, 0x12850, "41cb1a328ce46105dd61716a26ab935fefd2e1193edcce076eb7ec998c0fe487"),
        # the codec's data ends at 0x080EFA76; the code leaves 4174 bytes below the descriptor
        "placement": {"code": 0x080EFA78, "tables_len": 0x12746, "tag": 0x00441974},
        "placed": {"code": (0x1924F763, "82fd7abdc2cafc7b5b14b55668c32142da5ecc407e2abe9410cf87a23d5ed352"),
                   "tables": (0xAC72D37E, "923865299733fddd2a2e7140f43b8f7b81914b51ba3ac58b237d2edc6170d0c7")},
    },
    "14e5634eeffa5f99f85fd762df4dc141432762d75b933031000d1f398fc4d2e4": {
        "name": "MD-UV390(CSV-GPS)-P19.74.bin",
        "version": "P19.74 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1319424,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE4C00)],
        "code": (0x0807F450, 0xF49A, "ea58eb79ee38e1b558bf09cbb6428cbc6d45fd3a3ef1cc62430e010b9c7ab85a"),
        "tables": (0x080DC8EC, 0x12850, "a93493fd52dde407f0182e0740bfa000604ad0fb061ba671239cd93f16a67fff"),
        # the codec's data ends at 0x080EF0BA; the code leaves 6698 bytes below the descriptor
        "placement": {"code": 0x080EF0BC, "tables_len": 0x127CE, "tag": 0x00501974},
        "placed": {"code": (0x97C6E76B, "8418e8a1895bc98dacbed9618471d000fc5eb6e2467869023a93711dc8d96e29"),
                   "tables": (0x6E6FD4E0, "5d89a36edb8d975075bbc9d2192ec5ceca91922f36514d6b0f53a48bc99ed0b1")},
    },
    "3ebe806da0bbda993220cca9a97411225c1bf9e29097db3224ff722b00c6d023": {
        "name": "MD-UV390(GPS-REC)-S19.74.bin",
        "version": "S19.74 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1331712,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE7C00)],
        "code": (0x0808201C, 0xF48E, "052bd89a644cc0556aa85fc9924f02cf44156e5903d23ebc49081f83999aaa88"),
        "tables": (0x080DF8B0, 0x12850, "a67ca31a57b8c69bda716197d0170eda0631b0e2bccc2df91d465a0df6e31cf1"),
        # the codec's data ends at 0x080F2086, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x127D6, "tag": 0x00531974},
        "placed": {"code": (0x808DD9EE, "fe08a821a9429b0d6a99cb0491d81da8e4c4c68d8ab8145bda7b29468763d401"),
                   "tables": (0x8414A4A9, "bf20947619f8ee5941639769b019b547e5f81f81a2022c28db6fa251702d6691")},
    },
    "4acf6d48baca843d7e1220de8c0486670ee7f43953ebbb53d910ef1d94bfbe52": {
        "name": "MD-UV380-10W(CSV)-V19.74.bin",
        "version": "10W V19.74 CSV",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1309184,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE2400)],
        "code": (0x0807CE18, 0xF492, "d64a510fec7f77f0fe607fe798351ce1880edd7defbb66b858f0d6a694de2a4a"),
        "tables": (0x080DA2AC, 0x12850, "419e2ef2012298ba8aca092bc74d9ad6b0fd63514d7f1539458a7003ba6a16b7"),
        # the codec's data ends at 0x080EC9DA; the code leaves 16658 bytes below the descriptor
        "placement": {"code": 0x080EC9DC, "tables_len": 0x1272E, "tag": 0x40561974},
        "placed": {"code": (0xE86E3C65, "1633689c91a20a6d12090d621496fd8fba574b976b4a9582e09f99adf1e19f55"),
                   "tables": (0xAC3B0A5A, "4f5db7a08a3fae77b4c0283792098c7411bf7a4882795baf0f3152ce05848389")},
    },
    "2768ece93ddd4f1fd4ffc4e58ee073bd145b865f4ea6d256315f6e0019e93995": {
        "name": "MD-UV380-10W(REC)-D19.74.bin",
        "version": "10W D19.74 REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1321472,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE5400)],
        "code": (0x0807F9B0, 0xF4BA, "20878b67953dbf4a39f9c72f8adb2d58516eebe96bcb2e539c029bb7deeb7ebf"),
        "tables": (0x080DD270, 0x12850, "2a7118e72744f890430d48b3d5f4a9ae6263c62eecfa8e9dbd89703b51fa75b6"),
        # the codec's data ends at 0x080EF9B6; the code leaves 4366 bytes below the descriptor
        "placement": {"code": 0x080EF9B8, "tables_len": 0x12746, "tag": 0x40441974},
        "placed": {"code": (0xAA99FD17, "20ed11eac0453925a5dda66cea050039619df07185a426a10372fe96b86b5eda"),
                   "tables": (0x3F3E347B, "1a241a2adcb9516ba7887ca89df3bf39fb7877290c63151bce36c06bbc0c5ea5")},
    },
    "ebc5e34f4a00679a9200df8233c4b42b89dbd440a7487bbac2796223b796b392": {
        "name": "MD-UV390-10W(CSV-GPS)-P19.74.bin",
        "version": "10W P19.74 CSV-GPS",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1319424,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE4C00)],
        "code": (0x0807F3BC, 0xF4AE, "65aa7a28321218eaa127506ae36b87f66fa64a3b193053dc9474562e5ffa0255"),
        "tables": (0x080DC86C, 0x12850, "15a8f3154fde2f2ca035f563ea9fcc7e447f3b7ad0b2397b72ff3da7fd3f972e"),
        # the codec's data ends at 0x080EF03A; the code leaves 6806 bytes below the descriptor
        "placement": {"code": 0x080EF03C, "tables_len": 0x127CE, "tag": 0x40501974},
        "placed": {"code": (0x1B9C0A2B, "9b2568912d498264f8431d293e16b3a3b7e90e4d0aa522bd5b3972c2377f174b"),
                   "tables": (0x3CA2A02D, "e19f21c2509feaedf9ee150c78435c18353cbc65325e0964cb4ce384c9ae11d9")},
    },
    "b0345e8c9e28b353954f3b6aa66599b5ac682c2037e212052af16e2c888572cd": {
        "name": "MD-UV390-10W(GPS-REC)-S19.74.bin",
        "version": "10W S19.74 GPS-REC",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1331712,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xE7C00)],
        "code": (0x08081FA8, 0xF4C2, "2068803aa297e67aaf39bb17a0f49ddfb55b7c35e66f328923e28f0594ffdc7f"),
        "tables": (0x080DF870, 0x12850, "62d99c50bc1863230892b74b690b43af73822e5951b7879dfcda788923d61f9e"),
        # the codec's data ends at 0x080F2046, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x127D6, "tag": 0x40531974},
        "placed": {"code": (0x92924D87, "b451f426828ab5c35a451574387ba982ea9d777db50c9a720348b4202c55385c"),
                   "tables": (0xC39C3679, "0c6ad6c5e95686cedfb462d00a86da761d8a8b94e6851e2d80d9171fe38035a6")},
    },
    "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee": {
        "name": "RT3S-Firmware-AES255(CSV)-V219.60.bin",
        "version": "V219.60 CSV AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1346048,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEB400)],
        "code": (0x08085BAC, 0xF3BE, "1b9c8dd355cdc6ab18b8aa4579cfe382ce49ec1f493d6048c8ccf5150cdee286"),
        "tables": (0x080E322C, 0x12850, "8a0201819744a522727826dc91332c156c990e7e56244250ba76df9523ad5985"),
        # the codec's data ends at 0x080F5956, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x1272A, "tag": 0x80561960},
        "placed": {"code": (0x620F4AC5, "1326a3afe6b3c760b3e54058ccb631a77b6dd251173900c2a5a70d87095b6658"),
                   "tables": (0xE880FF1B, "d5916da7e1beca142f1c5684bf7dac946676c246dd2140fac268e04515ea3e78")},
    },
    "a7b277137c32ac3583083d8a650488415f62360b7f38a6d903fc732b858dd502": {
        "name": "RT3S-Firmware-AES255(CSV-GPS)-P219.60.bin",
        "version": "P219.60 CSV-GPS AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1356288,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEDC00)],
        "code": (0x08088150, 0xF39A, "0f374a92964736efd8cc2b4ea7a8cb83a190542b3ae1acd3696fa3d22f1bf375"),
        "tables": (0x080E57AC, 0x12850, "c86623e08780f10351696add74e202056d6732435faccdb226ba8b60d059923b"),
        # the codec's data ends at 0x080F7F76, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x127CA, "tag": 0x80501960},
        "placed": {"code": (0x307BE6B5, "e18a8cb7164ddaabebd5b9de75a080e60a647c5e2d6cef3ea22c37ec68aa9503"),
                   "tables": (0xC76CFEB9, "eed0b264b587ca2a7633c731d47ceb63b26e3dc79e683c2d041f2aa5f047c17b")},
    },
    "a8a5265d29f8f69ee050a0a47052a8925a3c403ac270d8e5fda324a4a697b220": {
        "name": "MD-UV380-AES255(CSV)-V219.74.bin",
        "version": "V219.74 CSV AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1348096,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEBC00)],
        "code": (0x080862F8, 0xF3B2, "c69b5bdc350aaccf87abf547e02f50ff6e28ac27036290ad297f9ce847fb65b3"),
        "tables": (0x080E396C, 0x12850, "6598089807521a5edc15cbe2d9812eed3b0d61b7103aeb4ae4a66645873ab36f"),
        # the codec's data ends at 0x080F60F6, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x1278A, "tag": 0x80561974},
        "placed": {"code": (0x254A2827, "4bebab40bd857db6f25934dd931da72c82eff8dd0c76b1594bedb63cbdc438b9"),
                   "tables": (0xEF3C7105, "c18a1932ade0bc8edfd53740d242212315f4d64a9341182fa44cd3dd03ad90e8")},
    },
    "3d1b9ed4a2293981d0915d0655172e7a4d9bc702fe493328be37901427d51108": {
        "name": "MD-UV380-AES255(REC)-D219.74.bin",
        "version": "D219.74 REC AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1360384,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEEC00)],
        "code": (0x08088D84, 0xF3A6, "ddfb1872e8c3439b95b7b4ddbd476f4cd014b74c67cada6209803a001bbeb98b"),
        "tables": (0x080E6774, 0x12850, "65a1dbe29be0db5cc46b3784c571607aff241110a7470ce312e55dab96817534"),
        # the codec's data ends at 0x080F8F16, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x127A2, "tag": 0x80441974},
        "placed": {"code": (0x3E5C5BC2, "b6eba0dd0d9db6b13cde667d2d04eb92379a7c78cf04beaf73fe57e25a1cfd46"),
                   "tables": (0xE588E20F, "7f9ad9a8aeaf92cdd093c8c98db551b66798e144bea53048e959471d7f28ce2a")},
    },
    "70a44a4a77d6ffb4b0b6b9b4a8f26afb9607d65875d2ba1094baf6c8e6a46c9a": {
        "name": "MD-UV390-AES255(CSV-GPS)-P219.74.bin",
        "version": "P219.74 CSV-GPS AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1358336,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEE400)],
        "code": (0x08088878, 0xF3B2, "f5b4998b80deb227262b0a7f5e3dc61c37fdbd64f24819bc8bda0802a00d35cb"),
        "tables": (0x080E5EEC, 0x12850, "47e73474789188eaae78dd8ce8d9624e4a108ba53831c0b87aaad183fa23c6fc"),
        # the codec's data ends at 0x080F8716, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x1282A, "tag": 0x80501974},
        "placed": {"code": (0xBA895AC5, "633d796ba302ba8d35bcafe989fa3e1206c12113af4908152402ce80b4ea0954"),
                   "tables": (0x9418379D, "022ab5139a212ef5a53f0f74d649da3f230548fdd9315fb1901f181436512a96")},
    },
    "bf58116bcd695d8b14634fbffab7cd33ba9bcd1325b92179ef6bf94d380d2151": {
        "name": "MD-UV390-AES255(GPS-REC)-S219.74.bin",
        "version": "S219.74 GPS-REC AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1369600,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xF1000)],
        "code": (0x0808B358, 0xF392, "1db9a37d905da1f6b586ad29d2ba2fe9fe51b930e1abf338ade32f104062713f"),
        "tables": (0x080E8D34, 0x12850, "b5ed5a9cbbb64214f8d02d6e21cfdaecba4697e6bf965efbf087b058ef951687"),
        # the codec's data ends at 0x080FB566, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x12832, "tag": 0x80531974},
        "placed": {"code": (0x87BCEDF2, "8debb133b51a3b890ad3f35c5a590c49d13da8500b61f63e28ae946f15ae22cb"),
                   "tables": (0x0B8910A3, "6bf8b81de1f5047be6e4688827790399f7ce001e27c1d096c196b455a047bcf3")},
    },
    "531947c0d4d7d3ae0c5a3f94d81fbf3a04259488a9af09f9a3a8b57cabf135ed": {
        "name": "MD-UV380-10W-AES255(CSV)-V219.74.bin",
        "version": "10W V219.74 CSV AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1348096,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEBC00)],
        "code": (0x08086244, 0xF3A6, "a71970c3d1699ad74fc9474798a4acb032fb7f8c9c7d1fb4dc337ffbce2fe146"),
        "tables": (0x080E38AC, 0x12850, "e6efe93e0e0193477a08595c738bff7cd794cf90692c55b3cc0ae9af08754025"),
        # the codec's data ends at 0x080F6036, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x1278A, "tag": 0xC0561974},
        "placed": {"code": (0x6A67A328, "c4335412308c809b9a25bde57459576988b82902b8e7e7e4a7b680eefc8d7c92"),
                   "tables": (0x5C092FDD, "dca29d90521cb4ce57ac8812c6cc796d834bbdbb4a9a8e9e57c7865a93bcacb4")},
    },
    "7b7ed3075a7735cde969402eda163f96bf2931ae55cbcc4fbf62625bdaf04855": {
        "name": "MD-UV380-10W-AES255(REC)-D219.74.bin",
        "version": "10W D219.74 REC AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1359360,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEE800)],
        "code": (0x08088CC0, 0xF3AA, "002d8a76707d5c65a5b2988b2c1356528677986ca5e0143bb65ae8b7ee060047"),
        "tables": (0x080E66B4, 0x12850, "8a8952a64c0821b99ed89d394486e37af3b36689833f77a21534c06d9ce90d78"),
        # the codec's data ends at 0x080F8E56, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x127A2, "tag": 0xC0441974},
        "placed": {"code": (0xD68C85D5, "9475f9e8c3144cbd07286dc71ea01d445210bbcc60fc93dc3a9e667787a10407"),
                   "tables": (0x60ACDF51, "ef40b4530d4610776055f32286cabd816df65bfdedd4b14a0da8cb329179a267")},
    },
    "bf98dc6ca9d888712924af75dd5eff7495de4ab82848f0701616e29eb9d5e5a2": {
        "name": "MD-UV390-10W-AES255(CSV-GPS)-P219.74.bin",
        "version": "10W P219.74 CSV-GPS AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1358336,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xEE400)],
        "code": (0x080887F0, 0xF3BA, "a55a811d85a5d23e3d599b312245b32ebdd70b2a75384772be12422025a4ee7e"),
        "tables": (0x080E5E6C, 0x12850, "76a96e79d10f71834d94a914ce359c6492cab2a037b23a472a5ed13b446d36b4"),
        # the codec's data ends at 0x080F8696, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x1282A, "tag": 0xC0501974},
        "placed": {"code": (0x84C3921C, "47c2588bc188623239215aec455a92d7a2a135351fdc7fca8b0fc9bc336274ec"),
                   "tables": (0xE429F823, "7c66ac0af9f1cdf0606f544c46c4725fcea01302d860a4c515a9c8ad5f5b2aa6")},
    },
    "e6c696a3f1a6ac3d622559d7422efc97399b748aac8923a7440239b28c03a5c1": {
        "name": "MD-UV390-10W-AES255(GPS-REC)-S219.74.bin",
        "version": "10W S219.74 GPS-REC AES 255",
        "board": "mduv380",
        "key_sha256": MDUV380_KEY_SHA256,
        "codec_profile": MDUV380_CODEC,
        "size": 1369600,
        "segments": [(0x00060000, 0x5D400), (0x0800C000, 0xF1000)],
        "code": (0x0808B2DC, 0xF38E, "fc272e75d71dcac08215794f5d153bac86a0787423f7b187c24b08caa3717eaa"),
        "tables": (0x080E8CB4, 0x12850, "934bbea39629c875be4c1aeab52c2e54605f7a74538435ff84b60eb400405c58"),
        # the codec's data ends at 0x080FB4E6, too near the descriptor for the code: it goes to the start of sector 10
        "placement": {"code": 0x080C0000, "tables_len": 0x12832, "tag": 0xC0531974},
        "placed": {"code": (0x4703BB9F, "cc49a7b443a622d410c3adbd41ec7738f6967985da7f33c2e4504084319677c5"),
                   "tables": (0x64B8EF5C, "c3e4eb53e2dc7ea257c66d731779362678064df29fa17050b30e120daf56e6ce")},
    },
    # TYT MD-380 D35.14 and MD-390 S35.14: the UV380 family's codec (its entry offsets), with three table windows. The
    # main one is in sector 9; the code goes right after it, the two small ones stay at their address in sector 11
    "ff6c3d79b205391c9c2096f8e4367c514c1e1e4dac7d421ed69077e5b42eaf2f": {
        "name": "TYT-72310-11-MD380-D35.14.bin",
        "version": "D35.14",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 947712,
        "segments": [(0x0800C000, 0xE7400)],
        "code": (0x08087A10, 0x10366, "e688dcafd05bf1cc6e0bd47987218cd2da60137b8621731906b39a1b808a42ef"),
        "tables": (0x080B7868, 0xCD68, "7bb8f589e446b6b94a3ca49a3afeceb4bbd97daf878fb959eee5ff2c10bb8174"),
        "extra": [
            (0x080ECC98, 0x87C, "2b71d2df1634ae8d0a9f3dd1623a9f34c3b5ae9b5e4f1298275cec69310fbd94"),
            (0x080F0C14, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x080C45D0, "tag": 0x00443514},
        "placed": {"code": (0x83A0361F, "618da7ae4148b8098718fe7faea599ad021cb7466fcb64abbeccc565aaeee9eb"),
                   "tables": (0x810CC7B3, "2614009182836cad98a91d4e86206353d06062c2b9bd9034d1e2c537888f8ce0"),
                   "extra": [0xA5F0D272, 0xEFDB66A5]},
    },
    "167e79f18e78d7d31460dc577c4efd0cb881344f5ee44a4c9a30798ead0fb79d": {
        "name": "TYT-72310-11-MD390-S35.14.bin",
        "version": "S35.14",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 951808,
        "segments": [(0x0800C000, 0xE8400)],
        "code": (0x08088AC4, 0x10372, "ec97cb7081c6e0f8c1eb02414e0cc98350b279565865728f018b8864e05aff6a"),
        "tables": (0x080B8928, 0xCDE8, "6688be0265fb9b8cc1b3cb00a37a84c3abe883459cf04ae1b3c3dab7497ecb92"),
        "extra": [
            (0x080EDDD8, 0x87C, "9758c5e73d747a202de7b7987a6c6080af5e8960cb983b70f7969af971298783"),
            (0x080F1D54, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x080C5710, "tag": 0x00533514},
        "placed": {"code": (0xD953C7A8, "ffc1337565f53176acb2a1f87380d78368846e342c24e9251c674aa11ce96e76"),
                   "tables": (0x68B2CC40, "8600f3786487d34b56af7540dd0b3c85ae93c4a9303ee9c25954eedc9ec18b4c"),
                   "extra": [0xA2B85A33, 0xEFDB66A5]},
    },
    "0897e946bb123a5942f1458d3d2be8f759befe769c6aeaad4fe093ad3a40ac01": {
        "name": "TYT-TFT-MD380-D3.32.bin",
        "version": "D3.32",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 997888,
        "segments": [(0x0800C000, 0xF3800)],
        "code": (0x080532CC, 0x1087A, "34993c7fd3e52587efd74703b9405c403cf6a9f5409908b5b40f4ffd7d416622"),
        "tables": (0x08090BF0, 0xAFF8, "ae853c31afe173fdbf4bf5bad61a761bbbeb53e2b229abbed76897960478aa89"),
        "extra": [
            (0x080CFFC0, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F86D8, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F8F2C, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FC61C, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440332},
        "placed": {"code": (0x07369C2C, "4f67d05cb97637bfb4853a588b9fafe238a92be0137509701952ac8a1a0b4356"),
                   "tables": (0x2445FE8F, "9c497a0f098ae62ac0cd8a56c655979e68497070fd2f37e3b73145c4dd031faa"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "0c954b0fa8426352cf9c99544be4811115f4102fc4ae491269d1cdaef3edd54a": {
        "name": "RETEVIS-Vocoder-RT3-with GPS-S14.04.bin",
        "version": "S14.04",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 999936,
        "segments": [(0x0800C000, 0xF4000)],
        "code": (0x080541F4, 0x10342, "54b241008f34f10a0102c26ea1ed91058cd78b2020528cbb04934cfc4d263edb"),
        "tables": (0x080915E0, 0x92FC, "2b20d1d352726e73b711646947916e4bbf83b07adbd8404d8120cca104d79e43"),
        "extra": [
            (0x080D098E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F90A4, 0x868, "48f772d9233e0b6d2e90b2f61b41c2b85af3be607372cd00aa3a94b9481fc2a5"),
            (0x080FCFF0, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x0809A8DC, "tag": 0x00531404},
        "placed": {"code": (0x82FE5C5D, "cfd4a239f3c80f85fd0a52549941ddf37e542429b8925baafef7ad3314577d6b"),
                   "tables": (0x2445ED1B, "4a91f09555569cd38108503973e93dbfe3c39784ea35d8d2094b090201e112e7"),
                   "extra": [0xA28CCD40, 0xB8A4973B, 0xEFDB66A5]},
    },
    "c375de54f172346d3f9aecc73cbfa024e753b482e484765b1a890182cfac157e": {
        "name": "D013.034.bin",
        "version": "D13.034",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 996864,
        "segments": [(0x0800C000, 0xF3400)],
        "code": (0x080533A0, 0x10356, "fec835cb7c1fbdfd51c9c57676da03d8cf92757eec117a474f48c06a7bd432d4"),
        "tables": (0x080907A0, 0x90D4, "97630959bba2380fd911c19157cd3669d098e7326aa954994fddf3564d216c25"),
        "extra": [
            (0x080CFACE, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F81E4, 0x868, "3addb679e4dcd0167a6873286d7dab19ef563e3f9334c9f7d5ddfe17f5969c09"),
            (0x080FC130, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x08099874, "tag": 0x00441334},
        "placed": {"code": (0x2E0C4561, "9e1ae53cce83ff2e1bef7caf0aed2cc7d30d015ba7636f757a94930cc3d44465"),
                   "tables": (0x15B8E25A, "b082aabd27e9ae978d4832a71ed32a8a23c9b0531ca917a394db2881ec3cf89c"),
                   "extra": [0xA28CCD40, 0x117A8303, 0xEFDB66A5]},
    },
    "db479b26c0c6aed81e9a3d2f7b2bf7476dfccc401c7bdd8976b321b31b967f42": {
        "name": "TYT-Vocoder-MD380-D13.14.bin",
        "version": "D13.14",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 995840,
        "segments": [(0x0800C000, 0xF3000)],
        "code": (0x08053014, 0x10362, "1b387ae5bfd41ac78a76185181d1a17524de24d6058356caa594347973058a61"),
        "tables": (0x08090410, 0x94E4, "ba9e1dc3e757c51143c5a0073d61506e2d8559373768feee35ae3027b7fe2cb2"),
        "extra": [
            (0x080CF71E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F7E1C, 0x868, "3b331ca3701a46ec2663d579b90e6b04dc975d24f4eb175478c3989aac56b356"),
            (0x080FBD50, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x080998F4, "tag": 0x00441314},
        "placed": {"code": (0x57BE4B8C, "4f015852398e306af4c97f0fecb85654f0390cb38552da3bbaac0238b8d3ce90"),
                   "tables": (0xF78E94DE, "8cb2128d970d1172364510efb5af1161dfd7fa8700163e5e8da89056b92c55d2"),
                   "extra": [0xA28CCD40, 0x42C39178, 0xEFDB66A5]},
    },
    "b92052ea958b15687992cbf796f00a3dbcb33113426def550c32c7dd4e20cd95": {
        "name": "TYT-Vocoder-MD380-D13.20.bin",
        "version": "D13.20",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 995840,
        "segments": [(0x0800C000, 0xF3000)],
        "code": (0x080530A8, 0x1034E, "432d352736b3c894bf09fb394e003fed404933aac5f650b9d81fed317ce01050"),
        "tables": (0x08090490, 0x94E4, "0277e18dcec6d3a6aa4ede5b8c61485e001d06a47265c33b522ce9ec75edabb9"),
        "extra": [
            (0x080CF79E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F7E9C, 0x868, "29314537dba2cc2c0ae1583a451e0510705f85504a94213ccca385020343c95e"),
            (0x080FBDD0, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x08099974, "tag": 0x00441320},
        "placed": {"code": (0x8AC4FAF5, "6e52bd232df80ccc418e0051b9256e009914ab0e73c5cfd3dbbab73e49eb60a0"),
                   "tables": (0xEC93E5BC, "5b95262d987d96eef10422b1acedf25e7c400d297cafa7b3f9694209838d0188"),
                   "extra": [0xA28CCD40, 0x6DE76FF7, 0xEFDB66A5]},
    },
    "c0053729cfe3155ae19089dd1accaa3a63e2382d6c8d8a50304c548c9dc2a311": {
        "name": "D014.004.bin",
        "version": "D14.004",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 996864,
        "segments": [(0x0800C000, 0xF3400)],
        "code": (0x080533B4, 0x10342, "bef67122e54f3336db830fc6142324ba2be6af3ef15c2317ef3c3d0735f084b9"),
        "tables": (0x080907A0, 0x90D4, "f89958871c799619365dab7e903939d365db6412f1aa6d4dc9c71d61bab7dc35"),
        "extra": [
            (0x080CFACE, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F81E4, 0x868, "3addb679e4dcd0167a6873286d7dab19ef563e3f9334c9f7d5ddfe17f5969c09"),
            (0x080FC130, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x08099874, "tag": 0x00441404},
        "placed": {"code": (0x3337E7B8, "7944e4d1f8003ee02bc2ea0dd2c071355da5cb1a6a8b0aa3d7e46e27f6b5e351"),
                   "tables": (0x843FB58D, "81e34035eebe7fd9742781f8bc7430f1930f69d82b8ceac3a57c8ef4883680c5"),
                   "extra": [0xA28CCD40, 0x117A8303, 0xEFDB66A5]},
    },
    "e0a65ef4ba358abd66ce7041b571df5a1b3047b9f113d3cad6f8ab99b6e972be": {
        "name": "TYT-Vocoder-MD380-D14.07.bin",
        "version": "D14.07",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 996864,
        "segments": [(0x0800C000, 0xF3400)],
        "code": (0x080535D0, 0x10366, "a075d45548528e80b42f023d8ba58664d9d3dc3b11e459d0d94df39ea693f460"),
        "tables": (0x080909E0, 0x9122, "0d79e0dbca5ed129c7602b6676d646aa6a6b8581783533a21688f43aaba3c6f3"),
        "extra": [
            (0x080CFD0E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F8424, 0x868, "7c5690358d089fd468b67074ce9fbef927b7772f68ab1019fda75ce88a9c6496"),
            (0x080FC370, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x08099B04, "tag": 0x00441407},
        "placed": {"code": (0x6AC85EF3, "7dda12c68d418873dd90dc04f11acd7b20aef9c1e545a563c52c1cb037d4d429"),
                   "tables": (0x0571A6F1, "003de637a35ad6b301fa9b8f1e1c426cd6ecb74133e69a4a206bf237cf8dd576"),
                   "extra": [0xA28CCD40, 0x091CD076, 0xEFDB66A5]},
    },
    "18d0080b41cde2871a4801ea0de1876cb3320c2498f3ea04f89d647b7d722e74": {
        "name": "Upgraded software for newest version.bin",
        "version": "D2.24 (upgrade pack)",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 999936,
        "segments": [(0x0800C000, 0xF4000)],
        "code": (0x08051FE8, 0x1089E, "aa0f05c12b26b5a5a19d363a030bfafe4897b25e856603c6cc7f8066d79ed893"),
        "tables": (0x08091024, 0xAFF8, "b3eb82756454e123fa9694d2398a0aeba166fcb424e5874bc660326cdbcde9e8"),
        "extra": [
            (0x080D2338, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080FA9F0, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080FB1CC, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FE7C0, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440224},
        "placed": {"code": (0x318B0FDA, "8b041d33ad6c64f29e94a2c394b21edbc1b9a355e32221640e6340450fdad32b"),
                   "tables": (0x68F0B40D, "5c6f49bfeb608563f5c95cc2f6fc518743a68907d35aeb82e2b3908b32d88c82"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "5d0e5671a2e2ee8d2163325b442448e5494b93bd1a3e48e978ddbee6bcfd5bc5": {
        "name": "MD-380-D2.25.bin",
        "version": "D2.25",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 993792,
        "segments": [(0x0800C000, 0xF2800)],
        "code": (0x08050A4C, 0x1087A, "88fac49703349067e2a9ee5dc9db167598eadae65e10ccee8034d51aaad683b4"),
        "tables": (0x0808FA64, 0xAFF8, "f9c81be2d60b67ca1c3be0c443aa2b3441966240b315287b60913f7c2b0409b4"),
        "extra": [
            (0x080D0DC8, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9480, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9C5C, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD250, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440225},
        "placed": {"code": (0x308F0A9C, "8939f32185d4caa9c756ad2f8c60ab0e686f7b7d9f7218edd49d531bf5791d5b"),
                   "tables": (0x8C7DA6F8, "b1b011a6284103afec955b46f17d3a3a524db9c89a5933f2de6eba226da1bd09"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "348f6a6bcddfe80327b5da29a4074fc036bace344891dcf8b7b2b76e7f20b60f": {
        "name": "MD-380-D2.26.bin",
        "version": "D2.26",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 993792,
        "segments": [(0x0800C000, 0xF2800)],
        "code": (0x080509E0, 0x10866, "13d2e102856feea6bf24eea41ef314379c3848e58da3de45baf6443f734dfedd"),
        "tables": (0x0808F9E4, 0xAFF8, "0e9d5791143be07cdb5c0ded703b43b6ea889859e3dbb5891983ac5214f40a0c"),
        "extra": [
            (0x080D0D48, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9400, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9BDC, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD1D0, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440226},
        "placed": {"code": (0xC4F140C2, "46a71a0fb0747429690615c2817709096c21255e374e6114311133db18b5ce30"),
                   "tables": (0xEB9AE318, "f65b2502c2b4ab926272869293507515d9a88a3936b362d4644f30e5d4f2ddcb"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "34ffbc629eb198c4c853c8081f441303a488f80eee5818826b3838b6abcf867c": {
        "name": "MD-380-D2.27.bin",
        "version": "D2.27",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 993792,
        "segments": [(0x0800C000, 0xF2800)],
        "code": (0x080509E0, 0x10866, "c86733b8ad74948c6ce3d25491594539d0da017d9cee8f19de8e924ffb7acd56"),
        "tables": (0x0808F9E4, 0xAFF8, "7b1b1d259324fb3efc4b6694b03cfba11349e9d909d23b98ce5f757f09c824d7"),
        "extra": [
            (0x080D0D48, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9400, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9BDC, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD1D0, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440227},
        "placed": {"code": (0x662DA478, "6937931beb7d96d20802f093617fe9819c49296fdbbb970955949dc0a87dd96e"),
                   "tables": (0xEB89B60B, "9dc304aa563361c03b9d6f0b02dc9b53fa77faaa0003d35090df78689ea4d8e4"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "1c903d440e35917e8a16906ccae1cbbd539230a5a3352e3eb896ffc3e96eccc6": {
        "name": "MD-380-D2.30.bin",
        "version": "D2.30",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 994816,
        "segments": [(0x0800C000, 0xF2C00)],
        "code": (0x08050C1C, 0x1086A, "0ef74c3c4150ff34bede75c60e60ca5415e839cfe42916cedf2d8b12c2f906ae"),
        "tables": (0x0808FC24, 0xAFF8, "1d15c8973bdec3e8a7ec1b03a3c9f5d9acb8e64ea29e698ed341de5e6d9bcc73"),
        "extra": [
            (0x080D0F88, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9658, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9E34, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD450, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440230},
        "placed": {"code": (0xF47AF71A, "f171f3d2ffd2d09a9d8eeb9c9804788b8565477ce65e74a51e2aec65430fce40"),
                   "tables": (0x41CFB9A5, "db5850acacdbea9baf90dbe9c2181bb270d47ef4e23b69f85f6ed701e9d7240f"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "9d9a9599f43ef82caefd9239cec3770cc6b411e47f760fa07c6f22167e26fdfb": {
        "name": "MD-380-D2.34.bin",
        "version": "D2.34",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 994816,
        "segments": [(0x0800C000, 0xF2C00)],
        "code": (0x08050D74, 0x10892, "5ff652f6ece172b04b7721ecb39e7ff718ca44ec07567dfc27284b89285e9d75"),
        "tables": (0x0808FDA4, 0xAFF8, "cc0a31a311b963294a95d22d333d4703a8b3cd0d697e0aaf8af391763f666993"),
        "extra": [
            (0x080D1108, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F97D8, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F9FB4, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FD5D0, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440234},
        "placed": {"code": (0x777AD2AB, "d6a2414104c68b786a22a370660fceaad4a37f6b63958a616a8fc017e44f02c8"),
                   "tables": (0x2F6FFE2F, "5e81579827e229dbc7cb1158a2ec30be028ba9edad482eedf1e96fd63c155188"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "466a5d6973926de2148f4ff32382e235ed047c2ef27d5ec5683ab9b91b89489d": {
        "name": "D003.008.bin",
        "version": "D3.008",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 995840,
        "segments": [(0x0800C000, 0xF3000)],
        "code": (0x08052D30, 0x10896, "17d8ddc9e8f3f54a49b7fa02d7af16a37650ff2ed033396095c09e9bef4737f9"),
        "tables": (0x08090660, 0xAFF8, "2f985f6f9e172d0f3b555ab86483fc7139b41e46ba4ff68af6ef818c919613df"),
        "extra": [
            (0x080CFA10, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F8110, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F8964, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FC03C, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440308},
        "placed": {"code": (0xB3773804, "23f08398b075ed7ffad2208238d7bd84054437514b55960117465af9429c2117"),
                   "tables": (0xBF6A2D83, "806394e6a72ba29479f7959cd59e7d78fc0eb97080b907ea50e582e7a20ba10c"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "38787da956234f9ec811cae08568f7bdb5f21b4070681ae689a0ba82c845f973": {
        "name": "TYT-TFT-MD380-D3.20.bin",
        "version": "D3.20",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 996864,
        "segments": [(0x0800C000, 0xF3400)],
        "code": (0x08053044, 0x10882, "82b09daa92f4de3033b7819f5aa40e0e74db14749f162eda8cfb8316d3457008"),
        "tables": (0x08090960, 0xAFF8, "6f0790a8e0f9e3e8e69f59636d3d1190c9e847752168d422337c26a5e995272b"),
        "extra": [
            (0x080CFD10, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F8410, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F8C64, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FC33C, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00440320},
        "placed": {"code": (0xAA1D7DD6, "9ef2a7ba68e0c5bfbda64aa617d54098858c1b5b37f960558844e07580c124ea"),
                   "tables": (0x1B844817, "8ad66d4b1ae0b7596fae138ca9b52f6dd431c1fd061227be217be8d4c8fea3b7"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "c8a98648d01c34ec8dca208eb0fff903ac0c9127a46ed482b7ca8e5755396d3d": {
        "name": "TYT-Vocoder-MD390-S13.12.bin",
        "version": "S13.12",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 998912,
        "segments": [(0x0800C000, 0xF3C00)],
        "code": (0x08053CB0, 0x10346, "da1169ef6de5e48090ad6e32de7a6994c3a21d3535bf8377f0ab3f2a8744ce21"),
        "tables": (0x08091090, 0x9294, "f21ed0df900ad3f21d00a68d0930d0cd5d6123a13acbb4cc4b4dd9f9aa3226d0"),
        "extra": [
            (0x080D041E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F8B1C, 0x868, "930454e770d09429d313ca1e22d3d403e4461119f640c1ab4de16fce0edf5b2a"),
            (0x080FCA50, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x0809A324, "tag": 0x00531312},
        "placed": {"code": (0x159EF17E, "d58838e873635789774396a30b0b156915980037effaddadaf755927ddbe7dac"),
                   "tables": (0xA649816D, "890351e1e7546f5ac04c6debc49546edeb217400754523ee25088b053d404e74"),
                   "extra": [0xA28CCD40, 0x1240DAC1, 0xEFDB66A5]},
    },
    "5ce02f5f68fcb4d977d3b209d0d3911743ed300ebfb89d583a4a83c10208ed26": {
        "name": "TYT-Vocoder-MD390-S13.20.bin",
        "version": "S13.20",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 999936,
        "segments": [(0x0800C000, 0xF4000)],
        "code": (0x08053EE8, 0x1034E, "f6ab8b19d65b1359472ac5eeb1baf7cdf5e60f36ac9632431c739cd2a47b8652"),
        "tables": (0x080912D0, 0x9564, "e71033661e4b7ec0d6a87e3784b151817038ee1daa2b234af761c375490f5174"),
        "extra": [
            (0x080D065E, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F8D5C, 0x868, "ee83d9087904246e5f5f4735055024c7e2f097038bb2616dd6eafa0390d1dac2"),
            (0x080FCC90, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x0809A834, "tag": 0x00531320},
        "placed": {"code": (0xB91DF0D1, "2ffc0646cd6e7efd69f35f5e4469028eadd7b2d67dadb331853e4e83de54434c"),
                   "tables": (0x7E7F431D, "af4410b425fe5a1d1f579f1ec59bef9dcf56cf33492e35572e977a33b5ef4aeb"),
                   "extra": [0xA28CCD40, 0xFB8963BF, 0xEFDB66A5]},
    },
    "1fc264949d497af9af7dbef541d59d9690f1f9c0167601f5011e17e196584757": {
        "name": "TYT-Vocoder-MD390-S14.07.bin",
        "version": "S14.07",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 999936,
        "segments": [(0x0800C000, 0xF4000)],
        "code": (0x08054238, 0x1033E, "fe2fd2462a035160b60d13907dbd44db8a372b37233ecb8ee03ed88ff192422b"),
        "tables": (0x08091620, 0x9154, "efb4585cb5be6f523c6941798241375ab162efce9e1b9618d4867775ddaf3c09"),
        "extra": [
            (0x080D09CE, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F90E4, 0x868, "9816d6dd320f2e11898b40f441b88593b258fcd9769d4a72f90636ab5dd35923"),
            (0x080FD030, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x0809A774, "tag": 0x00531407},
        "placed": {"code": (0x41705E75, "36c3ffd9f8c4c82b3ff022117f15a8fe9ee117d7cad312e1bd91c5ac83c6a844"),
                   "tables": (0x12AC6B06, "08d796a4a81e46c41599c65752ed7d2dca1c9942b3fd1ae51cdbc5a8204393bf"),
                   "extra": [0xA28CCD40, 0x8A5DE917, 0xEFDB66A5]},
    },
    "823439219e82f142ddb707003b6d6affa3993e6dd5bee99771a1f7f883de2046": {
        "name": "TYT-GPS-MD390-MD380-S003.012.bin",
        "version": "S3.012",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 999936,
        "segments": [(0x0800C000, 0xF4000)],
        "code": (0x08053C4C, 0x1087A, "6017e8613863bf086b1393951f8d05c9fc96f6b2fc6666b325e964a661b5d6ed"),
        "tables": (0x08091560, 0xAFF8, "dcafb6df9f87b6f6b98cf10aa8b20285031f53e662e56a464e6269742898dfc9"),
        "extra": [
            (0x080D0990, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F9090, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F98E4, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FCFBC, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x00530312},
        "placed": {"code": (0x04E6AF17, "ee4d5dd90565845c3ef9e5d6d7a948ed8d08f3ce216eccafbb0d205367ba9040"),
                   "tables": (0xB25BA64C, "30dc217d5512a06cbe4c35cc02430be1135c5cfebc8914f4d7f97c3515871fb9"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
    "6b83a74b72df4b13f74f5ac7ef2472593809055fc252c0aabe4c9eeeef1f115c": {
        "name": "TYT-Vocoder-MD446-D13.20.bin",
        "version": "D13.20",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 995840,
        "segments": [(0x0800C000, 0xF3000)],
        "code": (0x080530C4, 0x10372, "e371d69728e6fa2ddb107908eab9a2afc9a0cfc86a80ce04659c8b90eab2924e"),
        "tables": (0x080904D0, 0x90D4, "e4df44a26b37ca4453c73dc134b4d944b89bcaa69c14455794a3b5b9d7652ac4"),
        "extra": [
            (0x080CF7FE, 0x4E, "f4602c650e41e1caaa65e93bff8123f02f87ff488f46b49fffe2577f89eb9905"),
            (0x080F7EE4, 0x868, "62ebfe76db052595a1a02daf96388810effaa25b37956d92ea056cbe600fa544"),
            (0x080FBE1C, 0xE, "d75975040dd220ce557972812fd91bf6b58eb360185a7157ba910ce7fae4319d"),
        ],
        "placement": {"code": 0x080995A4, "tag": 0x04441320},
        "placed": {"code": (0xBD3277AE, "11bccdd802e5729d983c5f812ff4f81e9bb87611bf3645ce7c4acbc6d222203a"),
                   "tables": (0xCC12D2AA, "c2783e003f71db40b71437211c564c101a8a16c099d0d98cf855b66b503363b7"),
                   "extra": [0xA28CCD40, 0x9C044751, 0xEFDB66A5]},
    },
    "9892b2e95da0eee0ec87604007f157f6f9b249a9fb11ba0fc3798e78956e2587": {
        "name": "TYT-TFT-MD446-D3.20.bin",
        "version": "D3.20",
        "board": "md380",
        "key_sha256": MD380_KEY_SHA256,
        "codec_profile": MD380_CODEC,
        "size": 996864,
        "segments": [(0x0800C000, 0xF3400)],
        "code": (0x08053060, 0x10866, "6b9af1fc700fca61048e1478edece10365605a1d62871b30138d229689384fb7"),
        "tables": (0x08090960, 0xAFF8, "755f8143c6f43c1a585d28fbf5ac7d7c2b93e6d8f8ef7fb3cffdbf130e40988d"),
        "extra": [
            (0x080CFD30, 0x24, "d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2"),
            (0x080F8418, 0x18, "6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375"),
            (0x080F8C6C, 0x14, "0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017"),
            (0x080FC348, 0x20, "b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812"),
        ],
        "placement": {"code": 0x080E0000, "tag": 0x04440320},
        "placed": {"code": (0x54F942E7, "c95719b5fb2d403ea0c31d3fa70cb27482b4829cdfd5d5df5c945336b54824cf"),
                   "tables": (0x55648D6B, "2a388c51ec726f26514152efab7d3d5a572c5842be625f1f16f4ddfd55ecd635"),
                   "extra": [0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF]},
    },
}

# Stock files that are not codec donors but are known DM-1701 files: the key can be derived from them (any of them;
# codec-dm1701.md "Stock files"), and they tell the user which version they hold
KNOWN_FILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": "DM-1701 V02.06 CSV",
    "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9": "DM-1701 V02.06 noCSV",
    "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948": "DM-1701 V02.03 CSV",
    "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a": "DM-1701 V02.03 noCSV",
    "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7": "DM-1701 V02.02 CSV",
    "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713": "DM-1701 V02.01 CSV",
    "de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532": "MD-9600 V16.45 CSV",
    "7f4d760288f0c1875c7c5dc83dcdf3f355f5188becde2b505eedf609fdbe665f": "MD-9600 P16.45 CSV-GPS",
    "d2ef7a7e9c484dc1a8777488816f4da47181899ff8b7299a0ead7d141d55e10c": "MD-9600 D16.45 REC",
    "3d71b4358267fe5fdae8980f340b6a158e7b229c9d32c8ab894c4985964e8aff": "MD-9600 S16.45 GPS-REC",
    "79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09": "MD-9600 V06.09 CSV",
    "9454eb221ae1f206fca5a15ec1b540bf06b3db4991f466b1626a3a3073ca91d5": "MD-9600 P06.09 CSV-GPS",
    "7b1b1b33e661766a5b0c0b3ddab9c31c96e3855b09f4e280b07f4990b563b68f": "MD-9600 D06.09 REC",
    "4610ed115e80efa04be554a94915ec2e053ae00f6b64dfa4815451966840dba7": "MD-9600 S06.09 GPS-REC",
    "194526d60f796cc0f7339d14c1efeedc267731d108fd66a96876b459408de0ba": "MD-9600 V06.24 CSV",
    "1eafdb774fd71cb7bc3334d06617ed42264ee25df1c73f61744f6b21742bbd20": "MD-9600 P06.24 CSV-GPS",
    "ec44929e855442ad248f1a5c171a467922d6cb5742fc35f759db269c645e22ab": "MD-9600 S06.24 GPS-REC",
    "6643261e62b9778a5df43b1e5c3428d6d308bf54b34478513df3e08f6f4dbb63": "MD-9600 D06.24 REC",
    "98302fbc26fa0db93bd7ecbd29a410ac9fbb42cbd7d6ce15d54ce085fefff329": "MD-9600 V16.24 CSV",
    "e86b9e9775758548dd2d01334cfacc96c640fbf843aa6c5fa009b4bc7598e98d": "MD-9600 P16.24 CSV-GPS",
    "4e1c5c081d5bdf4bef8f205668fd7381391187d8b07150df7e99275b030b811b": "MD-9600 S16.24 GPS-REC",
    "e21c72e30716fb43e4d15798012aec15e964b9ec8c1a534ede5d0b0fe73f3acb": "MD-9600 D16.24 REC",
    "02e366687f122ed752581344db8316666d21782d9353334518b69237a81e74dc": "MD-9600 P26.45 CSV-GPS",
    "6df34152e9786530d61032ab71f1962cf194aca0b99cf0a54e3802fd2cd4ac72": "MD-9600 S26.45 GPS-REC",
    "baee36b0fd852dfc1fb14952242a1231dbfa4fabc37cba57c61f1b8d3e0d830d": "MD-9600 D26.45 REC",
    "2522008737d7845e43ab47a307d38498ee783c788a377fac855dff94dea53af0": "MD-9600 D26.57 REC",
    "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f": "MD-9600 V26.45 CSV",
    "e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43": "MD-380 D002.032",
    # the bootloader key and a restore to stock only: no codec profile (the codec comes from D002.032)
    "1c903d440e35917e8a16906ccae1cbbd539230a5a3352e3eb896ffc3e96eccc6": "MD-380 D002.030",
    "75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e": "MD-UV380 V19.26 CSV",
    "7dec4b751e8a0f44cf7d54d0ebc3b439a9f49899eadd7c8ff14d2804edbe8ac2": "MD-UV380 D19.26 REC",
    "84331dbc9bccf5021122b715dcb77d7589dba0745694bf3a249aae6e7c383ef1": "MD-UV380 P19.26 CSV-GPS",
    "77923d67aa75a07f7601a1c53ae1d5f5af37bd4367d52a50c8aeff89b834eceb": "MD-UV380 S19.26 GPS-REC",
    "fb1897b7123dc6a039a5266972c5bb6f563f574d3769bd1cbdc7bb02b5039cc7": "MD-UV380 / RT3S V18.11 CSV",
    "78650258f65fef33d78fad65678bedd74e44b75e924466829c9a360ce8915f39": "MD-UV380 / RT3S D18.11 REC",
    "e31ecd122f77ccd34e563193b004e8621ff5422676521503521f64f1045f73a9": "MD-UV380 P18.11 CSV-GPS",
    "fbb307fd14577e48e1fb6edb3395d4d684baf12dd9fed66280ad76d12e0e04c2": "MD-UV380 S18.11 GPS-REC",
    "6c2e43558290076e86671cb2d25bd18c90cbeb08e02a1b68e534c0ab4fe808bb": "MD-UV380 / RT3S P18.16 CSV-GPS",
    "23c14a9c30b2b626021c39159a8bb68a2403f4eda28fd8101a0264d022fb06da": "MD-UV380 / RT3S S18.16 GPS-REC",
    "195847c34a037f1c7ca8cc1c69bc9bfd12c2fada22fb1f74a86189281096129d": "MD-UV380 V18.03 CSV",
    "6f103808b733260903e03b24de375b7f5c046b8cb3f50551e3b665573221f0cd": "MD-UV380 D18.03 REC",
    "7b17e39185a89ada84e5f5e46389c8d9af62a54a4ed0b6096b7e5a6e0df9f9ea": "MD-UV380 P18.03 CSV-GPS",
    "f816ee4092a0b9e77d6a23bec49508ba5f190c2dc255ac6cc869dfdb67dfb2d3": "MD-UV380 S18.03 GPS-REC",
    "657cd4e6c86aa63428f1bc081ce0614b35db0607d34ba6b6065357a58a045c89": "RT3S V19.06 CSV",
    "9c58aca48de3d42b897852e424b0da1392f6e983d51ca69b1425b72a0a836ca1": "RT3S P19.06 CSV-GPS",
    "a2f38eaf4b98379fee6dfc00cae916bbc550e26ba0dd04c5fb56199a9e62b7f8": "MD-UV380 P19.33 CSV-GPS",
    "1e6a74b8cc62ffabd06dd83912f1bf0a7a7ef12d4bda63b961544c7e1393655a": "MD-UV380 V19.74 CSV",
    "d3996d672d6878ca823880320e240883ea64208f121923cd2bb6fe51d2e50784": "MD-UV380 D19.74 REC",
    "14e5634eeffa5f99f85fd762df4dc141432762d75b933031000d1f398fc4d2e4": "MD-UV380 P19.74 CSV-GPS",
    "3ebe806da0bbda993220cca9a97411225c1bf9e29097db3224ff722b00c6d023": "MD-UV380 S19.74 GPS-REC",
    "4acf6d48baca843d7e1220de8c0486670ee7f43953ebbb53d910ef1d94bfbe52": "MD-UV380 10 W V19.74 CSV",
    "2768ece93ddd4f1fd4ffc4e58ee073bd145b865f4ea6d256315f6e0019e93995": "MD-UV380 10 W D19.74 REC",
    "ebc5e34f4a00679a9200df8233c4b42b89dbd440a7487bbac2796223b796b392": "MD-UV380 10 W P19.74 CSV-GPS",
    "b0345e8c9e28b353954f3b6aa66599b5ac682c2037e212052af16e2c888572cd": "MD-UV380 10 W S19.74 GPS-REC",
    "447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee": "RT3S V219.60 CSV AES 255",
    "a7b277137c32ac3583083d8a650488415f62360b7f38a6d903fc732b858dd502": "RT3S P219.60 CSV-GPS AES 255",
    "a8a5265d29f8f69ee050a0a47052a8925a3c403ac270d8e5fda324a4a697b220": "MD-UV380 V219.74 CSV AES 255",
    "3d1b9ed4a2293981d0915d0655172e7a4d9bc702fe493328be37901427d51108": "MD-UV380 D219.74 REC AES 255",
    "70a44a4a77d6ffb4b0b6b9b4a8f26afb9607d65875d2ba1094baf6c8e6a46c9a": "MD-UV380 P219.74 CSV-GPS AES 255",
    "bf58116bcd695d8b14634fbffab7cd33ba9bcd1325b92179ef6bf94d380d2151": "MD-UV380 S219.74 GPS-REC AES 255",
    "531947c0d4d7d3ae0c5a3f94d81fbf3a04259488a9af09f9a3a8b57cabf135ed": "MD-UV380 10 W V219.74 CSV AES 255",
    "7b7ed3075a7735cde969402eda163f96bf2931ae55cbcc4fbf62625bdaf04855": "MD-UV380 10 W D219.74 REC AES 255",
    "bf98dc6ca9d888712924af75dd5eff7495de4ab82848f0701616e29eb9d5e5a2": "MD-UV380 10 W P219.74 CSV-GPS AES 255",
    "e6c696a3f1a6ac3d622559d7422efc97399b748aac8923a7440239b28c03a5c1": "MD-UV380 10 W S219.74 GPS-REC AES 255",
    "ff6c3d79b205391c9c2096f8e4367c514c1e1e4dac7d421ed69077e5b42eaf2f": "MD-380 D35.14",
    "167e79f18e78d7d31460dc577c4efd0cb881344f5ee44a4c9a30798ead0fb79d": "MD-390 S35.14",
    "c375de54f172346d3f9aecc73cbfa024e753b482e484765b1a890182cfac157e": "MD-380 D13.034",
    "db479b26c0c6aed81e9a3d2f7b2bf7476dfccc401c7bdd8976b321b31b967f42": "MD-380 D13.14",
    "b92052ea958b15687992cbf796f00a3dbcb33113426def550c32c7dd4e20cd95": "MD-380 D13.20",
    "c0053729cfe3155ae19089dd1accaa3a63e2382d6c8d8a50304c548c9dc2a311": "MD-380 D14.004",
    "e0a65ef4ba358abd66ce7041b571df5a1b3047b9f113d3cad6f8ab99b6e972be": "MD-380 D14.07",
    "18d0080b41cde2871a4801ea0de1876cb3320c2498f3ea04f89d647b7d722e74": "MD-380 D2.24 (upgrade pack)",
    "5d0e5671a2e2ee8d2163325b442448e5494b93bd1a3e48e978ddbee6bcfd5bc5": "MD-380 D2.25",
    "348f6a6bcddfe80327b5da29a4074fc036bace344891dcf8b7b2b76e7f20b60f": "MD-380 D2.26",
    "34ffbc629eb198c4c853c8081f441303a488f80eee5818826b3838b6abcf867c": "MD-380 D2.27",
    "9d9a9599f43ef82caefd9239cec3770cc6b411e47f760fa07c6f22167e26fdfb": "MD-380 D2.34",
    "466a5d6973926de2148f4ff32382e235ed047c2ef27d5ec5683ab9b91b89489d": "MD-380 D3.008",
    "38787da956234f9ec811cae08568f7bdb5f21b4070681ae689a0ba82c845f973": "MD-380 D3.20",
    "c8a98648d01c34ec8dca208eb0fff903ac0c9127a46ed482b7ca8e5755396d3d": "MD-390 S13.12",
    "5ce02f5f68fcb4d977d3b209d0d3911743ed300ebfb89d583a4a83c10208ed26": "MD-390 S13.20",
    "1fc264949d497af9af7dbef541d59d9690f1f9c0167601f5011e17e196584757": "MD-390 S14.07",
    "823439219e82f142ddb707003b6d6affa3993e6dd5bee99771a1f7f883de2046": "MD-390 S3.012",
    "6b83a74b72df4b13f74f5ac7ef2472593809055fc252c0aabe4c9eeeef1f115c": "MD-446 D13.20",
    "9892b2e95da0eee0ec87604007f157f6f9b249a9fb11ba0fc3798e78956e2587": "MD-446 D3.20",
    "0897e946bb123a5942f1458d3d2be8f759befe769c6aeaad4fe093ad3a40ac01": "MD-380 D3.32",
    "0c954b0fa8426352cf9c99544be4811115f4102fc4ae491269d1cdaef3edd54a": "RT3 S14.04",
}

# Files known to hold no codec at all, by SHA-256: named so the user is told why, never taken as a codec source or a
# stock file (no key, no restore). Not listed as supported anywhere. codec-dm1701.md "Files without the codec"
NO_CODEC_FILES = {
    "a09ab4d364d199853204ebdf068ba0886d7e7bb99689f08437aae52104474a7e": {
        "board": "dm1701", "name": "a DM-1701 open-firmware build",
        "why": "a plain application image without the codec (neither its code nor its tables)"},
}


class StockError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_container(data):
    """Return the segment list [(address, length), ...] of an OutSecurityBin file."""
    if len(data) < HEADER_LEN or not data.startswith(MAGIC):
        raise StockError("not a TYT/Baofeng OutSecurityBin firmware file (no \"OutSecurityBin\" header)")
    count = struct.unpack_from("<I", data, 0x7C)[0]
    if count == SINGLE_SEGMENT:
        count = 1	# the TYT MD-380 files (docs/spec/hardware-md380.md 14.1): one segment in the first table entry
    if not 1 <= count <= 15:
        raise StockError("implausible segment count %d" % count)
    segs = [struct.unpack_from("<II", data, 0x80 + 8 * i) for i in range(count)]
    total = sum(length for _, length in segs)
    if HEADER_LEN + total > len(data):	# the V02.06 files carry 0x100 more bytes after the last segment
        raise StockError("segment lengths (0x%X) run past the end of the file (0x%X)" % (total, len(data)))
    return segs


def app_file_offset(segs, addr):
    """File offset of flash address addr inside the application segment."""
    off = HEADER_LEN
    for base, length in segs:
        if base <= addr < base + length:
            return off + addr - base
        off += length
    raise StockError("address 0x%08X is in no segment" % addr)


def decrypt(data, off, length, key):
    """plain[i] = file[off + i] XOR key[(off + i - 0x100) mod 1024]"""
    if off < HEADER_LEN or off + length > len(data):
        raise StockError("window 0x%X+0x%X is outside the payload" % (off, length))
    return bytes(b ^ key[(off + i - HEADER_LEN) % KEY_LEN] for i, b in enumerate(data[off:off + length]))


def identify(stock):
    """A short description of a stock file ("DM-1701 V02.06 CSV"), or None when unknown."""
    return KNOWN_FILES.get(sha256(stock))


def no_codec(data):
    """Why a file known to hold no codec (NO_CODEC_FILES) can't be used, or None for any other file"""
    f = NO_CODEC_FILES.get(sha256(data))
    if f is None:
        return None
    return ("this file is %s: %s. The codec comes from the manufacturer's firmware file or a readout of a radio that "
            "holds it" % (f["name"], f["why"]))


def windows(prof):
    """Every codec window of a profile in the stock file, in a fixed order: [(addr, length, sha256)], code first, then
    tables, then extra. Where the merge puts them in the radio: layout()."""
    return [prof["code"], prof["tables"]] + list(prof.get("extra", ()))


def code_map(prof, code=None):
    """The codec map for a profile's code (a synthetic profile carries its own, "map"), or None"""
    if prof.get("map"):
        return prof["map"]
    found = codecmap.by_code_sha(codecmap.load_maps(), prof["code"][2])
    return found[0] if found else None


def placed_windows(prof):
    """Where the merge puts a profile's codec: [(addr, length, sha256)], code first, then the table windows (the extra
    ones at their stock address, unchanged). Without "placement", the stock windows."""
    if not prof.get("placement"):
        return windows(prof)
    pl, pd = prof["placement"], prof["placed"]
    tab = prof["tables"]
    return [(pl["code"], prof["code"][1], pd["code"][1]),
            (pl.get("tables", tab[0]), pl.get("tables_len", tab[1]), pd["tables"][1])] + list(prof.get("extra", ()))


def descriptor(prof):
    """The 128-byte descriptor for a placed codec (codec_desc.h): tag, code window, table windows, each with its CRC"""
    wins = placed_windows(prof)
    crcs = [prof["placed"]["code"][0], prof["placed"]["tables"][0]] + list(prof["placed"].get("extra", ()))
    if len(wins) - 1 > DESC_WINDOWS:
        raise StockError("%d table windows: the descriptor holds %d" % (len(wins) - 1, DESC_WINDOWS))
    words = [DESC_MAGIC, DESC_VERSION, prof["placement"]["tag"], wins[0][0], wins[0][1], crcs[0], len(wins) - 1]
    for i in range(DESC_WINDOWS):
        words += [wins[i + 1][0], wins[i + 1][1], crcs[i + 1]] if i + 1 < len(wins) else [0, 0, 0]
    body = struct.pack("<%dI" % len(words), *words)
    body += b"\0" * (DESC_LEN - 4 - len(body))
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def layout(prof):
    """Where the merge puts the codec in the radio: [(addr, length, sha256)], code first, then the tables, then the
    descriptor when the profile moves the code. Without "placement", the stock windows."""
    if not prof.get("placement"):
        return windows(prof)
    desc = descriptor(prof)
    return placed_windows(prof) + [(DESC_ADDR, DESC_LEN, sha256(desc))]


def place(prof, segments):
    """The decrypted stock windows [(stock address, bytes)] as the merge writes them (layout()). With "placement": the
    code moved (codec map: its self references rewritten), the table window cut to the codec's data with the islands'
    calls into the code re-encoded, the extra windows unchanged, then the descriptor. Every result is checked against
    "placed"."""
    if not prof.get("placement"):
        return list(segments)
    pl = prof["placement"]
    m = code_map(prof)
    if m is None:
        raise StockError("no codec map for %s: its code can't be moved" % prof["name"])
    (_, code), (tab_addr, tables) = segments[0], segments[1]
    try:
        if "tables" in pl:
            moved, tab = codecmap.move_window(code, tables, m, pl["code"], pl["tables"])
            tab_addr = pl["tables"]
        else:
            moved = codecmap.move_code(code, m, pl["code"])
            tab = codecmap.move_branches(tab_addr, tables[:pl.get("tables_len", len(tables))], m, pl["code"])
            if codecmap.branches_in(m, tab_addr, len(tab)) != len(m["branches"]):
                raise StockError("a branch into the code lies outside the tables the merge keeps")
    except codecmap.MapError as e:
        raise StockError("can't move the codec: %s" % e)
    extra = list(segments[2:])
    for (_, n, want), got, what in zip(placed_windows(prof), [moved, tab] + [d for _, d in extra],
                                       ["code", "tables"] + ["table 0x%08X" % a for a, _ in extra]):
        if sha256(got) != want:
            raise StockError("the moved %s doesn't match the profile (sha256 %s)" % (what, sha256(got)))
    return [(pl["code"], moved), (tab_addr, tab)] + extra + [(DESC_ADDR, descriptor(prof))]


def profile_for_board(board_id):
    """The SHA-256 of the first supported stock file for a board, or None."""
    return next((h for h, p in PROFILES.items() if p.get("board") == board_id), None)


def find_profile(stock, profiles=None):
    """The profile for a stock file (by SHA-256), or raise StockError naming the supported files."""
    profiles = PROFILES if profiles is None else profiles
    digest = sha256(stock)
    prof = profiles.get(digest)
    if prof is None:
        if no_codec(stock):
            raise StockError(no_codec(stock))
        known = KNOWN_FILES.get(digest)
        raise StockError("%s (sha256 %s...), not a supported codec source. Supported: %s"
                         % ("this file is " + known if known else "unknown stock file", digest[:16],
                            ", ".join("%s (%s...)" % (p["name"], h[:8]) for h, p in profiles.items())))
    return prof


def extract(stock, key, profiles=None, board=None):
    """Check the stock file and return (profile, segments): every codec window decrypted and checked, placed as the
    merge writes them, [(flash address, bytes)] in layout() order (segments[0] is the code). board: the radio being
    flashed; another radio's file is refused before it is decrypted with this radio's key, which could only fail as
    a window mismatch that blames the key."""
    prof = find_profile(stock, profiles)
    if board is not None and prof["board"] != board:
        raise StockError("%s is a firmware file for the %s, not the %s" % (prof["name"], prof["board"], board))
    if prof.get("file_radio"):
        key = file_key(stock, prof)
    if len(stock) != prof["size"]:
        raise StockError("size %d, expected %d" % (len(stock), prof["size"]))
    segs = parse_container(stock)
    if segs != prof["segments"]:
        raise StockError("segment table %s, expected %s" % (segs, prof["segments"]))
    out = []
    for i, (addr, length, want) in enumerate(windows(prof)):
        plain = decrypt(stock, app_file_offset(segs, addr), length, key)
        got = sha256(plain)
        if got != want:
            which = ("code", "tables")[i] if i < 2 else "table 0x%08X" % addr
            raise StockError("decrypted %s window sha256 %s, expected %s (wrong key?)" % (which, got, want))
        out.append((addr, plain))
    if struct.unpack_from("<I", out[0][1], 0)[0] != CODEC_SIG:
        raise StockError("codec first word is not 0x%08X" % CODEC_SIG)
    return prof, place(prof, out)


def codec_only(prof):
    """Why a codec source can't give the radio's bootloader key, or None when it can: a file for another radio is
    encrypted with that radio's key"""
    if not prof.get("file_radio"):
        return None
    return ("%s is %s firmware: it gives the codec only, not the bootloader key. The key comes from a stock file for "
            "the radio itself, a key file or the radio" % (prof["name"], prof["file_radio"]))


def file_key(stock, prof):
    """The key a codec source for another radio (file_radio) is encrypted with, derived from the file itself"""
    return derive_key(stock, prof["key_sha256"])


def is_container(data):
    """True for a stock firmware file (the OutSecurityBin container); a flash readout is plain bytes."""
    return data.startswith(MAGIC)


def from_flash(flash, base=FLASH_BASE, profiles=None):
    """The codec in a readout of a radio's internal flash (stub/f4stub.py --progmem: the bytes from base, normally the
    whole 1 MB from 0x08000000), as the merge writes it: (profile, segments in layout() order), or None when no
    supported codec is in it. A radio on its stock firmware holds the build's code and tables plain at their stock
    addresses: the build is found by the code window's SHA-256 there, every window is checked, and the codec is placed
    as a merge from the stock file places it. A radio running RadioFoundry holds it where the merge put it, with its
    descriptor, and is taken as it is. No key is needed: the flash holds the codec decrypted."""
    profiles = PROFILES if profiles is None else profiles

    def read(wins):
        parts = []
        for a, n, want in wins:
            off = a - base
            d = flash[off:off + n] if off >= 0 else b""
            if len(d) != n or sha256(d) != want:
                return None
            parts.append((a, bytes(d)))
        return parts if struct.unpack_from("<I", parts[0][1], 0)[0] == CODEC_SIG else None

    for prof in profiles.values():
        parts = read(windows(prof))
        if parts:
            return prof, place(prof, parts)
        if prof.get("placement"):
            desc = descriptor(prof)
            parts = read(placed_windows(prof) + [(DESC_ADDR, DESC_LEN, sha256(desc))])
            if parts:
                return prof, parts
    return None


def from_moved(flash, base=FLASH_BASE, profiles=None):
    """The codec in a readout where another firmware moved it (an open firmware runs it at an address of its own):
    (profile, segments in layout() order, code address), or None. Every word-aligned CODEC_SIG is a candidate start;
    for every profile with a codec map whose table window moves as one block (codecmap.table_window), the move is read
    from the copy and undone (codecmap.unmove_window: the code anywhere, the tables at their stock address or moved,
    the islands' calls into the code pointing anywhere), and the stock SHA-256s must match. The result is placed as a
    merge from the stock file places it. from_flash() first: this is for what it doesn't find"""
    profiles = PROFILES if profiles is None else profiles
    sig = struct.pack("<I", CODEC_SIG)
    cands = []
    for prof in profiles.values():
        m = code_map(prof) if prof.get("placement") else None
        if m is not None and prof["code"][1] == m["code"]["len"]:
            try:
                codecmap.table_window(m)
            except codecmap.MapError:
                continue
            cands.append((prof, m))
    at = flash.find(sig)
    while at >= 0:
        if at % 4 == 0:
            for prof, m in cands:
                got = codecmap.unmove_window(flash, base, m, base + at)
                if got is None:
                    continue
                code, tables, _ = got
                extra, ok = [], True
                for a, n, want in prof.get("extra", ()):
                    d = bytes(flash[a - base:a - base + n]) if a >= base else b""
                    ok = ok and len(d) == n and sha256(d) == want
                    extra.append((a, d))
                if not ok:
                    continue
                try:
                    return prof, place(prof, [(prof["code"][0], code), (prof["tables"][0], tables)] + extra), base + at
                except StockError:
                    continue
        at = flash.find(sig, at + 1)
    return None


def same_layout(prof, profiles=None):
    """Every profile whose codec is the same bytes at the same addresses as prof's, in profiles order: the same board,
    stock windows and placement. A readout of the radio's flash can't tell these files apart"""
    profiles = PROFILES if profiles is None else profiles
    key = lambda p: (p["board"], windows(p), p.get("placement"), p.get("placed"))
    return [p for p in profiles.values() if key(p) == key(prof)]


def layout_name(prof, profiles=None):
    """The versions a readout's codec could come from (same_layout): "V02.06 CSV" for a codec only one file holds,
    "V02.01/V02.02/V02.03 CSV" for one that several files hold, their common last words said once"""
    vs = [p["version"] for p in same_layout(prof, profiles)] or [prof["version"]]
    heads, tails = zip(*[(v.split(" ", 1) + [""])[:2] for v in vs])
    if len(vs) > 1 and len(set(tails)) == 1 and tails[0]:
        return "%s %s" % ("/".join(heads), tails[0])
    return " / ".join(vs)


def codec_segments(prof, segments):
    """The codec as flash segments [(address, bytes), ...] for rfoundry's planner."""
    return [(a, d) for a, d in segments]


def merge(image, prof, segments):
    """The built application image (plain, from 0x0800C000) with every codec segment (extract()) written at its
    address, as one contiguous image (0xFF between the parts)."""
    if image.startswith(MAGIC):
        raise StockError("the image is an official firmware file; pass the RadioFoundry build (rfoundry_%s.bin)"
                         % prof.get("board", "dm1701"))
    first = min(a for a, _ in segments)
    if APP_BASE + len(image) > first:
        raise StockError("the application image ends at 0x%08X, inside the codec window at 0x%08X: rebuild with "
                         "the window reserved (boards/%s/%s.ld)" % (APP_BASE + len(image), first,
                                                                    prof.get("board", "dm1701"),
                                                                    prof.get("board", "dm1701")))
    end = max(a + len(d) for a, d in segments)
    if end > FLASH_END:
        raise StockError("merged image would pass the end of flash")
    out = bytearray(b"\xFF" * (end - APP_BASE))
    out[0:len(image)] = image
    for a, d in segments:
        out[a - APP_BASE:a - APP_BASE + len(d)] = d
    return bytes(out)


def split_merged(image, profiles=None):
    """The reverse of merge(): (application image, profile, codec segments) when the image carries a codec where a
    profile's layout() puts it, None when it holds none (a plain image, which the planner checks as usual). A merged
    image has no manifest, so the codec windows are checked by their SHA-256 (which also names the radio) and every
    byte outside the application and the windows must be 0xFF, as merge() leaves it. Raises StockError for an image
    with the codec signature where a profile puts its code whose windows don't match that profile."""
    profiles = PROFILES if profiles is None else profiles
    candidates = []
    for prof in profiles.values():
        wins = layout(prof)
        first = min(a for a, _, _ in wins)
        if APP_BASE + len(image) <= first:
            continue
        code = wins[0][0] - APP_BASE
        if len(image) >= code + 4 and struct.unpack_from("<I", image, code)[0] == CODEC_SIG:
            candidates.append(prof)	# looks merged: a mismatch below is an error, not a plain image
        if APP_BASE + len(image) < max(a + n for a, n, _ in wins):
            continue
        segs = [(a, image[a - APP_BASE:a - APP_BASE + n]) for a, n, _ in wins]
        if any(sha256(d) != want for (_, d), (_, _, want) in zip(segs, wins)):
            continue
        rest = bytearray(image[first - APP_BASE:])
        for a, d in segs:
            rest[a - first:a - first + len(d)] = b"\xFF" * len(d)
        if rest.count(0xFF) != len(rest):
            raise StockError("the image holds the %s codec, but also other data between or after its windows: not "
                             "an image rfoundry merge (or the build's codec step) wrote" % prof["codec_profile"])
        return image[:first - APP_BASE].rstrip(b"\xFF"), prof, segs
    if candidates:
        raise StockError("the image has a codec at 0x%08X, but not the one from %s: a merged image must be made from "
                         "a supported stock file" % (layout(candidates[0])[0][0], candidates[0]["name"]))
    return None


def _alternatives(base, cands, order, want_sha256, max_alternatives):
    """Try the next candidates in the least certain columns (order) until the key matches its hash, or None."""
    for r in range(1, max_alternatives + 1):
        for js in itertools.combinations(order, r):
            for alts in itertools.product(*[cands[j][1:] for j in js]):
                k = list(base)
                for j, a in zip(js, alts):
                    k[j] = a
                k = bytes(k)
                if sha256(k) == want_sha256:
                    return k
    return None


def _refine(pairs, cands, key, seed_columns, strict, rounds=8):
    """Bigram refinement of a voted key, trained on the file itself.

    Firmware isn't random: the plain byte pairs that occur next to each other (Thumb-2 opcodes, little endian
    addresses, tables) have a strongly skewed distribution, with even and odd offsets distinct. pairs[j] counts the
    cipher byte pairs (column j-1, column j) over all rows, so with a key guess the plain pair counts follow without
    touching the payload again. Round 0 learns the pair model only from the seed columns (the clearest votes) and
    re-picks every other column among its vote candidates (strict: only columns next to a seed column, judged against
    their seed neighbors alone); later rounds learn from the whole image and re-pick all
    columns, until nothing changes. Returns (key, per-column score margin)."""
    import math
    key = list(key)
    seed = set(seed_columns)
    margin = [0.0] * KEY_LEN
    for rnd in range(rounds):
        counts = [[1] * 65536, [1] * 65536]
        for j in range(KEY_LEN):
            jl = (j - 1) % KEY_LEN
            if rnd == 0 and not (j in seed and jl in seed):
                continue
            m, kl, kj = counts[jl & 1], key[jl], key[j]
            for (a, c), n in pairs[j].items():
                m[(a ^ kl) << 8 | (c ^ kj)] += n
        logp = [[math.log(x) for x in m] for m in counts]
        changed = 0
        for j in range(KEY_LEN):
            if rnd == 0 and j in seed:
                continue
            jl, jr = (j - 1) % KEY_LEN, (j + 1) % KEY_LEN
            left, right = logp[jl & 1], logp[j & 1]
            kl, kr = key[jl], key[jr]
            # strict: round 0 trusts only seed neighbors (two wrong neighbors can hold each other in place)
            trust = rnd or not strict
            lp = [((a ^ kl) << 8, c, n) for (a, c), n in pairs[j].items()] if trust or jl in seed else []
            rp = [(c, b ^ kr, n) for (c, b), n in pairs[jr].items()] if trust or jr in seed else []
            if not lp and not rp:
                continue
            scored = sorted(((sum(n * left[a | (c ^ k)] for a, c, n in lp) +
                              sum(n * right[(c ^ k) << 8 | b] for c, b, n in rp), k) for k in cands[j]), reverse=True)
            margin[j] = scored[0][0] - scored[1][0] if len(scored) > 1 else 1e9
            if scored[0][1] != key[j]:
                key[j] = scored[0][1]
                changed += 1
        if rnd and not changed:
            break
    return bytes(key), margin


def derive_key(stock, want_sha256=DM1701_KEY_SHA256, max_alternatives=2, candidates=24):
    """Recover the 1024-byte bootloader key from a stock file, checked against its published SHA-256.

    The payload is plain XOR key[i mod 1024] (codec-dm1701.md "Container and encryption"). In a DM-1701 image the
    most common plain byte in every key column is 0x00, so the most common cipher byte of column j is key[j] for
    nearly every j, and the few columns where that guess is wrong are found by trying the next most common bytes in the
    least confident columns until the SHA-256 matches. The MD-380 image is far less zero-dominated (a third of the
    columns vote wrong), so when the vote fails the key is refined with a byte-pair model learned from the file itself
    (_refine) and then checked the same way. No key material is needed or stored: a wrong result can't pass the hash
    check. Raises StockError when it fails.
    """
    segs = parse_container(stock)
    total = sum(length for _, length in segs)
    payload = stock[HEADER_LEN:HEADER_LEN + total]
    if len(payload) < 64 * KEY_LEN:
        raise StockError("the file is too short to derive the key from")
    cols = [collections.Counter(payload[j::KEY_LEN]).most_common(8) for j in range(KEY_LEN)]
    base = bytes(c[0][0] for c in cols)
    if sha256(base) == want_sha256:
        return base
    cands = [[b for b, _ in c] for c in cols]
    margin = [c[0][1] - (c[1][1] if len(c) > 1 else 0) for c in cols]
    doubtful = sorted(range(KEY_LEN), key=lambda j: margin[j])[:candidates]
    k = _alternatives(base, [c[:4] for c in cands], doubtful, want_sha256, max_alternatives)
    if k:
        return k
    # cipher byte pairs (column j-1, column j) per column, counted once
    pairs = [collections.Counter() for _ in range(KEY_LEN)]
    for j in range(KEY_LEN):
        start = j if j else KEY_LEN
        pairs[j].update(zip(payload[start - 1::KEY_LEN], payload[start::KEY_LEN]))
    rel = sorted(range(KEY_LEN), key=lambda j: -(cols[j][0][1] - cols[j][1][1]) / cols[j][0][1] if len(cols[j]) > 1
                 else -1)
    for frac, strict in ((0.4, False), (0.4, True), (0.3, False), (0.3, True), (0.2, True)):
        k, score_margin = _refine(pairs, cands, base, rel[:int(KEY_LEN * frac)], strict)
        if sha256(k) == want_sha256:
            return k
        order = sorted(range(KEY_LEN), key=lambda j: score_margin[j])[:candidates // 2]
        alt = [[k[j]] + [b for b in cands[j] if b != k[j]][:2] for j in range(KEY_LEN)]
        k = _alternatives(k, alt, order, want_sha256, max_alternatives)
        if k:
            return k
    raise StockError("could not derive the bootloader key from this file (is it a stock firmware file for this radio?)")


def random_bytes(rng, n):
    return rng.getrandbits(8 * n).to_bytes(n, "little") if n else b""


SYN_SELF_REF = 0x40      # the synthetic codec's self reference (code offset) and island call (table window offset)
SYN_BL = 0x110


def build_synthetic_stock(key, rng, profile_sha=None):
    """A synthetic stock file with the layout of a real profile (default the DM-1701 V02.06 CSV), for tests:
    random key-independent content with the statistics of a firmware image (mostly zero bytes), random codec windows
    at the stock addresses. A codec-only profile (file_radio) gets key as its own key. Returns (stock bytes, profiles
    dict for it, code, tables)."""
    real = PROFILES[profile_sha or "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]
    segs = real["segments"]
    payload_len = sum(length for _, length in segs)
    # firmware-like plain text: about 60 % zero bytes, the rest random
    raw = random_bytes(rng, payload_len)
    plain = bytearray(raw.translate(bytes(b if b < 100 else 0 for b in range(256))))
    app_off = sum(length for base, length in segs if base != APP_BASE and base < APP_BASE)	# app segment offset
    wins = []
    run = real["code"][0]
    for i, (addr, length, _) in enumerate(windows(real)):
        data = bytearray(random_bytes(rng, length))
        if i == 0:
            struct.pack_into("<I", data, 0, CODEC_SIG)
            if real.get("placement"):
                struct.pack_into("<I", data, SYN_SELF_REF, run + 0x201)	# a Thumb pointer into the code
        elif i == 1 and real.get("placement"):
            codecmap.put_bl(data, SYN_BL, addr + SYN_BL, run + 0x200)	# an island's call into the code
        off = app_off + addr - APP_BASE
        plain[off:off + length] = data
        wins.append((addr, length, sha256(bytes(data)), bytes(data)))
    header = bytearray(HEADER_LEN)
    header[0:len(MAGIC)] = MAGIC
    struct.pack_into("<I", header, 0x7C, len(segs) if len(segs) > 1 else SINGLE_SEGMENT)
    for i, (a, n) in enumerate(segs):
        struct.pack_into("<II", header, 0x80 + 8 * i, a, n)
    stream = (key * (payload_len // KEY_LEN + 1))[:payload_len]
    enc = bytes(a ^ b for a, b in zip(plain, stream))
    stock = bytes(header) + enc + random_bytes(rng, 0x100)	# trailer, as the real files
    prof = dict(real, code=wins[0][:3], tables=wins[1][:3], size=len(stock))
    if real.get("file_radio"):
        prof["key_sha256"] = sha256(key)
    if "extra" in real:
        prof["extra"] = [w[:3] for w in wins[2:]]
    if real.get("placement"):	# its own codec map (a self reference, an island with a call into the code), placed
        code, tab = wins[0][3], wins[1][3]
        isl = wins[1][0] + SYN_BL - 0x10
        prof["map"] = codecmap.normalize({
            "format": codecmap.FORMAT, "profile": "synthetic-" + real["codec_profile"], "board": real["board"],
            "isa": "thumb2", "code": {"flash": run, "len": len(code), "run": run, "sha256": sha256(code)},
            "entries": {}, "self_refs": [{"off": SYN_SELF_REF, "value": run + 0x201}],
            "islands": [{"start": isl, "len": 0x40}],
            "branches": [{"at": wins[1][0] + SYN_BL, "target": run + 0x200}],
            "table_windows": [{"flash": wins[1][0], "run": wins[1][0], "len": len(tab), "min_start": wins[1][0],
                               "min_len": len(tab), "min_sha256": sha256(tab)}]})
        pl = real["placement"]
        at, n = pl["code"], pl.get("tables_len", len(tab))
        if "tables" in pl:
            mc, mt = codecmap.move_window(code, tab, prof["map"], at, pl["tables"])
        else:
            mc = codecmap.move_code(code, prof["map"], at)
            mt = codecmap.move_branches(wins[1][0], tab[:n], prof["map"], at)
        prof["placed"] = {"code": (zlib.crc32(mc) & 0xFFFFFFFF, sha256(mc)),
                          "tables": (zlib.crc32(mt) & 0xFFFFFFFF, sha256(mt))}
        if "extra" in real:
            prof["placed"]["extra"] = [zlib.crc32(w[3]) & 0xFFFFFFFF for w in wins[2:]]
    return stock, {sha256(stock): prof}, wins[0][3], wins[1][3]
