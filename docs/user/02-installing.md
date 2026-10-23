<!-- SPDX-License-Identifier: MIT -->

# Installing

Each RadioFoundry release carries the firmware for the radios in the releases and **rfoundry**, the program that puts it on
the radio. You don't build anything. Installing takes four steps: get rfoundry, get the stock firmware file, flash,
first boot. Nothing here changes the radio's calibration.

The steps are the same on every radio. What differs (the stock firmware file, the keys that start the bootloader) is
in your radio's "Installing" section of the [Radio reference](23-radios.md). Radios that aren't in the releases have
their own route: see [Radios not in the releases](#radios-not-in-the-releases).

## In your browser: the web flasher

The project's web site has a flasher that does everything on this page in the browser: it backs up the radio, picks
the build (Full, Amateur or Commercial, or your own feature selection), keeps or writes the codec and writes the
firmware. Later it can put the radio back from that backup. It needs Chrome, Edge or another Chromium browser on a
computer; on Windows the bootloader needs a driver once ([Windows: the bootloader driver](#windows-the-bootloader-driver)). Nothing you load into it leaves your
computer. **New: not yet tested on a radio.** Until it is, rfoundry (below) is the tested way.

## What you need

| Item | Where it comes from |
|---|---|
| A computer with Windows, macOS or Linux | |
| rfoundry | The release page: one file for your system, `rfoundry-<version>-<system>`. Or `pipx install` it (tools/rfoundry/README.md) |
| Your radio's stock firmware file | The manufacturer's file, in the version rfoundry recognizes. Your radio's "Installing" section names it and where to find it |
| The radio's USB programming cable | The cable that came with the radio |

The release firmware has everything except the AMBE codec. rfoundry either keeps the codec already in your radio,
or takes it out of the stock firmware file on your computer and writes it to the radio. The stock file also gives
rfoundry the key the radio's bootloader expects; once the radio runs RadioFoundry, rfoundry reads the key from the radio
instead. rfoundry checks the file by SHA-256 and refuses files it doesn't recognize.

Back up the radio's current codeplug with the program you use now (the manufacturer's CPS, or qdmr) before you
flash. RadioFoundry uses a different codeplug layout from the stock firmware. If you go back to stock later, you need that
backup.

## Step 1: get rfoundry

Download the rfoundry file for your system from the release page and put it somewhere handy.

| System | Once |
|---|---|
| Windows | Install the bootloader's driver ([below](#windows-the-bootloader-driver)). Windows may warn about the unsigned rfoundry file once |
| macOS | In Terminal: `xattr -d com.apple.quarantine rfoundry-*` and `chmod +x rfoundry-*` (the file isn't signed) |
| Linux | `chmod +x rfoundry-*`. If rfoundry says access is denied, it prints the udev rule to add. To reach the radio's serial port once it runs RadioFoundry (`rfoundry probe`, updates), join the `dialout` group: `sudo usermod -aG dialout $USER`, then log in again |

The examples below call it `rfoundry`, and the stock file `STOCK.bin`.

### Windows: the bootloader driver

Windows has no driver for the radio's bootloader, so neither rfoundry nor the web flasher can open it until you install
one. Do this once per computer, with Zadig:

1. Download Zadig from [zadig.akeo.ie](https://zadig.akeo.ie) and run it. It needs no install.
2. Put the radio in its bootloader ([step 3](#step-3-flash), your radio's keys are in its
   [Radio reference](23-radios.md) section) and plug it in.
3. In Zadig's list, pick **Digital Radio in USB mode**. The USB ID boxes show `0483` `DF11`. If it isn't listed,
   choose **Options › List All Devices**.
4. Choose **WinUSB** as the driver (to the right of the green arrow) and click **Install Driver** (or **Replace
   Driver**). Wait until Zadig says it succeeded.
5. Unplug the radio, put it back in its bootloader and connect again.

Only the bootloader needs this. The radio's serial port, once it runs RadioFoundry, works without a driver.

Vendor tools that use the same USB ID (Baofeng's or TYT's upgrade tool, the MD-380's CPS) need their own driver back.
In Device Manager, uninstall the device with **Delete the driver software for this device** checked, then replug.

## Step 2: get the stock firmware file

Download your radio's stock file and keep it; rfoundry reads it whenever it needs the codec or the key. If you'd
rather not pass it every time, let rfoundry keep the key:

```
rfoundry key --stock STOCK.bin --save
```

## Step 3: flash

```
rfoundry flash --release latest --stock STOCK.bin
```

Radios that come in hardware versions one image can't cover (the MD-380's UHF and VHF boards) have one image per
version. rfoundry picks it from what a radio already running RadioFoundry reports; otherwise it asks, or takes
`--option BAND=vhf`. Your radio's section names its versions.

rfoundry downloads the newest release, checks it, decides about the codec (below) and prints a plan: which flash
sectors it will erase and write, and which it leaves alone. Add `--dry-run` to see the plan without touching the radio.
Then it asks you to put the radio in its bootloader:

1. Turn the radio off.
2. Connect the programming cable to the radio and the computer.
3. Hold the radio's bootloader keys and turn it on. The screen stays dark.
4. Release the keys.

rfoundry finds the bootloader, asks once more, and writes. When it finishes, the radio restarts into RadioFoundry, and rfoundry
checks what the radio reports: the version it just wrote, and "DMR voice is ready" when the codec is in place.

### Keep the codec, or write it

| The radio runs | rfoundry |
|---|---|
| RadioFoundry, and DMR voice works | Keeps the codec: it writes only RadioFoundry's own sectors. It asks the running radio over USB before you switch it to the bootloader (or run `rfoundry probe` first) |
| Anything else, or you're not sure | Writes the codec: from the copy it saved from this radio the last time it ran RadioFoundry, otherwise from the stock file (`--stock`) |

The stock firmware keeps the codec where RadioFoundry's own firmware goes, so a radio fresh from stock always needs the stock
file once. After that, rfoundry keeps a copy of the codec on your computer.

If rfoundry keeps a codec that isn't actually in the radio, nothing breaks: RadioFoundry checks the codec before it uses it.
FM works, and DMR calls show "No codec: voice off". Flash again with `--stock` to fix it.

## Step 4: first boot

Continue with [First boot](03-first-boot.md).

## Updating RadioFoundry

Start the radio normally (RadioFoundry running), connect it and run:

```
rfoundry flash --release latest
```

rfoundry asks the radio whether its codec is in place, and if it is, writes only the new firmware and leaves the
codec alone; no stock file needed. The codeplug and the settings stay: they live in the radio's separate SPI flash,
which flashing never touches.

## No codec ("No codec: voice off")

DMR calls show on the screen but there's no audio: the radio has no usable codec. Write it from the stock file:

```
rfoundry flash --release latest --codec merge --stock STOCK.bin
```

## If something goes wrong

The bootloader lives in a part of the flash rfoundry never erases, so you can always start over: put the radio in its
bootloader (step 3) and run the same command again. If the radio stays dark after flashing, that's what to do.

## Going back to the stock firmware

Not tried on the test bench yet.

1. Put the radio in its bootloader (step 3).
2. Run the manufacturer's firmware upgrade tool and load the stock file.
3. Write your codeplug backup with the manufacturer's CPS. The stock firmware can't read the codeplug RadioFoundry used.

## Building it yourself

Developers build with `make BOARD=<board>` and flash the result with `rfoundry flash build/<board>/rfoundry_<board>.bin
--board <board>` (same codec choices). With the stock file in `codecs/`, the build also writes
`rfoundry_<board>_codec.bin`, which carries the codec: `rfoundry flash build/<board>/rfoundry_<board>_codec.bin` flashes it with
no `--board`, and `--stock` is needed only for the bootloader key. See [building](../dev/building.md).

## Radios not in the releases

| Radio | Route |
|---|---|
| MD-380 | [Moving from stock MD-380 firmware](18-moving-from-stock-md380.md) |
| DM-32UV | [Moving from stock DM-32UV firmware](19-moving-from-stock-dm32.md) |
| Others | No install procedure yet ([Radio reference](23-radios.md)) |
