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
   | GD-88 A (`gd88`) | Radioddity GD-88 A image `B49E.D64.EHRSAB.007` (2022-10-27) | `6b907bd067fc50893427f73159b3815cde37e407a738ab652df8fefef344e238` |
   | GD-88 A (`gd88`) | Radioddity GD-88 A image `B49E.D64.EOPSAB.013` (2023-03-27) | `10e9533cef29797f0c3610565f61d61aaea641d5112d928c714655ce77e6b220` |
   | GD-88 A (`gd88`) | Radioddity GD-88 A image `B49E.D64.EOPSAB.017` (2024-05-10) | `3b2ac6666ff0798c9c0f43f3963fe56c5483ac3347561c7fada0a4d24502dbcc` |
   | GD-88 A (`gd88`) | Radioddity GD-88 A image `B49E.D64.EOPSAB.018` (2024-06-07) | `94115bd990aba9dcf0fe27a5a8f911298dd914f3afe662b688a2e25fefcf625d` |
   | GD-88 A (`gd88`) | Radioddity GD-88 A image `B49E.D64.EOPSAB.021` (2025-01-19) | `2c408b492b1449f71cf9e513a4f25498fdabcbab2ce207062ec1969a46c102df` |
   | GD-88 B (`gd88b`) | Radioddity GD-88 B image `B49C.D64.EHRNAB.005` (2022-10-28) | `3e9cd8a65a36887485718e960539244ba700bca21f1059171fe58794056fb971` |
   | GD-88 B (`gd88b`) | Radioddity GD-88 B image `B49C.D64.EHRNAB.008` (2023-03-22) | `6949edbb2743549aa12c55bc16f513306ed823f77e8c3efe996f5f1f9b05653c` |
   | GD-88 B (`gd88b`) | Radioddity GD-88 B image `B49C.D64.EHRNAB.010` (2024-01-15) | `79b14687ef286bca90b139726bdbb7686fa6932c628f8679b4d8ffe5212be594` |
   | GD-88 B (`gd88b`) | Radioddity GD-88 B image `B49C.D64.EHRNAB.012` (2025-01-17) | `7cfd711d2fe2e2a7c885728fcc4c9f4a66514c3a1a4dadf7e615b28a3ec4c7c9` |
   | MD-380 | TYT `MD-380-D2.32(AD).bin` ("Firmware 2.32", D002.032) | `e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43` |
   | DM-1801 Pro | A 1 MB program flash read of the radio running its stock firmware (`stub/stubtool.py --progmem`) | Any file whose codec code window hashes to `8f7641b6…` (`tools/hrc7000_codec.py`) |

   The DM-1801 Pro has no published update file, so the codec comes from the radio's own flash. Read it **before** the
   first RadioFoundry install: RadioFoundry overwrites the stock firmware, and the codec with it.

2. Build as usual:

   ```
   make BOARD=dm1701
   ```

   Next to the plain image, the build writes `build/dm1701/rfoundry_dm1701_codec.bin`. Flash that one.

The DM-1701 and the MD-380 need no key file, because the codec tool derives the key from the stock file (a few seconds
for the MD-380). If you have the radio's 1024-byte firmware key file, you can put it here too, and the build uses it
instead.

The DB25-D writes `build/db25d/rfoundry_db25d_codec.bin`: the first stage, the application and the codec in one file, the one
to flash (DMR on that radio is a receive-only build option, `CONFIG_DMR=y`, hardware-db25d.md 9).

The GD-88 has two SoCs, A and B, each with its own firmware and its own copy of the codec. Its firmware package has an
A file and a B file: put both here. `make BOARD=gd88` merges the A file into `build/gd88/rfoundry_gd88_codec.bin` and
`make BOARD=gd88b` the B file into `build/gd88b/rfoundry_gd88b_codec.bin`. A program flash read of either SoC running its
stock firmware works too (`stub/stubtool.py --progmem`).

If no supported file is here, the build prints a line saying so and writes only the plain image. A radio running the
plain image works on FM only and shows "No codec: voice off".

## What stays local

Git ignores everything in this folder except this README and `.gitignore`, and merged images are written under
`build/`, which git also ignores. Don't copy a stock file, a key or a merged image anywhere that gets committed or
published.

To merge by hand instead (a different image or output path), run the board's codec tool directly:
`tools/codec_extract.py` for the DM-1701 and the MD-380, `tools/hrc7000_codec.py` for the DM-32, the DB25-D PRO (`--board db25d`) and the GD-88 (`--board gd88`, `--board gd88b`). For flashing, including keeping the
codec that's already in the radio, see [docs/dev/release.md](../docs/dev/release.md) and `tools/rfoundry`.
