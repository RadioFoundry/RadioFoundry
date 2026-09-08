<!-- SPDX-License-Identifier: MIT -->

# Settings

Open Settings with **OK**, **6**, or with **\***, **0**. Settings are grouped by what they are for. A number key opens
a group directly. Find also reaches each setting by name: type its first letters on Home.

<img src="../ux/screens/160x120i5/settings.png" width="200" alt="Settings">
<img src="../ux/screens/160x120i5/settings_display.png" width="200" alt="Settings, Display">

## Changing a setting

Each row shows its current value.

- On/off and list settings change with **OK**: each press goes to the next value.
- Number settings open an editor on **OK**. Change the value with **Up** and **Down** (or the channel knob), or type
  it with the number keys, press **OK** to keep it, **Back** to leave it unchanged. A typed value outside the range,
  or between steps, isn't taken: the radio says "Not changed" with the range.
- A setting takes effect and is saved as soon as you change it. There is no separate save step.

<img src="../ux/screens/160x120i5/settings_editor.png" width="200" alt="Editing a number setting">

Some settings are on the menu before the feature behind them exists, or don't apply to this radio. They are dimmed;
**OK** on a dimmed row says why. The table says so in the Status column:

| Status | Meaning |
|---|---|
| Works | Acts now and is kept across power off |
| Works, not kept | Acts now, back to the default after power off |
| Dimmed | Not built yet, or not on this radio; **OK** says why |

## Reference

### Sound (key 1)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Key tones | On, Off | On | A short beep on each key press | Works |
| Tone volume | 0 to 10 | 5 | Volume of the radio's beeps, relative to the volume knob | Works |
| Talk permit tone | On, Off | On | The beep when a DMR transmission starts ([DMR](08-dmr.md)) | Works |
| Voice prompts | On, Off | Off | Spoken prompts. There is no recorded speech yet | Dimmed |
| Message tone | On, Off | On | A three-note tone when a text message arrives | Works |

### Display (key 2)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Brightness | 10 to 100 %, steps of 10 | 100 % | Backlight brightness. Also in quick settings, tile 9 | Works |
| Backlight time | 5 s, 10 s, 30 s, 1 min, 2 min, Never | 30 s | The backlight dims after this time without a key press and goes off after four times as long | Works |
| Theme | Dark, Light, High contrast | Dark | The screen colors. Dimmed on black and white screens (GD-77) | Works |
| Large text | On, Off | Off | Home shows only the status line, the channel and the talk target, large | Works |
| Hint row | On, Off | On | The bottom line with key hints | Works |

### Talk & listen (key 3)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Reply window | 0 to 30 s | The codeplug's hang time, else 10 s | How long **PTT** answers the last call ([DMR](08-dmr.md#replying)). 0 turns it off | Works |
| TX timeout | Off, 30 s, 1, 2, 3, 5, 10 min | 3 min | Longest transmission, for channels without their own timeout in the codeplug | Works |
| Default squelch | 0 to 9 | 5 | The FM squelch level when a channel doesn't set one | Works |
| Mic gain | 0 to 10 | 5 | Microphone level on DMR, 2 dB a step; 5 is the radio's normal level. FM isn't changed yet | Works (DMR) |
| Hear all by default | On, Off | Off | Hear all goes on each time you select a DMR channel. **Back** on Home turns it off until the next channel | Works |
| Show talker alias | On, Off | On | Show received talker aliases (names) under the caller's ID | Works |

### Privacy (key 4)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Privacy key type | Off, Motorola, Anytone | Off | The kind of basic privacy key ([DMR](08-dmr.md#privacy)). Off removes the key | Works |
| Privacy key | Motorola 1 to 255, Anytone 0 to 65535 | | The key value, typed with the number keys. Dimmed until a type is chosen | Works |
| Auto lock | Off, 30 s, 1 min, 5 min | Off | Locks the keys after this time without a key press | Works |
| PTT when locked | On, Off | On | Whether **PTT** works while the keys are locked. Off: **PTT** does nothing until you unlock | Works |

### Location (key 5)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| GPS | On, Off | Off | GPS receiver, on radios that have one. Dimmed with "No GPS on this radio" on the DM-1701. The DM-32 keeps the value; it has no GPS driver yet | Dimmed without GPS |
| Position format | Degrees, Deg min sec, Grid | Degrees | How positions are shown | Dimmed without GPS |
| Send position | Off, 1 min, 5 min, 15 min | Off | Send your position at this interval | Dimmed without GPS |
| Answer position requests | On, Off | On | Answer LRRP location requests. Without GPS the answer is "no fix" | Works |

### Connections (key 6)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Hotspot allowed | On, Off | On | Lets a hotspot host on USB put the radio in hotspot mode ([Hotspot](13-hotspot.md)) | Works |
| Answer radio checks | On, Off | Off | Answer radio checks from other radios ([Radio check](09-messages-and-signaling.md#radio-check)) | Works |
| Answer call alerts | On, Off | On | Acknowledge call alerts, so the caller knows the page arrived | Works |

### Power (key 7)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Battery saver | On, Off | Off | On an idle FM channel the receiver sleeps between short listening windows ([Battery and power](16-battery-and-power.md)) | Works (not tested on the radio yet) |
| Auto power off | Off, 30 min, 1 h, 2 h | Off | Switch off after this time without use ([Battery and power](16-battery-and-power.md#auto-power-off)) | Works |
| Low battery warning | 5 to 30 %, steps of 5 | 15 % | Warn when the battery falls to this level ([Battery and power](16-battery-and-power.md)) | Works |

### Keys (key 8)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Long press time | 400 to 1200 ms, steps of 100 | 600 ms | How long you hold a key for a long press | Works |
| Key repeat | 60 to 300 ms, steps of 20 | 120 ms | The repeat rate of **Up** and **Down** while held | Works |
| Up/Down on Home | Channel, Zone, Talk target | Channel | What **Up** and **Down** change on Home. Talk target steps through the channel's contact and its listen list (DMR channels; on FM they change the channel) | Works |

### My radio (key 9)

| Setting | What it shows |
|---|---|
| DMR ID and name | Your DMR ID from the codeplug ("not set: set it in qdmr" without one) and the radio model |

### Advanced (key 0)

| Item | What it does |
|---|---|
| Firmware | "OpenDigitalRadio", the radio model and the firmware version (commit) |
| Codeplug | Where the channels come from (built-in or from the CPS), the number of zones and channels |
| Diagnostics | The diagnostics page (also **OK**, **5**, **7**) |
| Factory reset | Puts every setting back to its default ([below](#factory-reset)) |

<img src="../ux/screens/160x120i5/settings_advanced.png" width="200" alt="Settings, Advanced">

## Factory reset

**Settings > Advanced > Factory reset** asks "Erase all settings and go back to factory defaults? Channels stay. This
can't be undone." The focus starts on "No, keep everything". Focus "Yes, reset" and press **OK**.

<img src="../ux/screens/160x120i5/factory_reset.png" width="200" alt="Factory reset confirmation">

The reset puts every setting back to its default (brightness, radio settings, the privacy key, the channel
position), erases ODR's own data area, and restarts the radio. The codeplug and the calibration stay.

## Settings without a menu

A few settings exist in the radio but not yet on the menu: busy channel lockout, roger beep, the scan's resume mode
and timing, VOX sensitivity, ARS server, the emergency settings, the text message format, the roaming threshold,
hysteresis and periodic search, and the SFR idle beacon. The roaming survey and beacon settings are in **Tools >
Roaming** ([DMR](08-dmr.md#roaming)). Developers change them over
the debug link (`tools/odr_debug.py radio set ...`, `tools/odr_debug.py data settings ...`); the radio keeps them.

A build without the Settings menu (`CONFIG_UI_SETTINGS=n`) shows "Set them with qdmr" instead, and keeps the stored
values.
