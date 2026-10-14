<!-- SPDX-License-Identifier: MIT -->

# Radio reference

The rest of this manual describes what ODR does on every radio. This chapter lists what differs from radio to radio:
which physical key does each job, the power levels, the battery, the power switch, the display, what a radio lacks,
and what has been checked on it.

| Radio | Also sold as | ODR status |
|---|---|---|
| [Baofeng DM-1701](#baofeng-dm-1701) | Retevis RT84 | Supported and in the releases. The only radio ODR has run on |
| [TYT MD-380](#tyt-md-380) | TYT MD-390, Retevis RT3 | Builds and passes its host tests. Not run on a radio yet, not in the releases |
| [Baofeng DM-32UV](#baofeng-dm-32uv) | | Builds and passes its host tests. Not run on a radio yet, not in the releases |
| [Radioddity DB25-D PRO](#radioddity-db25-d-pro) | Kydera CDR-300UV | Early bring-up: FM receive only, not run on a radio yet. Not for use |

[Planned radios](#planned-radios) lists the radios that aren't built yet.

## Key roles

The manual names keys by the job they do. Each radio puts those jobs on its own keys:

| Name in this manual | Job (press / hold) | DM-1701 | MD-380 | DM-32UV | DB25-D PRO |
|---|---|---|---|---|---|
| **OK** | Open or choose / save, make permanent | Green Menu key | Green Menu key | Green Menu / OK key | Menu / OK key, or push the knob |
| **Back** | Back one level / Home | Red Back key | Red Back key | Red Back key | B/E key |
| **Up**, **Down** | Move the focus / repeat | Arrow keys | Arrow keys | Up and Down keys | None: turn the knob |
| **\*** | Quick settings / lock or unlock the keys | **\*** | **\*** | **\*** | P1 |
| **#** | Actions / Recent | **#** | **#** | **#** | P2 |
| **0** to **9** | Number keys | Number keys | Number keys | Number keys | None |
| **Monitor** | FM: squelch open while held. DMR: digital monitor on / off | SK1 | SK1 | SK1 | P3 |
| **Talk target** | Talk target picker / scan on or off | SK2 | SK2 | SK2 | TONE/T.SEL |
| **Power level** | Next power level / emergency (held 1.5 s) | Top side key | None | None | None |
| **Previous zone**, **Next zone** | Step the zone on Home | P1, P2 | None | Hold Left, hold Right | VOL/SQL: previous zone only |
| **PTT** | Transmit | PTT | PTT | PTT | PTT on the microphone |
| Channel knob | One step per click | Channel knob | Channel switch (16 positions) | Channel knob | Front knob |

These are the defaults. **Settings > Keys** can give SK1, SK2 and the DM-1701's top side key other jobs
([Settings](12-settings.md#keys-key-8)).

On a radio without a key for a job, the menus reach it:

| Job | Without the key |
|---|---|
| Power level | **\***, **1** (quick settings, Power) |
| Change the zone | **OK**, **4** (Channels), pick a zone |
| Talk target | **\***, **8** |
| Monitor | **\***, **5** latches it |
| Emergency | No other way on the radio yet |

## Baofeng DM-1701

Also sold as the Retevis RT84. A handheld for 136-174 and 400-480 MHz with a keypad, a channel knob, a volume knob
with the power switch, three side keys and a color screen.

### Installing on the DM-1701

Follow [Installing](02-installing.md) with these values:

| Item | DM-1701 |
|---|---|
| Stock firmware file | Baofeng's `DM-1701(CSV)-V02.06.bin` (V02.06 CSV). An archive is at github.com/daniel5600/BAOFENG-DM1701-Original-Firmware |
| `--stock-installed` | `"V02.06 CSV"` |
| Bootloader keys | Hold **PTT** and **SK1** together while you turn the radio on with the volume knob |
| Bootloader USB device (Zadig) | "Digital Radio in USB mode" (0483:DF11), [driver steps](02-installing.md#windows-the-bootloader-driver) |
| Board name | `dm1701` |
| Going back to stock | Baofeng's firmware upgrade tool, in the stock firmware archive. It writes the application and the resource area the stock firmware keeps in the SPI flash |

### DM-1701 keys

| Key | Where | Role |
|---|---|---|
| Green Menu key | Keypad | **OK** |
| Red Back key | Keypad | **Back** |
| Arrow keys | Keypad | **Up**, **Down** |
| P1, P2 | The two front function keys | **Previous zone**, **Next zone** |
| Top side key | The uppermost of the three side keys | **Power level**. Press: the next power level. Hold 1.5 s: emergency. A build without emergency says "Voice prompts: not in this build" on a hold |
| SK1 | The middle side key | **Monitor**. Held with **PTT** at power-on, it starts the bootloader |
| SK2 | The lowest side key | **Talk target**: press for the picker, hold for scan |
| **PTT** | | **PTT** |
| Channel knob | | Channel knob |

The three side keys are, top to bottom, the top side key, SK1 and SK2.

### DM-1701 power levels

Nine levels: 50 mW, 250 mW, 500 mW, 750 mW, 1 W, 2 W, 3 W, 4 W, 5 W. The values are nominal and use the radio's
factory calibration.

| Way to change it | Levels |
|---|---|
| A channel without a power in the codeplug | 1 W |
| **\***, **1** | 50 mW, 1 W, 5 W |
| Top side key on Home | All nine, one per press |

### DM-1701 battery

A 2-cell Li-ion pack. The percentage follows the resting voltage, 4.10 V a cell for full and 3.20 V for empty. It
hasn't been checked against a meter yet.

| Level | Below |
|---|---|
| Low | 6.2 V |
| Critical | 6.0 V |

### DM-1701 power switch

The power switch is on the volume knob. Turn it clockwise past the click to switch on, counterclockwise to the click
to switch off. The switch cuts the battery sense line, so the radio sees "off" as a reading below 5.5 V, finishes
what it is doing, saves and drops its own power. A deeply discharged battery reads the same way and the radio shuts
down.

Whether the radio can cut its own power while the knob is still on (auto power off) hasn't been checked. If it
can't, the radio comes back after 2 s and shows "Couldn't switch off / Turn the knob off".

### DM-1701 display

A 160 x 128 color screen with the top 8 rows hidden: 160 x 120 visible. The screenshots in this
manual are this size.

### What the DM-1701 lacks

- No GPS. LRRP location requests get "no fix". APRS positions come from a computer or a fixed position.
- APRS receive needs the squelch open: the radio hears packets strong enough to open the squelch, not weaker ones.
- No APRS iGate (passing heard packets to APRS-IS): it doesn't fit in the flash yet.

### Verified on the DM-1701

Tested on one DM-1701, on air against the SDR bench where RF is involved:

| Area | Verified |
|---|---|
| Basics | Boot, display, keys, calibration |
| FM | Receive and transmit at 12.5 and 25 kHz, CTCSS and DCS tones, 1750 Hz burst, roger beep |
| DMR voice | Group and private calls both ways, late entry, talker alias, positions in calls, the basic privacy signaling (the scrambling itself hasn't been checked against another radio) |
| DMR data | Text messages, call alert, radio check, emergency, all both ways |
| Scan | Zone scan (including stopping for DMR calls), VFO band scan, dual watch |
| Hotspot | Voice and data calls both ways. MMDVM SFR repeats a whole call, about 210 ms after the original instead of the intended 90 ms |
| APRS | Packets sent on FM and over DMR decode off air |
| Power | Battery saver: it cycles when idle, and monitor and an incoming call wake it |
| Programming | Codeplug reads and writes with qdmr's protocol (`tools/cps_client.py`) |
| USB network | The switch to network mode, the address from the computer (macOS) and ping |

Partly checked: VOX. Silence and the radio's own beeps don't key it. Keying on a voice hasn't been checked.
Host-tested only: the split duplex repeater, IP over DMR to another radio and the services in USB network mode,
control station mode, roaming, ARS and LRRP.

### DM-1701 with qdmr

The radio reports radio type 8 (or 10, depending on its screen), and qdmr uses its open-firmware driver for the
DM-1701, the one for the UV380 family.

## TYT MD-380

Covers the TYT MD-380, the MD-390 and the Retevis RT3. **Not run on a radio yet**: everything here comes from the
documentation of the hardware. Installing: [Moving from stock MD-380 firmware](18-moving-from-stock-md380.md).

### MD-380 keys

| Key | Role |
|---|---|
| Green Menu key | **OK** |
| Red Back key | **Back** |
| Arrow keys | **Up**, **Down** |
| SK1 | **Monitor** |
| SK2 | **Talk target** |
| **PTT** | **PTT** |
| Channel switch | Channel knob. A 16-position switch, not an endless knob: ODR turns each change of position into steps |

No **P1**, **P2** or top side key, so no **Power level** or zone keys and no emergency key. Change the zone with **OK**,
**4**, the power with **\***, **1**. Where each key sits in the key matrix hasn't been confirmed on a radio. The
bootloader starts when you hold **PTT** and the side key just above it at power-on.

### MD-380 bands and power levels

One band per radio: UHF (400-480 MHz) or VHF (136-174 MHz). There is one image for each band, and an image refuses
the other band.

Two power levels, 1 W and 5 W. A channel without a power in the codeplug uses 1 W. **\***, **1** switches between the
two. The radio refuses to transmit without a valid factory calibration.

### MD-380 battery

A 2-cell Li-ion pack, with the same percentage curve and the same thresholds as the DM-1701: low below 6.2 V,
critical below 6.0 V. The voltage scale is assumed, not measured.

### MD-380 power switch

The power switch is on the volume knob. It is believed to switch the supply directly, so ODR doesn't see the knob
turn off. When ODR switches itself off (auto power off), it drops its power latch. Unchecked on a radio.

### MD-380 display

A 160 x 128 color screen, every row visible. The controller type is unconfirmed.

### What the MD-380 lacks

- No CTCSS or DCS yet: a channel with a tone opens on any carrier and sends no tone.
- No roger beep, 1750 Hz burst or APRS on FM (the transmit tone path isn't written). APRS positions over DMR work.
- No APRS receive.
- No hotspot mode in the default image (left out for flash space). MMDVM SFR and the split duplex repeater need the
  DMR chip of the DM-1701 (HR_C6000) and aren't built for the MD-380 (HR_C5000).
- USB network mode and its services are in.
- Audio linking (USRP, [USB network](14-usb-network.md#linking-audio-allstarlink-and-dvswitch)) carries DMR only: the
  FM transmit path from the processor isn't written.
- GPS only on the "G" models. The radio listens for the GPS module for 5 s at start-up and hides the GPS features if
  it hears nothing.

### MD-380 with qdmr

The radio reports radio type 7. Moving from the stock firmware with the codeplug: `tools/md380_migrate.py`
([Moving from stock MD-380 firmware](18-moving-from-stock-md380.md)).

## Baofeng DM-32UV

A handheld for 136-174 and 400-480 MHz with a keypad, left and right arrow keys, a channel knob and a color screen.
**Not run on a radio yet.** Installing: [Moving from stock DM-32UV firmware](19-moving-from-stock-dm32.md).

### DM-32UV keys

| Key | Role |
|---|---|
| Green Menu / OK key | **OK** |
| Red Back key | **Back** |
| Up, Down | **Up**, **Down** |
| Left, Right | Press: not used yet. Hold: **Previous zone**, **Next zone**. The key labels are unchecked |
| SK1 (the side key below **PTT**) | **Monitor**. Held with **PTT** at power-on, it starts the bootloader |
| SK2 | **Talk target** |
| **PTT** | **PTT** |
| Channel knob | Channel knob |

No **Power level** key and no emergency key: the radio's third programmable side key hasn't been located yet. Change
the power with **\***, **1**.

### DM-32UV power levels

Nine levels, L-, L, L+, L++, M, M+, M++, H-, H, from the radio's factory calibration. The radio is rated 5 W (VHF) and
4 W (UHF) at high power. The levels haven't been measured. A channel without a power in the codeplug uses L.
**\***, **1** steps through L-, L and H.

### DM-32UV battery

The percentage is linear from 6.4 V (0 %) to 8.2 V (100 %). There are no voltage thresholds: only the percentage
warning (**Settings > Power > Low battery warning**) applies, and a low battery never stops a transmission.

### DM-32UV power switch

The power switch is on the knob. The radio sees it turn off (three readings in a row), saves and drops its own power.
The volume control works on the audio amplifier directly. ODR doesn't read it.

### DM-32UV display

A 240 x 320 color screen. ODR draws at half resolution for now.

### What the DM-32UV lacks

- No USB. The serial port is on the programming cable: hotspot mode, the debug link and the network (PPP,
  [USB network](14-usb-network.md#network-over-the-programming-cable-ppp)) all run there. USB network mode isn't
  available.
- The radio has a GPS module, but ODR has no GPS driver for it yet. **Settings > Location > GPS** keeps its value.
- The air band AM receiver and the broadcast FM receiver are built in, but the board code behind them isn't written,
  so nothing runs yet.
- Audio linking (USRP) carries DMR only: the FM transmit path from the processor isn't written.

### DM-32UV with qdmr

qdmr can't program a DM-32UV running ODR directly. ODR keeps the codeplug 7 MB into the radio's data flash, so the
addresses qdmr's driver uses land in the stock area, where ODR refuses writes. Edit the codeplug in qdmr, save it as
YAML, and write it with `dm32_migrate.py restore --yaml FILE` ([Moving from stock DM-32UV
firmware](19-moving-from-stock-dm32.md)), which adds the offset. The radio reports radio type 6 (MD-UV380) for now.

## Radioddity DB25-D PRO

A mobile radio for 136-174 and 400-480 MHz with a front knob, a few front keys and a hand microphone. **Early
bring-up, not for use:** ODR builds for it and receives FM, and nothing has run on a radio yet. There is no install
procedure.

### DB25-D PRO keys

The front keys' positions in the key matrix are provisional until checked on a radio. The hand microphone's keys
aren't decoded yet, so there are no number keys.

| Key | Role |
|---|---|
| Menu / OK, or push the knob | **OK** |
| B/E | **Back** |
| P1 | **\*** |
| P2 | **#** |
| P3 | **Monitor** |
| TONE/T.SEL | **Talk target** |
| VOL/SQL | **Previous zone** |
| **PTT** on the microphone | **PTT** |
| Front knob | Channel knob, and **Up** / **Down** in lists |
| Power key | Press to switch on. Hold 2 s to switch off. Hold P1 while you press it to start the bootloader (the red LED lights and the screen stays blank) |

### DB25-D PRO power and supply

Two levels, L and H (nominal 20 W). The radio refuses to transmit until ODR can read its power calibration.

The radio runs from a 13.8 V supply. ODR doesn't read the supply voltage yet: the status line shows 100 % and there
are no battery warnings.

### DB25-D PRO display

A 1.77" color screen, 160 x 128. There is no volume knob: the radio sets its volume digitally.

### What the DB25-D PRO lacks today

No transmit, no DMR, no hotspot, no programming with qdmr, and no stored settings: the radio runs on the defaults and
forgets changes at power off. Only the PRO (two BK4819 receiver chips) is supported, not the earlier batches.

## Planned radios

These radios aren't built yet. [The radio survey](../spec/radios.md) lists what is known about each and the rebadged
models it covers.

| Radio | Note |
|---|---|
| TYT MD-UV380 / MD-UV390, Retevis RT3S | Planned as a variant of the DM-1701 board |
| TYT MD-9600, Retevis RT90 | Mobile. Planned |
| Radioddity GD-77 family | Planned. Needs a new processor platform |
| Radioddity GD-88 | Planned |
| Baofeng DM-1801 Pro | Planned. Hardware needed |
| Radioddity DB40-D | Mobile. Planned. Hardware needed |
