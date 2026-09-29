<!-- SPDX-License-Identifier: MIT -->

# Battery and power

## Battery level

The status line shows the battery as an icon and a percentage. The percentage turns to the warning color at the low battery
warning level (**Settings > Power > Low battery warning**, 15 % by default) and below. **OK**, **5** (Tools), **6**
(Battery) shows the level ("charge soon" at that level and below). The radio reads the
battery once a second, and not while transmitting or for 3 s after, because the voltage sags under transmit.

The battery percentage scale hasn't been checked against a meter yet.

## Low and critical battery

At the low battery warning level the radio shows the banner "Battery 15%: charge soon" (with the actual percentage)
and plays the low battery beep. It repeats every 5 minutes while the battery stays at or below that level, and stops
once the battery is 3 % above it (charged). The radio's own voltage thresholds below warn as well, whichever comes
first.

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
Off by default. Verified on the DM-1701: it cycles when idle, and monitor and an incoming call wake it.

## Auto power off

**Settings > Power > Auto power off** (Off, 30 min, 1 h, 2 h; Off by default) switches the radio off after that long
without use. A key press, a call received or sent, a scan and hotspot mode count as use. One minute before, the
radio beeps and shows "Switching off in 1 min / Any key keeps it on". Any key cancels it.

The DM-1701's power switch is on the volume knob. Whether the radio can cut its own power while the knob is on hasn't
been checked on the radio yet. If it can't, the radio comes back up after 2 s and shows "Couldn't switch off / Turn
the knob off", and keeps running as before.

## Turning off

Turn the volume knob counterclockwise until it clicks. The radio finishes what it is doing (an ARS deregistration, at
most 3 s), saves its settings and switches off. A deeply discharged battery looks to the radio like the switch is off,
and it shuts down the same way.
