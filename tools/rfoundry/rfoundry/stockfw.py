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
# for; "key_sha256" the identity of that radio's bootloader key. "placement" (docs/spec/codec-placement.md): where the
# merge puts the code ("code"), how much of the table window it keeps ("tables_len": the codec's own data, from the
# codec map; the tables stay at their stock address) and the descriptor's tag; "placed": the CRC-32 and SHA-256 of the
# code and tables as placed (the code's self references and the islands' calls into it rewritten for the new address),
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
}

# Stock files that are not codec donors but are known DM-1701 files: the key can be derived from them (any of them;
# codec-dm1701.md "Stock files"), and they tell the user which version they hold
KNOWN_FILES = {
    "0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d": "DM-1701 V02.06 CSV",
    "4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9": "DM-1701 V02.06 noCSV",
    "35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948": "DM-1701 V02.03 CSV",
    "0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a": "DM-1701 V02.03 without CSV",
    "643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7": "DM-1701 V02.02 CSV",
    "cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713": "DM-1701 V02.01 CSV",
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
    "657cd4e6c86aa63428f1bc081ce0614b35db0607d34ba6b6065357a58a045c89": "RT3S V19.06 CSV",
    "9c58aca48de3d42b897852e424b0da1392f6e983d51ca69b1425b72a0a836ca1": "RT3S P19.06 CSV-GPS",
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
            (tab[0], pl.get("tables_len", tab[1]), pd["tables"][1])] + list(prof.get("extra", ()))


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
        moved = codecmap.move_code(code, m, pl["code"])
        tab = codecmap.move_branches(tab_addr, tables[:pl.get("tables_len", len(tables))], m, pl["code"])
    except codecmap.MapError as e:
        raise StockError("can't move the codec: %s" % e)
    if codecmap.branches_in(m, tab_addr, len(tab)) != len(m["branches"]):
        raise StockError("a branch into the code lies outside the tables the merge keeps")
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
    at the stock addresses. Returns (stock bytes, profiles dict for it, code, tables)."""
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
            "branches": [{"at": wins[1][0] + SYN_BL, "target": run + 0x200}]})
        at, n = real["placement"]["code"], real["placement"].get("tables_len", len(tab))
        mc = codecmap.move_code(code, prof["map"], at)
        mt = codecmap.move_branches(wins[1][0], tab[:n], prof["map"], at)
        prof["placed"] = {"code": (zlib.crc32(mc) & 0xFFFFFFFF, sha256(mc)),
                          "tables": (zlib.crc32(mt) & 0xFFFFFFFF, sha256(mt))}
        if "extra" in real:
            prof["placed"]["extra"] = [zlib.crc32(w[3]) & 0xFFFFFFFF for w in wins[2:]]
    return stock, {sha256(stock): prof}, wins[0][3], wins[1][3]
