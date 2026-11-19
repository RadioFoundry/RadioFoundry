<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# First boot

## Turning the radio on

Turn the radio on with its power switch. On the handhelds, this is the volume knob: turn it clockwise past its click.
The [Radio reference](23-radios.md) says where the switch is on each radio. The boot screen shows for about a second,
then the radio opens the Home screen. The radio ignores keys that you press during the boot screen.

The boot screen shows the RadioFoundry logo and the two boot text lines from the codeplug, for example your callsign
and name. Each line holds 16 characters. Along the bottom, the screen shows the radio model and the firmware version.
The version is the release name, such as v26.11 (the year and month it came out), or, for a build between releases, the
commit it was built from.

<img src="../ux/screens/160x120i5/boot.png" width="200" alt="Boot screen">

To turn the radio off, use the power switch again. On the handhelds, turn the volume knob counterclockwise until it
clicks. For details, see [Turning off](16-battery-and-power.md#turning-off).

## Channels on a new installation

RadioFoundry reads the codeplug from the radio's SPI flash, in the layout that the Codeplug Editor and qdmr write. The
codeplug holds the channels, zones, contacts, and your DMR ID. When RadioFoundry finds no channels there, it uses six
built-in FM simplex channels:

| Channel | Frequency | Bandwidth |
|---|---|---|
| Simplex 145.500 | 145.500 MHz | 25 kHz |
| Simplex 146.520 | 146.520 MHz | 25 kHz |
| Simplex 146.550 | 146.550 MHz | 25 kHz |
| Simplex 433.500 | 433.500 MHz | 12.5 kHz |
| Simplex 446.000 | 446.000 MHz | 12.5 kHz |
| Simplex 446.500 | 446.500 MHz | 12.5 kHz |

Before you transmit on these frequencies, check that they're legal for you. **Settings > Advanced > Codeplug** shows
which channels the radio uses: "built-in channels" or "from the CPS".

A radio that ran its original firmware still holds that firmware's codeplug. RadioFoundry checks the codeplug when it
starts. When it isn't a RadioFoundry (open-firmware) codeplug, the radio ignores it and uses the built-in channels.
**Settings > Advanced > Codeplug** then also shows "Other firmware's codeplug: ignored". Nothing is erased: the
Codeplug Editor can still read and convert the old codeplug. To replace it, write a codeplug.

The first time you save something on the radio (a channel, a contact, a zone, your DMR ID), the radio asks first, on
the **Old codeplug** screen:

| Choice | What it does |
|---|---|
| Cancel | Saves nothing. This is the default; **Back** does the same |
| Start new | Starts a new, empty RadioFoundry codeplug and saves your change. This erases the old codeplug's channels, contacts, DMR ID, and radio name. Back it up first |
| Use anyway | Erases nothing and uses the old codeplug as it is. Your change isn't saved: make it again |

The radio remembers **Use anyway** until a codeplug is written to the radio or you do a factory reset.

## Programming the radio

Program channels, zones, contacts, talkgroup lists, and your DMR ID with the Codeplug Editor on the project's web site
([Programming the radio](04-programming.md)). qdmr is an alternative. You can also change most settings on the radio
([Settings](17-settings.md)). Channels, contacts, and the DMR ID come from the codeplug.

DMR needs your DMR ID. Without one, the radio refuses to transmit on DMR channels and shows "No DMR ID in codeplug".
**Settings > My radio > DMR ID and name** shows the radio's ID, and sets one on the radio, which writes it into the
codeplug.

## Checks after installing

1. Open **Settings > My radio > About** (also under **Advanced**). It shows "RadioFoundry" with the version you
   installed and the radio model. Below them, it lists the authors and the licenses of everything in the firmware.
2. On an FM channel, hold **Monitor** to open the squelch. You hear noise, which shows that the receiver and the audio
   path work.
3. On a DMR channel, the Home screen shows "No codec: voice off" in its bottom line if the codec is missing. For the
   fix, see [No codec](20-troubleshooting.md#no-codec-no-codec-voice-off).
4. Change a setting, for example **Settings > Display > Brightness**. Turn the radio off and on again, and check that
   the setting stayed. If it didn't, see [Settings are not kept](20-troubleshooting.md#settings-are-not-kept).
