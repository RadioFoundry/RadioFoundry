<!-- SPDX-License-Identifier: MIT -->

# Moving from Stock MD-380 Firmware

This chapter is for a TYT MD-380, MD-390 or Retevis RT3 that runs TYT's stock firmware. One program, `md380_migrate.py`, does the whole move:

1. It reads your codeplug from the radio: channels, zones, contacts, RX group lists, your DMR ID.
2. It flashes RadioFoundry.
3. It writes the codeplug back, plus the DMR ID (callsign) database.

The web installer and the Codeplug Editor do the same in two steps: the installer backs up the radio and installs
RadioFoundry, and the editor converts the backup ([Installing](02-installing.md), [Programming the
radio](04-programming.md#files)). Use this program to do all of it in one run. What has been checked on the radio is in
the [Radio reference](23-radios.md#tyt-md-380). Keep your stock firmware file, so you can go back to stock with TYT's
updater.

## What you need

| Item | Notes |
|---|---|
| Python 3.8 or newer | macOS and Linux tested on files only, Windows untested |
| qdmr 0.12 or newer | Brings the `dmrconf` command that converts the codeplug. macOS: `brew install qdmr`. Linux: your distribution's `qdmr` package. Windows: the installer from dm3mat.darc.de/qdmr |
| `pip install pyusb pyserial` and libusb | libusb: macOS `brew install libusb`, Linux `apt install libusb-1.0-0` |
| The RadioFoundry image for your band | `rfoundry_md380_codec.bin` (codec included), or `rfoundry_md380.bin` with `--stock` (the tool merges the codec). A UHF image for a UHF radio, a VHF image for a VHF radio. See [Getting the image](#getting-the-image) |
| TYT's `MD-380-D2.32(AD).bin`, or the bootloader key file | The tool derives the bootloader key from the stock file and checks it. A key file (`--key`) works too |
| The programming cable | The radio's USB cable |

Where to get the stock file: TYT's download page ([tyt888.com/download.html](https://www.tyt888.com/download.html)) ships
the firmware update as a Windows program, and the `.bin` is inside it. The archive at
[md380.org/firmware/orig](https://md380.org/firmware/orig/) has every known MD-380 release as plain files. Use
**D2.32** (`MD-380-D2.32(AD).bin`): it gives both the bootloader key and the voice codec. D2.30 gives the key only.
The web flasher also takes TYT's download zip as it is, and checks every file by its SHA-256.

A tester kit (`md380_migrate.py package`) is one folder with the script, its helpers, the image, a `README.txt` and
a `requirements.txt`.

## Getting the image

The release page carries the image of each band. To build one yourself ([building](../dev/building.md)):

```
make BOARD=md380                                         # UHF: build/md380/rfoundry_md380.bin
make BOARD=md380 BAND=vhf                                # VHF: build/md380-vhf/rfoundry_md380.bin
```

With TYT's `MD-380-D2.32(AD).bin` in `codecs/`, the build also writes `rfoundry_md380_codec.bin` with the codec merged in.

## Run it

Look at every step first. This reads only files and touches nothing:

```
python3 md380_migrate.py all --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin" --dry-run
```

Then run it for real:

```
python3 md380_migrate.py all --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

The program asks before each step that needs you at the radio:

| Step | You do | The program does |
|---|---|---|
| 0. Check | nothing | Checks dmrconf, the USB libraries, the image and the key. If something is missing, it stops before it touches the radio |
| 1. Backup | Switch the radio on normally, cable in | Reads the codeplug (`backup-stock.dfu`, `backup-stock.yaml`). Converts it for RadioFoundry and shows the counts before and after |
| 2. Flash | Radio off. Hold **PTT** and the **upper side key**, switch on. The screen stays dark | Shows the USB device and asks once more, then writes RadioFoundry |
| 3. Settings area | Wait for the RadioFoundry Home screen | Checks the radio runs RadioFoundry. The stock firmware's data sits where RadioFoundry keeps its settings, so it asks, saves that area to a file, and gives it to RadioFoundry |
| 4. Codeplug | nothing | Writes the codeplug, waits for the restart, reads it back and compares |
| 5. ID database | nothing | Writes the DMR ID database. A large one takes several minutes |

It keeps everything in `~/RadioFoundry-MD380-migration/<date-time>/` and never deletes those files. If a step fails, it
stops and says why. When the failure comes after the flash, it prints the command that continues:

```
python3 md380_migrate.py restore --workdir ~/RadioFoundry-MD380-migration/20261011-101500
```

## Your codeplug from a file

If you have the codeplug as a file, the program doesn't need to read the radio first. Give it with `--from`:

| File | From |
|---|---|
| `.rdt` (262,709 bytes) | The TYT CPS |
| `.dfu` | dmrconf |
| `.yaml` | qdmr |

```
python3 md380_migrate.py all --from my-codeplug.rdt --image rfoundry_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

`md380_migrate.py backup --from FILE` only converts and checks, with no radio needed.

## What fits

RadioFoundry holds 1024 channels, 68 zones of 80 channels, 1024 contacts and 76 RX group lists. A stock MD-380 holds up to
250 zones and 250 RX group lists. If yours has more, the program lists what would be left out (the last ones). It
asks before it flashes. To choose yourself, open `backup-stock.yaml` in qdmr and delete what you don't need. Save it,
and run again with `--from` on the saved file.

The program also tidies the RX group lists. The TYT CPS heard only the talkgroups in a channel's list, so many
codeplugs have one list per channel holding just that channel's talkgroup. RadioFoundry always hears the talkgroup a channel
talks to, so those lists go, as do empty, duplicate and unused ones. Every channel keeps its talkgroup. `--no-cleanup`
keeps the lists as they are.

RadioFoundry uses the parts of the codeplug listed in [Programming the radio](04-programming.md). Scan lists, GPS
systems and TYT's menu and button settings are not carried over.

## The DMR ID database

The program writes RadioFoundry's DMR ID database in one of two ways:

- By default, dmrconf downloads the public DMR user database and picks the IDs nearest yours (internet needed).
- `--users FILE` takes a users CSV instead: radioid.net's `user.csv`. The program shows the first records it read. If a file has no header row and the columns
  look wrong, give their order with `--users-columns id,callsign,fname,surname,city,state,country`.

| Option | Does |
|---|---|
| `--db-limit N` | At most N records |
| `--id-prefix` | Picks records by your country's ID prefix |
| `--no-db` | Writes no database |

## Going back to stock

TYT's updater file goes through the same bootloader:

```
python3 md380_migrate.py flash --image "MD-380-D2.32(AD).bin"
```

Then write your `backup-stock.rdt` or `backup-stock.dfu` with the TYT CPS or qdmr (`dmrconf write --radio md390`).

## Limits and notes

- Some early VHF radios have a small 1 MB flash chip. RadioFoundry runs on them, but keeps no codeplug or settings. The
  program stops after the flash and tells you. Go back to stock if you need your channels.
- The program never changes the radio's calibration.
- Windows (untested): to flash, bind the bootloader (0483:DF11) to WinUSB with Zadig
  ([steps](02-installing.md#windows-the-bootloader-driver)). That replaces the driver the TYT CPS uses for the same USB
  ID, so run the backup step first.
- Linux: the program prints the udev rule for 0483:DF11 if access is denied. Add yourself to the `dialout` group for
  the radio's serial port once it runs RadioFoundry.
