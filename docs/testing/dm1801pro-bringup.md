<!-- SPDX-License-Identifier: MIT -->

# DM-1801 Pro bring-up checklist

The DM-1801 Pro port (`make BOARD=dm1801pro`) builds and passes its host tests, but it has never run on a radio. This
is the order to try it in, what each step proves, and what to change when it doesn't work. Hardware facts:
[hardware-dm1801pro.md](../spec/hardware-dm1801pro.md).

## 0. Before anything

1. **Back up both flashes** (1 MB program flash, 16 MB data flash). The port never writes the data flash below
   0x700000 and never touches the boot loader, but a backup is the only way back from a mistake.
2. Find how the radio enters update mode and confirm it works before the first flash: it is the recovery path for every
   step below ([hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.2 lists what isn't known).
3. There is no flashing route for this board yet (`odrflash` has no entry for it). Loading the first image needs one.
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

## 7. After this

DMR needs the codec recipe and the modem glue and the air band its own receive path. The
open items are in [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 10.
