# Flash tool user guide (`stubtool.py`)

For the DM-32UV, the DM-1801 Pro and the DB25-D. The tool backs up and checks the radio's two memory chips over the programming
cable and installs RadioFoundry builds. It needs only Python 3 and `pyserial` (`pip install pyserial`). Installing the stock or a
first RadioFoundry build still uses `tools/serial_loader.py`. Items marked **[confirm]** are not checked on every radio yet.

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
   python3 stubtool.py PORT --board dm1801pro --progmem progmem.bin --q128 q128.bin
   ```

   `--board` names the radio (`dm32`, `dm1801pro` or `db25d`) so the tool sets up the right pins and checks them before
   it writes anything. Without it a backup still works, but writing is refused.

3. When the tool says to, switch the radio **on**. The screen stays blank and the LED blinks slowly orange. This is
   expected: the radio is in loader mode.
4. Wait. The program memory takes about a minute and the data memory a few minutes. The LED blinks fast while data is
   moving. Do not touch the radio or the cable.
5. When the LED gives three green flashes and the tool prints the file names and a fingerprint (sha256), the backup is
   done. Keep both files somewhere safe.
6. Switch the radio off, then on again. It starts normally.

To read only part of a memory, add `--progmem-range START:END` or `--q128-range START:END` (byte addresses).

## Back up your radio (radio running RadioFoundry firmware)

If the radio runs a RadioFoundry build with the loader option, you do not need to switch it off.

1. Leave the radio **on** at its normal screen, connected by the cable.
2. Run the same command and add `--rfoundry-enter`:

   ```
   python3 stubtool.py PORT --rfoundry-enter --progmem progmem.bin --q128 q128.bin
   ```

3. The screen shows "PC Loader Connected" and then stops responding to the keys. This is expected. The LED blinks
   orange as above.
4. When it is done, restart the radio with the next section.

## Install a RadioFoundry build without entering the bootloader

For a radio that already runs a RadioFoundry build with the loader option (`CONFIG_STUB_ENTRY=y`). No key presses, no
switching off.

1. Leave the radio **on** at its normal screen, connected by the cable.
2. Run (`rfoundry_dm1801pro.bin` is the build you want to install):

   ```
   python3 stubtool.py PORT --rfoundry-enter --write-app rfoundry_dm1801pro.bin
   ```

3. The screen shows "PC Loader Connected". The tool compares the new build with what is in the radio, writes only the
   blocks that changed (a rebuilt image is often a few seconds, a very different one about 70 seconds), and checks every
   block after writing. It prints how many blocks it wrote.
4. Start the new build:

   ```
   python3 stubtool.py PORT --attach --boot reset
   ```

`rfoundry flash rfoundry_dm1801pro.bin --board dm1801pro --port PORT` does steps 2 to 4 in one run, and checks the radio
afterwards ([tools/rfoundry/README.md](../tools/rfoundry/README.md#the-dm-1801-pro)).

**DB25-D:** the same, with the install image (`rfoundry_db25d_install.bin`) and the script `db25d_update.sh IMAGE PORT`,
which also restarts the radio and checks that it answers. The tool picks the loader that matches the running build by
itself (a build with DMR keeps it in a different place than one without), so the same command installs a DMR build over
a build without DMR, one DMR build over another, and a build without DMR over a DMR build. Use `--rfoundry-stub FILE` only
for a stub built elsewhere; the tool refuses one that does not match the running build, before the radio is touched.
`--legacy-stub` uses the loaders of earlier tool versions, if the current one ever gives trouble.

The DB25-D also has a 256 MB NAND chip (recordings, and RadioFoundry's own area at 0xF000000). Read part of it with
`--nand nand.bin --nand-range START:END` and compare with `--verify-nand nand.bin`. The NAND is only read, never written.

Factory data is never written either. Each radio's `board.toml` lists it (`[[flash.protect]]`: the calibration, and on
the DB25-D the NAND's bad-block table and reserve blocks). The tool refuses any write or erase that would touch it, and
says which region and why, before anything is erased.

The radio's bootloader area is never written, so the usual update mode (hold the keys while switching on) always still
works. Do not unplug the cable or remove the battery while it writes. If it is interrupted, run the same command again
once the radio is back in loader mode, or use update mode and `tools/serial_loader.py`. A build for the **DM-1801 Pro** was
tested this way **[confirm on the DM-32UV, which also keeps codec data in the program flash]**. Use a raw RadioFoundry `.bin`
(linked for 0x0300C000), not a stock firmware file.

## Factory calibration (STM32 radios: the DM-1701 family) [confirm]

These radios keep their factory calibration and flags in three small "security registers" of the data memory chip
(768 bytes in all), apart from the normal data memory. A `--q128` backup does not include them. With the radio running a
RadioFoundry build with the loader option:

```
python3 stubtool.py PORT --rfoundry-enter --secreg secreg.bin
```

The tool saves the three registers and prints whether each one is locked. A locked register can never be changed again,
by anything. Keep the file with your other backups.

The registers are protected regions of these radios (`board.toml` `[[flash.protect]]`: registers 1 and 2 on every one,
register 3 too on the DM-1701 and MD-UV380). Putting a saved copy back is therefore refused for them, before anything
is written, unless the register already holds the saved bytes. The command stays for a register that isn't listed:

```
python3 stubtool.py PORT --attach --write-secreg secreg.bin --confirm-secreg
```

The tool refuses a file that is not 768 bytes, writes only the registers that differ, saves what was there first (in your
home folder, or `--backup-dir`), and refuses a locked register before anything is written. It never locks a register.
Do not unplug the cable while it writes: a register that was erased but not yet written must be put back from the saved
file.

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
| Slow orange blink (half a second on, half a second off) | The radio is in loader mode and waiting. Nothing is being copied. |
| Fast orange blink (an eighth of a second on and off) | Data is being read. Do not touch the radio or the cable. |
| Three green flashes | Finished. The slow orange blink comes back after. |
| Nothing, screen dark, tool says "waiting" | The radio is not in loader mode yet: switch it off, make sure the tool is waiting, then switch it on. |

Orange is the red LED at full and the green LED dimmed (about 16%), so it looks the same on every model (red = PTB1, green = PTB0, active high, the same pins on the DM-1801 Pro and the DM-32UV).

On the GD-88 the LED isn't a guide yet: what it shows during an installation is still changing. Follow the tool's
messages and the screen.

## Transfer speed

With a current loader (protocol 7), writes and backups pick the fastest serial speed your cable and radio manage, by
themselves. The tool tries faster speeds first, tests each with full-size transfers both ways, and keeps the fastest
that passes. It remembers the result for that port and radio, so the next run starts at it at once. If the link goes
bad later (a worn cable, a loose jack), the tool goes quiet for a few seconds, the radio goes back to normal speed by
itself, and the run carries on at normal speed. You do nothing.

| Option | What it does |
|---|---|
| none | Pick the speed by itself for writes and backups |
| `--baud auto` | The same, also for other jobs |
| `--baud off` | Stay at 115200 |
| `--baud 262500` | Ask for one speed; if it fails the test, the run stays at 115200 |

The radio's serial port has whole steps only (its clock divided by 16 times a whole number), so speeds such as 262500
are normal, and 230400 or 921600 are not offered as such. On the cables tested so far the fastest that held was about
260000.

## If something goes wrong

- **"The radio did not respond" / "did not start its serial boot ROM"**: switch the radio off, make sure the cable is in,
  run the tool, and only then switch the radio on.
- **"stopped waiting before the answer to its greeting reached it"** or **"the radio started its firmware"**: the boot
  ROM waits only about 48 ms for the tool's answer, and the cable or a USB hub was too slow. Switch the radio off, run
  the tool again with `--slow-cable`, then switch the radio on. The tool then keeps sending its answer while the radio
  starts. Run it only with the radio off: a running firmware would receive those answers too.
- **A message about retries**: the tool checks every block and asks again when a block arrives damaged. A few retries are
  normal. If it gives up, switch the radio off and on and run it again.
- **"flash does not answer"**: the radio is not in loader mode, or the cable is wrong. Switch off and on and try again.
- **RadioFoundry entry says it "refused to enter this stub" because of the watchdog**: the tool or web page carries a loader
  older than the radio's firmware. Nothing was written and the radio keeps running. Update the tool, or reload the web
  page, and run it again. The radio's watchdog never resets it during a backup or an install unless the loader itself
  hangs.
- **RadioFoundry entry says "no answer from RadioFoundry's debug link"**: the radio is not running a RadioFoundry build with the loader option, or
  the cable is on a different port. Use the switched-off method above instead.
- **The radio does not start after a backup**: remove and refit the battery, then switch it on.
