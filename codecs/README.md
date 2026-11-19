<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# codecs/

Put your radio's manufacturer firmware file in this folder, and every build also writes an image with the AMBE+2 codec
merged in.

DMR voice needs the AMBE+2 codec. The codec belongs to DVSI and the radio maker, so this repository and the release
images never contain it. Each radio uses the codec from its own manufacturer firmware.

## Use

1. Copy the manufacturer firmware file for your radio into this folder. Any file name works: the build recognizes the
   file by its SHA-256. The files each board accepts are named in its codec maps,
   `tools/rfoundry/rfoundry/codec_maps/<board>/*.json` (`stock.name`, `stock.version`, `stock.sha256`). A radio with
   no published update file takes a read of its own program flash (`stub/stubtool.py PORT --progmem FILE`) made while
   it still runs its manufacturer firmware.
2. Build as usual:

   ```
   make BOARD=<board>
   ```

   Next to the plain image, the build writes `build/<board>/rfoundry_<board>_codec.bin`. Flash that file. On a board
   with a first stage, that file holds the first stage, the application, and the codec.

The codec tool derives the bootloader key from the firmware file where the board needs one. If you have the radio's
1024-byte key file, you can put it in this folder too, and the build uses it instead.

If no supported file is here, the build prints a line that says so and writes only the plain image. A radio that runs
the plain image works on FM only and shows "No codec: voice off".

## What stays local

Git ignores everything in this folder except this README and `.gitignore`, and the build writes merged images under
`build/`, which git also ignores. Never copy a manufacturer file, a key, or a merged image anywhere that gets committed
or published.

To merge by hand (a different image or output path), run the board's codec tool directly: `tools/codec_extract.py` on
the `stm32f4` platform, or `tools/hrc7000_codec.py --board <board>` on `hrc7000`. For how merging and keeping work, see
[The AMBE codec](../docs/dev/codec.md). To flash, see [tools/rfoundry](../tools/rfoundry/README.md).
