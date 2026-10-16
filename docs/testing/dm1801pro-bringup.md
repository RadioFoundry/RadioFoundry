<!-- SPDX-License-Identifier: MIT -->

# DM-1801 Pro bring-up checklist

The order to bring the DM-1801 Pro port (`make BOARD=dm1801pro`) up on a radio, what each step proves, and what to
change when it doesn't work. Steps 0 to 3, 7 and 8 passed on 2026-10-15 (hardware-dm1801pro.md 12). Hardware facts:
[hardware-dm1801pro.md](../spec/hardware-dm1801pro.md).

## 0. Before anything

1. **Back up both flashes** (1 MB program flash, 16 MB data flash). The port never writes the data flash below
   0x700000 and never touches the boot loader, but a backup is the only way back from a mistake.
2. Confirm update mode before the first flash: it is the recovery path for every step below. The loader stays in it while
   PTT and SK1 are held at power-on ([hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.2). Push the cable fully
   into the jack, or the loader doesn't answer. `python3 tools/dm32_flash.py --model dm1801pro --dry-run IMAGE`
   checks the image and the cable first.
3. Flash: `python3 tools/dm32_flash.py --model dm1801pro build/dm1801pro/odr_dm1801pro.bin` over the programming cable.
   The erase takes about 13 s. `odrflash flash IMAGE --board dm1801pro` wraps the same tool, and the stub for a radio
   that runs ODR with `CONFIG_STUB_ENTRY=y` ([tools/odrflash/README.md](../../tools/odrflash/README.md#the-dm-1801-pro));
   neither odrflash route has flashed a radio yet.
5. The debug link runs on the same cable at 115200 (`ODR_PORT=/dev/cu.usbserial-... tools/odr_debug.py diag`). The
   cable's plug switches the internal speaker off: unplug it to listen.
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
| No audio on either band | The cable is plugged in (it mutes the speaker); `PIN_AMP_EN` (PTA16) or `PIN_SPK_SOURCE` (PTB15) |
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
| `tune(100000)`, `audio(true)` | A station, audible | The tuner's audio route; the amplifier (`PIN_AMP_EN`) is shared with the channel audio |
| `seek` | Stops on stations | Seek threshold (`cfg.seek_th`) |
| Receive a channel signal | The broadcast mutes while the channel has traffic | Core policy, not the board |

## 7. Accelerometer

The diagnostics page has an `acc` line: x, y and z in milli-g. Lay the radio flat: z reads about +1000 or -1000 and x
and y near 0; tilt it and the values follow. `accel none` means the chip didn't answer: check `PIN_ACCEL_SCL` /
`PIN_ACCEL_SDA` and the address (`ACCEL_ADDR` in `accel_dm1801pro.c`; 0x18 if the chip's address pin is low). `accel id
NN` means the chip answered with an ID the driver doesn't accept (this radio's SC7A20 reads 0x11).

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

## 10. VOX and the microphone level

The level comes from the codec ADC's mic PCM ([hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 12). The
programming cable in the speaker/mic jack cuts the microphone: unplug it for the talking steps, or read the values
from the diagnostics page.

| Step | Expect | If not |
|---|---|---|
| FM channel, VOX off; read the features view twice (`vox.level`), then the diagnostics line `mic lvl` | `vox.mic=1`; `mic lvl ... on`, and `n` grows by about 100 a second while the view is read | `off` or `n` stuck: the PCM interrupt or AUDIO_CONTROL b4 (`c7 ac` line, bit 4) |
| Quiet room, then talk at the radio from 10 cm | `lvl` and `pk` low in silence, well up while talking | Flat while talking: the samples don't carry the microphone in receive (PTB14, the codec ADC mute, CR_ADC b7 in the `cd` lines) |
| VOX on (Quick tile, FM), dummy load, talk | Keys on speech, drops `vox_tail_ms` after it | Keys in silence: the noise floor is above the threshold; lower `vox_level` or check `pk` |

## 11. After this

DMR voice needs the codec hosted (it is the DM-32UV's: [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 6.5) and the air band its own receive path. The
open items are in [hardware-dm1801pro.md](../spec/hardware-dm1801pro.md) 10.
