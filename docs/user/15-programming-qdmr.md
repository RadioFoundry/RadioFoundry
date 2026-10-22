<!-- SPDX-License-Identifier: MIT -->

# Programming the radio

The codeplug holds the channels, zones, contacts, RX group lists, the DMR ID database and your DMR ID. Two programs
edit it:

| Program | Runs | Radios |
|---|---|---|
| The Codeplug Editor on the project's web site | In a Chromium-based browser (Chrome, Edge, Opera), nothing to install | Every ODR radio: over USB, or over the programming cable |
| qdmr (github.com/hmatuschek/qdmr), the free codeplug editor | Windows, macOS, Linux | Radios with a USB port. Not the DM-32UV ([below](#radios-qdmr-cant-program-directly)) |

Both use the same codeplug format and the radio's own codeplug interface, so a codeplug written by one reads back in
the other. The radios this is tested on are in the [Radio reference](23-radios.md).

## The Codeplug Editor

The editor's page is Codeplug Editor in the site's menu. It reads and writes the radio from the browser. Nothing is
sent to a server: files, the codeplug and your RadioID API key stay on your computer.

### Reading and writing the radio

1. Turn the radio on with ODR running and connect it: the USB cable (DM-1701, MD-380), or the programming cable in the
   accessory jack (DM-32UV, DM-1801 Pro, DB25-D PRO). The radio must be in USB serial mode, not USB network mode.
2. Choose **Read from Radio**, then **USB Cable** or **Programming Cable**. The browser asks which port to use.
3. Edit the codeplug.
4. Choose **Write to Radio**. The editor compares the codeplug with the radio's and writes only the bytes that
   changed. It shows how many bytes and about how long it takes, then writes and verifies them. The radio loads the
   new codeplug without restarting.

The first write to a radio that came straight from its original firmware may ask to **Prepare Radio**. The original
firmware's data is still in the area where ODR keeps its settings, and until that area is erased the radio can't
store a codeplug or keep its settings when turned off. The web radio programming utility does this by itself after
the full backup it offers when it installs ODR. The backup holds a copy of that data; the calibration isn't touched.

Write to Radio is blocked while the **Problems** tab lists errors, such as a name that is too long or a frequency
that isn't valid. Warnings, such as a frequency outside the radio's bands or a DMR channel without a TX contact,
don't block it.

A browser can't reach a radio in USB network mode. There, choose **Save > For USB Network** and send the file with
TFTP ([USB network](14-usb-network.md#programming-over-the-network)).

### Editing

| Tab | Holds |
|---|---|
| Radio | DMR ID, radio name (callsign), boot text, group and private call hang times |
| Channels | Every channel. The pencil button opens the full form: FM, DMR, scan and power, location, single frequency repeater |
| Zones | Zones and their channels |
| Contacts | Talkgroups and private-call contacts |
| RX Group Lists | The talkgroups each channel listens to |
| DTMF Contacts, APRS | DTMF contacts and APRS configurations |
| DMR ID Database | Names for DMR IDs that aren't contacts, built from RadioID.net (below) |
| Problems | Errors and warnings, each with a link to the record |

In a table, double-click a cell or type on it to edit it in place. Click a column heading to sort, and type in the
filter to narrow the rows. Check rows and choose **Bulk Edit** to set one field on all of them. **Undo** and
**Redo** cover every change, including an import.

The page keeps the working codeplug in the browser, so closing or reloading the page doesn't lose edits. **New**
starts over.

### Files

**Open File** takes:

- a codeplug file (`.bin`) saved by the editor, `tools/cps_client.py read` or a TFTP read;
- a backup from the radio programming utility (`.zip`);
- a qdmr codeplug (`.yaml`);
- CSV files, one table each, or a `.zip` of them;
- the original firmware's codeplug, from a DM-1701, RT84, MD-UV380/390 or MD-380: in a backup, a dump of the radio's
  data flash, or a TYT MD-380 CPS file (`.rdt`).

A backup made when ODR was installed still holds the original firmware's codeplug, because ODR keeps its own
codeplug in a different layout. The editor recognizes that codeplug and converts it: channels, zones, contacts, RX
group lists, the DMR ID, the radio name and the boot text. Afterwards it lists what it left out, such as scan lists
and names that were shortened. A zone with an A and a B list becomes two zones, "Name A" and "Name B". Check the
result before writing it to the radio. The radio's codeplug stays as it is until you write it. MD-UV380 family CPS
files (`.rdt`) can't be opened yet: convert one with qdmr first (`dmrconf decode --manufacturer --radio dm1701
FILE.rdt FILE.yaml`) and open the YAML file.

**Save** writes a codeplug file, a qdmr codeplug (`.yaml`), the open table as CSV, every table as a `.zip` of CSV
files, or `codeplug.bin` for USB network mode.

CSV files have a header row with the column names, and refer to other records by name (a channel's TX contact, a
zone's channels). Save a table first to see the columns. An import matches rows to existing records by number, then
by name. A file can hold only some columns; the others keep their values. Records missing from the file are removed.
Values the editor can't take are listed by row and column after the import.

A qdmr file replaces the channels, zones, contacts, DTMF contacts, RX group lists, APRS configurations and the
Radio tab's settings in one change, so one **Undo** reverses it. The VFOs and the DMR ID database stay as they were.
Afterwards the editor lists what it changed or left out, such as scan lists, encryption keys and names that were
shortened. A qdmr zone with an A and a B list becomes two zones, "Name A" and "Name B". Saving as qdmr YAML lists
what qdmr's format can't hold: SFR settings, power levels qdmr has no name for, and locations, which are rounded to
6-character grid locators (about 5 km). The Tools page converts between qdmr YAML and codeplug files too, and converts an original firmware codeplug to
either.

### DMR ID database and contacts from RadioID.net

The **DMR ID Database** tab builds the database, or private-call contacts, from the users registered at
RadioID.net. It takes the users from either:

- **RadioID User File**: download `user.csv` from RadioID.net and open it. Any region, any size.
- **Fetch from RadioID.net**: fetch the users of a few states or a small country directly. RadioID.net returns
  200 users per request, so this suits up to a few thousand users. Your own RadioID API key is optional. The page
  keeps it in this browser and sends it only to RadioID.net. Until RadioID.net allows requests from the site, they
  pass through a public relay service; the key is never sent through it.

Then choose:

- regions (countries, and states or provinces);
- call sign prefixes and a DMR ID range;
- the text each record shows, built from fields such as call sign, name, city and state;
- the text length (16 to 48 characters).

The page previews the records, and shows the database size against the radio's room. **Write ID Database** puts it
in the codeplug; **Write to Radio** then sends it. **Add as Contacts** adds the users as private-call contacts
instead, skipping IDs that are already contacts. The editor remembers these choices in the browser.

## Programming with qdmr

The radio speaks the protocol of qdmr's open-firmware drivers through a compatibility layer, and stores the codeplug
in the layout qdmr writes. Reads and writes are tested with `tools/cps_client.py`, which talks to the radio the way
qdmr does.

### Reading and writing the codeplug

1. Turn the radio on normally and connect the USB cable. The radio appears as a USB serial port (USB ID 1FC9:0094).
2. In qdmr, read the codeplug from the radio. qdmr identifies the radio by the radio type ODR reports. The
   [Radio reference](23-radios.md) lists the type and the qdmr driver for each radio.
3. Edit the codeplug and write it to the radio.
4. After a write, the radio restarts and uses the new codeplug.

The radio keeps its own screen during the transfer.

## What the radio uses from the codeplug

| Item | Fields ODR uses |
|---|---|
| General settings | Your DMR ID. The callsign (radio name, 8 characters, sent as talker alias). The group and private call hang times (the reply window) |
| Channels (up to 1024) | Name, receive and transmit frequency, mode (FM or DMR), bandwidth, receive and transmit tones, power, receive only, color code, timeslot, talk target contact, RX group list (listen list), a per-channel DMR ID, force direct mode, transmit timeout, talker alias, zone scan skip, VOX allowed, no beep, no battery saving |
| Zones | Name and channel list. Without zones, the radio shows one zone with all channels |
| Contacts (up to 1024) | Name, DMR ID or talkgroup, private or group, timeslot override |
| RX group lists | The talkgroups each channel listens to |
| DMR ID database | Names for DMR IDs that aren't contacts, shown on the receive screen |
| VFO A | Frequency and settings of the VFO, read after the next restart |
| Boot text | The two lines on the boot screen |

The radio keeps every other part of the codeplug unchanged (boot image, APRS settings, DTMF contacts and so on), so
the editor and qdmr read back what they wrote, but ODR doesn't act on those parts.

Changes made on the radio (a channel saved with hold **OK**, a permanent talk target) are written to the same codeplug,
so the next read shows them.

## What the codeplug can't set yet

- Privacy keys ([DMR](08-dmr.md#privacy)).
- ODR's own settings ([Settings](12-settings.md)): they are kept separately from the codeplug. The codeplug's quick
  key assignments are ignored.
- Emergency, ARS and text message format.
- SFR settings, in qdmr. The Codeplug Editor sets them on each channel.

## Coming from stock firmware

On a radio with stock firmware, a migration tool reads the stock codeplug, flashes ODR and writes the codeplug back
in one run:

| Radio | Tool | Chapter |
|---|---|---|
| TYT MD-380, MD-390, RT3 | `tools/md380_migrate.py` | [Moving from stock MD-380 firmware](18-moving-from-stock-md380.md) |
| Baofeng DM-32UV | `tools/dm32_migrate.py` | [Moving from stock DM-32UV firmware](19-moving-from-stock-dm32.md) |

## Radios qdmr can't program directly

qdmr can't write the codeplug of a DM-32UV running ODR. [DM-32UV with qdmr](23-radios.md#dm-32uv-with-qdmr) says why
and how to write a qdmr codeplug to it. The Codeplug Editor programs it over the programming cable (not yet tested
on a radio).

## Backups

Read the codeplug and save it before you change it: **Save > Codeplug File (.bin)** in the editor, or a save in
qdmr. A raw backup works too: `tools/cps_client.py read
backup.bin` and `tools/cps_client.py write backup.bin` over the serial port, or TFTP in USB network mode
([USB network](14-usb-network.md#programming-over-the-network)).

## Limits and notes

- Writes are refused while the radio's settings area holds another firmware's data. See "Settings are not kept" in
  [Troubleshooting](20-troubleshooting.md).
- The calibration area accepts writes only to its own bytes. Don't edit calibration with a CPS.
