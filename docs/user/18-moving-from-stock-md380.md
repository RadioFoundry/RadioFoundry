<!-- SPDX-License-Identifier: MIT -->

# Moving from stock MD-380 firmware

This chapter is for a TYT MD-380, MD-390 or Retevis RT3 that runs TYT's stock firmware or the md380tools patched
firmware (the KD4Z toolkit). One program, `md380_migrate.py`, does the whole move:

1. It reads your codeplug from the radio: channels, zones, contacts, RX group lists, your DMR ID.
2. It flashes ODR.
3. It writes the codeplug back, plus the DMR ID (callsign) database.

> The MD-380 port has not run on a radio yet ([Radio reference](23-radios.md#tyt-md-380)). Try it only if you can go
> back to stock with TYT's updater, and keep your stock firmware file.

## What you need

| Item | Notes |
|---|---|
| Python 3.8 or newer | macOS and Linux tested on files only, Windows untested |
| qdmr 0.12 or newer | Brings the `dmrconf` command that converts the codeplug. macOS: `brew install qdmr`. Linux: your distribution's `qdmr` package. Windows: the installer from dm3mat.darc.de/qdmr |
| `pip install pyusb pyserial` and libusb | libusb: macOS `brew install libusb`, Linux `apt install libusb-1.0-0` |
| The ODR image for your band | `odr_md380_codec.bin` (codec included), or `odr_md380.bin` with `--stock` (the tool merges the codec). A UHF image for a UHF radio, a VHF image for a VHF radio. See [Getting the image](#getting-the-image) |
| TYT's `MD-380-D2.32(AD).bin`, or the bootloader key file | The tool derives the bootloader key from the stock file and checks it. A key file (`--key`) works too |
| The programming cable | The radio's USB cable |

A tester kit (`md380_migrate.py package`) is one folder with the script, its helpers, the image, a `README.txt` and
a `requirements.txt`.

## Getting the image

The MD-380 isn't in the releases yet. Build the image from the source ([building](../dev/building.md)):

```
make BOARD=md380                                         # UHF: build/md380/odr_md380.bin
make BOARD=md380 MD380_BAND=vhf BUILD=build/md380-vhf    # VHF
```

With TYT's `MD-380-D2.32(AD).bin` in `codecs/`, the build also writes `odr_md380_codec.bin` with the codec merged in.

## Run it

Look at every step first. This reads only files and touches nothing:

```
python3 md380_migrate.py all --image odr_md380_codec.bin --stock "MD-380-D2.32(AD).bin" --dry-run
```

Then run it for real:

```
python3 md380_migrate.py all --image odr_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

The program asks before each step that needs you at the radio:

| Step | You do | The program does |
|---|---|---|
| 0. Check | nothing | Checks dmrconf, the USB libraries, the image and the key. If something is missing, it stops before it touches the radio |
| 1. Backup | Switch the radio on normally, cable in | Reads the codeplug (`backup-stock.dfu`, `backup-stock.yaml`). Converts it for ODR and shows the counts before and after |
| 2. Flash | Radio off. Hold **PTT** and the **upper side key**, switch on. The screen stays dark | Shows the USB device and asks once more, then writes ODR |
| 3. Settings area | Wait for the ODR Home screen | Checks the radio runs ODR. The stock firmware's data sits where ODR keeps its settings, so it asks, saves that area to a file, and gives it to ODR |
| 4. Codeplug | nothing | Writes the codeplug, waits for the restart, reads it back and compares |
| 5. ID database | nothing | Writes the DMR ID database. A large one takes several minutes |

It keeps everything in `~/ODR-MD380-migration/<date-time>/` and never deletes those files. If a step fails, it
stops and says why. When the failure comes after the flash, it prints the command that continues:

```
python3 md380_migrate.py restore --workdir ~/ODR-MD380-migration/20261011-101500
```

## Your codeplug from a file

If you have the codeplug as a file, the program doesn't need to read the radio first. Give it with `--from`:

| File | From |
|---|---|
| `.rdt` (262,709 bytes) | The TYT CPS, or the KD4Z toolkit |
| a 256 KB `.img` / `.bin` | md380tools' codeplug dump |
| `.dfu` | dmrconf |
| `.yaml` | qdmr |

```
python3 md380_migrate.py all --from my-codeplug.rdt --image odr_md380_codec.bin --stock "MD-380-D2.32(AD).bin"
```

`md380_migrate.py backup --from FILE` only converts and checks, with no radio needed.

## What fits

ODR holds 1024 channels, 68 zones of 80 channels, 1024 contacts and 76 RX group lists. A stock MD-380 holds up to
250 zones and 250 RX group lists. If yours has more, the program lists what would be left out (the last ones). It
asks before it flashes. To choose yourself, open `backup-stock.yaml` in qdmr and delete what you don't need. Save it,
and run again with `--from` on the saved file.

The program also tidies the RX group lists. The TYT CPS heard only the talkgroups in a channel's list, so many
codeplugs have one list per channel holding just that channel's talkgroup. ODR always hears the talkgroup a channel
talks to, so those lists go, as do empty, duplicate and unused ones. Every channel keeps its talkgroup. `--no-cleanup`
keeps the lists as they are.

ODR uses the parts of the codeplug listed in [Programming with qdmr](15-programming-qdmr.md). Scan lists, GPS
systems and TYT's menu and button settings are not carried over.

## The DMR ID database

The stock radio shows names from its contacts. md380tools adds a user database. The program writes ODR's database
in one of two ways:

- By default, dmrconf downloads the public DMR user database and picks the IDs nearest yours (internet needed).
- `--users FILE` takes a users CSV instead: radioid.net's `user.csv`, or the users file the KD4Z toolkit or
  md380tools downloaded. The program shows the first records it read. If a file has no header row and the columns
  look wrong, give their order with `--users-columns id,callsign,fname,surname,city,state,country`.

The database stored in the md380tools radio can't be read out. It is the same public data, so a fresh download is
equivalent.

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

- Some early VHF radios have a small 1 MB flash chip. ODR runs on them, but keeps no codeplug or settings. The
  program stops after the flash and tells you. Go back to stock if you need your channels.
- The program never changes the radio's calibration.
- Windows (untested): to flash, bind the bootloader (0483:DF11) to WinUSB with Zadig
  ([steps](02-installing.md#windows-the-bootloader-driver)). That replaces the driver the TYT CPS uses for the same USB
  ID, so run the backup step first.
- Linux: the program prints the udev rule for 0483:DF11 if access is denied. Add yourself to the `dialout` group for
  the radio's serial port once it runs ODR.
