<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Programming the radio

The codeplug holds the channels, zones, contacts, RX group lists, the DMR ID database, and your DMR ID. Edit it with
the **Codeplug Editor** on the project's web site, [radiofoundry.org](https://radiofoundry.org/codeplug/index.html).
The editor runs in a Chromium-based browser (Chrome, Edge, Opera), needs nothing installed, and works with every
RadioFoundry radio, over USB or over the programming cable. qdmr, the free codeplug editor, is an alternative.

| Program | Runs | Use it for |
|---|---|---|
| The Codeplug Editor | In the browser | Everything on this page. This is the recommended way |
| [qdmr](https://github.com/hmatuschek/qdmr) | Windows, macOS, Linux | Your existing qdmr codeplugs, and scripting. It needs a radio that qdmr's driver can reach ([qdmr](#programming-with-qdmr)) |
| `tools/cps_client.py` and TFTP | A terminal | Raw backups and scripts ([Backups](#backups)) |

All of them use the same codeplug format and the radio's own codeplug interface, so a codeplug that one writes reads
back in the others. How each radio connects for programming is in the [Radio reference](23-radios.md#programming).

## The Codeplug Editor

Open **Codeplug Editor** in the site's menu. It reads and writes the radio from the browser. Nothing goes to a server:
files, the codeplug, and your RadioID API key stay on your computer.

### Reading and writing the radio

1. Turn the radio on with RadioFoundry running, and connect it: the USB cable on a radio with USB, otherwise the
   programming cable. The radio must be in USB serial mode, not USB network mode.
2. Choose **Read from Radio**. The browser lists the radio's USB port and any programming cable; choose the radio's.
   The editor tells from the port which of the two it is.

   When the radio's DMR ID database would take 20 seconds or more to read (megabytes over the programming cable), the
   editor first says how long and asks whether to read it too. Skip it unless you want to edit the database or keep it
   in a saved file; a write then leaves the radio's database as it is. The **DMR ID Database** tab can read it later.
3. Edit the codeplug.
4. Choose **Write to Radio**. The editor compares the codeplug with the radio's and writes only the bytes that changed.
   It shows how many bytes and about how long the write takes, then writes and verifies them. The radio loads the new
   codeplug without restarting.

After a read, the editor says when a newer RadioFoundry release is out for the radio, and what it adds to reading and
writing (faster transfers, the compact ID database). **Update Firmware** opens the
[web installer's Update](02-installing.md#updating-radiofoundry) in a new tab and releases the radio's port, so that
the installer can connect. The codeplug and settings stay on the radio through the update. If the update changes the
radio's power levels, the power picked in Settings, for the VFOs, and for APRS goes back to the radio's default: check
it before you transmit. A channel's power in the codeplug maps to the nearest new level.

### Programming several radios

To put one codeplug into several radios, write it to each in turn. Before each write, the editor checks which radio is
connected. When you swap the radio on the programming cable, the editor reads the new radio before it writes anything,
so it compares the write with that radio's codeplug, not the last one's.

When the radio's name, DMR ID, and boot text differ from the file's, the write dialog offers **Keep this radio's name,
DMR ID and boot text**. With it on, each radio keeps its own and gets everything else from the file. The editor
remembers the choice for the next radio.

The first write to a radio that came straight from its original firmware might ask you to **Prepare Radio**. The
original firmware's data is still in the area where RadioFoundry keeps its settings, and until that area is erased,
the radio can't store a codeplug or keep its settings when you turn it off. The [web installer](02-installing.md) does
this by itself after the backup it takes when it installs RadioFoundry. The backup holds a copy of that data. The
calibration isn't touched.

A radio whose memory chip is too small for RadioFoundry's settings area can't store a codeplug at all. The editor says
so instead of reading it. The [Radio reference](23-radios.md) lists the models where this can happen.

**Write to Radio** is blocked while the **Problems** tab lists errors, such as a name that's too long or a frequency
that isn't valid. Warnings, such as a frequency outside the radio's bands or a DMR channel without a TX contact, don't
block it.

A browser can't reach a radio in USB network mode. There, choose **Save > For USB Network** and send the file with
TFTP ([USB network](15-usb-network.md#programming-over-the-network)).

### Editing

| Tab | Holds |
|---|---|
| Radio | DMR ID, radio name (callsign), boot text, group and private call hang times, ARS server, IP gateway |
| Channels | Every channel. The pencil button opens the full form: FM, DMR, scan and power, location, single-frequency repeater |
| Zones | Zones and their channels |
| Contacts | Talkgroups and private-call contacts |
| RX Group Lists | The talkgroups that each channel listens to |
| DTMF Contacts, APRS | DTMF contacts and APRS configurations |
| Systems | Trunked systems and the identity of other digital modes: sites, control channels, channel plans, and keys. Shown when the radio reports such a mode, or the codeplug has one ([Digital modes and systems](#digital-modes-and-systems)) |
| DMR ID Database | Names for DMR IDs that aren't contacts, built from RadioID.net ([DMR ID database](#dmr-id-database-and-contacts-from-radioidnet)) |
| Problems | Errors and warnings, each with a link to the record |

In a table, double-click a cell or type on it to edit it in place. Click a column heading to sort, and type in the
filter to narrow the rows. To set one field on several rows, select them and choose **Bulk Edit**. Bulk Edit doesn't
work on Zones or RX Lists, whose members you edit in their own dialog. **Undo** and **Redo** cover every change,
including an import.

**ARS server** is the DMR ID of the presence server that the radio registers with
([ARS](10-messages-and-signaling.md#registration-ars-and-location-lrrp)). **IP gateway** is the DMR ID of the radio
that takes the computer's packets to other networks
([USB network](15-usb-network.md#other-networks-through-a-gateway-radio)). Leave a field empty to keep the radio's own
setting; enter 0 to turn it off. qdmr doesn't have these settings and leaves them as they are on the radio. Other
programming software that writes the radio's whole memory can't erase them either: the radio keeps its own unless the
software writes them in RadioFoundry's format.

The page keeps the working codeplug in the browser, so closing or reloading the page doesn't lose edits. **New** starts
over.

### Digital modes and systems

Besides FM and DMR, a channel can use another digital mode, such as a trunked system. The channel form's **Mode** lists
the modes that the connected radio's firmware reports, and any mode the codeplug already holds. A channel of such a
mode shows that mode's fields under **Mode settings**; a trunked channel takes its frequencies from its system. A radio
whose firmware doesn't run a channel's mode shows the mode's short name, receives nothing on that channel, and doesn't
transmit. The [feature matrix](../feature-matrix.md) lists which builds run which mode.

The **Systems** tab adds a system of a type. One form holds the whole system, in sections:

- the system's identity;
- its control channels: one row for each frequency, tried top to bottom (**+** adds a row, **−** removes it);
- this radio (**Transmit**, **Radio ID on this system**);
- calls, fallback, the key, and wireless sharing.

Sections that few systems need (sites and roaming, remote commands, a fixed channel plan) start closed. Click a heading
to open the section.

Each system has these fields:

| Field | Meaning |
|---|---|
| Transmit | **Never** (listen only): the radio follows calls but never registers or transmits, whatever a channel says. **Yes, after registering**. P25 also has **Yes, without registering**: PTT asks for a voice channel at once |
| Needs its key | The system can't be used without its key. The key's fields are dimmed until you select this |
| Wireless Zone/Channel Sharing | What zone sharing sends of the system: off, its identity only (the default; the receiver must have the system), or the whole system |
| Radio ID on this system | Your ID on the system. Blank uses the mode's own. Never shared. Dimmed while **Transmit** is **Never** |

When the codeplug holds keys, saving a codeplug file asks whether to include them. The file leaves them out unless you
select **Include the keys in this file**. Anyone with a file that holds the keys can read them. CSV and qdmr files hold
neither systems nor channel modes. A channel of another mode goes into such a file as a receive-only DMR channel, and
comes back from it the same way. To keep these channels, keep a codeplug file.

### P25 and NXDN

Pick **P25**, **P25 Trunking**, or **NXDN** in the channel form's **Mode**. To use these channels on the radio, see
[P25 channels](07-channels-zones-vfo.md#p25-channels). The talk group is the channel's **TX contact** (a group contact),
and the RX group list works as on DMR. **Own ID** on a channel overrides the radio's ID for that channel.

**P25 channel:**

| Field | Meaning |
|---|---|
| Squelch | **Normal**: every call on the NAC. **Selective**: the talk group, calls to everyone, and calls to this radio |
| RX NAC | The network access code to receive, 3 hex digits. Blank means 293. F7E receives every NAC |
| TX NAC | The code to send. Blank means the RX NAC (293 when the RX NAC is F7E) |

**NXDN channel:**

| Field | Meaning |
|---|---|
| Rate | 9600 bit/s (12.5 kHz) or 4800 bit/s (6.25 kHz) |
| Squelch | **RAN**: every call on the RAN. **Selective**: the group, and calls to this radio |
| RX RAN | The radio access number to receive, 0-63. 0 receives every RAN |
| TX RAN | The RAN to send. Blank means the RX RAN (1 when the RX RAN is 0) |

**Radio tab**, for each mode:

| Field | Meaning |
|---|---|
| P25 unit ID | Your P25 ID. Blank means the DMR ID |
| NXDN unit ID | Your NXDN ID, 1-65519. Blank means the DMR ID if it's 65519 or less, otherwise 1 |

**P25 Trunking system** (**Systems** tab, type **P25 Trunking**). A new system is listen only: set **Transmit** to
register and transmit.

| Field | Meaning |
|---|---|
| WACN, System ID | The system's identity, in hex. Blank means any |
| Control channel NAC | Blank means any |
| Control channels | RX in MHz, tried top to bottom. TX, identifier, and channel number can stay 0 when the control channel announces them |
| Skip encrypted calls | Don't follow a grant for an encrypted call |
| Hold after a call | Seconds on the voice channel after a call ends. 0 returns to the control channel at once |
| Use only the channel plan entered here (sites and roaming) | Ignore the plan that the control channel sends |
| Change site below (sites and roaming) | Kept in the codeplug. The radio doesn't change site on signal level |

**Sites** and **Channel plan** (for each identifier: base frequency, spacing, TX offset, bandwidth) are optional.
Without a plan entry, the radio uses the plan that the control channel sends.

**P25 trunked channel:** pick the **System** and the talk group (**TX contact**). **Follow every group of the system**
follows calls to every talk group, not only the channel's. **Fallback channel** is kept in the codeplug; the radio
doesn't use it and keeps hunting for a control channel. A trunked channel has no frequencies of its own.

### Files

**Open File** takes these files:

- A codeplug file (`.bin`) saved by the editor, by `tools/cps_client.py read`, or by a TFTP read.
- A backup from the web installer (`.zip`).
- A qdmr codeplug (`.yaml`).
- A channel list saved by CHIRP (`.csv`), or a CHIRP image (`.img`) of a radio that the editor can read.
- CSV files, one table each, or a `.zip` file of them.
- The original firmware's codeplug, for radios whose format the editor reads: in an installer backup, a dump of the
  radio's data flash, or the manufacturer's CPS file. The
  [Radio reference](23-radios.md#codeplug-files-from-the-original-firmware) lists them.

A backup made when RadioFoundry was installed still holds the original firmware's codeplug, because RadioFoundry keeps
its own codeplug in a different layout. The editor recognizes that codeplug and converts it: channels, zones,
contacts, RX group lists, the DMR ID, the radio name, and the boot text. Afterward, it lists what it left out, such as
scan lists and names that it shortened. A zone with an A and a B list becomes two zones, "Name A" and "Name B". Check
the result before you write it to the radio. The radio's codeplug stays as it is until you write it. To use a CPS file
that the editor can't open, convert it with qdmr first (`dmrconf decode`), then open the result as YAML.

**Save** writes one of these:

- a codeplug file;
- a qdmr codeplug (`.yaml`);
- a CHIRP channel list (`.csv`);
- the open table as CSV, or every table as a `.zip` file of CSV files;
- `codeplug.bin` for USB network mode.

Every save except `codeplug.bin` is named after the codeplug with the date and time added, such as
`codeplug-YYYY-MM-DD-HHMM.yaml`, so that saves never overwrite each other. When you open a dated file and save it
again, the save replaces the date and time.

CSV files have a header row with the column names, and refer to other records by name (a channel's TX contact, a
zone's channels). To see the columns, save a table first. An import matches rows to existing records by number, then
by name. A file can hold only some columns; the others keep their values. The import removes records that are missing
from the file. After the import, the editor lists the values it couldn't take, by row and column.

A qdmr file replaces these in one change, so one **Undo** reverses it: the channels, zones, contacts, DTMF contacts,
RX group lists, APRS configurations, and the **Radio** tab's settings other than the ARS server and IP gateway. The
VFOs and the DMR ID database stay as they were. Afterward, the editor lists what it changed or left out, such as scan
lists, encryption keys, and names that it shortened. Saving as qdmr YAML lists what qdmr's format can't hold: SFR
settings, power levels that qdmr has no name for, and locations, which are rounded to 6-character grid locators (about
5 km). The **Tools** page converts between qdmr YAML and codeplug files too, and converts an original firmware codeplug
to either.

A CHIRP CSV file replaces the channels and zones in one change, so one **Undo** reverses it. Contacts, settings, and
the DMR ID database stay as they were. The channels are the file's FM and NFM rows, numbered in file order and put in
zones named after the file, 80 channels to a zone. The import carries over offsets (`+`, `-`, split), CTCSS and DCS
tones (including cross modes), bandwidth, skip, and power. The editor lists what it left out: rows in other modes (AM,
DV, SSB), rows without a usable frequency, tones the radio can't hold, and names it shortened. Saving as CHIRP CSV
writes the FM channels only, because CHIRP has no DMR.

A CHIRP image is one radio's memory, so each radio model needs its own reader, and the editor reads only the models it
lists. It imports their channels the same way as a CSV file. For any other radio, the editor names the model it found
and asks you to export the channels from CHIRP with **File > Export to CSV**.

### DMR ID database and contacts from RadioID.net

The **DMR ID Database** tab builds the database, or private-call contacts, from the users registered at RadioID.net.
It takes the users from one of these sources:

- **RadioID User File**: download `user.csv` from RadioID.net and open it. Any region, any size.
- **Fetch from RadioID.net**: fetch the users of a few states or a small country directly. RadioID.net returns 200
  users for each request, so this suits up to a few thousand users. Your own RadioID API key is optional. The page
  keeps the key in this browser and sends it only to RadioID.net. Until RadioID.net allows requests from the site, the
  requests pass through a public relay service; the key is never sent through it.

Then choose:

- regions (countries, and states or provinces);
- call sign prefixes and a DMR ID range;
- the text that each record shows, built from fields such as call sign, name, city, and state;
- the text length (16 to 48 characters).

The page previews the records and shows the database size against the radio's room. **Write ID Database** puts the
database in the codeplug; **Write to Radio** then sends it. **Add as Contacts** adds the users as private-call contacts
instead, and skips IDs that are already contacts. The editor remembers these choices in the browser.

Keep contacts for what you call: your talkgroups and the few people you make private calls to. The codeplug holds at
most 1024 contacts, and each one is a row in **Contacts** and in the talk target picker. Let the ID database name
everyone else: the radio shows a caller who isn't a contact by the database's name for their ID.

RadioFoundry reads a **compact database**, about half the size of the older format, so the same room holds about twice
the names. After a refresh from RadioID.net, a write sends only the parts that changed. The editor picks the format
when it writes:

| Radio | What the editor does |
|---|---|
| Reads the compact format | Writes it. The editor offers to convert a database in the older format (read from the radio or an old file): one full write, then smaller ones. If you decline, the offer comes back after 12 hours |
| Runs older firmware | Writes the older format. If the names don't all fit, the editor says how many it left out. Updating the firmware makes room |
| Older firmware installed over a compact database | The radio shows no names (never wrong ones). The editor writes the database again in the older format the next time it writes to the radio |

TFTP (USB network mode) and other CPS programs use the older format.

## What the radio uses from the codeplug

| Item | Fields that RadioFoundry uses |
|---|---|
| General settings | Your DMR ID. The callsign (radio name, 8 characters, sent as talker alias). The group and private call hang times (the reply window). The ARS server and IP gateway, when set |
| Channels (up to 1024) | Name, receive and transmit frequency, mode (FM or DMR), bandwidth, receive and transmit tones, power, receive only, color code, timeslot, talk target contact, RX group list (listen list), a per-channel DMR ID, force direct mode, transmit timeout (**Radio Default** uses the radio's own, see [Transmit timeout](08-fm.md#transmit-timeout)), talker alias, zone scan skip, VOX allowed, 1750 Hz tone burst, no beep, no battery saving |
| Zones | Name and channel list. Without zones, the radio shows one zone with all channels |
| Contacts (up to 1024) | Name, DMR ID or talkgroup, private or group, timeslot override |
| RX group lists | The talkgroups that each channel listens to |
| Trunked systems (up to 8) | Every field of the **Systems** tab, and which channels are trunked |
| DMR ID database | Names for DMR IDs that aren't contacts, shown on the receive screen |
| VFO A | Frequency and settings of the VFO, read after the next restart |
| Boot text | The two lines on the boot screen |

The radio keeps every other part of the codeplug unchanged (boot image, DTMF contacts, and so on), so the editor and
qdmr read back what they wrote, but RadioFoundry doesn't act on those parts.

Changes that you make on the radio (a channel saved with hold **OK**, a permanent talk target) go into the same
codeplug, so the next read shows them.

The codeplug doesn't set these:

- Privacy keys ([Privacy](09-dmr.md#privacy)).
- RadioFoundry's own settings ([Settings](17-settings.md)). The radio keeps them apart from the codeplug, and ignores
  the codeplug's quick key assignments.
- The emergency and text message formats.
- SFR settings, in qdmr. The Codeplug Editor sets them on each channel.

## Programming with qdmr

The radio speaks the protocol of qdmr's open-firmware drivers through a compatibility layer, and stores the codeplug
in the layout that qdmr writes.

1. Turn the radio on normally and connect the USB cable. The radio appears as a USB serial port.
2. In qdmr, read the codeplug from the radio. qdmr identifies the radio by the radio type that RadioFoundry reports.
   The [Radio reference](23-radios.md#programming) lists the type and the qdmr driver for each radio.
3. Edit the codeplug and write it to the radio.
4. After a write, the radio restarts and uses the new codeplug.

The radio keeps its own screen during the transfer. A radio whose codeplug sits where qdmr's driver can't write it, or
that has no USB port, needs another route. The [Radio reference](23-radios.md#programming) says which.

## Backups

Read the codeplug and save it before you change it: **Save > Codeplug File (.bin)** in the editor, or a save in qdmr.
A raw backup works too: `tools/cps_client.py read backup.bin` and `tools/cps_client.py write backup.bin` over the
serial port, or TFTP in USB network mode ([USB network](15-usb-network.md#programming-over-the-network)).

To get the codeplugs out of a web installer backup (the `.zip` file) as qdmr YAML, run
`tools/backup_codeplug.py BACKUP.zip -o DIR`. It writes RadioFoundry's codeplug and, where the backup holds it, the
original firmware's codeplug, each as a `.yaml` and a `.dfu` file. Open the `.yaml` file in the Codeplug Editor or in
qdmr. The tool needs Python 3.11 or later and qdmr's `dmrconf` (0.15 or later), and it never touches the radio. Use it
to bring in an original firmware codeplug that the Codeplug Editor can't read, or reads only in part. The
[Radio reference](23-radios.md#codeplug-files-from-the-original-firmware) says which radios need this.

To carry a codeplug over from the original firmware while you install, see
[Moving from the original firmware](18-moving-from-stock.md).

## Limits and notes

- The radio refuses writes while its settings area holds another firmware's data. See
  [Settings are not kept](20-troubleshooting.md#settings-are-not-kept).
- The calibration area accepts writes only to its own bytes. Don't edit calibration with a CPS.
