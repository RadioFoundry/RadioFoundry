<!-- SPDX-License-Identifier: MIT -->

# Installing

Each ODR release carries the DM-1701 firmware and **odrflash**, the program that puts it on the radio. You don't
build anything. Installing takes four steps: get odrflash, get the stock firmware file, flash, first boot. Nothing
here changes the radio's calibration. For an MD-380, see [Moving from stock MD-380 firmware](18-moving-from-stock-md380.md)
instead.

## What you need

| Item | Where it comes from |
|---|---|
| A computer with Windows, macOS or Linux | |
| odrflash | The release page: one file for your system, `odrflash-<version>-<system>`. Or `pipx install` it (tools/odrflash/README.md) |
| The DM-1701 stock firmware, V02.06 CSV | Baofeng's file `DM-1701(CSV)-V02.06.bin`. An archive is at github.com/daniel5600/BAOFENG-DM1701-Original-Firmware |
| The radio's USB programming cable | The cable that came with the radio |

The release firmware has everything except the AMBE codec. odrflash either keeps the codec already in your radio,
or takes it out of the stock firmware file on your computer and writes it to the radio. The stock file also gives
odrflash the key the radio's bootloader expects. odrflash checks the file by SHA-256 and refuses files it doesn't
recognize; V02.06 CSV is the one it knows today.

Back up the radio's current codeplug with the program you use now (the Baofeng CPS, or qdmr) before you flash. ODR
uses a different codeplug layout from the stock firmware. If you go back to stock later, you need that backup.

## Step 1: get odrflash

Download the odrflash file for your system from the release page and put it somewhere handy.

| System | Once |
|---|---|
| Windows | Run Zadig (zadig.akeo.ie) with the radio in its bootloader (step 3), pick "STM32 BOOTLOADER" (0483:DF11) and install WinUSB. Windows may warn about the unsigned odrflash file once |
| macOS | In Terminal: `xattr -d com.apple.quarantine odrflash-*` and `chmod +x odrflash-*` (the file isn't signed) |
| Linux | `chmod +x odrflash-*`. If odrflash says access is denied, it prints the udev rule to add. To reach the radio's serial port once it runs ODR (`odrflash probe`, updates), join the `dialout` group: `sudo usermod -aG dialout $USER`, then log in again |

The examples below call it `odrflash`.

## Step 2: get the stock firmware file

Download `DM-1701(CSV)-V02.06.bin` (see the table above). Keep it; odrflash reads it whenever it needs the codec or
the key. If you'd rather not pass it every time, let odrflash keep the key:

```
odrflash key --stock "DM-1701(CSV)-V02.06.bin" --save
```

## Step 3: flash

```
odrflash flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
```

odrflash downloads the newest release, checks it, decides about the codec (below) and prints a plan: which flash
sectors it will erase and write, and which it leaves alone. Add `--dry-run` to see the plan without touching the radio.
Then it asks you to put the radio in its bootloader:

1. Turn the radio off.
2. Connect the programming cable to the radio and the computer.
3. Hold **PTT** and **SK1** together.
4. Turn the radio on with the volume knob while you hold both keys. The screen stays dark.
5. Release the keys.

odrflash finds the bootloader, asks once more, and writes. When it finishes, the radio restarts into ODR, and odrflash
checks what the radio reports: the version it just wrote, and "DMR voice is ready" when the codec is in place.

### Keep the codec, or write it

| The radio runs | odrflash |
|---|---|
| ODR, and DMR voice works | Keeps the codec: it writes only ODR's own sectors. It asks the running radio over USB before you switch it to the bootloader (or run `odrflash probe` first) |
| Baofeng stock firmware V02.06 CSV | Can keep the codec too: say so with `--stock-installed "V02.06 CSV"`, or answer yes when it asks |
| anything else, or you're not sure | Writes the codec from the stock file (`--stock`) |

If odrflash keeps a codec that isn't actually in the radio, nothing breaks: ODR checks the codec before it uses it.
FM works, and DMR calls show "No codec: voice off". Flash again with `--stock` to fix it.

## Step 4: first boot

Continue with [First boot](03-first-boot.md).

## Updating ODR

Start the radio normally (ODR running), connect it and run:

```
odrflash flash --release latest
```

odrflash asks the radio whether its codec is in place, and if it is, writes only the new firmware and leaves the
codec alone; no stock file needed. The codeplug and the settings stay: they live in the radio's separate SPI flash,
which flashing never touches.

## No codec ("No codec: voice off")

DMR calls show on the screen but there's no audio: the radio has no usable codec. Write it from the stock file:

```
odrflash flash --release latest --codec merge --stock "DM-1701(CSV)-V02.06.bin"
```

## If something goes wrong

The bootloader lives in a part of the flash odrflash never erases, so you can always start over: put the radio in its
bootloader (step 3) and run the same command again. If the radio stays dark after flashing, that's what to do.

## Going back to the stock firmware

Not tried on the test bench yet.

1. Put the radio in its bootloader (step 3).
2. Run Baofeng's firmware upgrade tool (the installer is in the stock firmware archive) and load the stock file, for
   example `DM-1701(CSV)-V02.06.bin`. The upgrade tool writes the application and the resource area the stock
   firmware keeps in the SPI flash.
3. Write your codeplug backup with the Baofeng CPS. The stock firmware can't read the codeplug ODR used.

## Building it yourself

Developers build with `make BOARD=dm1701` and flash the result with `odrflash flash build/dm1701/odr_dm1701.bin --board
dm1701` (same codec choices). See [building](../dev/building.md).

## DM-32UV

The DM-32UV port builds and passes its host tests, but it hasn't run on a radio yet and isn't in the releases. There
is no supported install procedure. Developers who want to try it follow the bring-up checklist in
[docs/testing/dm32-bringup.md](../testing/dm32-bringup.md). It uses `tools/dm32_codec.py` for the codec. Testers
flash with `tools/dm32_flash.py` over the programming cable, or move from stock with their codeplug using
`tools/dm32_migrate.py` ([Moving from stock DM-32UV firmware](19-moving-from-stock-dm32.md)).
