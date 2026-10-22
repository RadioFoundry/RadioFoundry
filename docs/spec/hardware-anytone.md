<!-- SPDX-License-Identifier: MIT -->
# Anytone AT-D868UV / AT-D878UV / AT-D578UV hardware

Status: research, 2026-10-12, facts only. ODR has no Anytone board. The owner has an AT-D868UV (hardware version and
MCU not yet checked). This page collects what is known about the family, what an ODR port would need, and what is
still missing. The survey entry is in [radios.md](radios.md). The AT-D868UV pin map in section 4 comes from the
owner's stock firmware image, read as data.

**Summary.** These radios are a GD32F303 (or, on late D578UVs, an Artery AT32F43x) MCU driving a Sicomm SCT3258 DMR
baseband over an 8-bit parallel host port. The SCT3258 runs DMR layers 1 and 2 and the AMBE+2 vocoder itself, and
voice audio goes codec ↔ SCT3258 over I2S without passing through the MCU. The RF chip is the AT1846S (or a
compatible QA4818N), which ODR already drives. On the AT-D868UV, every bus (the SCT3258 host port, the LCD, the SPI
NAND, the AT1846S and I2C) is bit-banged on GPIO: the stock firmware uses no SPI, I2C or EXMC peripheral. Loading our firmware looks possible: native USB, a documented vendor
update mode, and SWD pads with no read-out protection on the units documented so far. The cost is the baseband.
ODR's burst-level modem and vocoder interfaces don't map onto it directly, and features that need the AMBE bits
(privacy, the hotspot voice bridge) aren't possible unless a vocoder bypass is found.

## Confidence tags

| Tag | Meaning |
|---|---|
| **DS** | Sicomm CT3258 / SCT3258 datasheet and "Packet Interface of CT3258" v1.0 (2015). Copies mirrored on hackaday.io; their license is unclear, so cite them, don't copy them into this repository |
| **PKG** | The vendor's public update packages (D868UVE 1.40/2.40, D878UV and D878UVII PLUS 3.08N, D578UV PLUS 2.08, D578 PRO 1.21): their instruction PDFs, release notes and file listing |
| **COM** | Community teardown and mod pages, public protocol notes from flash tools (prose only), GitHub issues |
| **FW** | The stock application images (.CDD) read as data: peripheral register accesses, GPIO setup and call arguments. Facts only; no vendor code or data is copied here |
| **INF** | Inferred by us from the above; not seen directly |

---

## 1. Models and chips

| Item | AT-D868UV | AT-D878UV / UV PLUS, 878UVII PLUS | AT-D578UV / PLUS / PRO (mobile) | Tag |
|---|---|---|---|---|
| MCU | GD32F303VET6 (512 KB flash, 64 KB RAM) on V1 and early V2 boards; GD32F303VGT6 (1 MB, 96 KB) on late V2 | GD32F303VGT6. 878UVII: not found | GD32F303VKT6 (3 MB flash, 96 KB RAM) until 2022-07-01, then Artery AT32F435/437 | COM; PKG for the 578 change |
| DMR baseband | Sicomm SCT3258 (CML Microcircuits sells it as SCT3258TD) | SCT3258 | SCT3258 | COM, PKG |
| Vocoder | AMBE+2 inside the SCT3258. No separate DVSI chip | same | same | DS, COM |
| Audio codec | TI TLV320AIC3204, I2S to the SCT3258 | same | same | COM |
| RF transceiver | AT1846S, 26 MHz TCXO | AT1846S | 4× QA4818N (pin-compatible with the AT1846S): two as the VFO A/B local oscillators, two as receivers. True dual receive | COM |
| External flash | 1 Gbit SPI NAND, Toshiba TC58CVG0S3HRAIG (V2) | 1 Gbit; 878UVII: 2 Gbit (the stock firmware won't boot on 1 Gbit) | 2 Gbit TC58CVG1S3HRAIG | COM |
| FM broadcast | RDA5802N | RDA5802N | RDA5802N | COM |
| PA | 2× NXP AFT05MS006 | same | 2× AFT05MP075N | COM |
| Audio amp | HM8872 | HM8872 | 2× YD1517P, push-pull | COM |
| GPS | Fitted on some units (the owner's has one; community pages say none). USART1, 9600 baud, NMEA | ATGM336H or T3RV1 (GPS + BDS) | ATGM336H | COM; FW for the 868 |
| Bluetooth | none | PLUS: Beken BK3260 module. 878UVII: a daughter board with Bluetooth and an FSK receiver for APRS, with its own firmware ("BHT_BOARD") | PLUS: the same daughter board; "BT01" module | COM, PKG |
| Display | ST7735S-compatible, 128×160, RGB565, 8-bit 8080 parallel bus (section 4.2) | 1.77" color TFT, 160×128 (INF from the size); controller not found | not found | FW; COM, INF |
| Accelerometer | NXP MMA8653FC (I2C 0x1D, WHO_AM_I 0x5A). The firmware probes for it, so it may not be fitted | not checked | not checked | FW |
| Other MCUs | none known | the BT/APRS daughter board | the BT/APRS board and a small MCU in the handheld microphone | COM, PKG |

The 868UV ships two firmware lines, 1.xx and 2.xx (package "D868UVE-FW-1.40&2.40"). Which hardware each line is
for isn't stated in the package (PKG). The line probably follows the board version (INF).

## 2. SCT3258 baseband

### 2.1 Chip

| Item | Finding | Tag |
|---|---|---|
| Package | QFN64. Core 1.2 V, I/O 2.5–3.3 V | DS |
| Clock | 12.288 MHz input; on-chip PLL up to 110.592 MHz | DS |
| Host port (HPI) | 8-bit asynchronous parallel bus, Intel or Motorola timing. PIO1 at reset selects which (low = Intel). HOBIB is a handshake line for the host. 64-word FIFOs | DS |
| Audio | Two serial ports: one for the codec (I2S, 16-bit stereo, 24 kHz), one for an optional external vocoder (AMBE-3000 timing modes) | DS |
| Firmware | Held in the chip's own flash: application ("UserFlash") and several vocoder images (the vendor tool loads "Vocoder1, 2, 4, 5"). The vocoders are enabled per chip by separate "Activation" and "Voice Activation" files | DS, PKG |
| Slot timing | PIO3 can output a 60 ms slot signal (high = slot 1) | DS |

### 2.2 Boot

After every reset the chip waits for **exactly 1024 16-bit words** over the HPI (16-bit mode, low byte first). It
runs them at once. That block normally holds a second-stage loader, which starts the application from the chip's
flash. DMR and dPMR use different boot blocks, and the block chosen sets the chip's mode. On success the chip sends
the packet `84 A9 61 00 02 20 17 01` (DS).

So the MCU firmware must hold this 1024-word block and send it at every power-on. It is Sicomm's code. ODR can't
ship it, so like the AMBE codec on other radios, it has to come from the owner's own radio.

The stock firmware keeps it in the application image (internal flash), not in the NAND (FW). The 2048 bytes are the
same in every image examined (868 V1.40 and V2.40, 878 and 878UVII 3.08n, 578 GD 2.08). SHA-256 of the block:
`743515b9f7cd90737c1fff29b652b7558bc5a56487ec38df4fdea8877e7a619a`. A mode setting (0–3) makes the firmware
replace one or two bytes while sending it (offsets 0x34C and 0x352, or 0x45C–0x45D); mode 0 sends it as stored.
What the modes select isn't known yet. An extraction tool can find the block in any stock image by its hash.

Reset sequence on the 868 (FW): two enable outputs go high, the PIO1 strap line is driven low (Intel mode), RSTN is
pulsed high for 100 ms and then held low for at least 500 ms. RSTN then goes high, the strap line is released to a
pulled-down input, and the boot block follows over the host port.

### 2.3 Packet interface

Commands and reports are packets with a header (`84 A9 61 ...`), a TYPE byte and fields. TYPE bits: 7 external
vocoder bypass, 6 codec bypass (the bytes go straight to the codec), 5 read/write, 4 near/far end, 3–0 packet type.
Fields cover vocoder selection (`VOCODER_SEL`: 4 = AMBE+2, 16 = analog), mode (`PROCESS_MODE`), color code,
call options, slot, work mode (RX / TX / duplex), gains, BER test, tones, record and playback, and calibration (DS).

Two DMR modes (DS, packet interface 2.3):

| Mode | The chip does | The MCU does |
|---|---|---|
| Easy mode (layer 3) | Layers 1–3 per TS 102 361: dialing, ID and group matching, call setup | `DMR_CALL_START`, `CALL_STOP`; reacts to match reports |
| Layer 2 mode | Layers 1 and 2: FEC, burst building, 4FSK, sync, slot timing | Layer 3 and up, through `DIGC_DATA_FRAME`: LC headers, terminators, the LC for voice superframes, data blocks. Frames must go to the chip every 60 ms, no more and no less often |

Voice in layer 2 mode: the MCU sends the full LC (9 bytes, without CRC) for the voice header, terminator and voice
bursts. The AMBE bits come from the chip's own vocoder (DS). Encoded voice passes over the packet interface
(`CHAN_D`, 36 bytes per 80 ms) **in dPMR mode only** (DS). The manual also has base station activation (continuous
outbound with CACH) and listen-before-transmit (DS 2.3.4–2.3.5).

### 2.4 Fit with ODR

ODR's `hal_dmr_modem` takes bursts and gives burst events. `hal_vocoder` converts PCM frames to and from 49-bit
frames. On the SCT3258:

| ODR function | Layer 2 mode | Tag |
|---|---|---|
| Voice calls, group and private, talker alias, embedded GPS | Yes: the stack builds the LC; the chip does the bursts and voice | INF |
| CSBK, data headers, rate 1/2, 3/4, 1 data (SMS, IP over DMR, LRRP, ARS) | Likely, through `DIGC_DATA_FRAME` data types. Needs confirming per data type | INF |
| Basic privacy (Motorola, Anytone BP) | Not by ODR: it XORs the AMBE bits, which the MCU never sees. The chip may have its own scrambler (not checked) | INF |
| Hotspot / MMDVM voice bridge, voice relay from the network | Not unless AMBE frames can be passed through in DMR mode | INF |
| ODR's own FEC and burst code (`core/dmr/fec`, `ambeFec`) | Unused: the chip does it | INF |
| Split duplex, the hotspot's BS outbound | Maybe, through base station activation; voice still needs the AMBE bits | INF |
| The software vocoder (`hal_vocoder`) | Not needed. A stub reports the vocoder as hosted by the modem | INF |

A port therefore needs a modem driver that turns the burst queue into `DIGC_DATA_FRAME` commands, plus a way for the
core to know that voice frames don't pass through the MCU on this radio, so the features above are hidden
(a `board_t` capability, so other radios are unaffected).

### 2.5 What the stock firmware does (FW, 868)

| Item | Finding |
|---|---|
| DMR mode | Layer 3 easy mode for voice: the firmware sets `DMR_FLC`, `DMR_CC`, `DMR_CALLED_ID_BCD`, `DMR_OWN_ID_BCD`, `DMR_CALL_OPTION`, `DMR_CALL_SLOT`, `DMR_OFFSET` and starts calls with `DMR_CALL_START`. `DIGC_DATA_FRAME` is used only in a small group of functions (data services) |
| Encoded voice to and from the MCU | Used in DMR mode for call recording and playback. `VOCODER_IO_SET` is sent with values 0x04, 0x08, 0x10, 0x20 and 0x40 (0 to end), and voice moves in `CHAN_D` fields with 36-byte payloads, stored in and read back from the NAND. 0x40 feeds the modulator from the MCU, so a recorded call can be transmitted. So the v1.0 manual's "dPMR only" note doesn't hold for the chip firmware these radios run. 36 bytes per 80 ms fits four 72-bit (FEC-coded) AMBE+2 frames (INF) |
| Codec | Programmed through the SCT3258: about 20 small codec-bypass packets (TYPE bit 6), plus `CODEC_SELECT`, `SET_I2C_ADDRESS` and `I2C_OPERATION`. The MCU has no direct link to the codec |
| Scrambler | `SCRAMBLER_SEED` is used (the radio's basic privacy, INF) |
| Other fields | Gains (`MOD_GAIN`, `DEMOD_GAIN`, `DIGI_MIC_GAIN`, `DIGI_SPEAKER_GAIN`, `MISC_GAIN`), `ANALOG_CONFIG`, `CTC_PARAM`, `ANALOG_SUBAUDIO`, `DC_OFFSET`, `IQCOMP_COEF`, `AFC_CONFIG`, `MODOUT_CONFIG`, `EQUALIZER_FILTER`, `SQ_LEVEL`, `BER_REPORT`, `PARITY_ENABLE`. Two field IDs, 0x2B and 0x3B, aren't in the v1.0 manual |

This changes the fit in 2.4: if `CHAN_D` carries the AMBE frames in DMR mode, privacy and the hotspot voice bridge
are possible on this baseband, either in easy mode with the vocoder routed through the MCU or in layer 2 mode. To
confirm on the radio: the `CHAN_D` frame format in DMR mode and its timing against the 60 ms slots.

## 3. Update and recovery

| Item | Finding | Tag |
|---|---|---|
| PC link | Native USB on the MCU, a CDC serial port (`/dev/ttyACM*`). The cable is a plain USB cable; the baud setting makes no difference | COM |
| MCU update mode, 868/878 | Hold PTT + PF3 (top key) at power-on; the red LED blinks | PKG |
| MCU update mode, 578 | Hold MENU + EXIT at power-on (or while switching off); the status LED blinks. The radio stays in update mode until an update is written | PKG |
| Vendor tool | CPS → Tool → Firmware and Icon Update ("QX_Firmware_Update"): opens a `.spi` file, shows model and version, 921600 baud, a "Duplex" option and a grayed-out "Encrypt" option | PKG |
| Update files | Three per update: `.spi` (14 or 51 bytes, the file the user opens), `.CDI` (278 bytes for firmware, about 11 KB for icon sets), `.CDD` (the payload). The icon (graphics) update uses the same tool and file set | PKG |
| Image sizes (.CDD) | D878UV 3.08n: 939,168 bytes. D878UVII 3.08n: 939,240. D578UV2 GD 2.08: 1,020,408. D578UV2 AT 2.08: 1,040,008. Icon sets: 3.7 MB (878), 2.1–7.3 MB (578) | PKG |
| Model check | The 578 has separate GD and AT images, and the CPS refuses the wrong one, so the files carry a model / MCU identity | PKG |
| Transfer | The host sends "UPDATE", then the .CDD in 32-byte packets with an additive checksum. The image is staged in the external flash and installed on the next boot | COM |
| After the update | "MCU reboot": PTT + PF1 at power-on (578: P2 + channel knob), confirm, set the date and time. This initializes the radio and erases the codeplug | PKG |
| Encryption, signing | None on the images examined: each .CDD is a plain Cortex-M image starting with its vector table (FW). Community band-limit edits also suggest no signature. The tool's grayed "Encrypt" option shows the format can carry encryption | FW, COM, PKG |
| Bootloader | Anytone's own, internal flash 0x08000000–0x08003FFF; application from 0x08004000 | COM |
| SWD | Pads marked "AF" beside the MCU. A documented 868 → 878 conversion read and wrote internal flash with an ST-LINK, so read-out protection was off on those radios. Recovery from a bad write: hold NRST (MCU pin 14) low | COM |
| SWD while running | The stock application turns SWJ off early in its init (AFIO remap: SWJ fully disabled) and reuses PA13/PA14 as inputs. A debugger must connect under reset (NRST held low) | FW |
| SCT3258 update | Separate "Boot Mode for Sct": hold # + the top key at power-on. Sicomm's SCT_PORT tool talks to the chip through the radio's COM port (9600 shown) and writes an Intel-hex file (`SCT3258TD_UpdateFlash_V2_01_07BF.hex` in the 578 package) | PKG |
| SCT pass-through | In that mode the MCU is a transparent bridge: USB OUT data is written to the host port unchanged, and whenever HOBIB signals data the MCU reads the FIFO and returns it over USB. The PC tool speaks the SCT3258 packet protocol itself | FW |
| MCU ROM bootloader | Whether BOOT0 is reachable on the board is not known (GD32F303 and AT32F43x both have a ROM USB/UART loader) | — |

## 4. AT-D868UV pin map

From the 868 stock application (V2.40, FW). V1.40 drives the same buses on the same pins; it differs in a few key
inputs (pull-down instead of pull-up, not yet confirmed) and adds outputs PC10 and PC11. All buses are bit-banged:
the firmware sets and clears pins one at a time. ODR can do the same, or move a bus to a hardware peripheral where
the pins allow it (the NAND, LCD and host port pins don't match the GD32F303 SPI or EXMC pin options).

### 4.1 Buses

| Bus | Pins | Notes |
|---|---|---|
| SCT3258 host port (HPI) | Data PD8–PD15. HCSN PD2, HWRN PD3, HRDN PD4 (active low). HOBIB PD5 (input, low = data waiting) | Intel mode. The data pins switch between push-pull output and input (whole PD8–15 group). Single-byte and block reads and writes, with a short busy-wait between strobes |
| SCT3258 control | RSTN PD6. PIO1 strap PC12. Enables PA8, PC6 (both high before reset) | Sequence in 2.2 |
| LCD (8080, 8-bit) | Data PE7–PE14 (bit 0 on PE7). RS/DC PB10 (low = command). CS PB12, WR PB13 (active low). RD PE15 (held high). RESET PB11 | Section 4.2 |
| Keypad | Drive lines PE7–PE14 (shared with the LCD data bus). Sense inputs with pull-ups: PA10, PA14, PE1, PB1, PB2, PC5 | Scanned between LCD writes |
| Channel knob | PC8, PC9 (quadrature, EXTI on both) | Direction from comparing the two pins at each edge |
| AT1846S | SCLK PB3, SDIO PB4 (bidirectional), SEN PB5 (active low) | 3-wire: 8-bit address (bit 7 set = read) then 16-bit data, MSB first |
| SPI NAND | CS PB8, CLK PB14, MOSI/IO0 PB15, MISO/IO1 PA15 | Mode 0, MSB first. Reads use dual output (two bits per clock on PA15 and PB15). JEDEC ID 0x9F: the firmware accepts manufacturer 0xC8 (GigaDevice) and 0x98 (Toshiba) |
| I2C | SCL PB6, SDA PB7 | Devices: 0x10 (RDA5802 FM receiver, sequential register writes), 0x1D (MMA8653FC accelerometer) |
| GPS UART | USART1: TX PA2, RX PA3, 9600 baud | NMEA parser present ($GPRMC, $GNRMC, $GPGGA, $GPGSV, $PMTK commands). The owner's 868 has a GPS fitted |
| USB | PA11, PA12 (USB FS device) | CDC serial |

The audio codec is not on the MCU's I2C bus. The SCT3258's PIO0/PIO1 are an I2C master in its preloaded firmware
(DS), so the codec is probably set up by the baseband (INF, not confirmed).

### 4.2 Display

The stock init sequence is an ST7735S-class controller (FW): reset low 100 ms, then 500 ms; SLPOUT, 120 ms; frame
rate B1/B2/B3 = 01 01 01; inversion B4 = 03; power C0 = 28 08 84, C1 = C0, C2 = 0C 00, C3 = 8C 2A, C4 = 8A EE;
VCOM C5 = 0C; MADCTL = 0x88; gamma E0/E1; COLMOD = 0x55 (16-bit RGB565); column 0–0x80, row 0–0xA0. That is a
128×160 panel addressed in portrait. ODR's `st7735` driver covers the controller; it needs a parallel-bus
transport instead of SPI.

### 4.3 Other I/O

| Pin | Use | Tag |
|---|---|---|
| PA4 | DAC channel 1 (12-bit): TX power level. Value from the calibration tables (4.4) by power level and frequency | FW |
| PA5 | DAC channel 2 (configured, use not seen) | FW |
| PA6 | 12-bit PWM (TIMER2 channel 0) | FW |
| PB0 | 12-bit PWM (TIMER2 channel 2) or plain output | FW |
| PB9 | PWM (TIMER3 channel 3), switched off to analog when idle; probably the tone output | FW, INF |
| PC0, PC1, PC2 | ADC channels 10, 11, 12 (battery voltage and others) | FW |
| PD0 | Input with interrupt, acted on only while transmitting: high → PC4 and PE6 on and DAC0 at the power level; low → all off. Most likely the SCT3258 PIO3 slot-timing output, so the PA is keyed per TDMA slot | FW, INF |
| PA0 | Input, pull-up, read at start-up (power key or wake, INF) | FW, INF |
| PA7, PE0 | Outputs switched with the tone generator and on RX/TX changes. PA7 is probably the speaker amplifier enable | FW, INF |
| PA1, PC3, PC4, PC13, PD1, PD7, PC7, PE2–PE6 | Outputs; function not identified. PE3–PE5 are turned off in the SCT pass-through mode | FW |

The two PWM outputs (PA6, PB0) are probably the LCD backlight and a tuning voltage; not checked (INF).

### 4.4 Calibration and settings in the NAND

The radio's calibration is in the SPI NAND (FW): a primary copy at byte address 0x02F60000 and a fallback at
0x02F80000, used when the primary fails its check. It is about 1.5 KB of small blocks at fixed offsets. Two of them
match the bands: four 81-byte tables (one value per MHz, 400–480 MHz) and four 39-byte tables (136–174 MHz), one
of each per TX power level. The TX power DAC value is taken from them. The radio settings are a 34-byte record
with an A/B pair at 0x02501000 and 0x02521000.

A port must read this calibration from the owner's NAND and never overwrite it. If the stock programming protocol
addresses the NAND directly (the public flash-tool notes use 32-bit addresses in this range), the calibration can
be read over USB before ODR is installed. Not yet confirmed.

### 4.5 AT1846S setup

The stock firmware programs the AT1846S with a common table, two band and bandwidth tables (each with two variants)
and the AGC gain table on register page 1. TX and RX switch through register 0x30. The values are kept in the
reference directory notes, not here. ODR's AT1846S driver already covers the chip; the per-radio values come from
the owner's radio and its calibration.

## 5. What a port needs

| Part | ODR has | New work |
|---|---|---|
| MCU | `platform/stm32f4` (a different family) | `platform/gd32f30x` (clock tree, GPIO, timers, DAC, ADC, USART, USB FS device). The AT32F43x for late 578s is a second platform |
| Baseband | none | `drivers/modem/sct3258`: bit-banged HPI on GPIO, reset and boot block load, packet framing, layer 2 mode, 60 ms pacing, calibration |
| Boot block | none | A tool that finds the 2048-byte block in the owner's stock image by its hash (2.2) and puts it where our firmware reads it, like the codec tools |
| RF | `drivers/rf/at1846s` | A bit-banged 3-wire transport; per-radio tuning tables; QA4818N compatibility on the 578 |
| Audio codec | none | Codec setup sent through the SCT3258 (codec-bypass packets or `I2C_OPERATION`); register values to work out from the TLV320AIC3204 datasheet |
| Storage | SPI NOR drivers | SPI NAND driver (bit-banged, dual-output read) with bad-block handling; two vendors (also needed by the DB25-D and GD-88) |
| Display | `st7735`, `st7789` | An 8080 parallel transport for `st7735`, sharing its data lines with the keypad scan |
| Flashing | odrflash (DFU, UART loaders) | The Anytone update protocol over USB CDC, or SWD under reset for the first bring-up |
| Calibration | per-radio readers | A reader for the NAND calibration block (4.4), read-only |
| Core | — | A capability for "voice frames stay in the modem", in case `CHAN_D` pass-through turns out unusable in DMR |

## 6. Open questions

1. **The owner's 868UV**: board version and MCU (VET6 with 64 KB RAM, or VGT6). Read the MCU marking, or read the
   ID code over SWD. The 868 images of both lines fit 64 KB RAM.
2. **The unidentified 868 outputs** (4.3) and the DAC and PWM roles. Next step: follow each from the radio-state
   code (TX on/off, squelch, backlight level), then confirm on the radio with a meter.
3. **878 and 578 pin maps**: the same method on their images. The 878 has a larger display.
4. **The .CDD format**: plain, a Cortex-M image linked at 0x08004000 (868/878) or 0x08006000 (578) (FW). Still open:
   the 878 trailer after the stated length, how the identity check works, and whether the stock update mode loads
   any image. The update protocol lives in the bootloader, which isn't in the update files.
5. **Boot block modes**: what the four patch modes select (DMR or dPMR, clock, HPI options).
6. **AMBE pass-through in DMR mode.** The stock firmware uses `VOCODER_IO_SET` and `CHAN_D` in its DMR build
   (2.5). Still to confirm on the radio: the 36-byte `CHAN_D` format and timing in DMR. It decides whether privacy
   and the hotspot voice bridge are possible.
7. **Data types** that `DIGC_DATA_FRAME` accepts in layer 2 mode (CSBK, rate 1/2, 3/4, 1, UDT) and whether
   the chip filters any of them. (The stock firmware uses easy mode for voice, 2.5.)
10. **Calibration over USB**: whether the stock programming protocol reads NAND addresses directly (4.4).
8. **Read-out protection** on current production radios, and whether BOOT0 is reachable for recovery.
9. **878UVII MCU**, and **the AT32F43x pin map** on late 578s.

## Sources

- CT3258 datasheet (2015): <https://cdn.hackaday.io/files/1642237026116832/CT3258_datasheet.pdf>
- SCT3258 datasheet v2.0: <https://cdn.hackaday.io/files/1642237026116832/SCT3258_datasheet_v2_0.pdf>
- Packet Interface of CT3258 v1.0: <https://cdn.hackaday.io/files/1642237026116832/CT3258_Packet_Interface.pdf>
- CML Microcircuits SCT3258TD: <https://www.cmlmicro.com/products/sct3258td-dmr-dpmr-analog-processor/>
- D878UV technical mods: <https://hamradiosouthernrepeaters.co.uk/images/878Mods/878techmods.html>
- D578UV technical mods: <https://www.hamradiosouthernrepeaters.co.uk/images/578Mods/578techmods.html>
- 868UV mods: <http://members.optuszoo.com.au/jason.reilly1/868mods.htm>
- F5UII, converting an AT-D868UV to an AT-D878UV over SWD:
  <https://www.f5uii.net/en/convert-at-d868uv-to-at-d878uv-analog-aprs-roaming/amp/>
- anytone-flash-tools protocol notes (prose only):
  <https://github.com/reald/anytone-flash-tools/blob/master/at-d878uv_protocol.md>
- dmrconfig issues on Anytone USB: <https://github.com/OpenRTX/dmrconfig/issues/46>
- Vendor update packages (Anytone / BridgeCom): D868UVE FW 1.40 and 2.40, D878UV PLUS and D878UVII PLUS 3.08N
  (2025-06-14), D578UV PLUS 2.08 (2024-06-19), AT-D578 PRO 1.21; their "Firmware Update Instructions", "D878UV IC
  3258 Base Band Update" and "D578UV FW Update Including MCU Reboot" PDFs
