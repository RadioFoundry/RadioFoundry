<!-- SPDX-License-Identifier: MIT -->

# Moving from stock DM-32UV firmware

This chapter is for a Baofeng DM-32UV running Baofeng's stock firmware. One program, `dm32_migrate.py`, does the
whole move over the programming cable:

1. It reads your codeplug from the radio: channels, zones, contacts, RX group lists, your DMR ID.
2. It flashes ODR through the radio's own bootloader (update mode).
3. It writes the codeplug back, plus the DMR ID (callsign) database.

> The DM-32UV port runs on a radio but is still in testing; [the bring-up checklist](../testing/dm32-bringup.md)
> lists what is still open. Try it only if you can go back to stock, and keep Baofeng's firmware file
> (`DM32.01.01.049.bin`).

ODR never touches the bootloader, so update mode always works. It never touches the data flash below 7 MB either.
The stock codeplug, the factory calibration and the stock contact database live there. Going back to stock gives you your old
codeplug as it was.

## What you need

| Item | Notes |
|---|---|
| Python 3.8 or newer | macOS, Linux and Windows. Tested on files only |
| qdmr 0.15 or newer | Brings the `dmrconf` command that reads the stock radio and converts the codeplug. macOS: `brew install qdmr`. Linux: your distribution's `qdmr` package. Windows: the installer from dm3mat.darc.de/qdmr |
| `pip install pyserial` | For the cable |
| The ODR image | `odr_dm32_codec.bin` with the codec merged in, from the tester kit or your own build ([bring-up checklist](../testing/dm32-bringup.md), step 0). An image without the codec works with `--codec-from DM32.01.01.049.bin` |
| The programming cable | The DM-32's USB-serial cable (CH340, FTDI or CP210x). The radio has no USB of its own: the stock CPS, the bootloader and ODR all use this cable |

A tester kit (`dm32_migrate.py package`) is one folder with the scripts, their helpers, the image, a `README.txt`
and a `requirements.txt`.

## Run it

Look at every step first. This reads only files and touches nothing:

```
python3 dm32_migrate.py all --image odr_dm32_codec.bin --dry-run
```

Then run it for real. With one USB-serial cable plugged in, the program finds it. Otherwise add
`--port /dev/ttyUSB0` (Linux), `--port /dev/cu.usbserial-XXXX` (macOS) or `--port COM5` (Windows):

```
python3 dm32_migrate.py all --image odr_dm32_codec.bin
```

The program asks before each step that needs you at the radio:

| Step | You do | The program does |
|---|---|---|
| 0. Check | nothing | Checks dmrconf, pyserial, the cable and the image. If something is missing, it stops before it touches the radio |
| 1. Backup | Switch the radio on normally, cable in | Reads the codeplug (`backup-stock.dfu`, `backup-stock.yaml`). Converts it for ODR and shows what fits and what doesn't (`conversion-report.txt`) |
| 2. Flash | Radio off. Hold **PTT** and **SK1** (the side key just below PTT), switch on. The green LED lights | Asks once more, then writes ODR. The radio erases its old firmware first, which takes up to 15 s |
| 3. Settings area | Wait for the ODR Home screen | Checks the radio runs ODR. If stock data sits where ODR keeps its settings, it asks, saves that area to a file, and gives it to ODR |
| 3b. Data flash backup | nothing | Reads the stock codeplug and calibration area (800 KB, about 2 minutes) into the work directory. `--flash-backup full` saves all 16 MB (about 30 minutes), `none` skips it |
| 4. Codeplug | nothing | Saves what was in ODR's codeplug area, writes the codeplug, waits for the restart, reads it back and compares |
| 5. ID database | nothing | Writes the DMR ID database. A large one takes many minutes over the cable |

It keeps everything in `~/ODR-DM32-migration/<date-time>/` and never deletes those files. If a step fails, it
stops and says why. When the failure comes after the flash, it prints the command that continues:

```
python3 dm32_migrate.py restore --workdir ~/ODR-DM32-migration/20261011-101500
```

## Your codeplug from a file

`--from` takes a codeplug saved with qdmr from the stock radio. It can be a `.dfu` (`dmrconf read --radio dm32uv`)
or a `.yaml`. `dm32_migrate.py import --from FILE` only converts and checks, with no radio needed. Files from Baofeng's
own CPS aren't supported here. Read the radio with qdmr and save the codeplug from there.

## What fits

ODR holds 1024 channels, 68 zones of 80 channels, 1024 contacts and 76 RX group lists, with names of up to 16
characters. The stock radio holds more. If yours has more, the program lists what would be left out (the last ones).
It asks before it flashes. To choose yourself, open `backup-stock.yaml` in qdmr and delete what you don't need. Save
it, and run again with `--from` on the saved file.

The program also tidies the RX group lists: lists that hold only the channel's own talkgroup go (ODR always hears the
talkgroup a channel talks to), as do empty, duplicate and unused ones. Every channel keeps its talkgroup.
`--no-cleanup` keeps the lists as they are.

`conversion-report.txt` lists everything that changes: counts before and after, names that get shortened, channels
outside 136-174 and 400-480 MHz, and the parts of the codeplug ODR doesn't use. These parts aren't carried over: the
stock menu, key and display settings, scan lists, roaming, GPS and APRS settings, encryption keys and the stock
contact database. ODR
uses the parts of the codeplug listed in [Programming the radio](15-programming-qdmr.md).

## The DMR ID database

By default, dmrconf downloads the public DMR user database and picks the IDs nearest yours (internet needed). The
radio holds about 300,000 records. When the list is longer, the tool keeps the ones nearest your ID.
`--users FILE` takes radioid.net's `user.csv` instead. Options: `--db-limit N`, `--id-prefix`, `--no-db`.

## Flashing only

`serial_loader.py` flashes any image without touching the codeplug:

```
python3 serial_loader.py odr_dm32_codec.bin       # a raw image or a _BFUV32.bin file
python3 serial_loader.py --list-ports             # the cables it sees
```

## Going back to stock

Baofeng's firmware file goes through the same bootloader:

```
python3 serial_loader.py --stock DM32.01.01.049.bin
```

The stock codeplug was never overwritten, so the radio comes back with it. If it doesn't, write `backup-stock.dfu` with
qdmr (`dmrconf write --radio dm32uv`). ODR's own data stays at 7 MB and up. The stock firmware may later reuse that
space.

## Limits and notes

- If the radio doesn't start after a flash, remove and refit the battery. Update mode still works.
- The program never changes the radio's calibration.
- Linux: add yourself to the `dialout` group for the cable.
- Windows: install the cable's driver (CH340 or FTDI) first. Untested.
