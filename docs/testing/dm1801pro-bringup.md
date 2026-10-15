<!-- SPDX-License-Identifier: MIT -->

# DM-1801 Pro bring-up checklist

The DM-1801 Pro port (`make BOARD=dm1801pro`) builds and passes its host tests, but it has never run on a radio. This
is the order to try it in, what each step proves, and what to change when it doesn't work. Hardware facts:
[hardware-dm1801pro.md](../spec/hardware-dm1801pro.md).

## 0. Before anything

1. **Back up both flashes** (1 MB program flash, 16 MB data flash). The port never writes the data flash below
   0x700000 and never touches the boot loader, but a backup is the only way back from a mistake.
2. Confirm update mode before the first flash: it is the recovery path for every step below. The loader stays in it while
   PTT and the key on PTB26 are held at power-on ([hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.2); which key
   that is isn't known, so try SK1 and then each side key. `python3 tools/dm32_flash.py --model dm1801pro --dry-run IMAGE`
   checks the image and the cable first.
3. Flash: `python3 tools/dm32_flash.py --model dm1801pro build/dm1801pro/odr_dm1801pro.bin` over the programming cable.
   It has only run against a simulated loader. The first real run settles the unchecked parts: the update file header,
   the NAK behavior and the erase time. `odrflash` knows the board's flash map but has no serial route yet.
4. Build: `make BOARD=dm1801pro` (csky-elf GCC: `tools/build-csky-toolchain.sh`).

## 1. Power and display

| Step | Expect | If not |
|---|---|---|
| Switch on | The radio stays on after the power key is released | Wrong polarity of the power key (`PIN_POWER_KEY`, `power_dm1801pro.c`: the level at start-up counts as "on"); wrong latch pin |
| Backlight | Comes on | `PIN_LCD_BACKLIGHT` |
| Picture | The Home screen, upright and readable | Blank: the controller's init (`display_dm1801pro.c`, `st7567.c`); mirrored: `seg_reverse` / `com_reverse`; faint or black: `contrast`, `regulator`, `bias`; shifted: `col_offset` (4) |
| IO mux | The display and the flash both work | `io_init` in `board.c`: PTC2-PTC8 must be GPIOs, SPI0's pins on the peripheral |

## 2. Keys

Open the Diagnostics page (Settings > Diagnostics). The `keys` line shows the raw scan: matrix bit = strobe x 4 + input
(bits 0-19), the four direct pins in bits 20-23. Press every key in turn and write down the bit. Then fix, in
`input_dm1801pro.c`: the `map` table (matrix keys) and `direct_keys` (PTT, SK1, SK2, SOS). Until then keys are mixed up.

## 3. Flash, calibration, battery

| Step | Expect | If not |
|---|---|---|
| Diagnostics `flash` | A JEDEC ID (`C84018` GD25Q128, `852018` Puya) | Chip select, the SPI0 mux, `hrc_ssi_init` clock |
| `cal` | `factory`; the TX center, PA points at 145 and 435 MHz are non-zero | `none`: the page tagged 2 wasn't found; compare with a dump (hardware-dm1801pro.md 6.4) |
| Change a setting, switch off and on | It stays | The write floor and flash map in `board.c` |
| `bat` | Rises with a full battery; the percentage is close | The ADC divider isn't known: compute the scale from a multimeter and set it in `power_dm1801pro.c` |

## 4. FM receive

Tune a known local signal on VHF and UHF. Audio, RSSI (`rssi` line) and squelch should follow the signal.

| Symptom | Likely cause |
|---|---|
| No audio on either band | `PIN_SPK_SOURCE` / `PIN_MIC_PATH` polarity, or the speaker path isn't the DM-32UV's |
| Audio on one band only | The path pins' band side is inverted (`radio_dm1801pro.c` `path_pins`; hardware-dm1801pro.md 5.5) |
| Wrong frequency or very weak | `fd6818_variant_dm1801pro` tables, the RF bus speed (`RF_BIT_SCALE_US`), `PIN_RF_CS` |
| Narrow / wide swapped | The 12.5 kHz word (`bw_fm_12k5`, hardware-dm1801pro.md 5.2) |

## 5. FM transmit

Only with a dummy load or a test set on an allowed frequency. The radio refuses to transmit when the calibration isn't
read.

1. Key at low power: the carrier is on frequency with no splatter. Check the PA ramp and the TX path pins.
2. Compare the three power levels with a meter: L < M < H, the nominal values are
   [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.4's open item (which level uses which array).
3. Deviation and modulation balance with a test set; the modulation calibration byte path is the DM-32UV's.

## 6. Broadcast FM tuner

The tuner's hardware side is built (`CONFIG_BCAST_FM`, on); the screens that drive it are parked
([airband-and-broadcast.md](../spec/airband-and-broadcast.md)), so check it through the debug link or a test build that
calls `board.bcast`.

| Step | Expect | If not |
|---|---|---|
| `power(true)` | Returns true: the chip answers at 0x11 with ID 0x58 | `PIN_BCAST_EN` polarity, `PIN_BCAST_SDA` / `PIN_BCAST_SCL`, the I2C speed (`bus.half_us`), the reference clock mode (`cfg.clk_mode` in `bcast_dm1801pro.c`) |
| `tune(100000)`, `audio(true)` | A station, audible | `PIN_FM_AUDIO` is the wrong switch or needs `PIN_SPK_SOURCE` as well |
| `seek` | Stops on stations | Seek threshold (`cfg.seek_th`) |
| Receive a channel signal | The broadcast mutes while the channel has traffic | Core policy, not the board |

## 7. Accelerometer

The diagnostics page has an `acc` line: x, y and z in milli-g. Lay the radio flat: z reads about +1000 or -1000 and x
and y near 0; tilt it and the values follow. `accel none` means the chip didn't answer: check `PIN_ACCEL_SCL` /
`PIN_ACCEL_SDA` and the address (`ACCEL_ADDR` in `accel_dm1801pro.c`; 0x18 if the chip's address pin is low).

## 8. Vibration motor

The radio vibrates for about 0.3 s when it powers on. No pulse: put a meter on PTB24 (`PIN_VIBRATION`); if the pin moves
and the motor doesn't, the pin isn't the motor driver (hardware-dm1801pro.md 8 is inferred, no label names it).

## 9. DMR

DMR is built (`CONFIG_DMR`), without voice. Do this after FM receive and transmit work. The Diagnostics page starts with the
DMR counters.

| Step | Expect | If not |
|---|---|---|
| Select a DMR channel on a local repeater or hotspot | `tk` (ticks) climbs by about 33 a second, `ts` interrupts count up | No ticks: the modem never got timing; check the DMR mode register set (`dmr_table` in `fd6818_dm1801pro.c`) and the RX path |
| Receive a transmission | `H` (headers) and `V` / `T` counters rise, `crc` stays low | Many `crc` or `u` (unusable): the bandwidth word 0x3048 or the DMR register set |
| Hear a call | Silence: the codec is located but not hosted yet | |
| Transmit on a dummy load (data call or a short key-up) | A burst per slot on a meter or SDR, `tx`, `rf` and `ok` rise | `un` (underrun) or `rj`: the per-burst switch takes too long (`dmr_rf_switch`, `RF_BIT_SCALE_US`); no carrier: `dmr_pa_on`, the 28 ms PA delay |

## 10. After this

DMR voice needs the codec hosted (it is the DM-32UV's: [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.5) and the air band its own receive path. The
open items are in [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 10.
