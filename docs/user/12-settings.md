<!-- SPDX-License-Identifier: MIT -->

# Settings

Open Settings with **OK**, **6**, or with **\***, **0**. Settings are grouped by what they are for. A number key opens
a group directly. Find also reaches each setting by name: type its first letters on Home.

<img src="../ux/screens/160x120i5/settings.png" width="200" alt="Settings">
<img src="../ux/screens/160x120i5/settings_display.png" width="200" alt="Settings, Display">

## Changing a setting

Each row shows its current value.

- On/off and list settings change with **OK**: each press goes to the next value.
- Number settings open an editor on **OK**. Change the value with **Up** and **Down** (or the channel knob), press
  **OK** to keep it, **Back** to leave it unchanged.
- A setting takes effect and is saved as soon as you change it. There is no separate save step.

<img src="../ux/screens/160x120i5/settings_editor.png" width="200" alt="Editing a number setting">

Some settings are on the menu before the feature behind them exists. The table says so in the Status column:

| Status | Meaning |
|---|---|
| Works | Acts now and is kept across power off |
| Works, not kept | Acts now, back to the default after power off |
| Kept, no effect yet | Stored for a later feature; changing it does nothing yet |
| No effect yet | Neither acts nor is kept yet |

## Reference

### Sound (key 1)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Key tones | On, Off | On | A short beep on each key press | Works |
| Tone volume | 0 to 10 | 5 | Volume of the radio's beeps, relative to the volume knob | Works |
| Talk permit tone | On, Off | On | Planned: the beep when a DMR channel grants a transmission. Today the codeplug channel's beep setting controls it | No effect yet |
| Voice prompts | On, Off | Off | Planned: spoken prompts | No effect yet |
| Message tone | On, Off | On | Planned: the tone for a new message | No effect yet |

### Display (key 2)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Brightness | 10 to 100 %, steps of 10 | 100 % | Backlight brightness. Also in quick settings, tile 9 | Works |
| Backlight time | 5 s, 10 s, 30 s, 1 min, 2 min, Never | 30 s | The backlight dims after this time without a key press and goes off after four times as long | Works |
| Theme | Dark, Light, High contrast | Dark | Planned: color theme | No effect yet |
| Large text | On, Off | Off | Home shows only the status line, the channel and the talk target, large | Works, not kept |
| Hint row | On, Off | On | The bottom line with key hints | Works, not kept |

### Talk & listen (key 3)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Reply window | 0 to 30 s | The codeplug's hang time, else 10 s | How long **PTT** answers the last call ([DMR](08-dmr.md#replying)). 0 turns it off | Works |
| TX timeout | Off, 30 s, 1, 2, 3, 5, 10 min | 3 min | Longest transmission, for channels without their own timeout in the codeplug | Works |
| Default squelch | 0 to 9 | 5 | The FM squelch level when a channel doesn't set one | Works |
| Mic gain | 0 to 10 | 5 | Planned: microphone gain | Kept, no effect yet |
| Hear all by default | On, Off | Off | Planned: hear all on whenever a DMR channel is selected | No effect yet |
| Show talker alias | On, Off | On | Planned: show or hide received talker aliases (they are always shown) | No effect yet |

### Privacy (key 4)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Privacy keys | | | An information page about privacy keys ([DMR](08-dmr.md#privacy)) | |
| Auto lock | Off, 30 s, 1 min, 5 min | Off | Locks the keys after this time without a key press | Works |
| PTT when locked | On, Off | On | Planned: whether **PTT** works while the keys are locked (today it always does) | No effect yet |

### Location (key 5)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| GPS | On, Off | Off | Planned: GPS receiver on radios that have one. The DM-1701 has none | Kept, no effect yet |
| Position format | Degrees, Deg min sec, Grid | Degrees | Planned: how positions are shown | Kept, no effect yet |
| Send position | Off, 1 min, 5 min, 15 min | Off | Planned: send your position at this interval | Kept, no effect yet |
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
| Auto power off | Off, 30 min, 1 h, 2 h | Off | Planned: switch off after this time without use | No effect yet |
| Low battery warning | 5 to 30 %, steps of 5 | 15 % | Planned: the warning level. Today the radio warns at fixed voltages | No effect yet |

### Keys (key 8)

| Setting | Values | Default | What it does | Status |
|---|---|---|---|---|
| Long press time | 400 to 1200 ms, steps of 100 | 600 ms | How long you hold a key for a long press | Works |
| Key repeat | 60 to 300 ms, steps of 20 | 120 ms | The repeat rate of **Up** and **Down** while held | Works |
| Up/Down on Home | Channel, Zone, Talk target | Channel | Planned: what **Up** and **Down** change on Home (today: the channel) | No effect yet |

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

The reset puts every setting back to its default (brightness, radio settings, the stored privacy key, the channel
position), erases ODR's own data area, and restarts the radio. The codeplug and the calibration stay.

## Settings without a menu

A few settings exist in the radio but not yet on the menu: busy channel lockout, roger beep, the scan's resume mode
and timing, VOX sensitivity, ARS server, the emergency settings, the text message format. Developers change them over
the debug link (`tools/odr_debug.py radio set ...`, `tools/odr_debug.py data settings ...`); the radio keeps them.

A build without the Settings menu (`CONFIG_UI_SETTINGS=n`) shows "Set them with qdmr" instead, and keeps the stored
values.
