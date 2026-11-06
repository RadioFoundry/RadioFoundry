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
   | DM-1701 | Radioddity `Firmware-DM-1701(CSV)-V02.01.bin` | `cd9ff4bdb7ba1c464299f3e366bfe72f2e948f3505a8fe54ac19047bff4d6713` |
   | DM-1701 | Radioddity `DM-1701(CSV)-V02.02.bin` | `643650e821db3adf3e0b203437fcdd776d6a7754bc01c0359bd30785ff2478f7` |
   | DM-1701 | Radioddity `DM-1701(CSV)-V02.03 Only for Radioddity.bin` | `35a045d7a71164183b164d6ecfdd9e349c757b3539be67a16fecd68ca6f65948` |
   | DM-1701 | Baofeng `DM-1701(noCSV)-V02.06.bin` | `4230c8d0aabf15f3f165c29a118a5f8f9b906a2d9b7d215c1d8a7bc0e3978bd9` |
   | DM-1701 | Radioddity `DM-1701-V02.03 Only for Radioddity.bin` | `0a23d563fba04a1874c1e4eb4c0c23bf86e1d17da37cd3681f06ea71ad9c800a` |
   | DM-32 | Baofeng `DM32.01.01.049.bin` | `03107b6f600e7fa95105af8324cff349c0f17fe30db249c14d11e79baa8da97a` |
   | DM-32 | Baofeng `DM32.00.01.046HRVocoder.bin` | `f8484c13c994e920649d7b7bc2b98a98a76cfd0836865e5995a00627fc0c2887` |
   | DB25-D PRO | Radioddity `DB25-D PRO 909E.Dx.EARSAB.018_2025-01-21.bin` | `341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b` |
   | DB25-D PRO | Radioddity `DB25-D PRO 909E.Dx.EARSAB.018_2024-09-23.bin` | `f2a4b214720b971b58cdadcc53d44695e1462b75296afc41f2a0b53a704d36b4` |
   | DB25-D PRO | the 2024-05-09 build, `909E.D6x.EARSAB.018` | `9e199f54cd6545d153d39542ca5be3ac1076ee215eb93ca630a83cbdbbff4606` |
   | MD-380 | TYT `MD-380-D2.32(AD).bin` ("Firmware 2.32", D002.032) | `e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43` |
   | MD-9600 | TYT `MD9600-CSV-V06.09.bin` (V1-V3 boards) | `79a561eda2f5843334ea5974f5de511b6e172a1643f9163806d54a94404f0f09` |
   | MD-9600 | TYT `MD9600-CSV(V4)-V16.45.bin` (V4 boards) | `de368437de05307d3e217f049bb5e30b1a8ec201e50b8e81aa5c506089c53532` |
   | DM-1801 Pro | A 1 MB program flash read of the radio running its stock firmware (`stub/stubtool.py --progmem`) | Any file whose codec code window hashes to `8f7641b6…` (`tools/hrc7000_codec.py`) |

   The DM-1801 Pro has no published update file, so the codec comes from the radio's own flash. Read it **before** the
   first RadioFoundry install: RadioFoundry overwrites the stock firmware, and the codec with it.

2. Build as usual:

   ```
   make BOARD=dm1701
   ```

   Next to the plain image, the build writes `build/dm1701/rfoundry_dm1701_codec.bin`. Flash that one.

The DM-1701, the MD-380 and the MD-9600 need no key file, because the codec tool derives the key from the stock file (a few seconds
for the MD-380). If you have the radio's 1024-byte firmware key file, you can put it here too, and the build uses it
instead.

The DB25-D writes `build/db25d/rfoundry_db25d_codec.bin`: the first stage, the application and the codec in one file, the one
to flash (DMR on that radio is a receive-only build option, `CONFIG_DMR=y`, hardware-db25d.md 9).

If no supported file is here, the build prints a line saying so and writes only the plain image. A radio running the
plain image works on FM only and shows "No codec: voice off".

## What stays local

Git ignores everything in this folder except this README and `.gitignore`, and merged images are written under
`build/`, which git also ignores. Don't copy a stock file, a key or a merged image anywhere that gets committed or
published.

To merge by hand instead (a different image or output path), run the board's codec tool directly:
`tools/codec_extract.py` for the DM-1701 and the MD-380, `tools/hrc7000_codec.py` for the DM-32 and the DB25-D PRO (`--board db25d`). For flashing, including keeping the
codec that's already in the radio, see [docs/dev/release.md](../docs/dev/release.md) and `tools/rfoundry`.
