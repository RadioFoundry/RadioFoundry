<!-- SPDX-License-Identifier: MIT -->

# Programming the Radio

The codeplug holds the channels, zones, contacts, RX group lists, the DMR ID database and your DMR ID. Edit it with the
**Codeplug Editor** on the project's web site, [radiofoundry.org](https://radiofoundry.org/codeplug/index.html). It runs
in a Chromium-based browser (Chrome, Edge, Opera), needs nothing installed, and works with every RadioFoundry radio,
over USB or over the programming cable. qdmr, the free codeplug editor, is an alternative.

| Program | Runs | Use it for |
|---|---|---|
| The Codeplug Editor | In the browser | Everything on this page. The recommended way |
| [qdmr](https://github.com/hmatuschek/qdmr) | Windows, macOS, Linux | Your existing qdmr codeplugs, and scripting. Needs a radio qdmr's driver can reach ([qdmr](#programming-with-qdmr)) |
| `tools/cps_client.py` and TFTP | A terminal | Raw backups and scripts ([Backups](#backups)) |

All of them use the same codeplug format and the radio's own codeplug interface, so a codeplug written by one reads
back in the others. How each radio is wired for this is in the [Radio reference](23-radios.md#programming).

## The Codeplug Editor

Open **Codeplug Editor** in the site's menu. It reads and writes the radio from the browser. Nothing is sent to a
server: files, the codeplug and your RadioID API key stay on your computer.

### Reading and writing the radio

1. Turn the radio on with RadioFoundry running and connect it: the USB cable on a radio with USB, otherwise the
   programming cable. The radio must be in USB serial mode, not USB network mode.
2. Choose **Read from Radio**. The browser lists the radio's USB port and any programming cable; choose the radio's.
   The editor tells from the port which of the two it is.
   When the radio's DMR ID database would take 20 seconds or more to read (megabytes over the programming cable), the
   editor first says how long and asks whether to read it too. Skip it unless you want
   to edit the database or keep it in a saved file: a write then leaves the radio's database as it is. The
   **DMR ID Database** tab can read it later, or remove it without reading it.
3. Edit the codeplug.
4. Choose **Write to Radio**. The editor compares the codeplug with the radio's and writes only the bytes that
   changed. It shows how many bytes and about how long it takes, then writes and verifies them. The radio loads the
   new codeplug without restarting.

After a read, the editor says when a newer RadioFoundry release is out for the radio, and what it adds to reading and
writing (faster transfers, the compact ID database). **Update Firmware** opens the [web installer's Update](02-installing.md#updating-radiofoundry)
in a new tab and lets go of the radio's port, so the installer can connect. The codeplug and settings stay on the radio
through the update. If the update changes the radio's power levels, the power picked in Settings, for the VFOs and for
APRS goes back to the radio's default: check it before you transmit. A channel's power in the codeplug maps to the
nearest new level.

### Programming several radios

Opening a saved file or draft sends its valid settings area, including its channel modes and trunking systems.
After reading from a radio, edits on that connection merge only the settings you changed, preserving settings
changed in the radio's menus. A file without a valid settings area leaves the radio's existing area intact.

To put one codeplug into several radios, write it to each in turn. Before each write the editor checks which radio is
connected. A radio swapped on the programming cable is read again before anything is written, so the write is
compared with that radio's codeplug, not the last one's.

The write dialog offers **Keep this radio's name, DMR ID and boot text** when the radio's differ from the file's. With
it on, each radio keeps its own and gets everything else from the file. The editor remembers the choice for the next
radio.

The first write to a radio that came straight from its original firmware may ask to **Prepare Radio**. The original
firmware's data is still in the area where RadioFoundry keeps its settings, and until that area is erased the radio
can't store a codeplug or keep its settings when turned off. The [web installer](02-installing.md) does this by itself
after the backup it takes when it installs RadioFoundry. The backup holds a copy of that data. The calibration isn't
touched.

A radio whose memory chip is too small for RadioFoundry's settings area can't store a codeplug at all. The editor
says so instead of reading it ([radios](23-radios.md) lists the models where this can happen).

Write to Radio is blocked while the **Problems** tab lists errors, such as a name that is too long or a frequency that
isn't valid. Warnings, such as a frequency outside the radio's bands or a DMR channel without a TX contact, don't
block it.

A browser can't reach a radio in USB network mode. There, choose **Save > For USB Network** and send the file with
TFTP ([USB network](15-usb-network.md#programming-over-the-network)).

### Editing

| Tab | Holds |
|---|---|
| Radio | DMR ID, radio name (callsign), boot text, group and private call hang times, ARS server, IP gateway |
| Channels | Every channel. The pencil button opens the full form: FM, DMR, scan and power, location, single frequency repeater |
| Zones | Zones and their channels |
| Contacts | Talkgroups and private-call contacts |
| RX Group Lists | The talkgroups each channel listens to |
| DTMF Contacts, APRS | DTMF contacts and APRS configurations |
| Systems | Trunked systems and the identity of other digital modes: sites, control channels, channel plans and keys. Shown when the radio reports such a mode, or the codeplug has one ([below](#digital-modes-and-systems)) |
| DMR ID Database | Names for DMR IDs that aren't contacts, built from RadioID.net ([below](#dmr-id-database-and-contacts-from-radioidnet)) |
| Problems | Errors and warnings, each with a link to the record |

In a table, double-click a cell or type on it to edit it in place. The RX Tone and TX Tone fields list the CTCSS tones
and DCS codes. Typing narrows the list, and the arrow keys and Enter pick a tone. A tone missing from the list, such as
69.4, is taken as typed, and a short DCS code such as D23 becomes D023N. Click a column heading to sort, and type in the
filter to narrow the rows. Check rows and choose **Bulk Edit** to set one field on all of them (not on Zones or RX Lists, whose members are
edited in their own dialog). **Undo** and **Redo**
cover every change, including an import.

**ARS server** is the DMR ID of the presence server the radio registers with
([ARS](10-messages-and-signaling.md#registration-ars-and-location-lrrp)). **IP gateway** is the DMR ID of the radio
that takes the computer's packets to other networks ([USB network](15-usb-network.md#other-networks-through-a-gateway-radio)).
Leave a field empty and the radio keeps its own setting; enter 0 to turn it off. qdmr doesn't have these settings and
leaves them as they are on the radio. Other programming software that writes the radio's whole memory can't erase them
either: the radio keeps its own unless the software writes them in RadioFoundry's format.

The page keeps the working codeplug in the browser, so closing or reloading the page doesn't lose edits. **New** starts
over.

### Digital modes and systems

Besides FM and DMR, a channel can be of another digital mode, such as a trunked system. The channel form's **Mode**
lists the modes the connected radio's firmware reports, and any mode the codeplug already holds. A channel of such a
mode shows that mode's fields under **Mode settings**; a trunked channel takes its frequencies from its system. A radio
whose firmware doesn't run a channel's mode shows the mode's short name, receives nothing on it and doesn't transmit.
The [feature matrix](../feature-matrix.md) lists which builds run which mode.

The **Systems** tab adds a system of a type. One form holds all of it, in sections: the system's identity, its
control channels (a row per frequency, **+** to add one, **−** to remove it, tried top to bottom), this radio
(**Transmit**, **Radio ID on this system**), calls, fallback, the key and wireless sharing. Sections few systems need (sites and
roaming, remote commands, a fixed channel plan, sites, the channel plan) start closed: click the heading to open one.
Per system:

| Field | Meaning |
|---|---|
| Transmit | Never (listen only): the radio follows calls but never registers or transmits, whatever a channel says. Yes, after registering. P25 also has Yes, without registering: PTT asks for a voice channel at once |
| Needs its key | The system can't be used without its key. The key's fields are grayed out until this is ticked |
| Wireless Zone/Channel Sharing | What zone sharing sends of it: off, its identity only (the default; the receiver must have the system), or the whole system |
| Radio ID on this system | Your ID there; blank uses the mode's own. Never shared. Grayed out while Transmit is Never |

When the codeplug holds keys, **Save** as a codeplug file asks whether to include them;
they are left out unless you tick **Include the keys in this file**. Anyone with a file that holds them can read them.
A qdmr file holds systems and channel modes in a block of its own that qdmr ignores (below), but never keys. CSV files
hold neither: a channel of another mode goes into one as a receive-only DMR channel, and comes back from one as that.
Keep a codeplug file for them.

### P25 and NXDN

Pick **P25**, **P25 Trunking** or **NXDN** in the channel form's **Mode**. Using them on the radio:
[Channels, zones and the VFO](07-channels-zones-vfo.md#p25-channels). The talk group is the channel's **TX contact**
(a group contact) and the RX group list works as on DMR. **Own ID** on a channel overrides the radio's ID for that
channel.

**P25 channel:**

| Field | Meaning |
|---|---|
| Squelch | Normal: every call on the NAC. Selective: the talk group, calls to everyone and calls to this radio |
| RX NAC | Network access code heard, 3 hex digits. Blank: 293. F7E receives every NAC |
| TX NAC | Code sent. Blank: the RX NAC (293 when the RX NAC is F7E) |

**NXDN channel:**

| Field | Meaning |
|---|---|
| Rate | 9600 bit/s (12.5 kHz) or 4800 bit/s (6.25 kHz) |
| Squelch | RAN: every call on the RAN. Selective: the group and calls to this radio |
| RX RAN | Radio access number heard, 0-63. 0 receives every RAN |
| TX RAN | RAN sent. Blank: the RX RAN (1 when the RX RAN is 0) |

**Radio tab**, per mode:

| Field | Meaning |
|---|---|
| P25 unit ID | Your P25 ID. Blank: the DMR ID |
| NXDN unit ID | Your NXDN ID, 1-65519. Blank: the DMR ID if it is 65519 or less, else 1 |

#### NXDN Scrambler

The NXDN Scrambler is a 15-bit algorithm with 32,767 possible keys. The NXDN standard defines it for voice and
data calls, but RadioFoundry currently supports it for conventional voice calls only. It deters casual listening,
but does not provide dependable security, and the receiver cannot verify who sent a scrambled call. Use scrambling
only in licensed services that permit it. Most countries prohibit using codes or ciphers that obscure the meaning of amateur
radio messages ([Encryption on amateur frequencies](#encryption-on-amateur-frequencies)). The Amateur firmware variant
disables the NXDN Scrambler.

To use the NXDN scrambler:

1. In **Systems**, choose **Add System** and set the type to **NXDN Conventional**. Leave **Radio ID on this system**
   blank to use the **NXDN unit ID** from the **Radio** tab on channels without their own **Own ID**.
2. Under **Traffic keys**, select **Add key**. Enter a **Key ID** from 0 to 63 and a **Scrambler seed** from
   1 to 32767. Use each key ID once per system. Radios exchanging scrambled calls need the same key ID and seed.
3. Edit an NXDN channel, select the **System**, and choose a **Transmit key**. Choose **Clear** to transmit without
   scrambling while still receiving scrambled calls with matching system keys. Save the channel and write the
   codeplug to the radio.

The radio shows **ENC** when it plays scrambled voice. If a scrambled call has no matching receive key, the radio
shows **Encrypted: not played**. If a channel's selected transmit key is missing, pressing PTT on that channel
does not transmit.

**Show scrambler seeds** reveals the stored values. File exports omit the keys by default. Zone shares omit them
as well.

#### Encryption on amateur frequencies

Most countries don't allow messages encoded to obscure their meaning on amateur frequencies. In the US the rule is
47 CFR 97.113(a)(4). Encryption and scrambling belong in licensed services that permit them.

The Codeplug Editor lists a warning under **Problems** for a channel that transmits with an AES key or an NXDN
scrambler key on a frequency from 144 to 148 MHz, 222 to 225 MHz or 420 to 450 MHz, the edges included. These ranges are the
common amateur 2 m, 1.25 m and 70 cm allocations. A receive-only channel gets no warning. Basic privacy keys are set on the
radio, not in the codeplug, so the editor can't check them.

The warning blocks nothing. The codeplug still saves and writes, and the radio doesn't refuse to transmit. Receiving
encrypted calls is not affected. Each operator is responsible for following the terms of their license.

**P25 Trunking system** (Systems tab, type P25 Trunking). A new system is listen only: set **Transmit** to register and
transmit.

| Field | Meaning |
|---|---|
| WACN, System ID | The system's identity, hex. Blank: any |
| Control channel NAC | Blank: any |
| Control channels | RX in MHz, tried top to bottom. TX, identifier and channel number can stay 0 when the control channel announces them |
| Skip encrypted calls | Don't follow a grant for an encrypted call |
| Hold after a call | Seconds on the voice channel after a call ends. 0: back to the control channel at once |
| Use only the channel plan entered here (sites and roaming) | Ignore the plan the control channel sends |
| Change site below (sites and roaming) | Kept in the codeplug; the radio doesn't change site on signal level yet |

**Sites** and **Channel plan** (per identifier: base frequency, spacing, TX offset, bandwidth) are optional. Without a
plan entry the radio uses the plan the control channel sends.

**P25 trunked channel:** pick the **System** and the talk group (**TX contact**). **Follow every group of the system**
follows calls to every talk group, not just the channel's. **Fallback channel** is kept in the codeplug; the radio
doesn't use it yet and keeps hunting for a control channel. A trunked channel has no frequencies of its own.

### Files

**Open File** takes:

- a codeplug file (`.bin`) saved by the editor, `tools/cps_client.py read` or a TFTP read;
- a backup from the web installer (`.zip`);
- a qdmr codeplug (`.yaml`);
- a channel list saved by CHIRP (`.csv`), or a CHIRP image (`.img`) of a radio the editor can read;
- CSV files, one table each, or a `.zip` of them;
- the original firmware's codeplug, where the editor reads that radio's format: in an installer backup, a dump of the
  radio's data flash, or the manufacturer's CPS file. The [Radio reference](23-radios.md#codeplug-files-from-the-original-firmware)
  lists them.

A backup made when RadioFoundry was installed still holds the original firmware's codeplug, because RadioFoundry keeps
its own codeplug in a different layout. The editor recognizes that codeplug and converts it: channels, zones,
contacts, RX group lists, the DMR ID, the radio name and the boot text. Afterwards it lists what it left out, such as
scan lists and names that were shortened. A zone with an A and a B list becomes two zones, "Name A" and "Name B".
Check the result before writing it to the radio. The radio's codeplug stays as it is until you write it. A CPS file
the editor can't open yet can be converted with qdmr first (`dmrconf decode`), then opened as YAML.

**Save** writes a codeplug file, a qdmr codeplug (`.yaml`), a CHIRP channel list (`.csv`), the open table as CSV, every table as a `.zip` of CSV files,
or `codeplug.bin` for USB network mode. Every save but `codeplug.bin` is named after the codeplug with the date and time
added, such as `codeplug-2026-11-10-2215.yaml`, so saves never overwrite each other. Opening a dated file and
saving it again replaces the date and time.

CSV files have a header row with the column names, and refer to other records by name (a channel's TX contact, a
zone's channels). Save a table first to see the columns. An import matches rows to existing records by number, then by
name. A file can hold only some columns; the others keep their values. Records missing from the file are removed.
Values the editor can't take are listed by row and column after the import. A channel's TX timeout rounds up to the
next 15 s step, as the radio stores it (100 s becomes 105 s).

A qdmr file replaces the channels, zones, contacts, DTMF contacts, RX group lists, APRS configurations and the Radio
tab's settings other than the ARS server and IP gateway in one change, so one **Undo** reverses it. The VFOs and the DMR ID database stay as they were.
Afterwards the editor lists what it changed or left out, such as scan lists, encryption keys and names that were
shortened. Saving as qdmr YAML lists what qdmr's format can't hold: SFR settings, power levels qdmr has no name for,
and locations, which are rounded to 6-character grid locators (about 5 km). The Tools page converts between qdmr YAML
and codeplug files too, and converts an original firmware codeplug to either.

Systems, channel modes (Tier III, P25, NXDN) and **Accept Invalid CRC** go into the qdmr file in a `radioFoundry`
block. qdmr opens the file and ignores the block, and never writes one: save from the Codeplug Editor to keep the block.
Opening a file with the block replaces the codeplug's systems too. Opening a file without one, such as any file qdmr
wrote, keeps the systems. Keys are never saved in the file: when the file is opened again in a codeplug that has the
same system, the system keeps its key; anywhere else, enter the key again on the **Systems** tab. The block's keys are
in the [qdmr YAML spec](../spec/qdmr-yaml.md#37-the-radiofoundry-block).

### DMR ID database and contacts from RadioID.net

The **DMR ID Database** tab builds the database, or private-call contacts, from the users registered at RadioID.net.
It takes the users from either:

- **RadioID User File**: download `user.csv` from RadioID.net and open it. Any region, any size.
- **Fetch from RadioID.net**: fetch the users of a few states or a small country directly. RadioID.net returns 200
  users per request, so this suits up to a few thousand users. Your own RadioID API key is optional. The page keeps it
  in this browser and sends it only to RadioID.net. Until RadioID.net allows requests from the site, they pass through
  a public relay service; the key is never sent through it.

Then choose:

- regions (countries, and states or provinces);
- call sign prefixes and a DMR ID range;
- the text each record shows, built from fields such as call sign, name, city and state;
- the text length (16 to 48 characters).

The page previews the records, and shows the database size against the radio's room. **Write ID Database** puts it in
the codeplug; **Write to Radio** then sends it. **Add as Contacts** adds the users as private-call contacts instead,
skipping IDs that are already contacts. The editor remembers these choices in the browser.

A database too big for the connected radio is never sent. That happens when it was built with no radio chosen, or comes
from a file made for a bigger radio. **Write to Radio** then offers **Cut to Fit**: the names with the highest IDs are
left out, and the editor says how many. **Remove ID Database** clears the database, also one the read skipped or one
the editor can't read. **Write to Radio** then erases it on the radio, and a new one can be written.

Keep contacts for what you call: your talkgroups and the few people you make private calls to. The codeplug holds at
most 1024 contacts, and every one is a row in Contacts and the talk target picker. Let the ID database name everyone
else: a caller who isn't a contact is shown by the database's name for their ID.

Current firmware reads a **compact database**, about half the size, so the same room holds about twice the names.
After a refresh from RadioID.net, a write sends only the parts that changed. The editor picks the format when it
writes:

| Radio | What the editor does |
|---|---|
| Reads the compact format | Writes it. A database in the older format (read from the radio or an old file) is offered for conversion: one full write, then smaller ones. Declined, the offer comes back after 12 hours |
| Runs older firmware | Writes the older format. If the names don't all fit, it says how many are left out. Updating the firmware makes room |
| Older firmware flashed over a compact database | The radio shows no names (never wrong ones). The editor writes the database again in the older format the next time it writes to the radio |

TFTP (USB network mode) and other CPS programs use the older format.

## What the radio uses from the codeplug

| Item | Fields RadioFoundry uses |
|---|---|
| General settings | Your DMR ID. The callsign (radio name, 8 characters, sent as talker alias). The group and private call hang times (the reply window). The ARS server and IP gateway, when set |
| Channels (up to 1024) | Name, receive and transmit frequency, mode (FM or DMR), bandwidth, receive and transmit tones, power, receive only, color code, timeslot, talk target contact, RX group list (listen list), a per-channel DMR ID, force direct mode, transmit timeout (Radio Default: the radio's own, [TX timeout](08-fm.md#transmit-timeout)), talker alias, zone scan skip, VOX allowed, 1750 Hz tone burst, no beep, no battery saving |
| Zones | Name and channel list. Without zones, the radio shows one zone with all channels |
| Contacts (up to 1024) | Name, DMR ID or talkgroup, private or group, timeslot override |
| RX group lists | The talkgroups each channel listens to |
| Trunked systems (up to 8) | Every field of the **Systems** tab, and which channels are trunked |
| DMR ID database | Names for DMR IDs that aren't contacts, shown on the receive screen |
| VFO A | Frequency and settings of the VFO, read after the next restart |
| Boot text | The two lines on the boot screen |

The radio keeps every other part of the codeplug unchanged (boot image, DTMF contacts and so on), so the editor and
qdmr read back what they wrote, but RadioFoundry doesn't act on those parts.

Changes made on the radio (a channel saved with hold **OK**, a permanent talk target) are written to the same
codeplug, so the next read shows them.

The codeplug can't set these yet:

- Privacy keys ([DMR](09-dmr.md#privacy)).
- RadioFoundry's own settings ([Settings](17-settings.md)): they are kept separately from the codeplug. The codeplug's
  quick key assignments are ignored.
- Emergency and text message format.
- SFR settings, in qdmr. The Codeplug Editor sets them on each channel.

## Programming with qdmr

The radio speaks the protocol of qdmr's open-firmware drivers through a compatibility layer, and stores the codeplug
in the layout qdmr writes.

1. Turn the radio on normally and connect the USB cable. The radio appears as a USB serial port.
2. In qdmr, read the codeplug from the radio. qdmr identifies the radio by the radio type RadioFoundry reports. The
   [Radio reference](23-radios.md#programming) lists the type and the qdmr driver for each radio.
3. Edit the codeplug and write it to the radio.
4. After a write, the radio restarts and uses the new codeplug.

The radio keeps its own screen during the transfer. A radio whose codeplug sits where qdmr's driver can't write it, or
that has no USB port, needs another route. The [Radio reference](23-radios.md#programming) says which.

## Backups

Read the codeplug and save it before you change it: **Save > Codeplug File (.bin)** in the editor, or a save in qdmr.
A raw backup works too: `tools/cps_client.py read backup.bin` and `tools/cps_client.py write backup.bin` over the
serial port, or TFTP in USB network mode ([USB network](15-usb-network.md#programming-over-the-network)).

To get the codeplugs out of a web installer backup (the `.zip`) as qdmr YAML, run
`tools/backup_codeplug.py BACKUP.zip -o DIR`. It writes RadioFoundry's codeplug and, where the backup holds it, the
original (stock) one, each as a `.yaml` and a `.dfu`. Open the `.yaml` in the Codeplug Editor or in qdmr. It needs
Python 3.11 or newer and qdmr's `dmrconf` (0.15 or newer), and it never touches the radio. This is how a stock
codeplug the Codeplug Editor can't read, or only part of, gets into it ([Radio reference](23-radios.md#codeplug-files-from-the-original-firmware) says which radios).

To carry a stock codeplug over while you install, see [Moving from the stock firmware](README.md#moving-from-the-stock-firmware).

## Limits and notes

- Writes are refused while the radio's settings area holds another firmware's data. See "Settings are not kept" in
  [Troubleshooting](20-troubleshooting.md#settings-are-not-kept).
- The calibration area accepts writes only to its own bytes. Don't edit calibration with a CPS.

A CHIRP CSV file replaces the channels and zones in one change, so one **Undo** reverses it; contacts, settings and the
DMR ID database stay as they were. The channels are the file's FM and NFM rows, numbered in file order and put in zones
named after the file, 80 channels to a zone. Offsets (`+`, `-`, split), CTCSS and DCS tones (including cross modes),
bandwidth, skip and power are carried over. The editor lists what it left out: rows in other modes (AM, DV, SSB),
rows without a usable frequency, tones the radio can't hold, and names it shortened. Saving as CHIRP CSV writes the FM
channels only, since CHIRP has no DMR.

A CHIRP image is one radio's memory, so each radio model needs its own reader and the editor reads only the models it
lists ([CHIRP images the editor reads](../spec/hardware-uv5r-chirp-image.md)). It imports those channels the same
way as a CSV file. For any other radio the editor names the model it found
and says to export the channels from CHIRP with File > Export to CSV.
