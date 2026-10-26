<!-- SPDX-License-Identifier: MIT -->

# GD-88 bring-up plan

The plan for the first day with a Radioddity GD-88 (Kydera DR-880UV). Follow it top to bottom. Each step says what to
run, what success looks like, what to write down, and which open item it settles. Nothing in the port has run on a
GD-88: every pin role, chain bit and register value in the code is read from the original firmware and unverified.
Section 4 lists every `TODO(gd88)` and the display driver's `TODO(panel)` in one table.

Facts: [hardware-gd88.md](../spec/hardware-gd88.md) (sections 9-14), [rx2-link.md](../spec/rx2-link.md),
[boot/README.md](../../boot/README.md), [stub/README.md](../../stub/README.md). The other checklists are
[dm32-bringup.md](dm32-bringup.md) and [dm1801pro-bringup.md](dm1801pro-bringup.md).

The radio has two SoCs. **A** has the cable, display, keys, GPS and the transmitter. **B** is the second receiver; it
has no cable and is reached through A. Two boards, two images: `gd88` and `gd88b`.

## 1. What you need

| Item | Why |
|---|---|
| The GD-88, a charged battery, a charging base | The battery reading (step 6) needs a known state of charge |
| The programming cable (K1 jack) | The only link: install, debug link, CPS. The radio has no USB |
| The original firmware files for **A** and **B** from Radioddity's package: A `B49E.D64.EOPSAB.021` (2025-01-19) and B `B49C.D64.EHRNAB.012` (2025-01-17) | Build 021 is the only A build with the debug console the first install goes through. Both files are the way back (section 2). The codec comes out of them: each SoC has its own copy. Keep both files (and the whole package) |
| A dummy load (SMA, 50 ohm, 10 W or more) and a short antenna | The antenna for receive tests; the load for every transmit |
| The bench Pi / SDR (`docs/testing/dmr-bench.md`) | A known FM and DMR signal for steps 9-14, and the check of the transmitted signal in step 15 |
| A multimeter, a fully charged and a nearly flat battery if you have them | Battery and volume end points |
| A build host with csky-elf GCC (`tools/build-csky-toolchain.sh`), Python 3, `pyserial` and `pytest` | Both boards need csky-elf |

Set up the build before the radio arrives:

```
make BOARD=gd88 && make BOARD=gd88b        # build/gd88/rfoundry_gd88_install.bin, build/gd88b/rfoundry_gd88b_install.bin
make BOARD=gd88 GD88_TX=1 BUILD=build/gd88-tx   # kept for step 15 only; never flash it before then
```

The codec comes from the original files. Put them in `codecs/` as [codecs/README.md](../../codecs/README.md) says (the
directory is ignored by git), and `make` writes `rfoundry_gd88_codec.bin` and `rfoundry_gd88b_codec.bin`: the install image with
the codec merged in. Without the files, `tools/hrc7000_codec.py --board gd88 --stock A_FILE` and `--board gd88b
--stock B_FILE` do the same. The web flasher can also take the codec from the radio (step 5). **Never commit, share or
upload a merged image.**

## 2. Safety rules for the day

1. **Always write `*_install.bin`** (or its `_codec` version): the first stage, then the application. `stubtool
   --write-app` refuses the application alone over a first stage, because the loader would stay in update mode (white
   screen, red LED) until an IAP install. `--replace-stage` is for recovery work only; do not use it in this plan.
2. **Never write below 0xC000.** The vendor IAP loader lives at 0x03000000-0x0300BFFF on each SoC. The first stage
   keeps its entry bytes `2C 11 01 C0`; the loader runs the application only if they are there. `pad.py` fails the
   build if the stage loses them.
3. **Transmit stays off** (`GD88_TX` unset, the default) until step 15. No PTT, no TX test, `--allow-rf` not passed. A's
   power control is not known; the `GD88_TX=1` build only ramps a DAC as the DB25-D does and may drive the PA wrongly.
4. **Dummy load for every transmit**, lowest power, short keys. Never key into the antenna jack open.
5. Back up before the first write (step 3 for A). B has no RadioFoundry route for a backup before its first stage (step 7): the
   original B file is its backup.
6. After a rejected or interrupted command, check what state the radio is in (a write may have run) before the next one.

How to get back to the original firmware (the IAP loader is never written, so it always works):

1. Radio off, cable connected, the vendor CPS closed.
2. Hold **P2** (the upper side key) and turn the radio on with the volume knob. The status LED is red and the top line
   reads `IAP_A` or `IAP_B`. Release P2. It can take more than one try. Long-press P2 to switch between `IAP_A` and
   `IAP_B`.
3. Use the vendor IAP updater (Windows, it needs a COM port at 115200) to write the A file to `IAP_A` and the B file to
   `IAP_B`. Write the codeplug again afterwards with the vendor CPS.

Record what P2 power-on shows (step 3). The web flasher's radio entry, its troubleshooting texts and the user guide
follow the spec: the LED lights red and the top line shows `IAP_A` or `IAP_B`. If the radio shows something else, fix
them (`[flash.bootloader]` in `boards/gd88/board.toml` and `boards/gd88b/board.toml`, the `_gd88` and `_gd88b` texts in
`site/i18n/en.json`).

## 3. Step by step

A note on ports: A's cable is `PORT` (for example `/dev/cu.usbserial-XXXX`). The debug link on A runs at 115200 on the
same cable as the host link once RadioFoundry runs. Tools: `tools/rfoundry_debug.py --port PORT ...`, `stub/stubtool.py PORT ...`.

### 1. Identify the cable

Plug the cable into the computer, with no radio. No driver is needed for the check.

```
system_profiler SPUSBDataType | grep -B2 -A8 -i -E "serial|ftdi|ch34|cp210|pl23"     # macOS
lsusb -v 2>/dev/null | grep -E "idVendor|idProduct|iProduct"                            # Linux
ls /dev/cu.usb* /dev/ttyUSB*                                                            # the port name
```

Record VID:PID, the product string and the serial number. It settles hardware-gd88.md 5 question 2 (which converter
chip: FTDI 0403:6001/6015, CH340 1A86:7523, CP210x 10C4:EA60, or something else). The web flasher's port filter and
the `tests/hil` USB ID (1FC9:0094, which is the STM32 radios and does not match this cable) depend on it: always pass
`--radio-port` for this radio. The converter's logic level is still question 3 and needs a scope or the radio opened;
a 3.3 V cable on a 3.3 V UART is the expectation, and the DB40-D's documentation says 3.3 V.

### 2. Look at the original firmware

Before anything else, switch the radio on with its original firmware and record, for both SoCs if the menu shows them:
the firmware versions (must be A 021 for the console route; if A is older, update it with the vendor updater first:
the console is only in 021), the display (orientation, any odd color), what P2 power-on shows (section 2), and the
battery state. Read the codeplug with the vendor CPS and keep it.

### 3. Back up A through the console (build 021)

Use the web flasher (the GD-88 entry; [docs/dev/website.md](../dev/website.md)). With A running its original build 021,
the page finds the debug console on the cable (`@AT` frames, hijack word 0x18000848, free RAM from 0x523D0), loads the
universal stub into RAM through it (about 4 minutes at 115200), checks the radio's model string `+DRS-880plus`, sends
the stub its profile and checks the echo. Then it reads the **whole 2 MB program flash** and offers a backup bundle
(it also keeps it in the browser). Download it and copy it somewhere safe.

| Look for | If not |
|---|---|
| The page says build `021` and loads the stub (HELLO, then CONFIG echo) | Console not found: the build isn't 021, or the frame/hijack word differs on this radio. Record the version string, the console's answer to a bare `@AT` frame and what the radio shows. The other way in is the vendor IAP loader (step 7) |
| A 2 MB backup, readable twice with the same CRC | Re-read; a mismatch means a flaky cable |

Record: the stub's `stub RAM:` line, the flash IDs, the clock rates the host measured. Settles: the console route on a
GD-88 (PR checklist "Flashing"), `[flash.stub] tested` in `board.toml`, the A program-flash backup. From the backup, check the
words at 0x0300C000 (original start-up code), 0x031FD000 (`lcd` panel record) and 0x031FE000 (calibration copy,
`TUNE`): they are needed in steps 6 and 7. The files stay out of git.

The console route works on build 021 only. If it fails, stop and look; do not try the vendor-IAP route in the web flasher
(PR #132: unverified, packet format assumed the same as the DB25-D).

### 4. B's state

B on its original firmware does not speak RadioFoundry's link, so RadioFoundry can't read it, and there is no RadioFoundry route to B's flash
until its first stage is on. Only this can be checked now: with the radio running its original firmware, the vendor
CPS/updater show the B version (record it). Resolves nothing yet; it just fixes the file you will need to write back.

### 5. Install A

Same page, with the install image. It writes `rfoundry_gd88_codec.bin` (first stage at 0x0300C000, application at
0x0300D000, codec at 0x030E8000) as changed 4 KB sectors, the first application sector last, each read back, then a
final CRC pass, and restarts. From a terminal instead (A at power-on, or A running RadioFoundry with `CONFIG_STUB_ENTRY`):

```
python3 stub/stubtool.py PORT --board gd88 --probe
python3 stub/stubtool.py PORT --board gd88 --write-app build/gd88/rfoundry_gd88_codec.bin --boot reset
```

The boot ROM's serial window (UART0) is not known to be reachable on the GD-88 (hardware-gd88.md 4), so the first
install goes through the console with the web flasher. After RadioFoundry is on A, `stubtool` works through it (`--rfoundry-enter`
instead of the power-on wait); the first stage then also offers the handshake at power-on. `--boot reset` after a write left some boards unresponsive
on the DM-32 and DM-1801 Pro: if the host link is silent after the install, power-cycle (battery out if needed).

Then:

```
tools/rfoundry_debug.py --port PORT version
tools/rfoundry_debug.py --port PORT diag
python3 -m pytest tests/hil -v -rs --radio-port PORT -k gd88
```

| Look for | Resolves / if not |
|---|---|
| The radio stays on after the knob is turned on, backlight on, a Home screen | Nothing at all: the first stage didn't start the application, or the latch (PTB13) is wrong. Power-cycle; if the loader's update screen shows, the stage's entry word is missing: re-install the `_install` image through the IAP route |
| `version`: `board=GD-88`, `display=320x240x4`, features with `HOSTLINK,DEBUGLINK` and no `USB` | `test_identity`. Garbage on the link: baud (115200), PTA9/PTA10 mux (UART1) |
| Settings written and kept over a power cycle | The NAND storage (blocks 1920-1951), step 6 |

### 6. A: boot, display, keys, power, ADC, NAND, calibration

Open the diagnostics page (Settings > Diagnostics) or `rfoundry_debug.py diag`. Do these in order; each one that fails is
fixed in the file named, then rebuild, reinstall (`stubtool --rfoundry-enter --write-app`) and retest.

**Display.** The Home screen, upright, readable, right colors, no tearing.

| Check | If not | Resolves |
|---|---|---|
| `lcd vN rec` or `dflt` in diag: the panel record at 0x031FD000 was read (`rec`) and N is its variant. `dflt` means no record found; the default is variant 3 | `dflt`: dump 0x031FD000 from the backup and look at what follows `lcd` (digit or number): `display_gd88.c` takes it as the next byte | TODO display_gd88.c record layout and default |
| Picture upside down, mirrored, rotated | Variant sequences in `drivers/display/ili9341/ili9341.c` (MADCTL 0x48 variants 0 and 2; the register-style panel's R03 0x1030 orientation for variant 1, ID 0xC505) | ili9341.c orientation TODOs |
| Washed out or too contrasty colors | Gamma sets 0xE0/0xE1: variants 0 and 2 use the controller's reset gamma. Try the DB25-D/vendor-like values on the display, compare with the original firmware's picture | gamma TODOs (variants 0, 2) |
| Flicker, a torn first frame, or a hang in the LCD ready wait | `LCDC_CFG_VALUE` 0x303 (the DM-32UV's): try the neighbors | timing word |

Take screenshots (`rfoundry_debug.py screenshot -o /tmp/gd88.png`) of every variant tried. Record the panel variant, record
bytes, timing word that worked.

**Keys and encoder.** `rfoundry_debug.py view --section input` shows the raw key bits; press every key.

| Check | If not / resolves |
|---|---|
| Digits, `*`, `#`, P1, P2, P3 do what `board.c` says (P2 direct on PTB9, P1 PTB10, P3 PTB11). PTT is PTA9 low | Matrix map in `input_gd88.c`. PTT: check the line stays high with the cable idle |
| The six front keys (soft left, soft right, green, red, up, down; matrix codes 1-6): write down which physical key gives which code | Fix `map[]` in `input_gd88.c` (the code-to-key assignment is a guess; the key names, `keymap[]` and the drawing's `key-*` tags stay). TODO input_gd88.c |
| Encoder: one event per detent, clockwise = up. `rfoundry_debug.py`'s HIL: `pytest tests/hil -k encoder --manual` | Swap `PIN_ENC_A`/`PIN_ENC_B` if backwards; `ENC_STEPS` 1 vs 2 if a detent gives two events. TODO input_gd88.c |
| PTA30 (parked low): any key or effect | TODO pins.h |

**Power key and latch.** Turn the radio off with the knob while RadioFoundry runs: it saves and powers down. If PTB12 is the knob's
switch contact (high while on), power-off by the knob works and nothing else does; if PTB12 is a separate key, a press
should power off after 3 polls (300 ms). Write down which one. Resolves TODO power_gd88.c (PTB12's role).

**Battery and volume.** `diag`'s `bat` line: raw and mV; a full pack reads 8.2-8.4 V, a flat one about 6.0 V; compare
with the meter. The formula is V x 10 = (raw + 5.7818) / 7.8551. Turn the volume knob fully down and fully up and read
the ADC channel 1 raw at each end: the code assumes 0 and 1023 (`VOL_FULL_RAW`) with 21 levels. Resolves TODO
power_gd88.c (knob end readings) and `test_battery`.

**NAND and calibration.** `diag`: `nand ID ...` (not 0000 or FFFF; expected Toshiba TC58CVG1S3HRAIJ, 2 Gbit, 2 KB pages,
64 pages a block), `cal flash #N c M` or `cal nand #N`, and the `nst`/`set` lines (settings load, no errors).

| Look for | If not / resolves |
|---|---|
| The NAND ID is a real chip; a settings change survives a power cycle | `nand_gd88.c` geometry: read the parameter page and compare with the TC58 numbers; `GD88_NAND_RFOUNDRY_FIRST` 1920 and 32 blocks are the DB25-D's: check the top of the chip is not used by the original data. TODO nand_gd88.c, TODO gd88_rf.h range |
| `cal flash #N` and `cal nand #N` found, the newer one used; the TX center, PA values not zero | Compare with the page in the backup (0x031FE000, `TUNE` + counter; the NAND copy at 0x280000). Wrong: `gd88_cal.h`. RadioFoundry only reads both and never writes them |

The calibration items 0x602, 0x606-0x60B are not used; if the page is on hand, record their values and which radio
settings they match later (step 15). Resolves TODO gd88_cal.h.

**RF chips.** `diag`: `rf ... lo ...`. Both must read the BK4819 ID (0x4819), or the radio is receive-less (`supported`
false). If they read FD6818 (0x6818), 0x7813, AT1846S (0x1846) or AT1141 (0x1100), record the pair: the driver does not
know those. Resolves TODO radio_gd88.c (fitted chips) and hardware-gd88.md 14 item 1. Also record what the debug UART
prints (`RF_LO_IC_CHIPID`) if the board is accessible.

At the end of step 6, `python3 -m pytest tests/hil -v -rs --radio-port PORT -k gd88`: `test_identity`,
`test_panel_variant` (skips with `dflt`), `test_battery`, `test_nand_and_calibration` pass. `test_second_receiver`
fails until step 9.

### 7. B: first install through IAP_B

B on its original firmware does not speak RadioFoundry's link and has no cable. Its first install is through the vendor IAP
loader's `IAP_B` target. The web flasher offers it: on the Install Receiver B step, A answers but reports B down, and
the page offers Use Factory Update Mode with the `IAP_B` steps. It writes with the DB25-D's IAP packets (`iap.js`
`Iap`), which are assumed, not known, to match. Try the web flasher first; the vendor updater below is the fallback.

First find out who answers in each mode. This is read only. Enter update mode with `IAP_A` on the top line and send
`Flash Read ` for one NAND page (address 0, count 1: `iap.js` `readPacket`, or the DB25-D's IAP script on the bench
Pi). Record the version banner and the page. Switch to `IAP_B` and repeat. A different banner or different NAND data
means the cable reaches B's loader directly (hardware-gd88.md 4, "How IAP_B may reach B"). The same answers mean A
answers both, and relays to B or switches it by another way. The loader's `Flash Read ` reads NAND only, so this
gives no backup of B's program flash. Also check that A's PTT still works over the cable afterwards: RadioFoundry's B leaves
PTA9 and PTA10 as inputs, as the original firmware does.

1. Radio off, cable in, hold P2 and turn on. Long-press P2 until the top line reads `IAP_B`.
2. In the web flasher, select Use Factory Update Mode and Install. If the loader does not answer or refuses, save the
   page's log and use the vendor IAP updater instead: write `rfoundry_gd88b_install.bin` (or its `_codec` version) as B's file. This is the
   **unverified** part: the updater takes the vendor files; RadioFoundry's install image starts at 0x0300C000 like them, so it
   should be accepted, but the loader's checks (header? checksum? size?) are not known. If it refuses, record the
   updater's message and the file sizes.
3. Restore A: IAP mode with `IAP_A`, or switch the radio off. A keeps RadioFoundry (the loader only erases as far as the image
   reaches).

| Look for | If not |
|---|---|
| The updater reports "Write Flash Successfully" and "IAP Checksum right" for `IAP_B` | The loader wants a header or another size. Write the original B file back to leave B usable, record the message and stop; this needs the IAP packet format (hardware-gd88.md 14 item 8) |
| After power-on, B's LEDs (PTA16, PTB17) do something and `rfoundry_debug.py rx2 info` shows `up` (step 8) | B is dead or never started the application: IAP_B again with the original B file |

Settles hardware-gd88.md 5 question 1 in part (does the cable reach B when `IAP_B` is selected) and 14 item 8.

### 8. B: the link, HELLO, TUNE, status, LEDs

```
tools/rfoundry_debug.py --port PORT rx2 info
tools/rfoundry_debug.py --port PORT diag | grep '^b '
tools/rfoundry_debug.py --port PORT log --follow
```

| Look for | Resolves / if not |
|---|---|
| `rx2 info` shows `present bridge restart up`; diag `b up vN` with B's firmware version text | Link is UART3 (PTA13/PTA14), 115200. `down`: wrong pins on A or B, B not running (step 7), or the mux. Dump `A5 5A` frames if a scope is on hand |
| `dual.receivers` is 2 in `rfoundry_debug.py view --section features` | `test_second_receiver` |
| HELLO flags: bit 0 set (B decodes DMR) | |
| Put the radio on dual watch with two FM channels: B's status (RSSI, noise, active) follows a signal on its channel | TUNE/ACK/STATUS. `ACK` result nonzero: B refused (look at `LOG` messages in the log) |
| The status LEDs: LED 1 (PTA16) while A transmits, LED 2 (PTB17) while it receives: which one lights, which color | Swap `PIN_LED1`/`PIN_LED2` in `gd88b/pins.h` if reversed. TODO gd88b/pins.h colors |
| Pull B out of the link (restart it from the debug link: `rx2 restart`): A finds it again within a few seconds and re-sends TUNE/AUDIO/LED | the 1.5 s down rule and HELLO_REQ |

Update test: with B on RadioFoundry, update it through A (this is how every later B install goes):

```
python3 stub/stubtool.py PORT --board gd88b --write-app build/gd88b/rfoundry_gd88b_codec.bin --boot reset
```

The host asks A for the bridge (`DL_CMD_RX2`), A sends `STUB`, B writes `STAY` and jumps to its first stage, the stub
loads on UART3 (115200, no baud change), the image goes in, B restarts and A restarts 10 s after the last byte.
Success: `rx2 info` says `up` again with the new version. The web flasher's done step does the same and asks A whether
B answers. Record the time. Settles the B-through-A route on a radio.

### 9. FM receive on A and B

Tune the FM VFO to a known signal (the bench SDR, NOAA 162.550, a local repeater) and to a quiet channel, on VHF and on
UHF, on A's VFO and on B's (dual watch).

| Check | If not / resolves |
|---|---|
| Audio from the speaker with the squelch open; `rssi` and noise follow the signal; the squelch closes on a quiet channel | A: BK4819 main/LO set-up and the IF scheme (`radio_gd88.c`: IF 51.550 MHz, LO at f + IF); DAC C `DAC_C_INIT/RX` (the DB25-D's values; record what works: TODO radio_gd88.c DAC C); the band path (LO GPIO4/GPIO5: VHF GPIO5 high, GPIO4 low) |
| Right frequency on both bands (SDR waterfall at a known carrier) | the IF/LO frequencies, the page's reference trim |
| Narrow and wide bandwidth sound right | bandwidth words |
| CTCSS/DCS opens only on the right tone | the BK4819 detector |
| RSSI in dBm close to a signal generator or the bench's known level | the RSSI offset |
| B: the same, heard through A's speaker (A gives the speaker vfo 1 while B is heard) | B's RX path pairs and its audio source pair: mode 0 is taken as FM, mode 1 as DMR (TODO gd88b/pins.h modes and source pair). If B is silent in FM, swap the modes. B's PTB11/PTB12 roles (TODO): note if anything changes when they are set |
| The audio goes to the speaker, the headset (plug one in) or both, per the setting | the headset byte rides with the loudspeaker byte: TODO paths_gd88.c |

Record, for each of the combinations (FM/DMR x A/B x speaker/headset) which chain bits worked. The chain is visible as
the `chain` diagnostic line.

### 10. DMR receive on both, with the codec

Needs the codec merged (the build in section 1). Use the SDR bench's DMR call (`tools/bench/dmr_bench.py`, default 461.6875 MHz,
color code 2, talkgroup 9) and a channel with that frequency in the codeplug.

| Diag line | Healthy | If not |
|---|---|---|
| `DMR on+cb tk N` | ticks climb | no ticks: the modem not in DMR (`GD88_DMR_RF_MODE`, default 0x034C9860, the DB25-D's word: try the bench knob `make ... GD88_DMR_RF_MODE=0x...`); the IQ inversion; RF mode. TODO dmr_gd88.c |
| `ts a/b si c`, `V H T I` | slot interrupts, voice frames, headers | Bursts deframed but dropped: color code or CRC. Garbled voice or every frame with 3 bit errors: `HRC7000_RX_NEXT_SLOT_XOR` (0 here, as the DB25-D and DM-32UV) |
| `codec ...` and clean speech | the merged codec found in its slot (0x030E8000) | silence: codec missing or mismatched with the profile; check `test_codec_info` |

Do it on A, then on B (B decodes the call itself and sends audio via the audio source pair). The DMR input level is the
main chip's AF gain in DMR (`GD88_DMR_AF_GAIN`). Record the RF mode word, AF gain and which slot polarity worked.

### 11. Audio between A and B

With both VFOs in dual watch, one FM and one DMR, A and B alternately receiving: the speaker plays the one that is
active; when both are active, only one is heard (the core picks). Check:

- A's own receive plays through the speaker (chain byte 0) and B's through the same speaker (vfo 1).
- The volume knob scales both; the beep still works while B is heard.
- `AUDIO` on/off from A: B mutes within 100 ms of off.
- Class bits: if FM and DMR are swapped (class 1 = FM, class 2 = DMR is inferred), A or B plays the wrong source for a
  mode: swap in `paths_gd88.c`. Resolves TODO paths_gd88.c (classes, bytes 6/7, headset).

### 12. Broadcast FM

Needs `CONFIG_BCAST_FM` (on by default) and the antenna. Use the radio's broadcast mode with a strong local station.

| Check | If not / resolves |
|---|---|
| The tuner answers at I2C 0x11 on PTA7/PTA8 (the diagnostics or `view` shows the chip ID) | `IO_DIPLEX0` bits 5 and 6 (`DIPLEX0_I2C_GPIO`) are inferred: if the bus is dead, try the other bit combinations; the pads must be GPIOs. TODO bcast_gd88.c |
| Audio from the speaker (and the headset) | The chain's bit 6 with the amplifier bit as the tuner source is inferred (the vendor code sets it for "no source / tones"): if silent, try bit 6 in bytes 0/1 and PTC18. TODO paths_gd88.c bit 6 |
| The broadcast mutes and comes back after an FM or DMR call | |
| Stereo/mono, station search, RSSI | |

### 13. GPS

No fix needed: the module talks about 1 s after the power is on, indoors too. `diag`: `gps yes`, good sentences rising,
bad near 0. `gps no` with no bytes: PTB17 high (power), UART2 PTA11/PTA12 at 9600, the `$CFGPRT,,0,9600,3,3` sentence;
swap RX/TX if the pins are the other way (the notes say which pin is which only by inference). A fix outdoors in a few
minutes: Info > Position. Resolves the PTA11/PTA12 direction note in hardware-gd88.md 9.

### 14. Host link: CPS, PPP

Optional: `qdmr` or another CPS on the same port, and MMDVMHost, on the cable at 115200. Check the speaker still works with the cable in.

### 15. Transmit, last

Only after steps 6-14 pass. This is where the radio can hurt itself.

1. Build the TX image: `make BOARD=gd88 GD88_TX=1 BUILD=build/gd88-tx`. Keep the same codec merge. Install it with
   `stubtool --rfoundry-enter --write-app` (A running RadioFoundry).
2. Dummy load on the antenna jack, a power meter if you have one, the bench SDR or a spectrum analyzer in a
   directional coupler path to the load. Lowest power level.
3. FM TX, a few seconds, no modulation first (carrier only): measure the frequency (within a few hundred Hz), the
   power, and any spurs on the second band. Then speech or a tone: deviation about 2.5 kHz at 12.5 kHz.
4. Power control: the DAC B ramp in `GD88_TX=1` is the DB25-D's; A has no PA drive DAC (the vendor code writes DAC A
   with 0 for each of its 180 steps) so the power comes from the main BK4819's PA bias (register 0x36) and the page.
   Step the power levels low to high, one by one, and record the output at each. Do not go above what the load and the
   radio take (the GD-88 is a 7 W radio). Settles TODO gd88_rf.h (A's power control), hardware-gd88.md 14 item 2.
5. The TX line PTB2 (high = TX), the speaker-amplifier and TX route bits (bytes 6 or 7 depending on the volume: record
   at volume 6 and below, and above), and the other VFO's path during TX. Resolves the byte 6 / byte 7 question.
6. After the key-up: back to receive within a second.
7. DMR TX: the same, with a second radio or the SDR bench decoding the call (ID, talkgroup, voice). Bursts stay in
   their 30 ms slot (`DMR_PA_DELAY_US`); `GD88_DMR_TX_INVERT` if the second radio decodes noise. Through a repeater, if
   one is in reach.
8. Set the bench knob `GD88_TX_CENTER=N` for the reference trim sweep if the frequency is off (the page's TX center
   byte is replaced by N).
9. B never transmits (its TX pins stay low). Check B stays silent: the SDR sees nothing on B's TX path when A keys.

When all of this works, put the working power control in `radio_gd88.c` and `gd88_rf.h`, turn `GD88_TX` on by default
and remove the TODO.

### 16. The HIL suite

```
python3 -m pytest tests/hil -v -rs --radio-port PORT -k gd88                 # step 6 and 8 checks
python3 -m pytest tests/hil -v -rs --radio-port PORT                         # everything that doesn't transmit
python3 -m pytest tests/hil -v -rs --radio-port PORT --allow-rf --allow-service-stop    # after step 15, with the bench
```

Expected skips (every test skips cleanly, none should fail):

| Skipped | Why |
|---|---|
| `test_dm32.py` and the other boards' files | They skip on any other board |
| USB network and USB switch tests | No USB on this radio (`USB` is not a feature) |
| APRS receive | `CONFIG_APRS_RX=n` here (FM audio isn't given to the core as PCM); the transmit part needs `--allow-rf` |
| Split duplex and single frequency repeater tests | Not supported (`BOARD_UNSUPPORTED`: HOTSPOT_SPLIT, SFR) |
| Air band AM | `CONFIG_AM_RX=n`; the air band isn't wired |
| `rf` and `radio_tx` tests | Skip without `--allow-rf`; run them only after step 15, with the dummy load |
| CPS write, restarts | Need `--allow-codeplug-write` and `--allow-reboot` |
| `test_gd88.py::test_panel_variant` | skips with `dflt` (no panel record in flash) |
| `--manual` tests (encoder, keys) | run them with `-s --manual` in step 6 |

Any failure other than those is a real result: record the test and the diag lines.

## 4. Every `TODO(gd88)` and `TODO(panel)` in the code (25)

Grep: `grep -rn "TODO(gd88)" boards` and `grep -n "TODO(panel)" drivers/display/ili9341/ili9341.c`. "Step" is the
section 3 step that settles it.

| # | File:line | What is unknown | How to settle it | Step |
|---|---|---|---|---|
| 1 | `boards/gd88/input_gd88.c:25` | Which front key (soft left, soft right, green, red, up, down) each matrix code 1-6 is | Press each, read `view --section input`, fix `map[]` | 6 |
| 2 | `boards/gd88/pins.h:15` | PTA30's role (parked low from start-up) | Note any effect when high; leave low if none | 6 |
| 3 | `boards/gd88/input_gd88.c:44` | Encoder direction and steps per detent | Turn clockwise; swap `PIN_ENC_A`/`PIN_ENC_B` or set `ENC_STEPS` | 6 |
| 4 | `boards/gd88/display_gd88.c:20` | The LCD controller's timing word (`LCDC_CFG_VALUE`, the DM-32UV's 0x303) | Try neighbors if the picture tears or hangs; record the working one | 6 |
| 5 | `boards/gd88/display_gd88.c:28` | The panel record's layout past `lcd`, and the default variant without one | Read 0x031FD000 from the backup; `diag lcd vN rec/dflt` | 3, 6 |
| 6 | `drivers/display/ili9341/ili9341.c:59` | Variant 2 gamma sets 0xE0/0xE1 | Compare with the original firmware's picture; try values | 6 |
| 7 | `drivers/display/ili9341/ili9341.c:85` | Variant 0 gamma (as variant 2) | Same, only if a radio reports variant 0 | 6 |
| 8 | `drivers/display/ili9341/ili9341.c:108` | Register-style panel (variant 1, ID 0xC505): orientation (R03 0x1030) and the R00 writes' value | Screenshot; try the scan direction bits | 6 |
| 9 | `boards/gd88/power_gd88.c:15` | Whether PTB12 is the volume knob's switch contact or a separate key | Turn off with the knob and with a key press; watch `view --section input` | 6 |
| 10 | `boards/gd88/power_gd88.c:24` | The volume knob's end readings (taken as 0 and 1023) | Knob fully down and up, read ADC channel 1 | 6 |
| 11 | `boards/gd88/common/nand_gd88.c:8` | The NAND's geometry, and on B its chip select (PTA18 as on A) | Read the parameter page; `diag nand`; on B see #20 | 6, 7 |
| 12 | `boards/gd88/common/gd88_rf.h:96` | The NAND's size and whether its top is used (RadioFoundry's blocks 1920-1951) | Compare with the backup's NAND map and the chip; settings survive | 6 |
| 13 | `boards/gd88/common/gd88_cal.h:48` | The calibration page items 0x602 (4 bytes), 0x606, 0x607 (1 each), 0x608, 0x60B (3 each) | Record values from the page; match to modem fields at 0x100/0x104 by changing settings in the original firmware (needs the vendor tools) | 6, 15 |
| 14 | `boards/gd88/common/radio_gd88.c:21` | DAC C's role on each SoC (DB25-D's 1762/1780 are used) | Try values; the RSSI, the sensitivity or the frequency should change | 9 |
| 15 | `boards/gd88/common/radio_gd88.c:182` | Which RF chips are fitted (only a BK4819 pair is driven) | `diag rf ... lo ...` chip IDs | 6 |
| 16 | `boards/gd88/common/dmr_gd88.c:28` | The modem's RF mode word (`GD88_DMR_RF_MODE`, the DB25-D's 0x034C9860) | SDR bench call; try `GD88_DMR_RF_MODE=` variants | 10 |
| 17 | `boards/gd88/common/gd88_rf.h:18` | A's output power control (transmit) | Step the levels into the dummy load, measure | 15 |
| 18 | `boards/gd88/paths_gd88.c:13` | Chain classes (class 1 = FM, 2 = DMR inferred), byte 6 against 7 for the TX route, headset byte along with the loudspeaker, bit 6 as the broadcast tuner source | FM and DMR on A and B through the speaker/headset; TX at volume above and below 6; broadcast FM | 9, 11, 12, 15 |
| 19 | `boards/gd88/bcast_gd88.c:17` | `IO_DIPLEX0` bits 5 and 6 as the pads of PTA7/PTA8 (inferred) | Does the RDA5807 answer at 0x11 | 12 |
| 20 | `boards/gd88b/board.c:54` | Whether B's three DAC pins are wired as A's | Receive checks on B; compare RSSI with A; probe DAC C | 9 |
| 21 | `boards/gd88b/board.c:68` | B's NAND chip select (for the NAND copy of its calibration page) | Only if B's program-flash copy of the page is bad; otherwise leave | 7 |
| 22 | `boards/gd88b/pins.h:15` | B's RX path pairs: mode 0 = FM and mode 1 = DMR is taken | FM and DMR on B: swap if wrong | 9, 10 |
| 23 | `boards/gd88b/pins.h:21` | B's audio source pair to A: mode 0 = the main chip's AF (FM), mode 1 = the modem's line out | B audible in FM and in DMR through A | 9, 10, 11 |
| 24 | `boards/gd88b/pins.h:36` | PTB11 and PTB12's roles on B (low from start-up) | Note any effect when high | 9 |
| 25 | `boards/gd88b/pins.h:45` | The status LEDs' colors (LED 1 lights while A transmits, LED 2 while it receives) | Look | 8 |

That is 25 marks and 25 rows. Recount with the grep after any edit.

Items without a mark that this plan also settles (from [hardware-gd88.md](../spec/hardware-gd88.md) 5 and 14):

| What | Step |
|---|---|
| The cable's converter chip (5 question 2) | 1 |
| Whether `IAP_B` reaches B directly and the IAP packet format (5 question 1, 14 item 8) | 7 |
| The cable's logic level (5 question 3), UART0 pads (5 question 4), any USB path (5 question 5) | needs the radio opened; not part of day one |
| Link command ids of the original firmware (14 item 7) | not needed: RadioFoundry runs its own protocol |

## 5. What to update afterwards

| Where | Change |
|---|---|
| [hardware-gd88.md](../spec/hardware-gd88.md) | Tag each settled fact (sections 4, 5, 9, 10, 11, 13) as seen on a radio, replace the "Status: bring-up" header and the sentence "Neither has run on a radio yet" in 13; strike the answered questions in 5 and 14; add the VID:PID, the panel variant, the key table, the encoder direction, the battery and volume end points, NAND geometry |
| `TODO(gd88)` comments | Remove each one settled and put the real value in the code with a one-line "settled on the radio" note; the grep above must come back empty or with only the open ones |
| `boards/gd88/board.toml` `[status]` | The level from bring-up to the supported state, and the summary: what works (FM, DMR, dual watch, GPS, broadcast FM; transmit as far as it got) and what doesn't. The README, the feature matrix and [23-radios.md](../user/23-radios.md) tables are generated from it (`tools/boards.py docs`) |
| [docs/user/23-radios.md](../user/23-radios.md) GD-88 section | Describe the install and update mode (top line `IAP_A`/`IAP_B`, red LED, or what you saw), key names, the cable, the second receiver |
| User guide / manual radio list | Add the GD-88 where the supported radios are listed |
| `boards/gd88/board.toml` and `boards/gd88b/board.toml` | `[flash] stock_versions` (the original builds the console route works on: `021`), `[flash.stub] tested` true, the `[flash.bootloader]` steps (blank screen or `IAP_A`/`IAP_B`) |
| Web flasher | The cable's VID:PID in the port filter if it has one; the IAP route text; the done step's second-receiver check |
| [tests/hil/README.md](../../tests/hil/README.md) | The `test_gd88.py` row: what runs, what skips; add tests for the new facts (the keys, the volume ends, the LEDs) |
| [docs/testing/README.md](README.md) | Update the row for this page |
| This page | Move the results into "Open items"; follow the DM-32 page's shape |

Open items of the PR (#132) to close:

- [ ] Vendor IAP install route for A (the web flasher offers the DB25-D's; unverified): step 3 settles the console
  route; the vendor IAP route is step 7 for B and the fallback for A.
- [ ] Every pin role, the serial chain bits, key names (steps 6, 9-12, 15; table in section 4).
- [ ] Flashing: first stage, stub, IAP on both SoCs (steps 3, 5, 7, 8).
- [ ] RX / TX on both SoCs, DMR voice, audio routing between A and B (steps 9-11, 15).

Cross-radio changes in the PR (shared HR_C7000 ADC/UART, BK4819 GPIO, `gfx.c`, Makefile core dirs, `DL_CMD_RX2`): run
the HIL on the DM-1701, DM-1801 Pro and DM-32UV once the bench radios are free.

## Open items to settle on the bench

Fill this in as the steps run, in the shape of [dm32-bringup.md](dm32-bringup.md). Starting list:

- Cable VID:PID and the converter's logic level.
- The console route on a real build 021, and the time it takes.
- IAP packet format for `IAP_B` (and `IAP_A` as the fallback).
- Panel variant, orientation, gamma, timing word; encoder direction; navigation keys; PTB12.
- Battery and volume end points; NAND geometry; B's NAND chip select.
- RF chips fitted; DAC C on each SoC.
- B's mode and source pairs; the chain classes and bits (FM, DMR, headset, TX route, broadcast).
- The modem's RF mode word.
- A's power control and transmit.
