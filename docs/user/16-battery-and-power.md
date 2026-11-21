<!-- SPDX-License-Identifier: MIT -->

# Battery and Power

The battery, power level and power switch values depend on the radio. The [Radio reference](23-radios.md) has them
for each radio. A radio without a battery (a mobile on a fixed supply) has none of the battery items in this chapter:
no battery icon or percentage, no Battery saver or Low battery warning setting, and no battery warnings.

## Battery level

The status line shows the battery as an icon and a percentage. On a small screen a page's title takes over the
status line, so the battery shows on Home and on the larger screens' pages. The percentage turns to the warning color at the low
battery warning level and below. The level is set in **Settings > Power saving > Low battery warning** (15 % by default).

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

### Radio tuning

Open **Tools > Radio tuning** on the RadioFoundry site to fine-tune your radio. Connect the radio with its programming
cable. The page reads the radio's bands, power levels and modes, and shows what this radio can tune.

| Section | What it tunes | You need |
|---|---|---|
| Power | Each power level's output, per band | A dummy load and a wattmeter |
| Oscillator | The reference frequency of every mode at once | A frequency counter or a receiver you trust |
| Frequency per mode | A carrier offset seen in one mode only, such as FM but not DMR | A frequency counter or a receiver you trust |
| Deviation | FM deviation, on radios that support the trim | A deviation meter or a service monitor |

- Changes take effect right away. During a test transmission, some radios change the power at once; on the others,
  the page restarts the transmission with the new value. Press **Save to radio** to keep the changes. Unsaved changes last until the radio restarts or you press **Discard changes**. The oscillator
  trim is saved as soon as you change the trim.
- Each band has its own tuning. Tune one frequency per band, or up to three to follow the radio across a wide band.
  Between two tuned frequencies, the radio blends the trims.
- Rename any power level, for example **4W** instead of **5W**. A name can be up to 6 characters, and the status line
  shows the new name.
- The highest level can only go down, so the radio never transmits above its rating. The lowest level can only go up.
- **Restore factory values** removes every trim and name, and sets the oscillator trim back to 0. The oscillator trim
  is the **Freq trim** setting, so the page and the menu change the same value.

The trims are stored in the radio, outside the codeplug. A codeplug read, write or share never carries them to another
radio. A firmware update keeps them. If an update changes a level's factory setting, the page marks that level's trim
as out of date, and the radio stops using the trim until you tune that level again.

On some radios, the radio's own signal can disturb the USB cable while transmitting, and the page loses the
connection. The page shows **Connection lost**, keeps looking for the radio, and puts the unsaved changes back when
the radio answers again. A transmission that the page started ends by itself after a few seconds. To keep the
connection:

- Use the lowest power level that the test needs, into a dummy load, away from the cable.
- Put a ferrite choke on the cable near the radio, and use a short, good-quality cable.
- Plug the cable straight into the computer, not into a hub.

## Backlight

**Settings > Display > Brightness** sets the backlight level. Quick settings has it too (**\***, **9**).
On some radios a dimmed screen goes to full brightness while transmitting or receiving a signal, to keep the
backlight out of the audio, and returns to its set level when idle. Key beeps leave the brightness as set.

**Settings > Display > Backlight Dim/Off** sets two times: when the backlight dims to a fifth of the brightness, and
when it goes off. `30s/2m` dims after 30 s and goes off after 2 minutes. `30s/dim` dims after 30 s and stays on,
readable at a glance (on a desk or a bench), at the cost of some battery. Any key brings it back; with the
backlight off, that key only turns it on, except PTT, Monitor and Emergency, which act at once.

The backlight also comes on for what needs you:

| Comes on | Stays as it is |
|---|---|
| A call you hear (DMR), or a signal that opens the squelch (FM) | A DMR call on a talkgroup you don't listen to |
| A message, call alert, private call or emergency | The radio's own data: ARS registration, ACKs, location answers |
| Your own transmission, or a refused PTT | Timed beacons (APRS), roaming probes, hotspot traffic |
| | Broadcast FM that keeps playing |

## Battery saver

**Settings > Power saving > Battery saver** puts the receiver to sleep on an idle FM channel. After 10 s without activity the
receiver sleeps and wakes for 100 ms at a time to listen. Any signal in a listening window wakes it fully. It is off
by default.

These stop the battery saver:

- a key press, monitor, scan or VOX;
- a DMR channel, or hotspot mode;
- a channel the codeplug marks "no battery saving".

## Auto power off

**Settings > Power saving > Auto power off** switches the radio off after a time without use: Off, 30 min, 1 h or 2 h (Off by
default). A key press, a call received or sent, a scan and hotspot mode count as use. One minute before, the radio
beeps and shows "Switching off in 1 min / Any key keeps it on". Any key cancels it.

On a radio with the power switch on its volume knob, the knob is still on when auto power off acts. Whether each
radio can cut its own power then is in the [Radio reference](23-radios.md). A radio that can't cut it comes back up
after 2 s, shows "Couldn't switch off / Turn the knob off", and keeps running as before.

## Turning off

Turn the radio off with its power switch. On the handhelds this is the volume knob, turned counterclockwise until it
clicks. Where the radio sees the switch, it finishes what it is doing (an ARS deregistration, at most 3 s), saves its
settings and switches off. The [Radio reference](23-radios.md) describes the power switch of each radio.
