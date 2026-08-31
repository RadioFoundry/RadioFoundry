<!-- SPDX-License-Identifier: MIT -->

# Battery and power

## Battery level

The status line shows the battery as an icon and a percentage. The percentage turns to the warning color at 15 % and
below. **OK**, **5** (Tools), **6** (Battery) shows the level ("charge soon" at 15 % and below). The radio reads the
battery once a second, and not while transmitting or for 3 s after, because the voltage sags under transmit.

The battery percentage scale hasn't been checked against a meter yet.

## Low and critical battery

| Level | DM-1701 threshold | What the radio does |
|---|---|---|
| Low | below 6.2 V | Low battery beep (800 Hz, then 600 Hz), again every 2 minutes. The backlight is limited to 30 % |
| Critical | below 6.0 V | Critical beep (600 Hz three times), again every 30 s. Every transmission is refused |

Three readings in a row below a threshold change the level. It goes back up when the battery is 0.2 V above the
threshold.

## Transmit power

The DM-1701 has nine power levels: 50 mW, 250 mW, 500 mW, 750 mW, 1 W, 2 W, 3 W, 4 W, 5 W. Each channel's power comes
from the codeplug, or the radio's default of 1 W.

- **\***, **1** cycles through three levels: 50 mW, 1 W and 5 W.
- The top side key on Home steps through all nine levels.
- Both are temporary; hold **OK** to save the level to the channel.

Power levels use the radio's factory calibration. Use the lowest power that reaches the other station.

## Backlight

**Settings > Display > Brightness** sets the backlight level (also **\***, **9**). **Settings > Display > Backlight time**
sets when it dims (to a fifth of the brightness) and when it goes off (four times as long). Any key brings it back.

## Battery saver

**Settings > Power > Battery saver** puts the receiver to sleep on an idle FM channel. After 10 s without activity the
receiver sleeps and wakes for 100 ms at a time to listen. Any signal in a listening window wakes it fully. A key
press, monitor, scan, VOX, a DMR channel, hotspot mode and channels the codeplug marks "no battery saving" stop it.
Off by default: it hasn't been tested on the radio yet.

## Turning off

Turn the volume knob counterclockwise until it clicks. The radio finishes what it is doing (an ARS deregistration, at
most 3 s), saves its settings and switches off. A deeply discharged battery looks to the radio like the switch is off,
and it shuts down the same way.
