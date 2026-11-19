<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Moving from the original firmware

This chapter describes how to carry your codeplug over from the manufacturer's firmware to RadioFoundry: your
channels, zones, contacts, RX group lists, and DMR ID.

There are two ways:

- **The web installer.** It backs up the radio, installs RadioFoundry, then converts the codeplug and writes it back
  (the **Channels** step in [Installing](02-installing.md#install-with-the-web-installer)). The Codeplug Editor
  converts a backup later ([Files](04-programming.md#files)). Use this way unless you need the command line.
- **A migration program.** Some radios have a program that does the whole move from the command line in one run, and
  also writes the DMR ID database. Your radio's section of the [Radio reference](23-radios.md) names the program,
  what it needs, and the keys for its update mode.

Keep the manufacturer's firmware file for your radio, so that you can go back to the original firmware.

## What the migration program does

1. It reads your codeplug from the radio: channels, zones, contacts, RX group lists, and your DMR ID.
2. It installs RadioFoundry through the radio's bootloader (update mode).
3. It writes the codeplug back, plus the DMR ID (callsign) database.

The program never changes the radio's calibration.

## What you need

| Item | Notes |
|---|---|
| Python 3.8 or later | |
| qdmr | Brings the `dmrconf` command that reads the radio and converts the codeplug. macOS: `brew install qdmr`. Linux: your distribution's `qdmr` package. Windows: the installer from dm3mat.darc.de/qdmr. Your radio's section names the version it needs |
| The Python packages | Your radio's section lists them, for example `pip install pyserial` |
| The RadioFoundry image | From the release page or your own build ([Building](../dev/building.md)). An image with the codec merged in (`_codec.bin`) needs nothing else. For an image without the codec, give the original firmware file, as your radio's section describes |
| The programming cable | The radio's USB cable, or its programming cable on a radio without USB |

A tester kit (`package`) is one folder with the program, its helpers, the image, a `README.txt`, and a
`requirements.txt`.

## Run the program

The examples call the program `MIGRATE.py` and the image `IMAGE.bin`. Use the names from your radio's section.

1. Look at every step first. This reads only files and touches nothing:

   ```sh
   python3 MIGRATE.py all --image IMAGE.bin --dry-run
   ```

2. Run it for real:

   ```sh
   python3 MIGRATE.py all --image IMAGE.bin
   ```

The program asks before each step that needs you at the radio:

| Step | You do | The program does |
|---|---|---|
| 0. Check | Nothing | Checks dmrconf, the libraries, the cable, and the image. If something is missing, it stops before it touches the radio |
| 1. Backup | Turn the radio on normally, with the cable in | Reads the codeplug (`backup-stock.dfu`, `backup-stock.yaml`), converts it for RadioFoundry, and shows what fits |
| 2. Install | Turn the radio off. Hold your radio's update-mode keys and turn it on | Asks once more, then writes RadioFoundry |
| 3. Settings area | Wait for the RadioFoundry Home screen | Checks that the radio runs RadioFoundry. When the original firmware's data sits where RadioFoundry keeps its settings, it asks, saves that area to a file, and gives the area to RadioFoundry |
| 4. Codeplug | Nothing | Writes the codeplug, waits for the restart, reads it back, and compares |
| 5. ID database | Nothing | Writes the DMR ID database. A large one takes several minutes |

The program keeps everything in a work directory in your home folder, one for each run, and never deletes those files.
If a step fails, the program stops and says why. When the failure comes after the installation, it prints the command
that continues, for example:

```sh
python3 MIGRATE.py restore --workdir ~/<work directory>/<date-time>
```

## Your codeplug from a file

If you have the codeplug as a file, the program doesn't need to read the radio first. Give it with `--from`:

```sh
python3 MIGRATE.py all --from my-codeplug.dfu --image IMAGE.bin
```

The program takes a `.dfu` file from dmrconf, or a `.yaml` file from qdmr. Your radio's section lists any other
format. To convert and check a file with no radio, run the program's `backup --from FILE` (or `import --from FILE`,
where your radio's section names it).

## What fits

RadioFoundry holds 1024 channels, 68 zones of 80 channels, 1024 contacts, and 76 RX group lists, with names of up to
16 characters. The original firmware can hold more. If yours has more, the program lists what it would leave out (the
last ones), and asks before it installs. To choose yourself, open `backup-stock.yaml` in qdmr and delete what you
don't need. Save it, and run the program again with `--from` on the saved file.

The program also tidies the RX group lists. Many codeplugs have one list for each channel that holds only that
channel's talkgroup, because the original firmware heard only the talkgroups in a channel's list. RadioFoundry always
hears the talkgroup that a channel talks to, so those lists go, as do empty, duplicate, and unused ones. Every channel
keeps its talkgroup. `--no-cleanup` keeps the lists as they are.

RadioFoundry uses the parts of the codeplug listed in
[What the radio uses from the codeplug](04-programming.md#what-the-radio-uses-from-the-codeplug). Scan lists, GPS
systems, and the original firmware's menu and key settings aren't carried over.

## The DMR ID database

The program writes RadioFoundry's DMR ID database in one of two ways:

- By default, dmrconf downloads the public DMR user database and picks the IDs nearest yours. This needs the internet.
- `--users FILE` takes a users CSV file instead: RadioID.net's `user.csv`. The program shows the first records it
  read. If a file has no header row and the columns look wrong, give their order with
  `--users-columns id,callsign,fname,surname,city,state,country`.

| Option | Action |
|---|---|
| `--db-limit N` | Writes at most N records |
| `--id-prefix` | Picks records by your country's ID prefix |
| `--no-db` | Writes no database |

## Going back to the original firmware

The manufacturer's firmware file goes through the same bootloader. Your radio's section gives the command. Then write
your `backup-stock.dfu` or `backup-stock.yaml` with the manufacturer's CPS or qdmr.

## Notes for each system

- **Windows:** install the driver for the bootloader or the cable first. For a radio that installs over USB, see
  [Windows: the bootloader driver](02-installing.md#windows-the-bootloader-driver).
- **Linux:** if access is denied, the program prints the udev rule to add. To reach the radio's serial port or the
  cable, add yourself to the `dialout` group.
- **macOS:** on a radio with USB, its serial port is listed twice (`cu.usbmodem…` and `tty.usbmodem…`), and `dmrconf`
  can't choose between them. The program finds the radio by its USB ID and gives `dmrconf` the `cu.` name. `--port`
  takes `/dev/cu.usbmodem…` or `cu.usbmodem…`. With two radios connected, the program stops and asks for `--port`.
  `RFOUNDRY_PORT` works like `--port`.
