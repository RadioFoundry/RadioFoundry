<!-- SPDX-License-Identifier: MIT -->

# Troubleshooting

## Installing

| Problem | Cause and fix |
|---|---|
| `codec_extract.py` refuses the stock file | It accepts only the stock files it knows by SHA-256 (DM-1701: V02.06 CSV). Download that exact file |
| `codec_extract.py` or `stm32_dfu.py` refuses the key | The key file must be the 1024-byte DM-1701 key (SHA-256 `d983ce95...`) |
| `stm32_dfu.py --list` shows nothing | The radio isn't in its bootloader. Turn it off, hold **PTT** and **SK1**, turn it on, keep holding until the computer sees it. Try another USB port or cable |
| Access denied on Linux | Add the udev rule for 0483:DF11 that `stm32_dfu.py` prints, then unplug and replug |
| `pyusb` can't find a backend | Install libusb (`brew install libusb`, or `apt install libusb-1.0-0`) |
| The tool refuses the file as an official firmware | You passed a Baofeng "OutSecurityBin" file. Flash the ODR image, not the stock file |
| The screen stays dark after flashing | The radio is still in its bootloader. Turn it off and on again without holding any key |

## Everyday use

| Problem | Cause and fix |
|---|---|
| DMR calls show but there is no audio, "No codec: voice off" | The radio has no usable codec. See [Installing, no codec](02-installing.md#no-codec-no-codec-voice-off) |
| A DMR call shows "Other TG: not played" | Its talkgroup isn't the talk target, in the listen list or the reply target. Hold **SK1**, use hear all, or add the talkgroup to the channel's RX group list in qdmr |
| "Encrypted: not played" | The call uses basic privacy and the radio has no matching key, or stronger encryption the radio can't play |
| "No DMR ID in codeplug" when transmitting | Write a codeplug with your DMR ID ([Programming with qdmr](15-programming-qdmr.md)) |
| "Channel busy (polite)" | Someone is talking on the timeslot. Wait for the call to end |
| "No repeater answered" | The repeater didn't respond. Check the frequency, the color code, the timeslot and your distance to the repeater |
| No audio on FM | The squelch is closed or the channel needs a tone. Hold **SK1** to open the squelch; check the tone with **\***, **3** |
| A key does nothing and the screen says "Keys locked" | Hold **\*** to unlock. Check **Settings > Privacy > Auto lock** |
| "Not used here" | That key has no function on this screen |
| "... isn't in this build" | The feature is switched off in this build, or isn't built yet |
| The radio is stuck on the hotspot screen | A host has the serial port open. Press **OK**, or stop the host program. Turn **Settings > Connections > Hotspot allowed** off to prevent it |
| Zone or channel changed by itself | A scan is running (SCAN in the status line). Press **Back** on Home |

## Settings are not kept

If settings go back to their defaults after a restart, and qdmr writes fail, the radio has found another firmware's
data in the flash area where ODR keeps its settings. ODR doesn't erase it, so you don't lose data by accident. The
radio runs on the defaults in memory.

To give the area to ODR (developers, over the debug link):

```
tools/odr_debug.py storage info     # shows region=foreign
tools/odr_debug.py storage claim --out ~/odr-backup    # saves a copy of the area, then erases it for ODR
```

`storage claim` saves the area's old contents and the calibration copy to files in the `--out` directory before it
erases anything. It refuses to save inside a git work tree.

Some settings are "not kept" by design today: see the Status column in [Settings](12-settings.md).

## Diagnostics

**OK**, **5**, **7** (Tools > Diagnostics) shows raw readings for bench checks: keys, receiver levels, the DMR
modem's state and counters, battery, the board's own lines. **\*** pages through them. **Back** leaves.

<img src="../ux/screens/160x120i5/diagnostics.png" width="200" alt="The diagnostics page">

Developers get the same and more over USB with `tools/odr_debug.py diag` and `tools/odr_debug.py view`.

## Reporting a problem

Open an issue in the project repository. Include the firmware version (**Settings > Advanced > About**), the radio
model, what you did, what you expected and what happened. A screenshot helps: `tools/odr_debug.py screenshot -o
screen.png` saves one over USB.
