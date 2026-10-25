<!-- SPDX-License-Identifier: MIT -->

# The Codec

DMR voice needs DVSI's AMBE+2 codec. It is proprietary, so RadioFoundry never contains it: not in the repository, not in a
release, not in a test vector file. Each radio runs the codec its own vendor firmware carries. The user supplies that
firmware, and a tool merges the codec into their RadioFoundry image on their own machine.

## Per radio

Each radio's `codec-<id>.md` ([the spec index](../spec/README.md#codecs)) lists the stock files it accepts, their
format and where the codec lands. Two hosting schemes exist ([codec-placement.md](../spec/codec-placement.md)):

| Family | Tool | Hosting |
|---|---|---|
| STM32F4 radios | `tools/rfoundry` (`rfoundry/stockfw.py`; also `tools/codec_extract.py`) | Decrypted; the tables stay at their stock address, the code moves into the last flash sectors, and a descriptor says where. It runs in place, and updates keep it |
| HR_C7000 radios | `tools/hrc7000_codec.py` | Placed in a codec slot at the end of the program flash with a header, behind the first stage and application on a board that has one; copied to its stock RAM addresses at boot |

Each tool checks the stock file by SHA-256 against a profile and refuses anything else, checks the codec's first words
after carving, and writes the merged image into `build/` (ignored by git) or a path you choose. For release images,
rfoundry merges in memory and writes straight to the radio, or keeps the codec already in the radio when the
firmware reports it ([release.md](release.md), "Codec handling").

For source builds, drop the stock file into `codecs/` and `make` runs the board's tool after every build
(`tools/codec_auto.py`, [codecs/README.md](../../codecs/README.md)).

## In the firmware

| Part | Where |
|---|---|
| The interface the core calls | `hal/hal_vocoder.h` (`board.vocoder`) |
| STM32 radios' hosting: state init, per-frame calls, the descriptor checks | `platform/stm32f4/hrc/vocoder.c` |
| Which stock builds a radio accepts: entry offsets, placement, CRCs | `boards/<b>/vocoder_<b>.c` |
| HR_C7000 radios' hosting: the slot header, the copy to RAM | `platform/hrc7000/vocoder.c`; the slot and windows in the board's linker script, the profile prefix in its `board.mk` |
| The window kept free in flash | the board's linker script |
| AMBE frame FEC and the voice path around the codec | `core/dmr/ambe` |

Without a codec merged, the vocoder reports itself absent and the radio runs FM only.

## Adding a radio's codec

The spec side describes the codec in the radio's stock firmware in `docs/spec/codec-<b>.md`: the stock files and
their hashes, the container format, the code and table windows, entry points, calling convention, state sizes and
whether the code is position dependent. Use the existing specs as the model. The clean side then writes the
merge tool and the hosting from that spec.

A merge tool must:

- recognize each supported stock file by SHA-256 and refuse others;
- never write the codec or the stock file into the repository;
- write the merged image outside tracked files (default `build/`);
- leave the flashing to the radio's flash tool.

The board's `board.toml` `[codec]` table names the tool and the route: `stock`, the SHA-256 of each stock file the
codec is merged from, or `profile` for a codec in a linker slot ([The board registry](board-registry.md#codec)).
`codecs/` and `tools/codec_auto.py` find the tool there.

Host tests of the voice path (`tests/host/ambeVoiceTest.c`) use vectors recorded from the codec's FEC, not the
codec itself.
