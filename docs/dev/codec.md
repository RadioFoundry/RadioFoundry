<!-- SPDX-License-Identifier: MIT -->

# The codec

DMR voice needs DVSI's AMBE+2 codec. It is proprietary, so ODR never contains it: not in the repository, not in a
release, not in a test vector file. Each radio runs the codec its own stock firmware carries. The user downloads that
firmware, and a tool merges the codec into their ODR image on their own machine.

## Per radio

| Radio | Tool | Stock file | Hosting | Spec |
|---|---|---|---|---|
| DM-1701 | `tools/odrflash` (`odrflash/stockfw.py`; also `tools/codec_extract.py`) | DM-1701(CSV) V02.06, encrypted container | Decrypted; the tables stay at their stock address, the code moves to 0x080E3A74 in sector 11, a descriptor at 0x080FFF80 says where ([codec-placement.md](../spec/codec-placement.md)); runs in place. Updates keep it (sectors 10, 11) | [codec-dm1701.md](../spec/codec-dm1701.md) |
| DM-32UV | `tools/hrc7000_codec.py` | DM32.01.01.049 or DM32.00.01.046HRVocoder | Placed in a codec slot at 0x030E8000 with a header; copied to its stock RAM addresses at boot | [codec-dm32.md](../spec/codec-dm32.md) |
| DB25-D PRO | `tools/hrc7000_codec.py --board db25d` | 909E.Dx.EARSAB.018 of 2025-01-21, 2024-09-23 or 2024-05-09, plain images | Placed in the same slot (0x030E8000) behind the first stage and application; copied to its stock RAM addresses at boot | [hardware-db25d.md](../spec/hardware-db25d.md) 8 |
| MD-380 | `tools/codec_extract.py` (and odrflash) | TYT D002.032 ("Firmware 2.32"), encrypted container (single segment) | Decrypted; the table windows stay at their stock addresses, the code moves to 0x080E0000 in sector 11, with the descriptor ([codec-placement.md](../spec/codec-placement.md)); runs in place. Updates keep it (sectors 8, 10, 11) | [codec-md380.md](../spec/codec-md380.md) |
| GD-77 | `tools/gd77_codec.py` (and `tools/odrflash/odrflash/gd77codec.py`) | the seven GD-77 firmware builds V3.2.1 to V6.0.1 as a flat flash image from 0x4000 | The code moves to 0x55D00 behind the application, the table window stays at the build's address, a descriptor at 0x7B780 says where ([codec-gd77.md](../spec/codec-gd77.md)); runs in place. Every flash writes it again | [codec-gd77.md](../spec/codec-gd77.md) |

Each tool checks the stock file by SHA-256 against a profile and refuses anything else, checks the codec's first words
after carving, and writes the merged image into `build/` (ignored by git) or a path you choose. For release images,
odrflash merges in memory and writes straight to the radio, or keeps the codec already in the radio when the
firmware reports it ([release.md](release.md), "Codec handling").

For source builds, drop the stock file into `codecs/` and `make` runs the board's tool after every build
(`tools/codec_auto.py`, [codecs/README.md](../../codecs/README.md)).

## In the firmware

| Part | Where |
|---|---|
| The interface the core calls | `hal/hal_vocoder.h` (`board.vocoder`) |
| DM-1701 hosting: state init, per-frame calls | `boards/dm1701/vocoder_dm1701.c` |
| MD-380 hosting: the same, other addresses and window checks | `boards/md380/vocoder_md380.c` |
| HR_C7000 radios (DM-32UV, DB25-D PRO) hosting: the slot header, the copy to RAM | `platform/hrc7000/vocoder.c`; the slot and windows in the board's linker script, the profile prefix in its `board.mk` |
| The window kept free in flash | the board's linker script |
| AMBE frame FEC and the voice path around the codec | `core/dmr/ambe` |

Without a codec merged, the vocoder reports itself absent and the radio runs FM only.

## Adding a radio's codec

The spec side finds the codec in the radio's stock firmware and writes `docs/spec/codec-<b>.md`: the stock files and
their hashes, the container format, the code and table windows, entry points, calling convention, state sizes and
whether the code is position dependent. Use the existing specs as the model. The clean side then writes the
merge tool and the hosting from that spec.

A merge tool must:

- recognize each supported stock file by SHA-256 and refuse others;
- never write the codec or the stock file into the repository;
- write the merged image outside tracked files (default `build/`);
- leave the flashing to the radio's flash tool.

Host tests of the voice path (`tests/host/ambeVoiceTest.c`) use vectors recorded from the stock codec's FEC, not the
codec itself.
