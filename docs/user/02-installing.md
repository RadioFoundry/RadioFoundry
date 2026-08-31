<!-- SPDX-License-Identifier: MIT -->

# Installing

Installing ODR on a DM-1701 takes five steps: get the files, build the firmware, merge the codec, put the radio in
its bootloader, and flash. Everything runs on your own computer. Nothing here changes the radio's calibration.

## What you need

| Item | Where it comes from |
|---|---|
| A computer with macOS or Linux | Windows works for flashing (see below); building is tested on macOS and Linux |
| The ODR source code | The project repository, cloned with its submodules |
| `make`, Python 3 and `arm-none-eabi-gcc` | Your package manager (for example `brew install --cask gcc-arm-embedded`, or `apt install gcc-arm-none-eabi`) |
| `pyusb` and libusb | `pip install pyusb`, and `brew install libusb` (macOS) or `apt install libusb-1.0-0` (Linux) |
| The DM-1701 stock firmware, V02.06 CSV | Baofeng's file `DM-1701(CSV)-V02.06.bin`. An archive is at github.com/daniel5600/BAOFENG-DM1701-Original-Firmware |
| The DM-1701 firmware key | A 1024-byte file, not in the repository. You supply it. The tools check it by SHA-256 (`d983ce95...`) |
| The radio's USB programming cable | The cable that came with the radio |

The stock firmware file provides the codec and the key encrypts the image for the radio's bootloader. Neither file is
part of ODR, and neither goes into a release. The codec tool accepts only stock files it recognizes by SHA-256:
V02.06 CSV is the one it knows today.

Back up the radio's current codeplug with the program you use now (the Baofeng CPS, or qdmr) before you flash. ODR
uses a different codeplug layout from the stock firmware. If you go back to stock later, you need that backup.

## Step 1: build the firmware

```
git clone --recursive <repository URL> OpenDigitalRadio
cd OpenDigitalRadio
make BOARD=dm1701
```

The build ends with a line like `image build/dm1701/odr_dm1701.bin: N bytes, version 4285c1b`. The version is
the commit the image was built from; it shows on the radio's boot screen and in **Settings > Advanced > Firmware**.

## Step 2: merge the codec

```
tools/codec_extract.py --stock "DM-1701(CSV)-V02.06.bin" --key dm1701.key
```

The tool checks the stock file and the key, takes the codec out of the stock firmware and writes
`build/dm1701/odr_dm1701_codec.bin` next to the image. Use `--image` and `-o` to choose other files.

If you skip this step, the radio runs FM only for voice. DMR calls still show on the screen, with
"No codec: voice off".

## Step 3: enter the bootloader

1. Turn the radio off.
2. Connect the programming cable to the radio and the computer.
3. Hold **PTT** and **SK1** together.
4. Turn the radio on with the volume knob while you hold both keys. The screen stays dark.
5. Release the keys.

Check that the computer sees the bootloader:

```
tools/stm32_dfu.py --list
```

It lists a DFU device 0483:DF11 whose first string is "AnyRoad Technology".

## Step 4: flash

```
tools/stm32_dfu.py --model dm1701 --key dm1701.key build/dm1701/odr_dm1701_codec.bin
```

The tool encrypts the image with the key, erases the application area and writes it. When it finishes, the radio
restarts into ODR.

The tool refuses official Baofeng firmware files ("OutSecurityBin" files): they are for the Baofeng upgrade tool.

On Linux, if the tool reports that access to the device is denied, add the udev rule it prints. On Windows, bind
the DFU device to WinUSB with Zadig first.

## Step 5: first boot

Continue with [First boot](03-first-boot.md).

## Updating ODR

Build the new version, merge the codec again (step 2) and flash it (steps 3 and 4). The codeplug and the settings
stay: flashing writes only the application area of the processor's flash, and the codeplug and settings live in the
separate SPI flash.

## Going back to the stock firmware

Not tried on the test bench yet.

1. Put the radio in its bootloader (step 3).
2. Run Baofeng's firmware upgrade tool (the installer is in the stock firmware archive) and load the stock file, for
   example `DM-1701(CSV)-V02.06.bin`. The upgrade tool writes the application and the resource area the stock
   firmware keeps in the SPI flash.
3. Write your codeplug backup with the Baofeng CPS. The stock firmware can't read the codeplug ODR used.

## DM-32UV

The DM-32UV port builds and passes its host tests, but it hasn't run on a radio yet. There is no supported install
procedure. Developers who want to try it follow the bring-up checklist in
[docs/testing/dm32-bringup.md](../testing/dm32-bringup.md). It uses `tools/dm32_codec.py` for the codec and
`tools/dm32_pack.py` to wrap the image for Baofeng's updater.
