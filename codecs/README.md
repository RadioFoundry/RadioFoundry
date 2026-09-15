# codecs/

Put your radio's stock firmware file here and every build also writes an image with the AMBE+2 codec merged in.

DMR voice needs the radio maker's AMBE+2 codec. That codec belongs to DVSI and the radio maker, so this repository
and the release images never contain it. Each radio uses the codec from its own stock firmware.

## Use

1. Copy the stock firmware file for your radio into this folder. Any file name works: the build recognizes the file
   by its SHA-256.

   | Radio | Stock file | SHA-256 |
   |---|---|---|
   | DM-1701 | Baofeng `DM-1701(CSV)-V02.06.bin` | `0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d` |
   | DM-32 | Baofeng `DM32.01.01.049.bin` | `03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a` |
   | DM-32 | Baofeng `DM32.00.01.046HRVocoder.bin` | `f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887` |
   | MD-380 | TYT `MD-380-D2.32(AD).bin` ("Firmware 2.32", D002.032) | `e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43` |

2. Build as usual:

   ```
   make BOARD=dm1701
   ```

   Next to the plain image, the build writes `build/dm1701/odr_dm1701_codec.bin`. Flash that one.

The DM-1701 and the MD-380 need no key file, because the codec tool derives the key from the stock file (a few seconds
for the MD-380). If you have the radio's 1024-byte firmware key file, you can put it here too, and the build uses it
instead.

If no supported file is here, the build prints a line saying so and writes only the plain image. A radio running the
plain image works on FM only and shows "No codec: voice off".

## What stays local

Git ignores everything in this folder except this README and `.gitignore`, and merged images are written under
`build/`, which git also ignores. Don't copy a stock file, a key or a merged image anywhere that gets committed or
published.

To merge by hand instead (a different image or output path), run the board's codec tool directly:
`tools/codec_extract.py` for the DM-1701 and the MD-380, `tools/dm32_codec.py` for the DM-32. For flashing, including keeping the
codec that's already in the radio, see [docs/dev/release.md](../docs/dev/release.md) and `tools/odrflash`.
