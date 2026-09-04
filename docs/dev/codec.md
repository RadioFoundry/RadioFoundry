<!-- SPDX-License-Identifier: MIT -->

# The codec

DMR voice needs DVSI's AMBE+2 codec. It is proprietary, so ODR never contains it: not in the repository, not in a
release, not in a test vector file. Each radio runs the codec its own stock firmware carries. The user downloads that
firmware, and a tool merges the codec into their ODR image on their own machine.

## Per radio

| Radio | Tool | Stock file | Hosting | Spec |
|---|---|---|---|---|
| DM-1701 | `tools/odrflash` (`odrflash/stockfw.py`; also `tools/codec_extract.py`) | DM-1701(CSV) V02.06, encrypted container | Decrypted and placed at its stock flash addresses; runs in place, nothing relocated | [codec-dm1701.md](../spec/codec-dm1701.md) |
| DM-32UV | `tools/dm32_codec.py` | DM32.01.01.049 or DM32.00.01.046HRVocoder | Placed in a codec slot at 0x030E8000 with a header; copied to its stock RAM addresses at boot | [codec-dm32.md](../spec/codec-dm32.md) |

Both tools check the stock file by SHA-256 against a profile and refuse anything else, check the codec's first words
after carving, and write the merged image into `build/` (ignored by git) or a path you choose. For release images,
odrflash merges in memory and writes straight to the radio, or keeps the codec already in the radio when the
firmware reports it ([release.md](release.md), "Codec handling").

For source builds, drop the stock file into `codecs/` and `make` runs the board's tool after every build
(`tools/codec_auto.py`, [codecs/README.md](../../codecs/README.md)).

## In the firmware

| Part | Where |
|---|---|
| The interface the core calls | `hal/hal_vocoder.h` (`board.vocoder`) |
| DM-1701 hosting: state init, per-frame calls | `boards/dm1701/vocoder_dm1701.c` |
| DM-32UV hosting: the slot header, the copy to RAM | `boards/dm32/vocoder_dm32.c`, `platform/hrc7000/codec_call.c` |
| The window kept free in flash | the board's linker script |
| AMBE frame FEC and the voice path around the codec | `core/dmr/ambe` |

Without a codec merged, the vocoder reports itself absent and the radio runs FM only.

## Adding a radio's codec

The spec side finds the codec in the radio's stock firmware and writes `docs/spec/codec-<b>.md`: the stock files and
their hashes, the container format, the code and table windows, entry points, calling convention, state sizes and
whether the code is position dependent. Use the two existing specs as the model. The clean side then writes the
merge tool and the hosting from that spec.

A merge tool must:

- recognize each supported stock file by SHA-256 and refuse others;
- never write the codec or the stock file into the repository;
- write the merged image outside tracked files (default `build/`);
- leave the flashing to the radio's flash tool.

Host tests of the voice path (`tests/host/ambeVoiceTest.c`) use vectors recorded from the stock codec's FEC, not the
codec itself.
