<!-- SPDX-License-Identifier: MIT -->

# Battery and Power

The battery, power level and power switch values depend on the radio. The [Radio reference](23-radios.md) has them
for each radio. A radio without a battery (a mobile on a fixed supply) has none of the battery items in this chapter:
no battery icon or percentage, no Tools > Battery page, no Battery saver or Low battery warning setting, and no
battery warnings.

## Battery level

The status line shows the battery as an icon and a percentage. On a small screen a page's title takes over the
status line, so the battery shows on Home and on the larger screens' pages. The percentage turns to the warning color at the low
battery warning level and below. The level is set in **Settings > Power > Low battery warning** (15 % by default).

**OK**, **6**, **9** opens Tools > Battery, which shows the level. At the warning level and below it also shows "charge
soon".

The radio reads the battery once a second. It doesn't read it while transmitting or for 3 s after, because the
voltage sags under transmit.

## Low and critical battery

At the low battery warning level the radio shows the banner "Battery 15%: charge soon" (with the actual percentage)
and plays the low battery beep. The warning repeats every 5 minutes while the battery stays at or below that level.
It stops once the battery is 3 % above the level (charged).

Most radios also have two voltage thresholds. Whichever warning comes first applies.

| Level | What the radio does |
|---|---|
| Low | Low battery beep (800 Hz, then 600 Hz), again every 2 minutes. The backlight is limited to 30 % |
| Critical | Critical beep (600 Hz three times), again every 30 s. Every transmission is refused |

Three readings in a row below a threshold change the level. The level goes back up when the battery is 0.2 V above
the threshold. A radio without voltage thresholds has the percentage warning only.

## Transmit power

Each radio has its own set of power levels. Each channel's power comes from the codeplug. A channel without one uses
the radio's default level.

| Way to change it | Levels |
|---|---|
| **\***, **1** | Three steps: the radio's lowest level, its default, its highest |
| **Power level** key on Home | All of the radio's levels, one per press (radios with that key only) |

Both changes are temporary. Hold **OK** to save the level to the channel.

Power levels use the radio's factory calibration. Use the lowest power that reaches the other station.

## Backlight

**Settings > Display > Brightness** sets the backlight level. Quick settings has it too (**\***, **9**).

**Settings > Display > Backlight time** sets when the backlight dims to a fifth of the brightness. It goes off after
four times as long. Any key brings it back.

## Battery saver

**Settings > Power > Battery saver** puts the receiver to sleep on an idle FM channel. After 10 s without activity the
receiver sleeps and wakes for 100 ms at a time to listen. Any signal in a listening window wakes it fully. It is off
by default.

These stop the battery saver:

- a key press, monitor, scan or VOX;
- a DMR channel, or hotspot mode;
- a channel the codeplug marks "no battery saving".

## Auto power off

**Settings > Power > Auto power off** switches the radio off after a time without use: Off, 30 min, 1 h or 2 h (Off by
default). A key press, a call received or sent, a scan and hotspot mode count as use. One minute before, the radio
beeps and shows "Switching off in 1 min / Any key keeps it on". Any key cancels it.

On a radio with the power switch on its volume knob, the knob is still on when auto power off acts. Whether each
radio can cut its own power then is in the [Radio reference](23-radios.md). A radio that can't cut it comes back up
after 2 s, shows "Couldn't switch off / Turn the knob off", and keeps running as before.

## Turning off

Turn the radio off with its power switch. On the handhelds this is the volume knob, turned counterclockwise until it
clicks. Where the radio sees the switch, it finishes what it is doing (an ARS deregistration, at most 3 s), saves its
settings and switches off. The [Radio reference](23-radios.md) describes the power switch of each radio.
