<!-- SPDX-License-Identifier: MIT -->

# First boot

## Turning the radio on

Turn the volume knob clockwise past its click. The boot screen shows for about a second, then the radio opens the
Home screen. Keys pressed during the boot screen are ignored.

The boot screen shows the ODR logo, the two boot text lines from the codeplug (16 characters each, set with qdmr; for
example your callsign and name), and along the bottom the radio model and the firmware version.

<img src="../ux/screens/160x120i5/boot.png" width="200" alt="Boot screen">

To turn the radio off, turn the volume knob counterclockwise until it clicks. The radio saves its settings before it
switches off.

## Channels on a new installation

ODR reads the codeplug (channels, zones, contacts, your DMR ID) from the radio's SPI flash, in the layout qdmr writes.
When it finds no channels there, it uses six built-in FM simplex channels:

| Channel | Frequency | Bandwidth |
|---|---|---|
| Simplex 145.500 | 145.500 MHz | 25 kHz |
| Simplex 146.520 | 146.520 MHz | 25 kHz |
| Simplex 146.550 | 146.550 MHz | 25 kHz |
| Simplex 433.500 | 433.500 MHz | 12.5 kHz |
| Simplex 446.000 | 446.000 MHz | 12.5 kHz |
| Simplex 446.500 | 446.500 MHz | 12.5 kHz |

Check that these frequencies are legal for you before you transmit on them. **Settings > Advanced > Codeplug** shows
which channels the radio uses: "built-in channels" or "from the CPS".

A radio that ran the stock firmware has stock data in the SPI flash, which ODR may read as channels with unreadable
names. Write a codeplug right away in that case.

## Programming the radio

Program channels, zones, contacts, talkgroup lists and your DMR ID with qdmr on a computer. See
[Programming with qdmr](15-programming-qdmr.md). Most settings can also be changed on the radio
([Settings](12-settings.md)), but channels, contacts and the DMR ID come from the codeplug.

DMR needs your DMR ID. Without one, the radio refuses to transmit on DMR channels and shows "No DMR ID in codeplug".
**Settings > My radio > DMR ID and name** shows the ID the radio has.

## Checks after installing

1. **Settings > Advanced > Firmware** shows "OpenDigitalRadio", the radio model and the version you built.
2. On an FM channel, open the squelch with **SK1** (hold it). You hear noise: the receiver and the audio path work.
3. On a DMR channel, the Home screen shows "No codec: voice off" in its bottom line if the codec isn't merged. See
   [Installing, step 2](02-installing.md#step-2-merge-the-codec).
4. Change a setting, for example **Settings > Display > Brightness**, turn the radio off and on again, and check that
   the setting stayed. If it didn't, see "Settings are not kept" in [Troubleshooting](17-troubleshooting.md).
