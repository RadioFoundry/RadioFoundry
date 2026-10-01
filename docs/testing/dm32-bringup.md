<!-- SPDX-License-Identifier: MIT -->

# DM-32UV bring-up checklist

The DM-32UV port (`make BOARD=dm32`) builds, links in the stock memory map and passes its host tests, but it has
never run on a radio. This is the order to try it in, what each step proves, and what to change when it doesn't
work. Every step names the knob in the source that the likely failure points at.

Hardware facts: `docs/spec/hardware-dm32.md`, `docs/spec/modem-hrc7000.md`, `docs/spec/codec-dm32.md`.

## 0. Before anything

1. **Back up both flashes** with the stock tools (hardware-dm32.md 13.4): the 1 MB program flash and the 16 MB data
   flash. The port never writes the data flash below 0x700000 (the stock codeplug and the factory calibration live
   there), and it never touches the bootloader, but a backup is the only way back from a mistake.
2. Check that update mode works with the stock firmware: hold **PTT + SK1** while turning the radio on; the green LED
   lights (13.2). This is the recovery path for every step below: if an image doesn't boot, take the battery off,
   enter update mode and load the stock file again.
3. Build and pack:

   ```
   make BOARD=dm32                                   # csky-elf GCC: tools/build-csky-toolchain.sh
   tools/dm32_pack.py build/dm32/odr_dm32.bin build/dm32/odr_dm32_BFUV32.bin
   ```

   For DMR voice, merge your own stock codec first (never commit or share the result):

   ```
   tools/dm32_codec.py --stock DM32.01.01.049.bin    # writes build/dm32/odr_dm32_codec.bin
   tools/dm32_pack.py build/dm32/odr_dm32_codec.bin build/dm32/odr_dm32_codec_BFUV32.bin
   ```

4. Load the image in update mode: `tools/dm32_flash.py build/dm32/odr_dm32_codec.bin` (a raw image or the
   `_BFUV32.bin` file, over the programming cable; `--stock DM32.01.01.049.bin` goes back), or the Baofeng updater
   with the `_BFUV32.bin` file. To keep the stock codeplug and get it onto ODR, run `tools/dm32_migrate.py all
   --image build/dm32/odr_dm32_codec.bin` instead: it backs the codeplug up with qdmr, flashes, and writes it back
   once ODR runs ([dm32-migration.md](../spec/dm32-migration.md), users:
   [the manual](../user/19-moving-from-stock-dm32.md)). Both tools are untested on a radio: run them with `--dry-run`
   first, and the codeplug part only after step 2 works.

Run the first boots with the antenna on and **no PTT**. Transmit only into a dummy load, from step 7 on.

## 1. It boots

| Look for | Proves | If not |
|---|---|---|
| Radio stays on after you let go of the knob (power latch) | crt0 ran, clocks, `board_early_init` (latch first) | Nothing ran at all: check the image starts with `psrclr ie` at 0x0300C000 (`csky-elf-objdump -d build/dm32/odr_dm32.elf | head`); re-enter update mode |
| Backlight on, then the Home screen | RTOS tick (Timer 1), LCD controller, ST7789 init | Backlight but no picture: `display_dm32.c` (LCDC_CFG 0x303, IO_DIPLEX2 hand-over). Picture upside down or mirrored: MADCTL in `display_dm32.c` (0xC0 now; try 0x00, 0x40, 0x80) |
| Colors right (black background, not white) | inversion on | `st7789_t.invert` |
| Picture fills the panel, no tearing seams | 2x scaling, window | `st7789_write_rows` |
| Hangs before the screen | interrupts or the scheduler | `platform/hrc7000/irq.c`: the PIC may deliver all sources on autovector 10 instead of vectors 32+n (open item); the irq entry then reads the wrong source. `freertos/port.c` / `portasm.S` for a hang after the first task switch |

## 2. The debug link on the programming cable

The DM-32 has no USB: the host link (debug link, CPS, MMDVM hotspot) runs on UART0, 115200 8N1, through the
programming cable (CH340 or FTDI).

```
tools/odr_debug.py --port /dev/cu.usbserial-XXXX version
tools/odr_debug.py --port /dev/cu.usbserial-XXXX diag
tools/odr_debug.py --port /dev/cu.usbserial-XXXX screenshot -o /tmp/dm32.png
python3 -m pytest tests/hil -v -rs --radio-port /dev/cu.usbserial-XXXX -k "dm32 or boot or ui"
```

| Look for | Proves | If not |
|---|---|---|
| `board=DM-32UV`, `display=120x160x16`, features with `HOSTLINK,DEBUGLINK` and no `USB` | UART RX interrupt, ring buffer, TX | Garbage: baud divisor (`uart.c`, 46 for 115200 at APB 84 MHz; 45 is the old port's) or APB not 84 MHz (clock.c skipped the PLL because the bootloader left it on: check `CLK_MGR_CTRL`) |
| `diag` lines ticking (`uo 0` = no UART overruns) | the debug link task | |

## 3. Keys, encoder, PTT, LEDs, power

| Check | If not |
|---|---|
| Every keypad key moves the UI as its label says (6.1 table); `odr_debug.py view --section input` shows the physical key bits | Rows / columns or the matrix map in `input_dm32.c`; phantom keys: raise the 5 us settle delay |
| SK1 (below PTT) = monitor, SK2 = talk target; holding SK1 doesn't kill the column 3 keys' neighbors | `direct[]` in `input_dm32.c` |
| Encoder: one step per detent, right direction | `keymatrix_encoder_step` sign; one count per edge of B (6.3): if a detent gives two, count one edge only (`hrc_pin_irq` in `input_dm32.c`) |
| PTT shows as key bit 20 | GPIO 0x2B |
| Red LED on transmit, green on receive | swap `PIN_LED_RED` / `PIN_LED_GREEN` in `pins.h` (the two sources disagree) |
| Turning the knob off saves and powers down | GPIO 0x10 sense in `power_dm32.c` |
| `bat` line: raw about 600-690 and 7000-8400 mV on a charged pack | ADC channel C set-up in `power_dm32.c`; the scale raw / 8.2 = V x 10 |
| Backlight dimming levels look even | `set_backlight` assumes FP counts against PER (duty = percent x 2); if half brightness is already full, use the percentage |

## 4. Data flash and calibration

`diag`: `flash C84018 cal factory` (or `852018` on Puya-flash radios), `txc` not 128, `pa` values not 1408.

| If | Then |
|---|---|
| `flash 000000` or `FFFFFF` | SPI0 set-up in `flash_dm32.c` (BAUDR 4 = 21 MHz; try 8), CS pin 0x12 |
| `cal none` | The factory page (block ID 2 in the last byte of a 4 KB page in 0x1000..0xC8000) wasn't found: dump the codeplug area through the stock CPS and compare (`calibration_dm32.c`) |

## 5. FM receive

Tune the FM VFO to a known signal (NOAA 162.550 MHz, a local repeater output) and to a quiet channel.

| Check | If not |
|---|---|
| Audio from the speaker with the squelch open | Speaker path: GPIO 0x2F low and 0x35 high (`radio_dm32.c audio()`); C7000 `fm_play` bit; codec line out |
| `rssi` / `nz` move with the signal; the squelch closes on a quiet channel | FD6818 reads (`fd6818_read` timing). The squelch uses the FD6818 noise byte (0x65): if its scale isn't dB-like, map it in `radio_dm32.c status()` |
| RSSI in dBm roughly right against a signal generator | the offset in `status()` (raw - 135 now, a guess) |
| CTCSS / DCS RX opens only on the right tone | C7000 sub-audio (`hrc7000_set_subtone`); the FD6818 has its own CTCSS detector if the C7000's doesn't work here |
| Both bands: 145 MHz and 435 MHz | LNA selects 0x22 / 0x24, band words |

## 6. Hotspot / CPS on the cable (optional)

MMDVMHost or a CPS on the same serial port: the host link switches by what it receives (`core/hostlink`).

## 7. FM transmit (dummy load, power meter, SDR or service monitor)

| Check | If not |
|---|---|
| Carrier on frequency within a few hundred Hz | TX center / SIG_CENTER (factory TX center byte) |
| Power levels 0-8 rise monotonically; level 8 near 5 W VHF / 4 W UHF | `dm32_cal_power_level` and the 11.3 fractions; DAC B must be powered (PD_CTRL bit 1 clear) |
| Deviation about 2.5 kHz (12.5 kHz channel) with normal speech; no distortion | `hrc7000_fm_tx` deviation, mic gain (GCR_ADCL 11 dB) |
| CTCSS on the air at the right frequency | C7000 TX sub-audio; or switch to the FD6818's (`fd6818_set_tx_ctcss`) |
| PA off and receiver back on PTT release | `tx_off()` |

## 8. DMR receive

Select a DMR channel with traffic (or let the SDR bench send a call), then `diag`.

| Diag line | Healthy | If not |
|---|---|---|
| `DMR on+cb tk N` | ticks climb by about 33 per second once a signal gives the chip timing | No ticks: modem interrupts 30 / 31 not arriving (PIC vectors, `irq.c`), or the modem not in DMR (`hrc7000_dmr_enter`) |
| `ts a/b si c` | RX timeslot (b) and system (c) interrupts counting | |
| `V H T I` | voice, headers, terminators counted in a call | Bursts deframed but dropped: color code (`cc` line) or CRC flags |
| `cc 1 ts 1 <RX_TYPE_INFO>` | the call's color code and slot | |
| Voice heard from both slots of a repeater, and a simplex call | `HRC7000_RX_NEXT_SLOT_XOR` in `hrc7000_dmr.c` (1 now): if every other voice burst is lost or only one slot plays, build with `-DHRC7000_RX_NEXT_SLOT_XOR=0` |
| `codec 049` (or 046) and clean audio | without a merged codec DMR is silent; garbled audio: PCM byte order (`audio_dm32.c`), the codec call parameters (`vocoder_dm32.c`: flags 0x1840 / 0x800, E[0x1736] / D[0x1166]) |

## 9. DMR transmit (dummy load and an SDR or a second DMR radio)

| Check | If not |
|---|---|
| A second radio decodes our call (ID, talkgroup, voice) | send types (LCSS on B-E per the user guide; F is 0x58 by the formula, the user guide's example says 0x50: try `e->lcss = false` if the embedded LC isn't decoded) |
| Bursts stay inside their 30 ms slot at every power level (SDR waterfall or `tools/bench`) | the DMR PA delay (`DMR_PA_DELAY_US`, 28 ms, Timer 3) |
| `pa` on the `tx` line about 30 ms; `rf` counts one per burst | PA off rule (20 ms after the RF TX interrupt) |
| Through a repeater: the repeater keys up and relays both directions | repeater timing after TX (`tx_finish`) |

## 10. Long run

An hour on a busy DMR channel and an hour of FM scanning: `uo`, `ov` (event overflow), `aud ov / un` stay near 0,
no stalls (`st`), no resets.

## 11. Air band AM and broadcast FM (once the board glue exists)

The glue isn't written yet ([airband-and-broadcast.md](../spec/airband-and-broadcast.md), parked). When it is:

| Check | Settles |
|---|---|
| The stock radio (or a scanner) on the local ATIS; ODR on the same frequency: AM audio, `am=1` in the view, read back FD6818 0x47 / 0x30 | which chip demodulates AM, the 0x47 AF type (spec 2.3 A1) |
| No audio: the other level of the speaker source switch (GPIO 0x2F), then the C7000 path | AM audio route (A2) |
| A strong and a weak station; a generator at -100 dBm with `rssi` on the link | AM AGC and filter (A3), the AM squelch offset (A4) |
| PTT on an air channel and on a split channel with TX in 108-137 MHz | deny screen, red LED off, nothing on the SDR |
| The RDA5807M chip ID (0x58xx at 0x11) on PTA25 / PTA26 | it is there, the SDA pull-up (B1, B3) |
| A strong local station with each reference clock setting; with and without the headset | reference clock (B2), audio path (B4), antenna (B5) |
| A call on the channel while listening | the broadcast mutes and comes back 2 s after |

## Open items to settle on the bench

- PIC: vectored (32 + source, assumed) or one autovector.
- L2_STATUS slot polarity (`HRC7000_RX_NEXT_SLOT_XOR`).
- LED colors, panel orientation, backlight PWM scale.
- FD6818 init writes to 0xB3 / 0xB4 / 0xC7 (sent with bit 7 set, exactly as the working firmware does).
- C7000 or FD6818 sub-audio for CTCSS TX and RX.
- Codec: 049 vs 046 window, D / E state sizes, the stock call constants, PCM byte order.
- UART divisor 46 (0.9 % slow) or 45 (1.3 % fast).
- RSSI dBm offset and the noise scale for the squelch.
