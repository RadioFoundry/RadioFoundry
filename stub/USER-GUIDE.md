# Flash tool user guide (`stubtool.py`)

For the DM-32UV and the DM-1801 Pro. The tool backs up and checks the radio's two memory chips over the programming
cable and installs ODR builds. It needs only Python 3 and `pyserial` (`pip install pyserial`). Installing the stock or a
first ODR build still uses `tools/dm32_flash.py`. Items marked **[confirm]** are not checked on every radio yet.

## Before you start

- Use a charged battery. Do not unplug the cable or remove the battery while the tool is working.
- Use the programming cable that came with the radio, or a USB-serial cable known to work. Some cheap cables do not work
  **[confirm which]**. Find the cable's port name with `python3 -m serial.tools.list_ports` (for example
  `/dev/cu.usbserial-210` on macOS, `COM5` on Windows, `/dev/ttyUSB0` on Linux).
- Your radio has two memory chips: the program memory (the radio's software) and the data memory (settings, contacts,
  channels, calibration). Back up both.

## Back up your radio (radio switched off)

1. Switch the radio **off** and connect the cable.
2. Run:

   ```
   python3 stubtool.py PORT --progmem progmem.bin --q128 q128.bin
   ```

3. When the tool says to, switch the radio **on**. The screen stays blank and the LED blinks slowly yellow. This is
   expected: the radio is in loader mode.
4. Wait. The program memory takes about a minute and the data memory a few minutes. The LED blinks fast while data is
   moving. Do not touch the radio or the cable.
5. When the LED gives three green flashes and the tool prints the file names and a fingerprint (sha256), the backup is
   done. Keep both files somewhere safe.
6. Switch the radio off, then on again. It starts normally.

To read only part of a memory, add `--progmem-range START:END` or `--q128-range START:END` (byte addresses).

## Back up your radio (radio running ODR firmware)

If the radio runs an ODR build with the loader option, you do not need to switch it off.

1. Leave the radio **on** at its normal screen, connected by the cable.
2. Run the same command and add `--odr-enter`:

   ```
   python3 stubtool.py PORT --odr-enter --progmem progmem.bin --q128 q128.bin
   ```

3. The screen shows "PC Loader Connected" and then stops responding to the keys. This is expected. The LED blinks
   yellow as above.
4. When it is done, restart the radio with the next section.

## Install an ODR build without entering the bootloader

For a radio that already runs an ODR build with the loader option (`CONFIG_STUB_ENTRY=y`). No key presses, no
switching off.

1. Leave the radio **on** at its normal screen, connected by the cable.
2. Run (`odr_dm1801pro.bin` is the build you want to install):

   ```
   python3 stubtool.py PORT --odr-enter --write-app odr_dm1801pro.bin
   ```

3. The screen shows "PC Loader Connected". The tool compares the new build with what is in the radio, writes only the
   blocks that changed (a rebuilt image is often a few seconds, a very different one about 70 seconds), and checks every
   block after writing. It prints how many blocks it wrote.
4. Start the new build:

   ```
   python3 stubtool.py PORT --attach --boot reset
   ```

`odrflash flash odr_dm1801pro.bin --board dm1801pro --port PORT` does steps 2 to 4 in one run, and checks the radio
afterwards ([tools/odrflash/README.md](../tools/odrflash/README.md#the-dm-1801-pro)).

The radio's bootloader area is never written, so the usual update mode (hold the keys while switching on) always still
works. Do not unplug the cable or remove the battery while it writes. If it is interrupted, run the same command again
once the radio is back in loader mode, or use update mode and `tools/dm32_flash.py`. A build for the **DM-1801 Pro** was
tested this way **[confirm on the DM-32UV, which also keeps codec data in the program flash]**. Use a raw ODR `.bin`
(linked for 0x0300C000), not a stock firmware file.

## Check a backup (quick)

```
python3 stubtool.py PORT --verify-progmem progmem.bin --verify-q128 q128.bin
```

The tool compares the radio's memory with your files and names any 4 KB blocks that differ. If you used the radio since
the backup, small differences in settings are normal.

## Leave loader mode

Switch the radio off and on again. If it will not switch off (the power key does nothing while the loader runs), run:

```
python3 stubtool.py PORT --attach --boot reset
```

The radio restarts into its normal firmware. If neither works, remove and refit the battery.

## What the LED means

| LED | Meaning |
|---|---|
| Slow yellow blink (half a second on, half a second off) | The radio is in loader mode and waiting. Nothing is being copied. |
| Fast yellow blink (an eighth of a second on and off) | Data is being read. Do not touch the radio or the cable. |
| Three green flashes | Finished. The slow yellow blink comes back after. |
| Nothing, screen dark, tool says "waiting" | The radio is not in loader mode yet: switch it off, make sure the tool is waiting, then switch it on. |

Yellow is the red and green LEDs lit together, so it looks the same on every model **[confirm on the DM-32UV]**.

## Faster transfer (experimental)

`--baud 750000` (or 500000, 375000, 250000; `--baud auto` tries them) asks the radio to talk faster. The tool tests the
new speed first and goes back to normal speed by itself if the cable cannot keep up. On the cable tested so far it
failed on long reads, so leave it off unless a normal run is too slow and you have a different cable **[confirm]**.

## If something goes wrong

- **"The radio did not respond" / "did not start its serial boot ROM"**: switch the radio off, make sure the cable is in,
  run the tool, and only then switch the radio on.
- **A message about retries**: the tool checks every block and asks again when a block arrives damaged. A few retries are
  normal. If it gives up, switch the radio off and on and run it again.
- **"flash does not answer"**: the radio is not in loader mode, or the cable is wrong. Switch off and on and try again.
- **ODR entry says "no answer from ODR's debug link"**: the radio is not running an ODR build with the loader option, or
  the cable is on a different port. Use the switched-off method above instead.
- **The radio does not start after a backup**: remove and refit the battery, then switch it on.
