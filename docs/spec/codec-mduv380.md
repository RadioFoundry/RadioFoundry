<!-- SPDX-License-Identifier: MIT -->
# MD-UV380 / MD-UV390: hosting the AMBE+2 codec

Status: format description, against the reference build ([codec-placement.md](codec-placement.md)). The codec of
every build below was run in a host instruction set emulator (measured); nothing has run on a radio.

Rule: the radio runs exactly the codec its own TYT firmware shipped. The user's machine extracts it from a firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

## Stock files

Published by TYT and Retevis and mirrored by dealers. The 19.26 builds are the first supported and the ones described
in detail below; every other build has the same shape at its own addresses. Retevis ships TYT's 18.11 and 18.16 files
under its own names (`RT3S(CSV)-V18.11.bin` and so on), byte for byte, and builds of its own for 19.06 and 219.60. Every
file, TYT's and Retevis's, is encrypted with the same key (below).

| version | file | size | sha256 | radios |
|---|---|---|---|---|
| **V19.26 CSV** | `MD-UV380(CSV)-V19.26.bin` | 1303040 | `75e4d470b451cb5d191a647c0836c71801433b826cc4a7fe2c48d750036bd98e` | non-GPS, large contact list |
| D19.26 REC | `MD-UV380(REC)-D19.26.bin` | 1315328 | `7dec4b751e8a0f44cf7d54d0ebc3b439a9f49899eadd7c8ff14d2804edbe8ac2` | non-GPS, recording build |
| P19.26 CSV-GPS | `MD-UV390(CSV-GPS)-P19.26.bin` | 1312256 | `84331dbc9bccf5021122b715dcb77d7589dba0745694bf3a249aae6e7c383ef1` | GPS, large contact list |
| S19.26 GPS-REC | `MD-UV390(GPS-REC)-S19.26.bin` | 1324544 | `77923d67aa75a07f7601a1c53ae1d5f5af37bd4367d52a50c8aeff89b834eceb` | GPS, recording build |
| V18.11 CSV | `MD-UV380(CSV)-V18.11.bin` | 1289728 | `fb1897b7123dc6a039a5266972c5bb6f563f574d3769bd1cbdc7bb02b5039cc7` | non-GPS, large contact list, also Retevis's `RT3S(CSV)-V18.11.bin` |
| D18.11 REC | `MD-UV380(REC)-D18.11.bin` | 1300992 | `78650258f65fef33d78fad65678bedd74e44b75e924466829c9a360ce8915f39` | non-GPS, recording build, also Retevis's `RT3S(REC)-D18.11.bin` |
| P18.11 CSV-GPS | `MD-UV390(CSV-GPS)-P18.11.bin` | 1296896 | `e31ecd122f77ccd34e563193b004e8621ff5422676521503521f64f1045f73a9` | GPS, large contact list |
| S18.11 GPS-REC | `MD-UV390(GPS-REC)-S18.11.bin` | 1308160 | `fbb307fd14577e48e1fb6edb3395d4d684baf12dd9fed66280ad76d12e0e04c2` | GPS, recording build |
| P18.16 CSV-GPS | `MD-UV390(CSV-GPS)-P18.16.bin` | 1300992 | `6c2e43558290076e86671cb2d25bd18c90cbeb08e02a1b68e534c0ab4fe808bb` | GPS, large contact list, also Retevis's `RT3S(CSV-GPS)-P18.16.bin` |
| S18.16 GPS-REC | `MD-UV390(GPS-REC)-S18.16.bin` | 1313280 | `23c14a9c30b2b626021c39159a8bb68a2403f4eda28fd8101a0264d022fb06da` | GPS, recording build, also Retevis's `RT3S(GPS-REC)-S18.16.bin` |
| V19.06 CSV | `RT3S(CSV)-V19.06.bin` | 1295872 | `657cd4e6c86aa63428f1bc081ce0614b35db0607d34ba6b6065357a58a045c89` | non-GPS, large contact list, Retevis RT3S file |
| P19.06 CSV-GPS | `RT3S(CSV-GPS)-P19.06.bin` | 1305088 | `9c58aca48de3d42b897852e424b0da1392f6e983d51ca69b1425b72a0a836ca1` | GPS, large contact list, Retevis RT3S file |
| V19.74 CSV | `MD-UV380(CSV)-V19.74.bin` | 1309184 | `1e6a74b8cc62ffabd06dd83912f1bf0a7a7ef12d4bda63b961544c7e1393655a` | non-GPS, large contact list |
| D19.74 REC | `MD-UV380(REC)-D19.74.bin` | 1321472 | `d3996d672d6878ca823880320e240883ea64208f121923cd2bb6fe51d2e50784` | non-GPS, recording build |
| P19.74 CSV-GPS | `MD-UV390(CSV-GPS)-P19.74.bin` | 1319424 | `14e5634eeffa5f99f85fd762df4dc141432762d75b933031000d1f398fc4d2e4` | GPS, large contact list |
| S19.74 GPS-REC | `MD-UV390(GPS-REC)-S19.74.bin` | 1331712 | `3ebe806da0bbda993220cca9a97411225c1bf9e29097db3224ff722b00c6d023` | GPS, recording build |
| 10 W V19.74 CSV | `MD-UV380-10W(CSV)-V19.74.bin` | 1309184 | `4acf6d48baca843d7e1220de8c0486670ee7f43953ebbb53d910ef1d94bfbe52` | non-GPS, large contact list, 10 W |
| 10 W D19.74 REC | `MD-UV380-10W(REC)-D19.74.bin` | 1321472 | `2768ece93ddd4f1fd4ffc4e58ee073bd145b865f4ea6d256315f6e0019e93995` | non-GPS, recording build, 10 W |
| 10 W P19.74 CSV-GPS | `MD-UV390-10W(CSV-GPS)-P19.74.bin` | 1319424 | `ebc5e34f4a00679a9200df8233c4b42b89dbd440a7487bbac2796223b796b392` | GPS, large contact list, 10 W |
| 10 W S19.74 GPS-REC | `MD-UV390-10W(GPS-REC)-S19.74.bin` | 1331712 | `b0345e8c9e28b353954f3b6aa66599b5ac682c2037e212052af16e2c888572cd` | GPS, recording build, 10 W |
| V219.60 CSV AES 255 | `RT3S-Firmware-AES255(CSV)-V219.60.bin` | 1346048 | `447ed4e09482c45661cd6e4c83d1e9f777463993ab7dd48b0e0597bb14f519ee` | non-GPS, large contact list, AES, 255 groups, Retevis RT3S file |
| P219.60 CSV-GPS AES 255 | `RT3S-Firmware-AES255(CSV-GPS)-P219.60.bin` | 1356288 | `a7b277137c32ac3583083d8a650488415f62360b7f38a6d903fc732b858dd502` | GPS, large contact list, AES, 255 groups, Retevis RT3S file |
| V219.74 CSV AES 255 | `MD-UV380-AES255(CSV)-V219.74.bin` | 1348096 | `a8a5265d29f8f69ee050a0a47052a8925a3c403ac270d8e5fda324a4a697b220` | non-GPS, large contact list, AES, 255 groups |
| D219.74 REC AES 255 | `MD-UV380-AES255(REC)-D219.74.bin` | 1360384 | `3d1b9ed4a2293981d0915d0655172e7a4d9bc702fe493328be37901427d51108` | non-GPS, recording build, AES, 255 groups |
| P219.74 CSV-GPS AES 255 | `MD-UV390-AES255(CSV-GPS)-P219.74.bin` | 1358336 | `70a44a4a77d6ffb4b0b6b9b4a8f26afb9607d65875d2ba1094baf6c8e6a46c9a` | GPS, large contact list, AES, 255 groups |
| S219.74 GPS-REC AES 255 | `MD-UV390-AES255(GPS-REC)-S219.74.bin` | 1369600 | `bf58116bcd695d8b14634fbffab7cd33ba9bcd1325b92179ef6bf94d380d2151` | GPS, recording build, AES, 255 groups |
| 10 W V219.74 CSV AES 255 | `MD-UV380-10W-AES255(CSV)-V219.74.bin` | 1348096 | `531947c0d4d7d3ae0c5a3f94d81fbf3a04259488a9af09f9a3a8b57cabf135ed` | non-GPS, large contact list, 10 W, AES, 255 groups |
| 10 W D219.74 REC AES 255 | `MD-UV380-10W-AES255(REC)-D219.74.bin` | 1359360 | `7b7ed3075a7735cde969402eda163f96bf2931ae55cbcc4fbf62625bdaf04855` | non-GPS, recording build, 10 W, AES, 255 groups |
| 10 W P219.74 CSV-GPS AES 255 | `MD-UV390-10W-AES255(CSV-GPS)-P219.74.bin` | 1358336 | `bf98dc6ca9d888712924af75dd5eff7495de4ab82848f0701616e29eb9d5e5a2` | GPS, large contact list, 10 W, AES, 255 groups |
| 10 W S219.74 GPS-REC AES 255 | `MD-UV390-10W-AES255(GPS-REC)-S219.74.bin` | 1369600 | `e6c696a3f1a6ac3d622559d7422efc97399b748aac8923a7440239b28c03a5c1` | GPS, recording build, 10 W, AES, 255 groups |

TYT's 19.74 and 219.74 come in 5 W and 10 W ("Plus") builds; the 219.xx line is the AES build with 255 groups. The
header carries no model string, so a user must pick the file for their radio; for RadioFoundry any of them is only a
codec donor and works on any MD-UV380 family radio, because the codec behaves identically in every build (below).

## Container and encryption

The "OutSecurityBin" two-segment form, as for the reference build:

| offset | contents |
|---|---|
| 0x00 | `OutSecurityBin`, two zero bytes |
| 0x10 | model field, blank (0xFF); `MD-9600` in the 18.xx files |
| 0x30 | `02 17 0C 03 ...` in the non-GPS 19.xx files, `02 16 0C 03 ...` in the GPS ones, `01 06` / `01 07` in the 219.xx files (meaning unknown, ?) |
| 0x7C | u32 segment count, 2 |
| 0x80 | (0x00060000, 0x5D400): SPI flash resources; (0x0800C000, length): the application |
| 0x100 | payload |

Application lengths: V19.26 0xE0C00, D19.26 0xE3C00, P19.26 0xE3000, S19.26 0xE6000 (every build's:
`stockfw.py` PROFILES, "segments"). The application starts at file
offset 0x5D500. The payload is XORed with the MD-UV380 bootloader key (SHA-256
`739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991`, first bytes 00 AA 89 89, the `mduv380` model of
`tools/stm32_dfu.py`):

```
plain[i] = file[0x100 + i] ^ key[i % 1024]
```

For an application address A: file offset = A - 0x0800C000 + 0x5D500, key index = (file offset - 0x100) % 1024.

**Key recovery (measured).** `rfoundry/stockfw.derive_key(stock, <the hash above>)` recovers the key from each of
every file and checks it by that hash: in 0.1 to 5 s per file. No key file is needed.

Decrypted application vector tables of the 19.26 builds: SP 0x20002848 in all four; reset 0x080E8A09 (V19.26), 0x080EB999 (D19.26),
0x080EAE69 (P19.26), 0x080EDE39 (S19.26).

## Codec in the vendor images

Code windows (decrypted):

| build | flash | length | sha256 |
|---|---|---|---|
| V19.26 CSV | 0x0807B830..0x0808ACEA | 0xF4BA | `5de390664c831412f37ae9ec1f8aadaf77d93112bded424e5bfc9d6b77a64aa1` |
| D19.26 REC | 0x0807E3C0..0x0808D86A | 0xF4AA | `7e885b7f6f96bd433a310c7b1bea4ae9db55a71d74bcc102e8684bf0b1e88e96` |
| P19.26 CSV-GPS | 0x0807DBF0..0x0808D0AA | 0xF4BA | `bb8035df0a200c827b65128a3f24358fafd1c10601626bd54888595173bb6ae8` |
| S19.26 GPS-REC | 0x080807B4..0x0808FC6A | 0xF4B6 | `96c018bc0956b86a7ac4a90713eb53fb457cebb9f2584b927d785fefa2085613` |
| V18.11 CSV | 0x0807884C..0x08087F2A | 0xF6DE | `bc9af04eec52ab3b8f222e81d63e52c106f98d60f66d7010ea2e6c807894c46f` |
| D18.11 REC | 0x0807B2D8..0x0808A9AA | 0xF6D2 | `42ab9d8090c3f630ccd38658bd4e7ee4b22a981cb2fb77f1980aff9a068af5b9` |
| P18.11 CSV-GPS | 0x0807A388..0x08089A6A | 0xF6E2 | `2ed2ea20323649a8463c76e03a42772e490ead803c854f73541626fe7f2b03f0` |
| S18.11 GPS-REC | 0x0807CE28..0x0808C4EA | 0xF6C2 | `cc51722f43551ab7dd1dd7e8815ac27a8a0dff586cb3efda9b3ccd276f38abd9` |
| P18.16 CSV-GPS | 0x0807B4F4..0x0808ABEA | 0xF6F6 | `0862316ff1dfbc3ae12143796379edafa1b8c8c4131b4842de05dd29bcacbc77` |
| S18.16 GPS-REC | 0x0807DF58..0x0808D62A | 0xF6D2 | `5b3e6520620a97c45e648a9707c147417f29063902e54aff2a90787e074507ed` |
| V19.06 CSV | 0x08079E14..0x080894EA | 0xF6D6 | `21a64c6e0b9b99507765e409f3c26ed28cfdaca4499a7d452c241b4fb88efe5f` |
| P19.06 CSV-GPS | 0x0807C1CC..0x0808B8AA | 0xF6DE | `713e2ba7003f5229c78f0b04707ca4ec597ba8aca6953472a66247ec61905870` |
| V19.74 CSV | 0x0807CEC4..0x0808C36A | 0xF4A6 | `b2ef9546969f9c529ecc5ab06b31a9529a1af78939594e81e255c49f0dcf085d` |
| D19.74 REC | 0x0807FA70..0x0808EF2A | 0xF4BA | `0d9b2aad9150af6f3a583d123d69d0ee5c1074bf0282be4fc351a87344581256` |
| P19.74 CSV-GPS | 0x0807F450..0x0808E8EA | 0xF49A | `ea58eb79ee38e1b558bf09cbb6428cbc6d45fd3a3ef1cc62430e010b9c7ab85a` |
| S19.74 GPS-REC | 0x0808201C..0x080914AA | 0xF48E | `052bd89a644cc0556aa85fc9924f02cf44156e5903d23ebc49081f83999aaa88` |
| 10 W V19.74 CSV | 0x0807CE18..0x0808C2AA | 0xF492 | `d64a510fec7f77f0fe607fe798351ce1880edd7defbb66b858f0d6a694de2a4a` |
| 10 W D19.74 REC | 0x0807F9B0..0x0808EE6A | 0xF4BA | `20878b67953dbf4a39f9c72f8adb2d58516eebe96bcb2e539c029bb7deeb7ebf` |
| 10 W P19.74 CSV-GPS | 0x0807F3BC..0x0808E86A | 0xF4AE | `65aa7a28321218eaa127506ae36b87f66fa64a3b193053dc9474562e5ffa0255` |
| 10 W S19.74 GPS-REC | 0x08081FA8..0x0809146A | 0xF4C2 | `2068803aa297e67aaf39bb17a0f49ddfb55b7c35e66f328923e28f0594ffdc7f` |
| V219.60 CSV AES 255 | 0x08085BAC..0x08094F6A | 0xF3BE | `1b9c8dd355cdc6ab18b8aa4579cfe382ce49ec1f493d6048c8ccf5150cdee286` |
| P219.60 CSV-GPS AES 255 | 0x08088150..0x080974EA | 0xF39A | `0f374a92964736efd8cc2b4ea7a8cb83a190542b3ae1acd3696fa3d22f1bf375` |
| V219.74 CSV AES 255 | 0x080862F8..0x080956AA | 0xF3B2 | `c69b5bdc350aaccf87abf547e02f50ff6e28ac27036290ad297f9ce847fb65b3` |
| D219.74 REC AES 255 | 0x08088D84..0x0809812A | 0xF3A6 | `ddfb1872e8c3439b95b7b4ddbd476f4cd014b74c67cada6209803a001bbeb98b` |
| P219.74 CSV-GPS AES 255 | 0x08088878..0x08097C2A | 0xF3B2 | `f5b4998b80deb227262b0a7f5e3dc61c37fdbd64f24819bc8bda0802a00d35cb` |
| S219.74 GPS-REC AES 255 | 0x0808B358..0x0809A6EA | 0xF392 | `1db9a37d905da1f6b586ad29d2ba2fe9fe51b930e1abf338ade32f104062713f` |
| 10 W V219.74 CSV AES 255 | 0x08086244..0x080955EA | 0xF3A6 | `a71970c3d1699ad74fc9474798a4acb032fb7f8c9c7d1fb4dc337ffbce2fe146` |
| 10 W D219.74 REC AES 255 | 0x08088CC0..0x0809806A | 0xF3AA | `002d8a76707d5c65a5b2988b2c1356528677986ca5e0143bb65ae8b7ee060047` |
| 10 W P219.74 CSV-GPS AES 255 | 0x080887F0..0x08097BAA | 0xF3BA | `a55a811d85a5d23e3d599b312245b32ebdd70b2a75384772be12422025a4ee7e` |
| 10 W S219.74 GPS-REC AES 255 | 0x0808B2DC..0x0809A66A | 0xF38E | `fc272e75d71dcac08215794f5d153bac86a0787423f7b187c24b08caa3717eaa` |

Table windows (decrypted). Every file's is 0x12850 bytes. The codec's own data ends earlier; the merge cuts to it:

| build | flash | codec data ends | data length | sha256 of the window |
|---|---|---|---|---|
| V19.26 CSV | 0x080D8C94..0x080EB4E4 | 0x080EB2FE | 0x1266A | `6ea640d8da93215d6b3c9d7ce8db3663c32743a83c9478b1e9997e0905edc53c` |
| D19.26 REC | 0x080DBC1C..0x080EE46C | 0x080EE28E | 0x12672 | `96331ad1617b965e2b59a633767bfcc505c4840f3924e8fb82d5e522ff699599` |
| P19.26 CSV-GPS | 0x080DB054..0x080ED8A4 | 0x080ED75E | 0x1270A | `1f19c01eb5c0e9bcf10f2aeb7bfaee68580b19f4933448d3742d5aef2e6aadea` |
| S19.26 GPS-REC | 0x080DE01C..0x080F086C | 0x080F072E | 0x12712 | `08adbdac3d3c95e2b5e2e895e77e045aee15a7839933d328727310b517a854f1` |
| V18.11 CSV | 0x080D5B34..0x080E8384 | 0x080E7D22 | 0x121EE | `f4547d849bb694b8e86fb3a55c4320a6945e458f1b3a133b1111bd6dc34684e5` |
| D18.11 REC | 0x080D8998..0x080EB1E8 | 0x080EAB8E | 0x121F6 | `029aecd4f281be17f56362aa570935da57e5868a43deca1d6b3c33d8c0270b29` |
| P18.11 CSV-GPS | 0x080D7674..0x080E9EC4 | 0x080E9902 | 0x1228E | `c586fe9ab586eb2cf92d0f65064fc9d83bcc690b58846ad9824246cdd99b359a` |
| S18.11 GPS-REC | 0x080DA4D8..0x080ECD28 | 0x080EC76E | 0x12296 | `adf44ada74eed62ef90de73a702775b62dd8e97fce17b06ffb80e4a57a66b01a` |
| P18.16 CSV-GPS | 0x080D87F4..0x080EB044 | 0x080EAA82 | 0x1228E | `801d435cb5cb1a0a18b5468f7d2b58e3e1efd4fe11cc99811bf08b8fc9a314ff` |
| S18.16 GPS-REC | 0x080DB618..0x080EDE68 | 0x080ED8AE | 0x12296 | `be4549cded7e7acf2d522ef208a69e70fd372dbf1494592c618ddb4d519861d6` |
| V19.06 CSV | 0x080D70F4..0x080E9944 | 0x080E946A | 0x12376 | `3a77386ecd001053ea661bea94fd6fe6d024f9482a999bcd8f131da8cb818b92` |
| P19.06 CSV-GPS | 0x080D94B4..0x080EBD04 | 0x080EB8CA | 0x12416 | `c71415bda534d4872c595e8372554c2f9d83f64fbc385cc140e19797f321abfb` |
| V19.74 CSV | 0x080DA36C..0x080ECBBC | 0x080ECA9A | 0x1272E | `b05685290b253f9438d2b9ad39c95fae104019b3a59d36290e5e96191defb469` |
| D19.74 REC | 0x080DD330..0x080EFB80 | 0x080EFA76 | 0x12746 | `41cb1a328ce46105dd61716a26ab935fefd2e1193edcce076eb7ec998c0fe487` |
| P19.74 CSV-GPS | 0x080DC8EC..0x080EF13C | 0x080EF0BA | 0x127CE | `a93493fd52dde407f0182e0740bfa000604ad0fb061ba671239cd93f16a67fff` |
| S19.74 GPS-REC | 0x080DF8B0..0x080F2100 | 0x080F2086 | 0x127D6 | `a67ca31a57b8c69bda716197d0170eda0631b0e2bccc2df91d465a0df6e31cf1` |
| 10 W V19.74 CSV | 0x080DA2AC..0x080ECAFC | 0x080EC9DA | 0x1272E | `419e2ef2012298ba8aca092bc74d9ad6b0fd63514d7f1539458a7003ba6a16b7` |
| 10 W D19.74 REC | 0x080DD270..0x080EFAC0 | 0x080EF9B6 | 0x12746 | `2a7118e72744f890430d48b3d5f4a9ae6263c62eecfa8e9dbd89703b51fa75b6` |
| 10 W P19.74 CSV-GPS | 0x080DC86C..0x080EF0BC | 0x080EF03A | 0x127CE | `15a8f3154fde2f2ca035f563ea9fcc7e447f3b7ad0b2397b72ff3da7fd3f972e` |
| 10 W S19.74 GPS-REC | 0x080DF870..0x080F20C0 | 0x080F2046 | 0x127D6 | `62d99c50bc1863230892b74b690b43af73822e5951b7879dfcda788923d61f9e` |
| V219.60 CSV AES 255 | 0x080E322C..0x080F5A7C | 0x080F5956 | 0x1272A | `8a0201819744a522727826dc91332c156c990e7e56244250ba76df9523ad5985` |
| P219.60 CSV-GPS AES 255 | 0x080E57AC..0x080F7FFC | 0x080F7F76 | 0x127CA | `c86623e08780f10351696add74e202056d6732435faccdb226ba8b60d059923b` |
| V219.74 CSV AES 255 | 0x080E396C..0x080F61BC | 0x080F60F6 | 0x1278A | `6598089807521a5edc15cbe2d9812eed3b0d61b7103aeb4ae4a66645873ab36f` |
| D219.74 REC AES 255 | 0x080E6774..0x080F8FC4 | 0x080F8F16 | 0x127A2 | `65a1dbe29be0db5cc46b3784c571607aff241110a7470ce312e55dab96817534` |
| P219.74 CSV-GPS AES 255 | 0x080E5EEC..0x080F873C | 0x080F8716 | 0x1282A | `47e73474789188eaae78dd8ce8d9624e4a108ba53831c0b87aaad183fa23c6fc` |
| S219.74 GPS-REC AES 255 | 0x080E8D34..0x080FB584 | 0x080FB566 | 0x12832 | `b5ed5a9cbbb64214f8d02d6e21cfdaecba4697e6bf965efbf087b058ef951687` |
| 10 W V219.74 CSV AES 255 | 0x080E38AC..0x080F60FC | 0x080F6036 | 0x1278A | `e6efe93e0e0193477a08595c738bff7cd794cf90692c55b3cc0ae9af08754025` |
| 10 W D219.74 REC AES 255 | 0x080E66B4..0x080F8F04 | 0x080F8E56 | 0x127A2 | `8a8952a64c0821b99ed89d394486e37af3b36689833f77a21534c06d9ce90d78` |
| 10 W P219.74 CSV-GPS AES 255 | 0x080E5E6C..0x080F86BC | 0x080F8696 | 0x1282A | `76a96e79d10f71834d94a914ce359c6492cab2a037b23a472a5ed13b446d36b4` |
| 10 W S219.74 GPS-REC AES 255 | 0x080E8CB4..0x080FB504 | 0x080FB4E6 | 0x12832 | `934bbea39629c875be4c1aeab52c2e54605f7a74538435ff84b60eb400405c58` |

First word of every code window: 0x4FF0E92D, as in the reference build. The code-window SHA-256 alone tells the
builds apart: no two builds share one.

* **Code window (V19.26).** Execution covers 0x0807B830..0x0807C872 and 0x0807DD4C..0x0808ACEA; the gap between holds
  other code that the codec never executes, carried to keep the window contiguous. The window end is the last executed
  byte.
* **Table window (V19.26).** The constant tables at the reference table window's layout shifted by +0x72A4 (start) to
  +0x7340 (most tables), plus one codec code island at 0x080DFF3C..0x080E0024 (the reference build's is at
  0x080D8BFC). Starting the window at 0x080D8C98 changes the output. The other three builds have the same shape at the
  same offsets from their window start.

## Entry points and calling convention

All AAPCS, Thumb, the reference build's argument lists and semantics ("Per-frame calls" in its codec page). The offsets
from the code start are **the reference build's** (another hosted family has different ones):

| call | offset | V19.26 CSV | D19.26 | P19.26 | S19.26 |
|---|---|---|---|---|---|
| encode | +0x130 | 0x0807B960 | 0x0807E4F0 | 0x0807DD20 | 0x080808E4 |
| encoder set | +0x1C0 | 0x0807B9F0 | 0x0807E580 | 0x0807DDB0 | 0x08080974 |
| encoder init | +0x284 | 0x0807BAB4 | 0x0807E644 | 0x0807DE74 | 0x08080A38 |
| FEC init | +0x4CC | 0x0807BCFC | 0x0807E88C | 0x0807E0BC | 0x08080C80 |
| encode ECC | +0x4E8 | 0x0807BD18 | 0x0807E8A8 | 0x0807E0D8 | 0x08080C9C |
| decode | +0x5D8 | 0x0807BE08 | 0x0807E998 | 0x0807E1C8 | 0x08080D8C |
| decoder set | +0x688 | 0x0807BEB8 | 0x0807EA48 | 0x0807E278 | 0x08080E3C |
| decoder init | +0x6D0 | 0x0807BF00 | 0x0807EA90 | 0x0807E2C0 | 0x08080E84 |
| encoder parameters | +0x848 | 0x0807C078 | 0x0807EC08 | 0x0807E438 | 0x08080FFC |
| FEC decode | +0xB54 | 0x0807C384 | 0x0807EF14 | 0x0807E744 | 0x08081308 |

Every other build has the same offsets, except FEC decode: +0xB78 in the CSV and CSV-GPS builds of 18.11, 18.16 and
19.06, +0xB68 in their REC and GPS-REC builds. RadioFoundry never calls FEC decode (the air frame FEC is its own), so
one offset table serves every build.

All four columns have been executed (and the other builds' entries, at the same offsets). The init sequence is the hosted builds': encoder init (E, 1, 1),
FEC init (each FEC block, 1), decoder init (D, 1), encoder parameters (E, -22, 20, -10, stack: -40, 0x4000, 3276, 3276,
1310, 10, 20), encoder set (E, 128, -20), decoder set (D, 128, -20), one decode of an all-zero frame with flags 8. The
sequence is the reference build's, and the output matches it.

### State sizes

With every block ending at an unmapped page: encoder state 0x16E0, decoder state 0x7EC, FEC state 0x44 each run without
a fault (the sizes of the hosted builds; smaller sizes were not tried). Stack peak 3822 bytes from the entry.
No fixed RAM address is touched.

## Checks (measured)

In a host emulator (Thumb, Cortex-M4) with only the two windows (or the codec as the merge places it), the state blocks, the I/O buffers and a stack mapped. For a 1 kHz tone
(30 frames) and a 300 frame stress run (noise, silence, full scale sweeps, injected bit errors, random frames, flags 8),
the codec's air bits and decoded PCM are **bit-identical** to the reference codec's and to the DM-1701 V02.06 codec's,
for every build listed: run from the whole image, from the windows only and from the placed layout (same 300 frame
hash). The FEC decode results agree (0 and 4).

## Hosting

The merge places the codec like the DM-1701's ([codec-placement.md](codec-placement.md)): the tables at their stock
address, cut to the codec's own data, the code right after them (word aligned) and the descriptor at 0x080FFF80, the
end of sector 11. Where the code doesn't fit between the codec's data and the descriptor (the 219.xx builds, S19.74 and
10 W S19.74, whose tables sit higher), it goes to the start of sector 10 instead, below the tables. The code reaches its tables through absolute addresses, which don't move; the merge rewrites one
pointer into the code and the 12 calls from the code kept in the table window back into the code (the codec map,
`tools/rfoundry/rfoundry/codec_maps/mduv380/`).

* The codec lives in sectors 10 and 11 (0x080C0000..0x080FFFFF). The application has sectors 3-9
  (0x0800C000..0x080BFFFF), and an update that writes only those keeps the codec ("keep codec" works).
* One codec id for every build, `mduv380-1926-s11` (named after the first build supported); the descriptor's tag tells
  the builds apart: the build letter and version in BCD, 0x40000000 for a 10 W build, 0x80000000 for the AES 255
  line. Each build has its own placed addresses, in `boards/mduv380/vocoder_mduv380.c`:

| build | tag | tables (address, length, CRC-32) | code (address, length, CRC-32) | free |
|---|---|---|---|---|
| V19.26 CSV | 0x00561926 | 0x080D8C94, 0x1266A, 0xCE661968 | 0x080EB300, 0xF4BA, 0x10AC40E0 | 22470 B below the descriptor |
| D19.26 REC | 0x00441926 | 0x080DBC1C, 0x12672, 0x7E080ADE | 0x080EE290, 0xF4AA, 0x3367A6D0 | 10310 B below the descriptor |
| P19.26 CSV-GPS | 0x00501926 | 0x080DB054, 0x1270A, 0x06DB6E3C | 0x080ED760, 0xF4BA, 0x0AEE6C4A | 13158 B below the descriptor |
| S19.26 GPS-REC | 0x00531926 | 0x080DE01C, 0x12712, 0xDED111EF | 0x080F0730, 0xF4B6, 0x11E43041 | 922 B below the descriptor |
| V18.11 CSV | 0x00561811 | 0x080D5B34, 0x121EE, 0x28A30A27 | 0x080E7D24, 0xF6DE, 0x0B1FFBD7 | 35710 B below the descriptor |
| D18.11 REC | 0x00441811 | 0x080D8998, 0x121F6, 0x6249AF48 | 0x080EAB90, 0xF6D2, 0x8DB9F023 | 23838 B below the descriptor |
| P18.11 CSV-GPS | 0x00501811 | 0x080D7674, 0x1228E, 0x1AC8D7F3 | 0x080E9904, 0xF6E2, 0x229BC3B4 | 28570 B below the descriptor |
| S18.11 GPS-REC | 0x00531811 | 0x080DA4D8, 0x12296, 0x87CA8BC7 | 0x080EC770, 0xF6C2, 0x21EBA180 | 16718 B below the descriptor |
| P18.16 CSV-GPS | 0x00501816 | 0x080D87F4, 0x1228E, 0xD772CFAD | 0x080EAA84, 0xF6F6, 0x9C51AF50 | 24070 B below the descriptor |
| S18.16 GPS-REC | 0x00531816 | 0x080DB618, 0x12296, 0x3D1F111C | 0x080ED8B0, 0xF6D2, 0x1986B8FF | 12286 B below the descriptor |
| V19.06 CSV | 0x00561906 | 0x080D70F4, 0x12376, 0x22393F1B | 0x080E946C, 0xF6D6, 0x25117C5E | 29758 B below the descriptor |
| P19.06 CSV-GPS | 0x00501906 | 0x080D94B4, 0x12416, 0x02CB0AB8 | 0x080EB8CC, 0xF6DE, 0xB18F0F57 | 20438 B below the descriptor |
| V19.74 CSV | 0x00561974 | 0x080DA36C, 0x1272E, 0x605799E9 | 0x080ECA9C, 0xF4A6, 0x54149419 | 16446 B below the descriptor |
| D19.74 REC | 0x00441974 | 0x080DD330, 0x12746, 0xAC72D37E | 0x080EFA78, 0xF4BA, 0x1924F763 | 4174 B below the descriptor |
| P19.74 CSV-GPS | 0x00501974 | 0x080DC8EC, 0x127CE, 0x6E6FD4E0 | 0x080EF0BC, 0xF49A, 0x97C6E76B | 6698 B below the descriptor |
| S19.74 GPS-REC | 0x00531974 | 0x080DF8B0, 0x127D6, 0x8414A4A9 | 0x080C0000, 0xF48E, 0x808DD9EE | 66594 B below the tables |
| 10 W V19.74 CSV | 0x40561974 | 0x080DA2AC, 0x1272E, 0xAC3B0A5A | 0x080EC9DC, 0xF492, 0xE86E3C65 | 16658 B below the descriptor |
| 10 W D19.74 REC | 0x40441974 | 0x080DD270, 0x12746, 0x3F3E347B | 0x080EF9B8, 0xF4BA, 0xAA99FD17 | 4366 B below the descriptor |
| 10 W P19.74 CSV-GPS | 0x40501974 | 0x080DC86C, 0x127CE, 0x3CA2A02D | 0x080EF03C, 0xF4AE, 0x1B9C0A2B | 6806 B below the descriptor |
| 10 W S19.74 GPS-REC | 0x40531974 | 0x080DF870, 0x127D6, 0xC39C3679 | 0x080C0000, 0xF4C2, 0x92924D87 | 66478 B below the tables |
| V219.60 CSV AES 255 | 0x80561960 | 0x080E322C, 0x1272A, 0xE880FF1B | 0x080C0000, 0xF3BE, 0x620F4AC5 | 81518 B below the tables |
| P219.60 CSV-GPS AES 255 | 0x80501960 | 0x080E57AC, 0x127CA, 0xC76CFEB9 | 0x080C0000, 0xF39A, 0x307BE6B5 | 91154 B below the tables |
| V219.74 CSV AES 255 | 0x80561974 | 0x080E396C, 0x1278A, 0xEF3C7105 | 0x080C0000, 0xF3B2, 0x254A2827 | 83386 B below the tables |
| D219.74 REC AES 255 | 0x80441974 | 0x080E6774, 0x127A2, 0xE588E20F | 0x080C0000, 0xF3A6, 0x3E5C5BC2 | 95182 B below the tables |
| P219.74 CSV-GPS AES 255 | 0x80501974 | 0x080E5EEC, 0x1282A, 0x9418379D | 0x080C0000, 0xF3B2, 0xBA895AC5 | 92986 B below the tables |
| S219.74 GPS-REC AES 255 | 0x80531974 | 0x080E8D34, 0x12832, 0x0B8910A3 | 0x080C0000, 0xF392, 0x87BCEDF2 | 104866 B below the tables |
| 10 W V219.74 CSV AES 255 | 0xC0561974 | 0x080E38AC, 0x1278A, 0x5C092FDD | 0x080C0000, 0xF3A6, 0x6A67A328 | 83206 B below the tables |
| 10 W D219.74 REC AES 255 | 0xC0441974 | 0x080E66B4, 0x127A2, 0x60ACDF51 | 0x080C0000, 0xF3AA, 0xD68C85D5 | 94986 B below the tables |
| 10 W P219.74 CSV-GPS AES 255 | 0xC0501974 | 0x080E5E6C, 0x1282A, 0xE429F823 | 0x080C0000, 0xF3BA, 0x84C3921C | 92850 B below the tables |
| 10 W S219.74 GPS-REC AES 255 | 0xC0531974 | 0x080E8CB4, 0x12832, 0x64B8EF5C | 0x080C0000, 0xF38E, 0x4703BB9F | 104742 B below the tables |

* The CRCs are CRC-32 (IEEE 802.3) of each part after the rewrites. The descriptor must carry exactly these.
* Every build holds the same codec, bit-identical in behavior to the DM-1701 V02.06's (above).
* Boot check: the descriptor, then the CRC of both parts; otherwise FM only.
* Older releases: the id is the same for every build, but a release accepts only the tags it was built with.
  Releases before the one that added the other builds' maps accept the four 19.26 tags only. A radio whose codec came
  from any other build and is flashed with such a release, keeping the codec, finds no codec and is FM only. Going
  back to it needs the codec merged again from a 19.26 file ([codec-placement.md](codec-placement.md), "One id, many
  builds").
* Identifying a radio still on TYT firmware: a program flash readout (rfoundry, `codec_extract`, the web flasher
  library) hashes the code window with SHA-256 and so knows the build. The application is plain in flash from
  0x0800C000, so a readout equals the decrypted application by address.
* Maps exist for every build in the tables above. 18.03 is not supported: no 18.03 file could be found again to
  check its SHA-256 against.

## Extraction recipe

1. Check the file's SHA-256 against the table above and the two-segment header.
2. Get the key: derive it from the file (above) or a key file checked by its SHA-256.
3. Decrypt the code and table windows (file offset = address - 0x0800C000 + 0x5D500). Check the first word and both
   SHA-256 values.
4. Place them as the table in Hosting says. The merge happens on the user's machine, never in a release.

## Open items

* 19.03 and 19.06 from TYT, the 119.xx line, and REC / GPS-REC builds of Retevis 19.06 and 219.60: named in release
  notes or expected, no file found.
* On a radio: TX and RX voice through the codec as placed.
