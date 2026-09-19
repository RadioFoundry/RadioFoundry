<!-- SPDX-License-Identifier: MIT -->

# Programming with qdmr

ODR is programmed with qdmr, the free codeplug editor (github.com/hmatuschek/qdmr). The radio speaks the protocol of
qdmr's open-firmware drivers through a compatibility layer, and stores the codeplug in the layout qdmr writes. The
layer is built and passes the host tests with a client that does what qdmr does; it hasn't been tried with qdmr on
the radio yet.

## Reading and writing the codeplug

1. Turn the radio on normally and connect the USB cable. The radio appears as a USB serial port (USB ID 1FC9:0094).
2. In qdmr, read the codeplug from the radio. qdmr identifies the DM-1701 by the radio type ODR reports and uses its
   open-firmware driver for the DM-1701 (the one for the UV380 family).
3. Edit the codeplug and write it to the radio.
4. After a write, the radio restarts and uses the new codeplug.

The radio keeps its own screen during the transfer.

## What the radio uses from the codeplug

| Item | Fields ODR uses |
|---|---|
| General settings | Your DMR ID; the callsign (radio name, 8 characters, sent as talker alias); the group and private call hang times (the reply window) |
| Channels (up to 1024) | Name, receive and transmit frequency, mode (FM or DMR), bandwidth, receive and transmit tones, power, receive only, color code, timeslot, talk target contact, RX group list (listen list), a per-channel DMR ID, force direct mode, transmit timeout, talker alias, zone scan skip, VOX allowed, no beep, no battery saving |
| Zones | Name and channel list. Without zones, the radio shows one zone with all channels |
| Contacts (up to 1024) | Name, DMR ID or talkgroup, private or group, timeslot override |
| RX group lists | The talkgroups each channel listens to |
| DMR ID database | Names for DMR IDs that aren't contacts, shown on the receive screen |
| VFO A | Frequency and settings of the VFO, read after the next restart |
| Boot text | The two lines on the boot screen |

The radio keeps every other part of the codeplug unchanged (boot image, APRS settings, DTMF contacts and so on), so
qdmr reads back what it wrote, but ODR doesn't act on those parts.

Changes made on the radio (a channel saved with hold **OK**, a permanent talk target) are written to the same codeplug,
so qdmr reads them.

## What qdmr can't set yet

- Privacy keys ([DMR](08-dmr.md#privacy)).
- ODR's own settings ([Settings](12-settings.md)): they are kept separately from the codeplug. The codeplug's quick
  key assignments are ignored.
- Emergency, ARS, text message format and SFR settings.

## Coming from an MD-380 with stock firmware

On a TYT MD-380, MD-390 or RT3, `tools/md380_migrate.py` reads the stock codeplug, flashes ODR and writes the
codeplug back, all in one run. See [Moving from stock or md380tools firmware](21-moving-from-stock-md380.md).

## Backups

Read the codeplug with qdmr and save it before you change it. Developers can also take a raw backup with
`tools/cps_client.py read backup.bin` and write it back with `tools/cps_client.py write backup.bin`.

## Limits and notes

- Writes are refused while the radio's settings area holds another firmware's data. See "Settings are not kept" in
  [Troubleshooting](17-troubleshooting.md).
- The calibration area accepts writes only to its own bytes. Don't edit calibration with a CPS.
- The DM-32UV reports a radio type qdmr doesn't know yet.
