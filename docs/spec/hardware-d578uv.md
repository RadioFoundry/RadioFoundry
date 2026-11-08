<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# AnyTone AT-D578UV hardware

The AT-D578UV (also sold as the PLUS and the PRO) is a dual-band mobile: an MCU, a Sicomm SCT3258 DMR baseband with
its own vocoder, four RF chips (two receivers and two local oscillators) and an SPI NAND. Boards: `boards/d578uv`
(the GD32 builds) and `boards/d578uva` (the Artery builds, section 10).
The family survey (the 868 and 878 handhelds share most parts) is [hardware-anytone.md](hardware-anytone.md).

Sources: static analysis of the manufacturer's firmware (V2.08 and PRO V1.21, GD32 builds; Artery builds for
sections 1 and 10), the chips' public data sheets, and public photos. Nothing in this page has been checked on a radio yet:
`(inferred)` marks what was deduced from how the firmware uses a part, `(unverified)` what has no source.

## 1. Models and MCUs

The radio was built with two MCUs. Each needs its own firmware build; the manufacturer ships separate files.

| Variant | MCU | Built | Firmware model names (update mode answer) |
| --- | --- | --- | --- |
| GD | GigaDevice GD32F303VKT6 (Cortex-M4, 3 MB flash, 96 KB RAM, no CCM) | until about mid-2022 | `D578UV2` (578UV), `D578UV` (PRO); `D578UV` confirmed on a PRO (bootloader V100) |
| AT | Artery AT32F435/437 (Cortex-M4F) | from about mid-2022 | `578UV2A` (578UV), `D578UVA` (PRO) |

- The stock firmware's version screen names the build: `GD_Ver` or `AT_Ver`.
- The 578UV and the PRO GD builds differ in one RF register value (section 5); no pin or calibration field tells
  them apart at run time, so one RadioFoundry image serves both.
- The Artery build uses the same pins and logic on STM32F4-style GPIO (0x40020000 + 0x400 per port), 12 MHz crystal,
  PLL to 120 MHz, APB1 and APB2 at 60 MHz. Each variant takes its own RadioFoundry image: `d578uv` on the GD32,
  `d578uva` on the Artery (section 10). The flashing tools refuse the other variant's image (section 8).

## 2. MCU, clock and memory

| Item | Value |
| --- | --- |
| Crystal | 12 MHz (HXTAL) |
| SYSCLK | PLL x10 = 120 MHz; AHB /1, APB1 /2 (60 MHz), APB2 /1 |
| Flash | 2 wait states, prefetch on |
| Vendor bootloader | 0x08000000-0x08005FFF, never written |
| Application | from 0x08006000 (vector table there); `APP_BASE` in `board.mk` |
| Modem boot slot | 0x080FF000-0x080FFFFF: the SCT3258 boot block (section 4.3), `__codec_slot` in `memory.ld` |
| RAM | 96 KB at 0x20000000 |
| Debug | SWD on PA13/PA14, no read-out protection seen. PA13/PA14 are also the level device's lines (section 3.4): RadioFoundry turns SWD off when it starts so it can set the speaker level; build with `D578UV_LEVEL_IC=0` to keep SWD. AFIO_PCF0's SWJ field goes to 0 first, then to "off": a direct change from "SW-DP only" to "off" doesn't take (seen on a radio) |

USB (seen on a radio):

| Item | Value |
| --- | --- |
| Clock | IRC48M, as the stock firmware sets it (RCU_ADDCTL bit 16 on, then bit 0 to select it; 0x0809B3B2). CK_PLL / 2.5 failed to enumerate after the bootloader: it leaves the USBD clock on, so the prescaler doesn't change |
| D+ pull-up | switched by PE2, high = on (push-pull; 0x0809B370 and 0x0809B396). Without it the host sees no device |

## 3. Inputs, outputs and power

The full pin map is `boards/d578uv/pins.h`.

### 3.1 Display

ST7735S-class controller, 128 x 160 panel, 8-bit 8080 bus: data PE7-PE14, WR PB10, RS PB11, RESET PB12, CS PB13,
RD PE15 (held high). The stock firmware sets MADCTL 0x48 and swaps the column and row windows in software, so the UI
is landscape, 160 x 128. RadioFoundry draws a 4 bpp frame buffer (16 colors per screen, `GFX_INDEXED`).

The backlight level is register 8 of the serial device on PA5 (section 3.4). The firmware writes it from the "Back
Light" setting, five levels:

| Level | 1 | 2 | 3 | 4 | 5 |
| --- | --- | --- | --- | --- | --- |
| Register 8 | 0x38 | 0x3D | 0x42 | 0x4B | 0xFF |

- 0xFF during the boot splash and on the update and service screens; 0 at init, power-off and standby, and when the
  radio is disabled remotely (then the panel also gets sleep-in, command 0x10).
- No backlight timeout: the level stays until the setting changes.
- Whether the part behind PA5 is a DAC or a PWM driver is not known; nothing but these bytes is.

RadioFoundry maps the core's backlight percent onto the five levels: 0 % off, 1-20 % level 1, 21-40 % level 2,
41-60 % level 3, 61-80 % level 4, 81-100 % level 5 (`boards/d578uv/front_io.h`). The core's dim and off times apply;
the panel is never put to sleep. The UI and start-up tasks write the register while the radio task drives the RF
chips on the same lines, so each frame on the bus holds the scheduler (not the interrupts) until it ends.

### 3.2 Keys and encoder

| Source | Stock key codes | Read |
| --- | --- | --- |
| Ladder on PB0 (ADC 8) | 0x14, 0x15, 0x16, 0x0E | 12-bit sample <= 500, 1500, 2500, 3500; above: none |
| Ladder on PB1 (ADC 9) | 0x17, 0x18, 0x19, 0x0F | the same windows |
| PA1, pull-up, low = pressed | 0x1E | |
| Encoder PC8 / PC9, pull-ups | 0x12 / 0x13 | both edges (EXTI 8 and 9) |
| Microphone PA10 low, PA9 high | 0x1F press, 0x20 release: PTT | plain microphone only (below) |
| Microphone PA9 low, PA10 high | 0x10: channel +1 (the microphone's Up) | |
| Microphone PA9 and PA10 low | 0x11: channel -1 (Down) | |
| Serial microphone frames | 0x01-0x0D, 0x10, 0x11, 0x1A-0x1D, PTT | serial microphone only (below) |

What the stock firmware does with each code:

| Code | Firmware's name | Idle screen | Menus |
| --- | --- | --- | --- |
| 0x14-0x16 | P1-P3 (PB0 keys 1-3) | programmable function | 0x14 and 0x16 are list up and down; 0x15 its function |
| 0x17-0x19 | P4-P6 (PB1 keys 1-3) | programmable function | its function |
| 0x0E | (PB0 key 4) | opens the menu | confirm |
| 0x0F | (PB1 key 4) | long: key lock on and off | back |
| 0x1E | PUSH (PA1, the encoder's switch) | programmable function | confirm |
| 0x12 / 0x13 | encoder | channel +1 / -1 | list keys |
| 0x1A-0x1D | PA-PD (serial microphone only) | programmable function | |
| 0x0D | (serial microphone only) | transmit on the other side ("A/B SUBPTT") | |
| 0x01-0x0C | (serial microphone keypad) | digits and direct entry | |

- Every programmable key has a short and a long function ("Key Func"), a function id from a list of 58. First-boot
  defaults: P1 VFO/MR, P2 Tx Power, P3 TalkAround, P4 Speaker Mode, P5 APRS Set, P6 Switch ChType; PA Key Menu, PB
  Zone Up, PC Zone Dn, PD Key Exit; PUSH and every long press Off. The programming software sets its own.
- The firmware names keys only by code (P1-P6, PA-PD, PUSH). On a radio the printed keys read as their codes: P1-P3
  are PB0's positions 1-3 (0x14-0x16), P4-P6 PB1's (0x17-0x19), MENU PB0's fourth (0x0E), EXIT PB1's fourth (0x0F).
  PUSH is the encoder's switch. The serial microphone's codes are confirmed on a radio (table below).
- Encoder: an edge on PC8 with PC9 equal to PC8 afterwards is 0x12 (+1), unequal 0x13. Clockwise is +1, one step a
  click (seen on a radio).
- Volume knobs: A on PC2, B (bottom left) on PC5, each 0 to 4092 over its turn (seen on a radio).
- Power-on combinations, read by the stock application in its reset init (PA1 and the ladders, PA1 debounced twice
  10 ms apart):

  | Held at power-on | Result |
  | --- | --- |
  | PA1 + 0x14 | `UPDATE MODE` on the USB serial port: the application's resource update (fonts and icons, section 8) |
  | PA1 alone | `Hand-Soft`: the hand microphone's update, USART0 at 9600 baud on PA9/PA10, backlight at level 5 |
  | PA1 + 0x15 | "Initialize radio" |
  | PA1 + 0x19 | the accessory board update (USART0 at 115200 baud) |
  | PA1 + 0x16, 0x17, 0x18, 0x0E | other service states (a service mode, a boot message change) |

  PA9 and PA10 are never read at power-on, so "PTT + key" is no combination of the application. The firmware update
  entry (MENU + EXIT held at power-on, public instructions) is not in the application image: the vendor bootloader
  (0x08000000-0x08005FFF, not in the firmware files) reads it (unverified). With RadioFoundry installed only the
  bootloader's combination is left.

- PTT is on the hand microphone. The stock firmware has two microphone types, its "Hand Type" setting:
  - **Enhance Type** (1): the plain microphone. PA9 and PA10 are GPIO inputs with pull-ups and the microphone pulls
    them low: PA10 alone is PTT, PA9 alone 0x10, both 0x11 (table above).
  - **Normal Type** (0, the default): the serial keypad microphone. PA9 (TX) and PA10 (RX) are USART0, 115200 8N1.

  Bluetooth PTT and the CPS's remote PTT post the same key events. PD0, once taken for PTT, is a transmit burst line
  from the baseband (section 5.1).

The serial microphone, as seen on a radio with the keypad microphone:

| Control | Where | Code |
| --- | --- | --- |
| PTT | left side | frame byte 1 |
| A/B (manual: "A/B SUBPTT") | left side, the small button above PTT | 0x0D |
| Up, Down | top | 0x10, 0x11 |
| `0`, `1`-`9` | keypad | 0x01, 0x02-0x0A |
| `*`, `#` | keypad | 0x0B, 0x0C |
| A, B, C, D (printed ZONE, ZONE, EXIT, MENU) | keypad, bottom row | 0x1A-0x1D (the firmware's PA-PD) |
| Key Lock | right side, slide switch | no frame; the keypad sends in both positions |
| A/B TX/RX and main channel lights | top | driven from the radio (status frame, below) |

The stock firmware also names codes 0x0E and 0x0F (menu, exit); the keypad microphone doesn't send them.

Its frames, at 115200 8N1:

| Item | Value |
| --- | --- |
| Frame types | the first byte gives the length: `A` (0x41) 8 bytes, the microphone's; `F` 7; `+` 31 (the PC link's); 0x06 one byte (an acknowledgement) |
| Accepted | an `A` frame whose byte 7 is 0x06 |
| Byte 4 = 0x82 | presence: `41 00 00 00 82 00 00 06` |
| Otherwise | byte 1 PTT (nonzero = pressed), byte 2 the key (1 pressed, 0 released), byte 4 its code. Bytes 3, 5 and 6 are 0 |
| Answer | the radio sends 0xAA for each accepted frame |
| Status frame | 16 bytes from the radio: `S`, bytes 1-7 the state for the microphone's lights (side A busy, the main side's audio open, side A transmitting, side B busy, the other side's audio open, side B transmitting, the main side), byte 9 0x10, byte 12 the long press time (100 ms steps less one), byte 15 0x06; the rest 0. The light fields are the stock firmware's, not checked on the microphone |

The handshake: the microphone sends the presence frame 10 times a second until the radio answers with 0xAA and a
status frame. Until then it sends no PTT and no key. Once answered it sends a frame only when something changes: none
while idle and none while PTT is held. It answers each status frame with a run of 0x06 bytes. The stock firmware
answers from its key loop and spaces status frames 200 ms apart (inferred).

RadioFoundry reads both microphones without a setting (`boards/d578uv/input_d578uv.c`,
`drivers/input/anytone_mic`). Pull-ups on PA9 and PA10, so without a microphone both lines are high and nothing keys
the transmitter. The USART receives on PA10 all the time. Once a frame is accepted the keys and PTT come from the
frames and PA9 becomes the USART's transmit line. The key scan sends the answers and the status frames: one after
each presence frame and one a second after that, no closer than 200 ms. Any byte from the microphone, its 0x06 runs
too, keeps it present, so PTT held for minutes stays on. After 3 s without a byte the microphone counts as gone, with
its PTT and key, and PA9 is an input again: a microphone pulled out mid-transmission unkeys within 3 s. For 50 ms
after any byte the two lines aren't read as keys, so a frame's bits never look like PTT; a plain microphone's PTT is a
break to the USART, which counts as no byte. The core debounces all of them as the other keys. The status frame is
sent idle, so the microphone's lights stay off. The diagnostics line `hmic` shows whether a serial microphone is
present, its frames and dropped bytes and the key code held.

RadioFoundry's key names (`d578uv.h`): `KEY_UP`, `KEY_DOWN`, `KEY_MENU`, `KEY_EXIT` for 0x14, 0x16, 0x0E and 0x0F,
`KEY_C15`, `KEY_C17`, `KEY_C18`, `KEY_C19`, `KEY_C1E` (the stock code) for the others, `KEY_PTT`, `KEY_MIC_UP`,
`KEY_MIC_DOWN` and `KEY_MIC_0`-`KEY_MIC_9`, `KEY_MIC_STAR`, `KEY_MIC_HASH`, `KEY_MIC_AB`, `KEY_MIC_PA`-`KEY_MIC_PD`
for the microphones. The microphone's Up and Down act as the front Up and Down, its digits enter digits, `*` and `#`
act as down and up, A/B switches the side, and A-D follow their printed labels: zone up, zone down, back (EXIT) and
confirm (MENU). PUSH (`KEY_C1E`) confirms, as the menu key does.

### 3.3 Power

| Pin | Use |
| --- | --- |
| PC3 | power hold, high = on, set first at boot, never cleared by the stock firmware (inferred) |
| PA0 | power key, pull-up, low = pressed; the stock firmware counts it only with PA6 below 1.33 V |
| PA6 (ADC 6) | the PA's temperature sensor, higher = hotter; the fan's thresholds (section 5) |
| PC1 (ADC 11) | supply voltage, 11:1 divider (inferred) |
| PE0 | switched rail of the backlight, the RF chips and the audio, high = on; the stock firmware raises it at boot and waits 150 ms. Left low, the panel shows a picture without backlight, the receiver is dead and the radio draws less current (seen on a radio) |

The stock "off" is a soft standby: the shift register cleared, pins parked analog, PC3 kept high, and an IWDG reset
loop that wakes on PA0 held for 1 s.

### 3.4 Audio and RF control outputs

| Pin | Use |
| --- | --- |
| PE3 / PE4 / PE5 | 8-bit shift register (data, clock, latch), LSB first |
| PE6 | TX path enable, high while transmitting (inferred) |
| PA4 | DAC 0: PA drive level from the calibration, corrected per level (5.8) |
| PA7 | speaker mute, high = muted (inferred) |
| PC4 | follows the transmit state; PA band select (inferred) |
| PB9 | TIMER3 CH3 PWM: beeps and tones |
| PA5 | chip select of a serial device on the RF bus (4-bit address, 8-bit data, write only): register 8 the backlight (section 3.1); registers 3 and 0xF 0xFF and 9, 0xE, 0xC, 2, 0xA, 6, 1 zero at init; 1, 2, 4, 6, 0xA, 0xC calibration trims (5.5) |
| PA13 / PA14 | data and clock of a two-channel volume device (inferred) |
| PC2 / PC5 (ADC 12 / 15) | the A and B volume pots |

Shift register bits (inferred unless marked): 0x02 main audio enable, 0x04 the main side's demodulator output to the
codec, FM and DMR alike (measured: with it clear the baseband hears nothing; the stock firmware sets it on every
receive start), 0x08 speaker from the RF chip (clear in DMR), 0x10 TX path, 0x20 cleared at the end of TX turn-on,
0x40 RX audio gate, 0x01 and 0x80 rarely used.

The audio codec is a TLV320AIC3204 class part on the baseband's I2C; the MCU sets it up through the baseband's codec
bypass packets ([modem-sct.md](modem-sct.md) 3).

Audio into the MCU (stock firmware, static analysis):

| Signal | Path | Stock use |
| --- | --- | --- |
| Receive audio | PC0, ADC channel 10 (rank 4 of the DMA scan), biased at mid-scale | DTMF, 2-Tone and 5-Tone decoding in software: TIMER5 samples it only while receiving, at about 9.02 kHz for DTMF (8 bits), 4 times the tone for 2-Tone (8 bits), 11.2 kHz for 5-Tone (a zero-crossing slicer on the 12-bit value). That it carries the selected side's demodulated audio is inferred |
| Microphone | none: no ADC rank carries it; chip 0 measures it (5.6) | the stock firmware has no VOX (no VOX function or string; the key slots other AnyTone models give VOX hold the AM and FM squelch levels here) |
| APRS | the APRS add-on board on the Bluetooth UART (PC10 / PC11) | the board decodes AFSK itself and reports `+APRS:` lines; the MCU never sees the audio |

RadioFoundry's APRS receive (`CONFIG_APRS_RX`, `boards/d578uv/afsk_d578uv.c`) samples PC0 itself and doesn't use the
add-on board:

| Item | Value |
| --- | --- |
| Rate | 9600 Hz, eight samples a bit at 1200 baud: TIM6 (the GD32 manual's TIMER5, the stock sampling timer) update interrupt every 12,500 clocks at 120 MHz, priority 3 (above the RTOS's critical sections) |
| Conversion | ADC channel 10 on the injected group (the GD32's inserted, the AT32's preempted group): its own trigger and data register, so the key ladders, knobs and supply readings keep the regular group. The interrupt reads the conversion it started on the last tick and starts the next by software; a conversion takes about 25 us |
| Buffer | 1024 readings (107 ms, 2 KB), read by the radio task every 25 ms as signed 16-bit PCM with the bias tracked out (`drivers/audio/adc_pcm`) |
| When | the timer runs while the core listens for APRS (an FM channel on the APRS frequency, not transmitting); readings reach the demodulator only while side A receives FM, not in AM and not transmitting |

No DMA: the regular group the DMA would scan is the one the board's other readings share. Whether PC0 carries side A's
audio while side B also receives, and whether it is there with the squelch closed (the shift register's RX audio gate,
0x40), is not known.

### 3.5 Serial ports and other parts

| Port | Pins | Use |
| --- | --- | --- |
| USART1 | PA2 (TX) / PA3 (RX), 9600 baud 8N1 | GPS (NMEA); the module's supply is RF chip 0's register 0x25 bit 2 (section 5) |
| UART3 (the Artery's UART4) | PC10 / PC11, 115200 baud 8N1 | the Bluetooth module, or the APRS add-on board (3.7) |
| USART0 | PA9 / PA10, 115200 baud | the serial keypad microphone (section 3.2); GPIO inputs with a plain microphone |
| USB | PA11 / PA12 | native USB, the host link and the update mode |
| I2C (bit-banged) | PB6 SDA / PB7 SCL | the KTMicro AM/FM receiver or the RDA5807 (5.4). The stock firmware drives SDA push-pull and reads it as an input with pull-up |

### 3.6 GPS

| Item | Stock firmware | RadioFoundry |
| --- | --- | --- |
| UART | USART1, 9600 baud 8N1, PA2 alternate push-pull, PA3 floating input; receive interrupt (IRQ 38); transmit polled | the same (`platform/gd32f30x/gps_uart`); PA2 is the USART's output only while the module is powered, a floating input otherwise |
| Supply | RF chip 0 register 0x25 bit 2, set with the GPS setting on and the UART enabled in the same routine; cleared in standby and at power-off | the same bit, through the radio's shadow of the register under the RF bus lock (`radio_d578uv.c`) |
| Module type | `$PMTK010,001` (an MTK module's start-up message) makes it MTK; any `$xxTXT` sentence makes it CASIC. The first of the two after power-on sends that type's set-up, once until the module is powered off | `$xxTXT` sends the CASIC set-up, once per power-on; an MTK module gets nothing |
| CASIC set-up | `$PCAS02,1000` (one fix a second), `$PCAS03,1,0,0,0,1,0,0,0` (GGA and RMC), `$PCAS04,n` with n = the satellite setting + 1 (0 to 6, default 0: `$PCAS04,1`, GPS only). The satellite screen sends `$PCAS03,0,0,0,1,0,0,0,0` (GSV only) | `$PCAS02,1000`, `$PCAS04,1`, and `$PCAS03,1,0,0,0,1,0,0,0`, or with the GPS status screen (`CONFIG_GPS_SKY`) `$PCAS03,1,0,1,1,1,0,0,0` (GSA and GSV too: the screen reads them all the time) |
| MTK set-up | `$PMTK225,0,1,0,1,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0`: command 225 (power mode) with the 19 fields of a 314 output list (RMC, GGA, GSV every 5th fix) | not sent: a 225 with those fields doesn't set the output |
| Detection | none seen | one checksum-valid sentence within 8 s of power-up; nothing heard, the module is powered off and the radio reports no GPS |

Settings > Location > GPS switches the supply and the receive path together once the module has been heard. Each
sentence goes out as `$`, the body, `*`, the XOR checksum in two upper-case hex digits and CR LF, back to back. The
`$PCAS03` field order (GGA, GLL, GSA, GSV, RMC, VTG, ZDA, ANT) is from the CASIC protocol; the stock firmware's use
of fields 1, 4 and 5 agrees with it.

### 3.7 Bluetooth module

The module speaks text lines on PC10 / PC11 (`drivers/input/bt_at`, `bt_d578uv.c`, `CONFIG_BT_PTT`). Its own firmware
answers `+VER:ET12_AQQX_V10043`; the chip isn't named in the image. All of it is from the stock firmware's code and
untested on a radio.

| Item | Stock firmware | RadioFoundry |
| --- | --- | --- |
| UART | 8N1, no flow control, receive interrupt (IRQ 52) | the same |
| Supply | the shift register's bit 0x02, 1 = on; PE15 driven low with 150 ms waits around it (its role isn't known) | bit 0x02 on from boot; PE15 untouched |
| Detection | no pin. At start `AT+VER?` CR LF up to 5 times, 200 ms apart; any line back (even `ERR`) is the module. No answer costs about 1 s of start-up | the same probe from the key scan, without holding up the start |
| APRS board | a `+VER` answer starting `APRS` is the APRS add-on board on the same UART; `+APRS:` starts its binary frames | known by its answer and left alone |
| Set-up | `PIN?`, `LENAME?`, `CONN?`, `AGHF=0`, the gains, `SPKEN=0`, each sent up to four times | none: the module keeps the pairings it has (inferred) |
| Frames | `AT+` text CR LF to the module; lines back start with `+`, `O` or `E` and end with LF; 300 bytes without a line end restarts the line | lines over 64 bytes are dropped whole |

Lines the radio acts on:

| Line | Meaning | RadioFoundry |
| --- | --- | --- |
| `+PTTKEY=1` / `=0` | the PTT button (key fob) pressed, released | PTT |
| `+HFREQ:1` / `:0` | the hands-free button: 1 toggles a PTT latch, 0 clears it | PTT |
| `+ADATA:nn,len` | the next `len` bytes are the Bluetooth hand mic's own lines: `+PTT=P`, `+PTT=R`, `+KEY=NN` | PTT; keys below |
| `+CONN=<12 hex>,<name>` | a link: name `PTT...` the key fob, `HOST...` a host, else an audio device; the all-zero address is a disconnect | the disconnect releases PTT |
| `+DISC=<12 hex>,<name>` | a link ended | releases PTT |
| `+READY` | the module restarted | releases PTT |
| `+AUDIO=`, `+PTTBATT=`, `+SCAN:`, `+VER`, `+PIN`, `+LENAME`, `OK`, `ERR` | audio state, the fob's battery, a scan result, replies | counted, not used |

`+KEY=NN` takes two decimal digits; the stock firmware acts on 01-04 only: 01 and 02 post two key codes it also uses
for the encoder, 03 and 04 two it uses for Monitor (both inferred). RadioFoundry gives 01 and 02 the microphone's Up
and Down keys for 150 ms each and counts 03 and 04 (`input.bt_key`, `input.bt_keys`). The key fob's "PTT Hold"
setting (a press toggles) and audio (`AUDIO`, `MICEN`, `SPKEN` at each transmission) aren't written. The stock
firmware also turns the module off while dual receive is on; RadioFoundry doesn't.

## 4. DMR baseband (SCT3258)

The baseband's protocol, boot and the driver are [modem-sct.md](modem-sct.md). This radio's wiring:

### 4.1 Host port

| Signal | Pin |
| --- | --- |
| D0-D7 | PD8-PD15 |
| HCSN / HWRN / HRDN | PD2 / PD3 / PD4 |
| HOBIB (data waiting, low) | PD5 |
| RSTN | PD6 |
| Supply enable | PA8, high = on |
| Strap PIO1 | PC12, low = Intel bus timing |
| Others in the reset sequence | PD1, PD7, PC7 low; PC6 high |

The bus is bit-banged, as the stock firmware does it (`boards/d578uv/modem_d578uv.c`).

### 4.2 Reset

1. PA8 high; PD2, PD3, PD4 high; PD1, PD7, PC12, PC7 low; PC6 high.
2. 100 ms, RSTN high, 100 ms, RSTN low, 500 ms, RSTN high (0x080930F0; left low, the chip never answers).
3. The boot block (4.3), then `ACK_MESSAGE` 0x01.

### 4.3 Boot block

All four stock images (578UV and PRO, GD and AT) carry the same 2048-byte boot block (CRC-32 0x024F600A). The
flashing tool takes it from the user's stock firmware file and writes it to the modem boot slot (`[codec]` in
`board.toml`, tool `modem_boot`). Without it the radio runs FM only. None of the images has SPI or boot ROM code: this
radio carries the SCT3258 only.

### 4.4 Settings

The voice set-up after the boot ([modem-sct.md](modem-sct.md) 2), as the stock firmware sends it:

| Step | Value |
| --- | --- |
| `CODEC_SELECT` | 1, the TLV320AIC3204 |
| `VOCODER_SEL` | 4, AMBE+2 |
| `DIGI_SPEAKER_GAIN` | from calibration (7, offset 0x0D9), MSB first |
| `DIGI_MIC_GAIN` | from calibration (7, offset 0x0DB), MSB first |
| Codec page 1, registers 0x12, 0x13, 0x3C, 0x10 | four levels from calibration (7, offset 0x0D2): line outputs left and right, the right input, the headphone driver |
| Codec page 1, register 0x34 | 0x80 |
| Codec page 0, registers 0x56 and 0x58 | the microphone gain: `0x80 \| g` and `g`, g one of 0x18, 0x1C, 0x20, 0x24, 0x2A by the user's microphone gain; RadioFoundry sends 0x20, the middle step |

`MOD_GAIN` (RadioFoundry only) goes before each DMR transmission, by band (`tx_fields`, [modem-sct.md](modem-sct.md) 5): the VCO's modulation gain differs by band, so one value puts the outer symbols at 1944 Hz on one band only. At the chip's default (0 dB) the deviation is 1255 Hz at 446 MHz and about 430 Hz at 151 MHz.

| Band | `MOD_GAIN` | Deviation (measured) |
| --- | --- | --- |
| UHF (300 MHz and up) | 0x0C60 (+3.8 dB) | 1946 Hz at 446.075 MHz |
| VHF | 0x2475 (+13.1 dB) | 1938 Hz at 151.5125 MHz, about 1900 Hz at 146.535 MHz |

The bench's burst decoder reads 1948 Hz from a reference transmitter. Every burst decodes clean on both bands. In the DMR start (TYPE 0x05) the field changed nothing.

Without a current calibration block the four levels and the two gains are the stock factory defaults: 0x08, 0x04,
0x14, 0x14, speaker 0x0546 (-3.6 dB) and microphone 0x07D0 (-0.2 dB). The stock firmware sends other gains only for
settings RadioFoundry doesn't have: speaker 0x0151 with its lowest volume limit, microphone 0x0708 with an equalizer
preset (inferred names). What each codec register does is inferred from the stock firmware's use, not from the
codec's datasheet.

The DMR start ([modem-sct.md](modem-sct.md) 4), as the stock firmware sends it at its baseband bring-up:

| Field | Value | Meaning |
| --- | --- | --- |
| `DMR_CALL_OPTION` | `41 40 42 4F 08` | byte 0: squelch detection by the baseband (RadioFoundry: off, 0x01, below), traffic mode 1, no RF control (the MCU keys the RF chips); byte 1: voice delay 0, reserved bit 6 set; byte 2: four good CACH, two LC header repeats; byte 3: every idle frame, CSBK, LC header and terminator reported, reserved bit 6 set (the original firmware: 0x44, below). Without every idle frame the baseband reports a repeater's idle outbound once, and the stack holds a repeater as up only 300 ms after its last burst: a radio on a repeater channel never saw it up (HIL `test_dmr_outbound_voice`); byte 4 reserved |
| `REPORT_FIELD` | 0xFF | the four documented reports and the reserved bits 4-1 |
| `MODOUT_CONFIG` | `00 10 00 00 00` | no two-point delay; 0x10 in a reserved byte. RadioFoundry sends `EB 10 00 00 00` before each transmission instead (5.1) |
| `DMR_OFFSET` | 8 bytes from calibration (7, offset 0x140) | the baseband's RF_TIMING output; changing it moved neither PD0 nor the burst's modulation (5.1, measured). Without a current calibration block the default `00 82 02 D0 02 D0 02 D0` |
| `SQ_LEVEL` | `00 02 00 3C 17 70 15 7C 17 70 15 7C` | the baseband's squelch: no gain adjustment, external LNA 2, AGC, OOBE every 80 ms, carrier thresholds 0x1770 high and 0x157C low for digital and analog. Fixed in the stock firmware |
| `DEMOD_GAIN` | `00 00` | auto calibration. The stock firmware sends 0 though it passes calibration offset 0x0D7 |

The stock firmware takes the LC header repeats from a user setting (clamped to 2-9); RadioFoundry sends 2, the
lowest. Its byte 3, 0x44, reports identical LC headers and terminators once; RadioFoundry sets bits 1 and 0 as well,
so a second call with the same LC can't go unreported ([modem-sct.md](modem-sct.md) 3). The stock firmware sets
byte 3 bit 0 and other byte 4 values only with the second receiver; RadioFoundry runs side B for FM only (5.2) and
keeps the single receiver's values. It sends
`SQ_LEVEL` and `DEMOD_GAIN` once at start-up; RadioFoundry sends both in every start. The baseband needs
`CARRIER_READY` 2 (ready, demodulated phase reversed): RadioFoundry sends 2 when side A's carrier comes up and 0
when it goes, from the status poll (`d578uv_modem_carrier`); with 1 no call decodes. RadioFoundry clears byte 0
bit 6, so the baseband looks for sync on `CARRIER_READY` alone, and sends 2 14 ms after each return to receive
from a transmission (`rx_ready`, [modem-sct.md](modem-sct.md) 3), before the status poll has a reading. With bit 6
set, the original firmware's value, a lone burst soon after a transmission went undecoded. Without its own squelch the
baseband now and then reports noise as frames (seen as MBC continuations with a random color code); the stack drops
them by color code.
The `MODOUT_CONFIG` delay byte is a setting, 0 from start-up in the stock firmware, not a calibration value. The equalizer filters stay at
the baseband's default, off.

While it transmits, the stock firmware mutes the headphone driver (codec page 1 register 0x10 to 0x40) and writes
the calibrated level back for receive. RadioFoundry does the same: the mute before `WORK_MODE` transmit, the level
after `WORK_MODE` receive (`codec_tx` and `codec_rx`, [modem-sct.md](modem-sct.md) 5).

The voice goes from the baseband to the codec, so the board sets `CAP_MODEM_VOICE`, and privacy, the hotspot and the
store-and-forward repeater are not written for this radio. RAM decides the rest: the USB network adapter and zone
sharing are off with DMR on.

The speaker during a DMR call (seen on a radio, a bench call and the owner listening):

| Path | What reaches the speaker |
| --- | --- |
| Codec headphone driver (page 1 register 0x10) | the call's voice. Muted, the voice goes and nothing else changes |
| Volume device, side A's channel | side A's RF chip audio, which in DMR is the 4FSK demodulator output (0x47 = 0x4840 or 0x6840): a loud data sound under the voice, with shift register bit 0x08 clear. Code 0 is not a mute (the 4FSK still plays loudly with the gate below open), and a jump from code 0 to the volume clicks: RadioFoundry leaves the channel at the volume and mutes with the gate |
| Chip 1 GPIO (0x25) bit 1 | the gate on side A's RF chip audio: clearing bit 1 stops it (measured by microphone and heard on a radio), and the voice is unchanged. Opening and shutting it on silence doesn't click. RadioFoundry sets the bit only while side A's RF audio plays (FM with the squelch open, a beep, the KTMicro receiver) or side B is heard. Bit 5 cleared stops the voice too |
| PA7 | the amplifier. RadioFoundry turns it on only while something plays (the receiver's audio, side B, a beep, the KTMicro receiver) and a volume knob is up, and off while it transmits. With it held on, a hiss that doesn't follow the volume came from the speaker with the squelch shut and no hand mic connected (heard on a radio, not every time); switching it at the squelch's edges or for a beep adds no click and doesn't shorten a beep: a key beep is heard as long with it held on (measured by microphone) |

Beeps: the board has no PCM speaker audio, so the core plays a beep's tones through `beep_tone`. Side A's chip sounds
each on its tone generator (0x71 the tone, 0x70 the gain, 0x47 = 0x6341) with 0x03 = 0xBFF7: the receive enable plus
bit 1, without which the tone is silent (seen on a radio). The PA gain bit stays off, so nothing is sent. With the
receiver on the generator sounds at twice the frequency of its transmit word (measured by microphone, 500 to 1500 Hz),
so a beep writes half the word. Its level follows page 1 0x1D: 12 dB louder at the analog path's 0x39FD than at the
digital path's 0x29A1, which only DMR transmit needs, so DMR receive takes 0x39FD as well and a beep sounds the same in
both modes (measured by microphone; DMR receive unchanged on bench calls). The chip's output switches to the tone
before the gate opens, and the gate shuts before it switches back. A tone longer than about 1.1 s drops 12 dB by
itself; no beep is that long. In DMR the tone takes the chip's output from the baseband, which then has nothing to
demodulate: a beep during a call cost 12 voice bursts, or ended the call (HIL, the alert beep of an emergency call). No
beep sounds while side A hears a DMR signal, and one sounding stops when a signal comes.

## 5. RF chips

Four chips of one type on a 3-wire bus: PB3 clock, PB4 data, one enable each (low = selected): chip 0 PB5, chip 1
PB2, chip 2 PC13, chip 3 PE1. Frame: register (bit 7 = read), then 16 data bits, MSB first. Register 0x7F selects
page 0 or 1; writing 0x8000 then 0 to register 0x00 is a soft reset. The parts are marked QA4818N or WX4818N
(public photos); no data sheet was found. The register map follows Beken's BK4819 (V3 register list, 2020) outside
the paged range 0x10-0x3F; the meanings below come from the stock firmware, and the BK4819 list names some more:

| Register | Stock use | BK4819 meaning |
| --- | --- | --- |
| 0x03 | the enable word (0xBFF5, 0xC1FE) | not listed; the same word is the BK4819's 0x30 (bit 2 the microphone ADC, 1 TX DSP, 0 RX DSP, 3 PA gain, 7:4 PLL, 13:10 RX chain, 15 VCO calibration), which the QA4818 uses otherwise |
| 0x09 | ten writes at init, 0x0000 to 0x9076 | DTMF/SelCall detector coefficients: bits 15:12 the symbol, 7:0 its coefficient |
| 0x10-0x13 | 0x0038, 0x025A, 0x035B, 0x0396 | RX AGC gain table (defaults 0x0038, 0x025A, 0x037B, 0x03DE) |
| 0x40 | the "drive": 0x3600-0x3900 by band + the level << 1, less 0x100 on a 25 kHz channel | TX deviation: bit 12 on, 11:0 the amount (default 0x4D0) |
| 0x47 | 0x6140; in DMR receive 0x4840 at 300 MHz and above, 0x6840 below | AF output: bits 11:8 = 1 normal audio, bit 13 inverted |
| 0x51 | bit 15 on, low 7 bits a calibrated level | sub-audio TX on, bits 6:0 its gain |
| 0x64 | not read | voice amplitude, the VOX detector's input (5.6) |
| 0x78 | the gain code << 8 \| 0xFF | RSSI squelch thresholds (the stock firmware decides the squelch itself) |

Where the two agree the BK4819 meaning is likely but inferred. 0x40 as the deviation, not the PA drive, fits the
measurement: one step less on a 25 kHz channel doubles every deviation (CTCSS 374 Hz at 12.5 kHz, 748 Hz at 25
kHz). The calibration's MODU (modulation) bytes feed it.

| Chip | Role |
| --- | --- |
| 0 | side A: receives at the IF, transmits on the channel |
| 1 | side B: receives at the IF |
| 2 | side A local oscillator |
| 3 | side B local oscillator, and the air band receiver's LO |

The debug link's CHIP_REG ([debuglink.md](debuglink.md) 3.24) uses these numbers: chips 0-3 on pages 0 and 1, every
register but 0x7F (the page itself); chip 4 is the PA5 trims device (5.5), page 0, registers 0-0xF, write-only.

RadioFoundry clocks the bus as fast as the pin writes go, with one 1 us wait between the register byte and the data
(the BK4819's limits: clock to 8 MHz, 1 us from the register byte's last falling edge to the next rising one), and
sets the data pin's direction only when it changes. With a 1 us wait per half bit and the direction set on every bit
the receive set-up after a DMR transmission (65 register writes) took about 15 ms; now about 6 ms.

| Item | Value |
| --- | --- |
| Frequency | registers 0x38 (low) / 0x39 (high), 10 Hz units |
| VCO sub-band | nine entries of registers 0x3C, 0x3D, 0x1A, 0x2A, chosen by frequency |
| Enable | register 0x03: 0 off, 0xBFF5 transceiver on, 0xC1FE LO on |
| RX IF | 38.85 MHz side A, 38.55 MHz side B. LO = F - IF when that is at least 300 MHz, else F + IF |
| TX | chip 0 on the channel frequency; PA level from PA4's DAC |
| CTCSS | register 0x07 = Hz x 20.649, register 0x51 = 0x90C5, register 0x52 the detector thresholds per tone |
| DCS | register 0x08 the Golay word in two halves, register 0x07 = 0x0AD7 (134.4 Hz), register 0x51 = 0x80B0. Receive writes the normal word for either polarity |
| Status | register 0x0C bit 1 signal present; 0x65 noise; 0x67 RSSI; 0x61 the RSSI the S-meter uses; 0x60 tone flags: bit 0 the CTCSS tone, bit 10 the DCS code (normal), bit 11 the DCS code (inverted). Bit 1 shows only at a carrier's start, for any code |
| Band switch | each transceiver's register 0x25 is a GPIO register. Chip 0's bits 3/6 are side A's complementary pair (UHF sets 3), bits 7/4 side B's (UHF sets 7), by the 300 MHz rule; side B's are both clear in some single-receiver cases |
| TX tone generator | register 0x70 = 0xA000 on, 0 off; register 0x71 = Hz x 10.32 (truncated), halved (5.3); with the receiver on (a beep) half that again |

The squelch is decided in firmware from register 0x0C bit 1, the noise and the RSSI against calibration values; no
squelch threshold register is written. Back in receive after a transmission, register 0x0C bit 1 and the RSSI read
as a signal for a moment, and the squelch opened on noise at every unkey (about 50 ms, loud, measured by microphone):
RadioFoundry reports no carrier for 150 ms after the transmitter goes off. The 578UV and the PRO differ only in register 0x49 of chips 0 and 1 (0x26AC
on the PRO; 0x29AA on the 578UV unless a flag is set).

Chip 0's register 0x25 bit 0 is set in receive and while a TX tone sounds and cleared at each key-up: a microphone
mute (inferred from where the stock firmware sets it). Bit 1 is the fan, high = on (seen on a radio): the stock "Fan
Open" setting (PTT, Temperature or Both, default PTT) runs it while transmitting, while PA6's average of four readings
is hot (on at 0x3E0 and above, off at 0x364 and below), or either (0x0808BE9C). The status LED is an RGB LED whose
output is not found. Bit 2 is the GPS module's enable, not an LED: the firmware
sets it with the GPS setting on, together with the GPS UART, and clears it in standby and at power-off (inferred from
the code). RadioFoundry runs the fan while transmitting or hot (the stock "Both"), and sets bit 2 while the GPS module is
powered (section 3.6).

### 5.1 DMR

The stock firmware has two register sets, analog and digital, written at receive set-up:

| Register | Analog | Digital |
| --- | --- | --- |
| 0x13 | 0x0396 | 0x0375 (0x0396 with the second receiver) |
| 0x44 | 0x43EC | 0x53EC |
| 0x73 | 0x464A (0x564A at 25 kHz) | 0x4E8A (0x569A with the second receiver) |
| 0x7E | 0x342E | 0x361E |
| Page 1: 0x1B, 0x1C, 0x1D, 0x27, 0x36 | 0x0004, 0x04D7, 0x39FD, 0x091B, 0x9D08 (0x1D08 TX) | 0x07FF, 0x04D3, 0x39FD (0x29A1 TX), 0x891B, 0x1D59 |
| 0x43 (receive) | 0x6009 | 0x6E09 |
| 0x3A (receive) | 0x0040 (0x00E0 at 25 kHz, not yet copied) | 0x0040 |
| 0x18, page 1 0x17 (receive) | not written | 0x4125, 0x0AA8, as for DMR transmit |
| 0x47 (receive) | 0x6140 | 0x4840 at 300 MHz and above, 0x6840 below |

In the BK4819 map 0x43 bits 11:9 are the weak-signal channel filter: 0x6E09 the widest (4.5 kHz), 0x6009 1.7 kHz
(inferred). 0x47 bits 11:8 = 8 gives the baseband the demodulator output instead of the voice audio, and bit 13
inverts it where the LO is above the signal (VHF), so the baseband sees one polarity on both bands. With the analog
values in DMR receive the baseband finds sync but few bursts decode (a strong control channel: about one burst in
six); with the digital ones every burst decodes (d578uv-bringup.md, status 8).

Transmit keys the RF chips once, before the baseband gets `DMR_CALL_SLOT` with the transmit bit, and keeps them in
transmit for the whole call; the digital set carries over. Per burst only the PA is switched, from PD0 (`pa_gate.h`).
At the end the baseband gets `WORK_MODE` and `DMR_CALL_SLOT` 0x10, then the RF goes back to receive.

PD0 is high for 30.0 ms per burst, but the burst's modulation lags it: it starts 7.0 ms after the rising edge and ends
4.5 ms after the falling edge (measured on the bench by the sync's position in the RF envelope, the same on every
burst). A PA that followed PD0 keyed 5.75 ms into the slot before and cut off the burst's last 4 ms: every LC header
and terminator came out with 16 to 25 bit errors in its last 21 symbols, and voice bursts with 1 to 6. So the PA
follows PD0 late: the DAC and PE6 on 5.75 ms after the rising edge, the DAC off 5 ms after the falling edge and PE6
800 us later. The RF is then on from 1.25 ms before the burst to about 1 ms after it, and every burst decodes with 0
bit errors. `DMR_OFFSET` (offsets 0, 0x82 and 0xFF, length 1200) and `MODOUT_CONFIG` byte 1 (0x00, 0x28) moved
neither PD0 nor the modulation.

Through a repeater, with the baseband's own set-up, the inbound burst ran **0.85 ms late**: against the outbound plus
30 ms, median +0.854 ms, range +0.792 to +0.917 ms, 136 bursts. ETSI TS 102 361-1 10.2.3.1.3 allows 1 ms for
propagation and drift; the test allows the radio 0.25 ms of its own. The baseband times its transmit from the slot
timing it receives, and its 4FSK reaches the RF through the codec's DAC and the modulator outputs
([modem-sct.md](modem-sct.md) 5). Neither setting above moves the burst. These do:

| Setting | Where | Effect |
| --- | --- | --- |
| The codec's DAC on processing block PRB_P17 (page 0 register 60 = 17): interpolation filter C, 13 samples of delay where the baseband's set-up has 18 | `codec_start` in `modem_d578uv.c`, sent at each DMR start | the burst 0.41 ms earlier |
| `MODOUT_CONFIG` byte 0 = 0xEB: LOL, which carries the 4FSK, 21 steps of 10.4 us later than LOR | `tx_fields`, before each transmission | sets the burst on the grid with the DAC filter above. Positive values moved it by less than a sample |
| `tx_fields` sent once at each start as well | `sct_dmr.c` | the first transmission after the baseband's boot no longer runs 0.65 ms later than the ones after it |

With all three, five calls of 142 bursts each, two of them the first call after a fresh boot: medians -0.020 to
+0.042 ms, every burst within -0.063 and +0.063 ms. Burst start from the sync correlation, a bench board's idle
outbound as the reference, both sides decoded on a second board (`test_dmr_repeater_burst_offset`).

| Step | Stock firmware | RadioFoundry (`radio_d578uv.c`) |
| --- | --- | --- |
| Key-up | chips off, chip 0 to the TX frequency, 0x28 output stage, 0x18 = 0x4125, page 1 0x17 = 0x0AA8, sub-audio off, drive 0x40 = 0, 0x47, 0x03 = 0xC1FE, GPIO bit 5 above 300 MHz, 10 ms | the same, with the digital set rewritten first (`qa4818_dmr_tx`) |
| PA level | the FM transmit's calibrated DAC value | the same |
| PD0 | input with pull-up, EXTI 0 both edges, enabled at start-up; the handler ignores it unless transmitting | the same; the interrupt is on only while keyed |
| PE6 tail | PD0 low: the PA DAC to 0 and TIMER1 started, one-shot, period 0x77 and prescaler 0x31F: 800 us at the 120 MHz timer clock. At its end PE6 is cleared only if PD0 is still low | TIMER1 one-shot: on 5.75 ms after PD0 rises, the DAC to 0 5 ms after it falls, PE6 800 us later |
| A burst under way at key-up | on a digital channel PE6 set at once only if PD0 is already high (an FM channel sets it at once without reading PD0) | the key-up is refused, a deliberate difference: the baseband has not been told to transmit yet, so a high PD0 is a line nobody drives (it floats high on the pull-up) or the opposite polarity; only a rising edge turns the PA on |
| PC4 | set at a burst in some baseband states when two frequencies (not traced which) are on the same side of 300 MHz | not driven (as in FM) |

The core keys the radio before its call stack starts the modem's transmission, so the RF is keyed before
`DMR_CALL_SLOT` with the transmit bit in every DMR transmission, and released after `TX_DONE`.

Back to receive, from the last burst's PD0 fall: the board counts PD0's edges (`burst_edges`,
[modem-sct.md](modem-sct.md) 5), so a transmission without voice ends at that fall: `TX_DONE` and `tx_off()` at
1.6 ms, the RF chips in receive at 7.8 ms (`diag` line 5, `rx`), `CARRIER_READY` 2 at about 16 ms. The baseband's
own end (ACK 0x0A) comes about 17 ms after the fall. A voice transmission still ends on it.

A one-burst reply (a link ACK) from a simulated station on the bench:

| Reply starts after our last burst ends | Decoded |
| --- | --- |
| 32.5 ms (the slot after next) | no: the baseband finds no sync in it |
| 62.5 ms | every try |

The 32.5 ms reply stayed undecoded with every change tried: `CARRIER_READY` 5 to 40 ms after the return to receive,
the original firmware's full-duplex `WORK_MODE` 0x23 after the transmission with `CARRIER_READY` at once, a fixed
`DEMOD_GAIN` (2048, 0 dB), a much weaker reply (to rule out an overloaded receiver), and the LO chip (chip 2) left on through the
transmission. The limit is in the receive path after `tx_off()`; which part is not known. The RF chip's RSSI and
noise reach their receive values 30-46 ms after the return to receive.

### 5.2 Side B

Side B is chips 1 (receiver) and 3 (LO), set up after side A when dual receive is on; either side can transmit. Shift
register bit 0x08 picks the speaker's source: clear for the baseband's audio (a DMR call on the open side), set for
FM. The volume device on PA13/PA14 sets each side's level: channel 2 side A, channel 3 side B. The stock firmware sets
both once at start-up; PA7 mutes the speaker for both.

The stock side B set-up, FM:

| Step | Writes |
| --- | --- |
| Off | chips 1 and 3: 0x03 = 0 |
| Path | chip 1: the FM receive path and constants as side A's, 0x70 = 0, 0x52 the tone detector |
| Tune | chip 3 (LO) and chip 1 at the 38.55 MHz IF; the LO by the 300 MHz rule (section 5) |
| Outputs | chip 1 0x28 = 0x0005, chip 3 0x28 = 0x0020 |
| Tone, gain | chip 1: the receive sub-audio; the gain code from side B's AGC base (UHF and VHF block byte 9) and tables (calibration 0x640 VHF, 0x660 UHF) |
| On | chip 3 0x03 = 0xC1FE; chip 1 0x47 = 0x6140, 0x03 = 0xBFF5 |
| Band | chip 0 bits 7/4 (section 5) |

Dual receive and the baseband (stock firmware, seen): with dual receive on and neither side's channel analog (both
digital, or mixed), the stock firmware sets a baseband parameter of its own, the second-receiver flag (its DMR
parameter 0x1C). With an FM channel on either side the flag stays clear. The flag changes three things:

| Item | Without | With the second receiver |
| --- | --- | --- |
| DMR_CALL_OPTION byte 3 | bit 0 clear | bit 0 set (every LC terminator reported) |
| DMR_CALL_OPTION byte 4 | 0x08 | 0x29 (bits 5 and 0 added); the Packet Interface document calls the byte reserved |
| Side A's chip 0 in DMR (5.1) | 0x13 = 0x0375, 0x73 = 0x4E8A | 0x13 = 0x0396, 0x73 = 0x569A |

A change of the flag resends the call option. A digital side B sets chip 1's 0x47 to 0x4840 or 0x6840 instead of
0x6140. How the baseband takes side B's signal, and how its reports name the side, is not known (section 9).

RadioFoundry (`radio_d578uv.c`, `CONFIG_DUAL_WATCH`): side B is the core's second receiver (`board.rx2`) for an FM
channel; a DMR channel is refused, so dual watch alternates side A as on a one-receiver radio. The squelch is the
core's noise levels with its hysteresis, and the tone flags of 0x60. Side B gets the speaker only while side A is
quiet (the core's rule): then side A's level goes to 0, side B's to the volume and shift register bit 0x08 is set. Side
B stops while side A transmits and comes back after. The AGC bytes and tables, the band pair's sense and the level
channels are inferred from the stock code; untested on the radio.

The air band uses side B's chips too (5.4).

### 5.3 TX tones

The 1750 Hz burst comes from chip 0's tone generator; DTMF is synthesized on the MCU and fed to the chip's audio input
pin. Both mute the microphone with chip 0's 0x25 bit 0 while they sound.

| Step | 1750 Hz burst | DTMF |
| --- | --- | --- |
| Audio source | page 1 0x19 = 0x7800 | page 1 0x19 = 0x7800, 0x18 = 0x4125, page 1 0x17 = 0x03B9 (0x03B8 for the microphone: bit 0 the input pin, inferred) |
| Drive 0x40 | above 300 MHz 0x3900 + level 0x32, above 200 MHz 0x3900 + 0xB4, else 0x3800 + 0x82 (base, level << 1, one step less at 25 kHz) | base 0x3900 / 0x3800 / 0x3700 by the same limits, level the UHF block's byte 0x17 above 200 MHz, 0x18 below |
| Path | page 1 0x1B = 5 | page 1 0x1B = 5 |
| Tone | 0x71 = 9030 (1750 Hz), 0x70 = 0xA000 | PB9 PWM (below) |
| End | page 1 0x1B = 4, 0x70 = 0, the voice drive | page 1 0x1B = 4, page 1 0x17 = 0x03B8, the voice drive |

The stock firmware picks the burst's tone from a table of 1000, 1450, 1750 and 2100 Hz by a setting.

APRS transmit (RadioFoundry, `CONFIG_APRS`; the stock firmware's APRS is the add-on board, 3.4): the DTMF path
with one tone. The chip's audio input at the calibrated DTMF drive, the microphone muted; TIMER4's interrupt writes one
sample of a phase continuous 1200 / 2200 Hz tone per tick, and counts timer clocks to change the tone every 100,000
clocks (1200 baud; a symbol is about 21.8 samples). At the core's APRS level 50 the PWM's peak is 90 counts of 128,
near the DTMF pair's 102 (the level is a setting the debug link changes); the deviation that gives is inferred until
measured.

DTMF: TIMER3 CH3 runs PB9 as an 8-bit PWM (period 256, prescaler 0, PWM mode 1). TIMER4's update interrupt, period
4592 clocks (26,132 Hz at 120 MHz), steps two 16-bit phase accumulators by f x 65536 / 26,132 and writes the sum of
two sine tables as the next duty: the high tone at amplitude 63, the low tone at 39 (about 4.2 dB twist), centered
on 128. At the end PB9 goes back to analog.

RadioFoundry (`tx_tone`, `tx_tone_key`, `tx_dtmf`): the same sequences. Keying a tone (Morse) switches 0x70 with the
microphone still muted. The DTMF synthesis is `tone_d578uv.c` (`tone_synth.h`); its sine table is RadioFoundry's own. Untested on the
radio.

### 5.4 Air band AM and broadcast FM: the KTMicro or RDA5807 receiver

The air band receiver is a superhet: chip 3 is the LO and first mixer, and a KTMicro AM/FM receiver demodulates AM at
a 29.25 MHz IF. The receiver sits on a bit-banged I2C bus (PB6 SDA, PB7 SCL) at address 0x6A / 0x6B, 8-bit registers
and data; registers 0x02 / 0x03 hold its ID 0x4B 0x54 ("KT"). The stock firmware reads it at start-up and uses it only
when it answers. Its broadcast FM mode receives 87.5-108 MHz in 50 kHz steps (87.5 MHz the default, both ends wrap in
the stock firmware); chip 3's output bit 6 clear selects that input (inferred). No register map is public: the values
below are the stock firmware's, their roles inferred. Driver: `drivers/bcast/ktamfm`.
Some units carry an RDA5807 instead, on the same two pins (seen on a 578UV PRO: chip ID 0x5804, nothing at
0x6A). The stock firmware uses it whenever the KTMicro probe fails, without probing it: broadcast FM only, no air
band (the AM settings and menu entries are hidden). RadioFoundry probes the KTMicro first, then reads the RDA5807's
chip ID (`diag` line 7: `bc kt`, `bc rda` with its 0x02 / 0x0A / 0x0B, or `bc no`), and drives it with the shared
driver ([airband-and-broadcast.md](airband-and-broadcast.md) 4.2).

| RDA5807 item | Stock |
| --- | --- |
| Bus | sequential mode at 0x10 (0x20 / 0x21); writes retried, then 5 ms. The chip also answers at 0x11 (random access, the driver's) and 0x60 |
| On | 0x02 = 0xC009, 0x03 = channel \| TUNE \| band 2 \| 50 kHz, 0x04 = 0x0402 (GPIO1 low), 0x05 = 0x85A9 (volume 9 fixed: the level device sets the loudness); 32.768 kHz crystal; STC not polled |
| Off | 0x02 = 0x8000 (disabled), 0x04 = 0x0403 (GPIO1 high) |
| At boot | soft reset, 5 ms, then 0x02-0x21 in one write from a table (chip left disabled; RadioFoundry doesn't write it) |
| Station | 0x0B bit 8 (FM_TRUE); RSSI, stereo and seek unused |
| Routing | as the KTMicro's FM, but chip 3's bit 6, PA5 register 2 and the receiver mute in the squelch paths are KTMicro only. What GPIO1 switches isn't traced |

On the radio (a wire on the antenna jack): local stations at RSSI 38-67 with FM_TRUE, empty
channels 26-35; broadcast FM plays through the speaker and mutes with knob A at 0. With the jack bare, RSSI 2: the
receiver takes its signal from the antenna jack. The stock boot table changed no reading and isn't written.

| Step | Stock writes (receiver registers, read-modify-write where masked) |
| --- | --- |
| Wake | 0x0E bit 5 clear; 1 ms; 0x76 bit 1 set; 5 ms; 0x10 bit 6 clear |
| Set-up, once | wake, 50 ms, 0x06 low 3 bits = 2, 0x07 = 0x9C, 0x08 low 4 bits = 8, 0x09 = 0x0A = 0, 0x0D bit 4 clear, 0x04 = (0x04 & 0xF8) \| 0x80, wait for 0x1B bit 2 (100 us polls), 0x66 bit 3 clear, 0x38 bits 6:4 = 5, 0x62 = 0x51, 0x2B = (0x2B & 0x76) \| 8, 0x2F = 0x3F, 0x7E high 4 bits = 2, 0x16 bit 4 set, 0x57 = 0x20, 0x24 bit 7 clear, 0x41 low 6 bits = 0x10, 0x43 high 2 bits = 1, 0x2D bits 6:5 clear, 0x42 = 0x32, 0x3B bit 7 set, then muted |
| AM mode | 0x88 bit 6 set (clear: FM), 0x8D = 0x4C, 0x8C low 7 bits = 4, 0x3A and 0x52 low 6 bits = 0x19, 0x0E bit 4 set; level 0x1E (0x1F with Bluetooth audio) |
| AM tune | kHz: 0x8D = bits 7:0, 0x8C low 7 bits = bits 14:8, then 0x88 bit 7 set |
| FM mode | 0x88 bit 6 clear, nothing else (the AM mode's other values stay); level 0x1A |
| FM tune | channel = MHz x 20: 0x2F bit 1 clear; 0x87 bit 3 clear; 0x89 = bits 7:0, 0x88 low 4 bits = bits 11:8 with bit 7 set. Channels 0x699, 0x81A and 0x840 (84.45, 103.70, 105.60 MHz) set 0x87 bit 3 instead and 0x85's low nibble to 3 (0x699) or 8 |
| FM station | 0xE6 bits 6:0 > 14, 0xE2 bits 5:0 >= 12 and \|2 x (0x79 bits 6:0) - 0xE9\| < 12, three polls in a row |
| Level | 0x0F bits 4:0; 1 mutes. The squelch's close mutes it. The stock FM level adds 3 by a flag and a setting's offset (neither identified) |

The stock AM set-up (part of side B's): chips 1 and 3 off, chip 1 0x70 = 0 and 0x52 = 0x028A, chip 3 0x25 bit 6 set,
the receiver set up, AM mode and tuned to 29250 kHz, chip 3 tuned to f - 29.25 MHz (f + 29.25 MHz by the "AM Offset" setting;
below is its default, 1, likely labeled "Negative"; 0 "Positive" puts it above. The labels are inferred from the
only two-entry sign list in the image) in sub-band 3 at or above 140 MHz, 4 at or above 85 MHz, else 5, then chip 3 0x03 = 0xC1FE. Chip 0's
side B pair is bits 4/7 = 1/0 (the VHF sense) and shift register bit 0x04 is set. Either side in AM uses this
hardware; chip 1 stays off.

AM squelch: two 7-bit readings, 0xEA (the level) and 0xEC. With level L, open when 0xEC exceeds the VHF calibration
block's byte 0x19 and 0xEA reaches byte 0x1A + 3 x (L - 1); stay open while 0xEC > byte 0x19 - 3 and 0xEA > that
threshold - 3; three polls in a row decide. How the receiver's audio reaches the speaker isn't traced.

RadioFoundry (`radio_d578uv.c`, `CONFIG_AM_RX`): `rx_am()` runs the stock set-up for the main receiver and turns
chips 0 and 2 off as well; side B (dual watch) waits until the next `rx()`. The LO is below the signal, the stock default (`rf_plan.h`), and bit 0x04 is set. `status()`
reports no carrier while 0xEC is at or below byte 0x19, else an RSSI of -82 dBm + (0xEA - byte 0x1A), and the AM
squelch value 64 - (0xEA - byte 0x1A) (142 with no carrier), so the core's AM squelch levels (level 1 below 64, 3 a
level) open at the same thresholds. AM is refused without a
calibration block or when the receiver didn't answer at start-up; the core then receives FM. The receiver's level is
up only while the core gives the main side the speaker, and both level device channels take the volume (which one
carries the receiver is inferred). Host tested (the receiver's sequences), untested on the radio.

The stock broadcast set-up is a side B command: chip 3's output bit 6 clear, the receiver set up once, FM mode, tuned.
It doesn't wait for the tune and reads no stereo flag that was found.

RadioFoundry (`CONFIG_BCAST_FM`, `d578uv_bcast`): the core's broadcast receiver (`hal_bcast_t`, 87.5-108 MHz, 50 kHz
grid). `power(true)` takes the receiver from AM if it has it (chip 3 off, side B again), clears chip 3's bit 6 and runs
the stock set-up; `power(false)` mutes it and leaves an AM set-up made since alone (the core sets AM up first). The
receiver is never put in standby: no standby write is known. Side A keeps its last set-up, muted; dual watch's side B
runs on. The level is 0x1A while the core plays the broadcast (muted at volume 0), and the level device's two channels
take the volume, as for AM. `status()` reports 0xE6 as the RSSI (its scale against dBuV isn't known, so the core's
broadcast squelch levels, used by a scan, are a guess) and the stock station test; no tune-done or stereo reading.
There is no seek. Host tested (the receiver's sequences), untested on the radio.

### 5.5 Trims on the PA5 device

The serial device on PA5 (3.4: chip select PA5, clock PB3, data PB4; a 12-bit frame, a 4-bit register then 8 data
bits, MSB first, write only) takes calibration bytes besides the backlight. What they drive is not known: their
per-band calibration points suggest filter trims (inferred). The stock firmware writes them after each retune, from
the calibration blocks (7):

| Register | When | Value |
| --- | --- | --- |
| 6 | each side A retune, not in TX | the block's 0x10 by side A's frequency (UHF at or above 300 MHz) |
| 0xA | the same, after 6 | interpolated over side A's frequency from the block's 0x0A..0x0C (below) |
| 1 | the same, after 0xA | the UHF block's 0x00, the factory item "FQCA" (default 0x23): the reference oscillator's trim (below); the VHF block's 0x00 is unused |
| 2 | each side B retune | AM: the VHF block's 0x1B. Side B the main channel or keyed: side B's band's 0x11. Broadcast FM without dual receive: the VHF block's 0x11. Else 0 |
| 0xC | each side B retune | AM: the air band table from the UHF block's 0x1B..0x1D at the AM frequency. Side B the main channel: interpolated from 0x0D..0x0F at side B's frequency. Else 0 |
| 4 | each TX start; kept after | UHF: 0x78 at Low and Mid, 0x82 at High, the UHF block's 0x16 at Turbo. VHF: 0x78 up to High, the VHF block's 0x16 at Turbo. With dual receive on, Turbo counts as High |

Register 1 trims the reference oscillator: measured on one radio at 446.075 MHz, the carrier moves linearly over the
whole range, about 41.5 Hz a step (0.093 ppm), from -2.6 kHz at 0 to +5.4 kHz at 192, settling within about 1 s. It
moves every carrier, receive and transmit. That radio's calibration (0x27) left it 2.3 ppm low; the reference trim
setting corrects such a radio ([radio-features.md](radio-features.md) 6.1).

Each interpolated trim is a table of byte cells from three points p0, p1, p2; a frequency's value lies between two
cells:

| Table | Points | Cells | Steps (signed, truncated toward zero) |
| --- | --- | --- | --- |
| UHF | 0x0A..0x0C (0xA), 0x0D..0x0F (0xC) | 13: 400 MHz + 10 MHz x i | (p1 - p0) / 6 up from cell 0 to cell 6 = p1, (p2 - p1) / 6 up from cell 6 to cell 12 = p2 |
| VHF | the VHF block's, the same bytes | 45: 136-174 MHz in 1 MHz (cells 0-38), 220-225 MHz (39-44) | (p1 - p0) / 38 up to cell 38 = p1 (174 MHz); (p2 - p1) / 51 down from cell 44 = p2 (225 MHz) |
| Air band | the UHF block's 0x1B..0x1D | 38: 108 MHz + 1 MHz x i | (p1 - p0) / 19 up to cell 19 = p1 (127 MHz); (p2 - p1) / 18 down from cell 37 = p2 |

Cells wrap modulo 256. The value at a frequency: cell i, plus m tenths of the way to cell i + 1 (m the next digit:
1 MHz in a UHF cell, 100 kHz in the others), taking the difference as an unsigned 16-bit value, so a falling table
doesn't interpolate down. Past the last cell, the last point. Register 0xA uses the UHF table above 300 MHz and the
VHF block's 220 MHz cells from 200 MHz. Factory default points: 0x0A..0x0C 1E 87 FF (UHF), 03 87 FF (VHF); 0x10 B4,
AA; 0x11 AA, AA; 0x16 96, 91; the UHF block's 0x1B..0x1D 41 6E B4 and the VHF block's 0x1B B4. The Artery firmware
computes the same.

RadioFoundry (`rf_plan.h`, `radio_d578uv.c`): the same values by the same arithmetic, computed when written (no
tables in RAM). While side B receives (dual watch), registers 2 and 0xC take side B's band's 0x11 and the table from
0x0D..0x0F at its frequency, as for side B the main channel: with both 0 side B hears nothing (measured: its RSSI
stays at the floor through a strong signal side A hears). They are written when side B tunes and when it stops.
Turbo is not lowered with dual receive. Without a calibration block the start-up's 0s stay. Host tested against the
factory defaults' tables and lookups worked out apart from the code.

### 5.6 Microphone level (VOX)

The stock firmware has no VOX, and no ADC input carries the microphone. Chip 0 has it: its enable word turns the
microphone ADC on in receive as well as in transmit (bit 2 of 0xBFF5 and 0xC1FE), and in the BK4819 map register
0x64 is the voice amplitude its VOX detector compares (thresholds 0x46 and 0x79, 11 bits).

RadioFoundry (`hal_power_t.mic_level`, `radio_d578uv.c`): while a reader has asked for the level within the last
second, the radio task reads chip 0's 0x64 at each status poll, in FM receive and on an FM transmission, and gives
the core `miclevel_log(min(value, 0x7FF), 11)`. In receive it clears chip 0's 0x25 bit 0 (the microphone mute) while
the meter runs and sets it again after. The chip's own VOX detector isn't used: the core decides. Inferred: the
register, its scale, that the mute bit is the only gate between the microphone and chip 0, and that unmuting in
receive has no side effect. Written, untested on the radio.

### 5.7 Analog microphone gain

At each FM key-up the stock firmware writes the FM TX table (page 1 0x17 = 0x03B8 among others), then chip 0's
microphone level, which overrides 0x17:

| Register | Value |
| --- | --- |
| 0x18 | 0x0125 \| g << 11; g = 2, 3, 4, 5, 6, 7 for the six levels of a user setting (default g = 4) |
| page 1 0x17 | 0x0200 \| k << 3; k = the UHF calibration block's byte 0x19 (factory item "MIC", 0..0x3F), adjusted by the setting: -2, 0 or +1 |

DMR transmit and the audio input pin (DTMF, 5.3) use fixed values instead: 0x18 = 0x4125, page 1 0x17 = 0x0AA8 and
0x03B9. The end of a DTMF sequence runs the microphone level again. The setting's menu name isn't tied to it in the
code ("AnaMic Level" is likely). The 0x03B8 in the table is the same formula with k = 0x37.

RadioFoundry writes the default level (g = 4, k the calibration byte) at each FM key-up and after each tone
(`qa4818_mic_level`). The core's Mic gain setting isn't mapped to g: the dB per step isn't known.

### 5.8 Transmit power

PA4's DAC sets the PA drive. The factory calibration (7) gives an 8-bit value per level and frequency; the DAC takes
it shifted left by 4. RadioFoundry's drive unit is the 12-bit DAC word (`hal_radio_t.tx_drive_level`, debug link RADIO
op 36). Each key-up also writes register 4 of the PA5 device by band and level (5.5, `trim_tx` in `rf_plan.h`), so a
drive gives a different power at another level: each level's drive is measured at that level.

Measured with a wattmeter into a dummy load, FM, no sub-audio tone, 3 s key-ups ([power
calibration](../testing/power-calibration.md)). UHF at 446.050 MHz and a second point about 15 MHz higher; VHF at
146.535 MHz and a second point about 5 MHz higher. Drive and watts:

| | UHF 446.050 | UHF, higher point | VHF 146.535 | VHF, higher point |
| --- | --- | --- | --- | --- |
| Factory rows, lowest to top | 384 → 1.50, 1424 → 13.2, 2240 → 30.9, 2960 → 53.0 | 432 → 2.20, 1472 → 13.9, 2336 → 30.2, 3120 → 38.5 | 368 → 1.88, 1040 → 13.7, 1600 → 32.8, 2496 → 73.2 | 384 → 1.77, 1056 → 13.1, 1664 → 31.5, 2368 → 71.2 |
| Low end (lowest level's settings) | 64, 128, 192 → 0; 224 → 0.15, 256 → 0.41, 320 → 0.94, 328 → 1.01 | 160 → 0; 192 → 0.20, 224 → 0.46, 320 → 1.26, 369 → 1.67 | 192 → 0; 256 → 0.55, 301 → 1.08, 320 → 1.29 | 256 → 0.28, 301 → 0.79 |
| Top (top level's settings) | 2600 → 43.8, 2741 → 47.7 | 2789 → 38.3 | 2000 → 53.5, 2150 → 61.0 | 2000 → 52.1, 2150 → 59.7 |

- The factory rows put out about 1.5-2 times the rated 1, 10, 25 and 45 / 50 W, the VHF top row 71-73 W.
- No cliff: output falls smoothly to zero (zero point UHF 192-224 at 446.050, 160-192 at the higher point; VHF about
  192-256) and stays at zero down to drive 64.
- Hot (three 5 s key-ups at 25 W, then at once): 192, 224 and 320 read 0.00, 0.16 and 0.95 W at 446.050, as cold.
- The UHF top saturates at about 38.5 W at the higher point (2789 and 3120 read the same).
- The factory's lowest row rises with frequency where the PA's gain at a fixed drive also rises (1.50 W against
  2.20 W), so a fixed drive holds the lowest level closer.

RadioFoundry's levels (`pa_drive` in `rf_plan.h`): the lowest a fixed drive per band, the others the
factory value scaled.

| Level | UHF | VHF |
| --- | --- | --- |
| 1 W | 316 | 304 |
| 10 W | factory × 0.865 | factory × 0.865 |
| 25 W | factory × 0.883 | factory × 0.890 |
| 45 W (UHF), 50 W (VHF) | factory × 0.894 | factory × 0.800 |

The short check with this table (section 4 of the procedure; dark step UHF 160 or 192, VHF 192), watts:

| Level | UHF 446.050 | UHF, higher point | VHF 146.535 | VHF, higher point |
| --- | --- | --- | --- | --- |
| 1 W | 0.92 | 1.24 | 1.09 | 0.81 |
| 10 W | 10.3 | 10.9 | 10.3 | 9.87 |
| 25 W | 25.2 | 26.0 | 26.0 | 25.0 |
| 45 W / 50 W | 45.4 | 38.4 (saturated) | 53.4 | 47.0 |
| Dark step | 0.00 | 0.00 (160) | 0.00 | 0.00 |

Every level is within ±1 dB of its name. The table holds only where the factory calibration is current; DMR uses the
same drives.

## 6. SPI NAND

### 6.1 Chip

2 Gbit (256 MB): 2048 blocks of 64 pages of 2048 bytes plus 64 spare bytes. Bit-banged SPI: CS PB8, CLK PB15, MOSI
PB14, MISO PA15 (PA15 and PB3/PB4 need JTAG off, SWD kept; the clock and data out pins confirmed on a radio, ID
0x98 0xCB). No SPI peripheral has these pins. The stock firmware's fast read is dual SPI: PB14 becomes the second
data input. Two parts are in use; the stock firmware accepts both and
differs only in the read path:

| Part | ID | Read from cache | ECC uncorrectable |
| --- | --- | --- | --- |
| Kioxia TC58CVG1S3HRAIG | 0x98 | `03` / `3B`, 2-byte column, dummy | status & 0x30 == 0x20 |
| GigaDevice | 0xC8 | 3-byte address (column in the low 16 bits), dummy | status & 0x70 == 0x70 |

`drivers/flash/spi_nand` handles both by manufacturer ID.

### 6.2 Stock layout

| NAND address | Use |
| --- | --- |
| 0x00000000 (block 0) | bad block table: 0x1F8-byte copies at each 0x400; signature "CHECK 2G BLOCK FOR VALID." at 0x1FF00 |
| 0x00020000-0x0047FFFF | fonts and icons (the resource update) |
| 0x00800000-0x00FFFFFF | channels, 32 pairs of 128 |
| 0x01000000 / 0x01020000 | zone members (a pair) |
| 0x01080000-0x0147FFFF | scan lists, 16 pairs of 16 |
| 0x024C0000 / 0x024E0000 | settings and the zone, ID, scan list and channel bitmaps (a pair) |
| 0x02500000 / 0x02520000 | settings (a pair) |
| 0x02540000 / 0x02560000 | zone names (a pair) |
| 0x02580000 / 0x025A0000 | radio IDs (a pair) |
| 0x025C0000 / 0x025E0000 | the RX group list bitmap at 0x025C0B10 (a pair) |
| 0x02640000 / 0x02660000 | the contact bitmap (a pair) |
| 0x02680000-0x028FFFFF | contacts, 10 pairs of 1000 |
| 0x02900000, 0x02940000 | digital contact list headers (role not known) |
| 0x02980000 / 0x029A0000 | RX group lists (a pair) |
| 0x02F60000 / 0x02F80000 | calibration and its backup (markers in the last 12 bytes of each block) |
| 0x03000000-0x03FFFFFF | spare blocks for the bad block table |
| 0x05500000-0x0D1FFFFF | a list of 500 records of 100 bytes |
| 0x0D700000-0x0DFFFFFF | a record index and a data ring |
| 0x0E000000-0x0FFFFFFF | never addressed by the stock firmware |

- If block 0's signature is missing at boot, the stock firmware erases and tests every block from 0x20000 up. Block 0
  must never be erased or rewritten.
- "Initialize radio" clears the codeplug, contacts, the record index and the ring; it doesn't reach 0x0E000000.
- The stock firmware replaces bad blocks from its spare pool by its own table, not by spare-area marks.
- The codeplug's tables, their block pairs and records: [codeplug-stock-anytone.md](codeplug-stock-anytone.md). The web
  flasher reads them through the stock firmware's PC mode before an install and writes them converted afterwards.

### 6.3 RadioFoundry's range

RadioFoundry programs and erases only blocks 2016-2047 (0x0FC00000-0x0FFFFFFF, 4 MB), the top of the range the stock
firmware never touches (`D578UV_NAND_RFOUNDRY_FIRST`, `D578UV_NAND_RFOUNDRY_BLOCKS`). The driver's write window
refuses anything else. The core's storage runs on [nand-sectors.md](nand-sectors.md) there (`flash_d578uv.c`):

| Logical address | Area |
| --- | --- |
| 0x000000 | open-firmware "EEPROM" image, 64 KB |
| 0x011000 | settings ring, 8 sectors |
| 0x019000 | sector journal, 2 sectors |
| 0x020000 | open-firmware "flash" image, 1 MB |
| 0x120000 | native codeplug, 1 MB |
| 0x220000 | staging, 256 KB |

The calibration stays in the stock blocks, read only. The sector layer retires a block whose program or erase fails;
it doesn't read the stock bad block table.

## 7. Calibration

Two 128 KB blocks at NAND 0x02F60000 and 0x02F80000. A block is current when its last 12 bytes hold `55 55 AA AA`
at +0x1FFFC and not `AA AA 55 55` at +0x1FFF8; the firmware reads the first current one.

| Offset | Bytes | Content |
| --- | --- | --- |
| 0x000 | 0x20 | UHF block: per-band values; +0x12 the S-meter base RSSI |
| 0x040 | 0x20 | VHF block; +0x19 / +0x1A the air band squelch values |
| 0x080 | | further tables: TX power DAC bases and offsets, 25 AGC entries (layout in progress) |
| 0x0D2 | 4 | the codec levels of the DMR voice set-up (4.4); factory default `08 04 14 14` |
| 0x0D7 | 2 | a demodulator gain, factory default 0; the stock firmware sends 0 in its place (4.4) |
| 0x0D9 | 2 | the baseband's digital mode speaker gain, MSB first; factory default 0x0546 |
| 0x0DB | 2 | the baseband's digital mode microphone gain, MSB first; factory default 0x07D0 |
| 0x140 | 8 | the baseband's `DMR_OFFSET` (4.4); factory default `00 82 02 D0 02 D0 02 D0` |

The S-meter: level = how many of base, base + 5, base + 10, base + 15 the RSSI (register 0x61) reaches.

## 8. Update mode

The vendor update mode runs on the radio's own USB serial port at 921600 baud. The bootloader enters it when the
radio is turned on with MENU and EXIT held (unverified, section 3.2).

| Step | Host sends | Radio answers |
| --- | --- | --- |
| Hello | `UPDATE` (ASCII) | `06` (ACK) on the bootloader seen so far (GD32, V100); the tools take `QX 06` too |
| Identify | `02` | 16 bytes: `I`, model (7 bytes, NUL padded), hardware byte, version (4 bytes), `00 00 06` |
| Data | `01`, address (4 bytes LE), 32 bytes, 16-bit LE sum of bytes 1-36, `06` | `06` ACK or `15` NAK |
| Finish | `18` | `06` |

- The model names differ by MCU (section 1). The vendor's updater compares the answer with the names in the
  firmware's `.spi` file and refuses a mismatch; RadioFoundry's tools refuse in the same way, and refuse to write
  one variant's image to the other (the GD32 image to an Artery radio, or the reverse).
- A vendor firmware set is a `.CDD` (the image), a `.CDI` (278-byte entries: name, address, length, offset) and a
  `.spi` (the model names). The application's address in the `.CDI` is the image's flash address.
- The application's own `UPDATE` answer (a `PROGRAM` session) writes fonts and icons to the NAND only; the firmware
  image is written by the bootloader.

Tools: `tools/anytone_update.py` and the flasher's AnyTone update backend.

## 9. Bring-up state

The order to check it in on a radio, with the diagnostics lines and what to change when a step fails, is
[d578uv-bringup.md](../testing/d578uv-bringup.md).

| Part | State |
| --- | --- |
| Clocks, GPIO, ADC, USB | written from the analysis, untested |
| Display, keys, encoder | the 8 front keys, the encoder's steps and switch and both volume knobs work on a radio (3.2); the display works |
| Backlight | register 8 of the PA5 device, the five stock levels (3.1); written, untested |
| PA5 trims | registers 1, 2, 4, 6, 0xA, 0xC from the calibration after each retune and key-up (5.5); written, host tested, untested on the radio |
| Microphone | the plain microphone on PA9/PA10 and the serial keypad microphone on USART0, told apart by its frames (3.2); the serial microphone's PTT and keys work on a radio. The analog microphone level from the calibration (5.7); written, untested |
| NAND, core storage | written, untested |
| Crash log | the platform's ([crash-log.md](crash-log.md) 2.4), untested: whether the record at the top of SRAM survives the bootloader |
| DMR baseband | driver, voice set-up and DMR start as the stock firmware sends them (4.4) with the squelch levels and digital mode gains, every LC header and terminator reported, the headphone mute while transmitting; written and host tested, untested on the radio |
| RF chips | `drivers/rf/qa4818` and `boards/d578uv/radio_d578uv.c`: FM receive and transmit and DMR receive and transmit keying (the PA per burst, 5.1) on side A; side B as the second receiver for FM (5.2); the 1750 Hz burst and DTMF (5.3). Host tested (the chip sequences), untested on the radio |
| GPS | the module on USART1, powered by chip 0's register 0x25 bit 2, the CASIC set-up on its first `$xxTXT` (3.6); written, the set-up's framing host tested, untested on the radio |
| Reference trim | the factory item "FQCA", the UHF block's byte 0 (default 0x23), is the reference oscillator's trim: PA5 register 1, sent after each side A retune (5.5); the frequency words carry no correction. RadioFoundry sends it the same way. Its own trim on top: each synthesizer is programmed at f x (1 + trim), side A's and B's LOs, the air band LO and the transmitter, with the IF chips at their IF ([radio-features.md](radio-features.md) 6.1). Measured: +2.3 ppm took a carrier on 446.075 MHz from -1018 Hz to +23 Hz |
| Flash stub | `CONFIG_STUB_ENTRY` on: the GD32F30x stub ([stub-gd32f30x.md](stub-gd32f30x.md)) with board record id 6, application base 0x08006000; `stub/stubtool.py --board d578uv` drives it. Host tested, untested on the radio |
| Air band AM | side B's chips and the KTMicro receiver (5.4); written, host tested (the receiver's sequences), untested on the radio |
| Broadcast FM | the KTMicro receiver's FM mode, or the RDA5807 where it is fitted (5.4); written, host tested. The RDA5807 works on the radio; the KTMicro untested on the radio. No stereo, no seek |
| Bluetooth | PTT and keys from the module's lines (3.7, `CONFIG_BT_PTT`); written, the line parser host tested, untested on the radio. Pairing, audio and the module's settings not written |
| DMR on side B | not written: what is missing is listed below |
| VOX | chip 0's voice amplitude as the microphone level (5.6); written, untested on the radio. The register and its scale are inferred from the BK4819 map |
| APRS receive | PC0 sampled at 9600 Hz into the core's demodulator (3.4); written, the sample path host tested with the demodulator, untested on the radio. Off on the GD32 builds: it takes about 11.6 KB of RAM (the demodulator, the station list, the iGate and the 2 KB sample ring) and about 6.4 KB is free with DMR on. On in the Artery builds (section 10) |
| APRS transmit | the tones on PB9 into chip 0's audio input with the DTMF set-up (5.3); written, the tones host tested through the core's demodulator, untested on the radio. The deviation per APRS level is inferred |
| Artery variant | `boards/d578uva` on the AT32F43x platform (section 10): builds, untested on the radio |

DMR on side B needs facts not in the stock code's static analysis or the documents, and a model the core doesn't have:

- The baseband's input. The Packet Interface document gives the SCT3258 one demodulator: baseband on the codec's
  right input channel, or one IF I/Q pair. It has no second-receiver mode, and calls DMR_CALL_OPTION byte 4 reserved.
  What byte 4 bits 5 and 0 select, which input side B's discriminator reaches, and whether the chip decodes both
  sides at once or one slot from each, are not known. There is no schematic.
- The reports. Which field of DMR_SLOT_FOUND, the LC reports and DIGC_DATA_FRAME names the side a burst came from,
  and whether the color code, the call slot and the call match apply to both sides or to each. The stock receive path
  that reads the flag is not traced.
- The voice. The baseband plays the voice to the codec itself (section 4). Which side's call it plays when both have
  one is not known; the speaker source bit (shift register 0x08) has one setting for baseband audio.
- Side A with the flag set: what chip 0's 0x13 = 0x0396 and 0x73 = 0x569A change. RadioFoundry uses the values
  without the flag, as the stock firmware does with an FM side B.
- The core. `hal_rx2_t` is a second receiver that decodes DMR itself, matches the groups and plays the call, as the
  GD-88's second SoC does. Here both sides' bursts would come through the one baseband and the one `hal_dmr_modem_t`
  that the core's DMR stack owns. Side B's calls would need either their own LC decoding and call matching in the
  board, which repeats the stack, or a second DMR receive path in the core. Neither exists, and about 6.4 KB of RAM
  is free.

Open on the radio:

- PTT on PA10 and the plain microphone's Up and Down (3.2).
- The Bluetooth module (3.7): whether it answers the probe, keeps its pairings without the stock set-up, and what
  `+KEY=01`-`04` are on the hand mic.
- The serial microphone (3.2): the status frame's light fields, and what the Key Lock switch does.
- The backlight levels' brightness, and the part behind PA5.
- The LED's colors: where the second color is switched.
- The deviation per 0x40 step (5) and the dB per microphone gain step (5.7); the microphone level's register and
  scale (5.6); the
  audio routing to the speaker (shift register 0x08/0x40, PA7,
  the volume device).
- Air band (5.4): the receiver's audio path to the speaker, chip 3's output bit 6, the
  RSSI scale against the core's AM squelch levels, and that chip 0's outputs (the GPS supply, the band pairs) hold
  while chips 0 and 2 are off.
- Broadcast FM (5.4): the receiver's input with chip 3's bit 6 clear, its 0xE6 reading against dBuV (the core's
  broadcast squelch), a tune's settling time, and whether a stereo flag exists.
- Side B (5.2): its gain from the side B AGC bytes, the band pair's sense and the volume device's channel 3.
- Chip 0's 0x25 bit 0 as the microphone mute; the burst's and DTMF's deviation (5.3).
- DMR voice: the codec levels, digital mode gains and microphone gain of the voice set-up, and the DMR start's
  values (4.4): the baseband's squelch with the stock thresholds, `DMR_OFFSET` against the PA gate, and whether
  every LC header and terminator reported reaches the stack as on a burst modem.
- The bootloader's update entry keys.
- Whether the bootloader stays in its update mode when the application's first word is erased (the flash stub's
  interlock relies on it: an install cut short must leave a radio the update mode finishes), and the stub's hand-over:
  the adopted and the re-enumerated USB start, the FMC erase and program.
- GPS: that bit 2 of chip 0's register 0x25 powers the module, which module it is (CASIC or MTK), that a CASIC
  module sends `$xxTXT` after power-on and takes the set-up, and an MTK module's default output at 9600 baud (3.6).

## 10. Artery builds

The Artery radios run `boards/d578uva`: the same radio code (`boards/d578uv`) built on the AT32F43x platform
(`platform/at32f43x`). The board code reaches the MCU through `mcu_io.h`, which each platform provides with the same
names. Facts below are from the PRO V1.21 Artery image; `(inferred)` marks a register bit's meaning taken from how
the image uses it, without the reference manual.

| Item | Value |
| --- | --- |
| MCU | AT32F435 or AT32F437 (which is not known), more than 1 MB of flash: the image erases and writes a block at 0x082F5800 |
| Application | from 0x08006000, as on the GD32 builds; the vendor bootloader below it is never written |
| Modem boot slot | RadioFoundry uses the GD32 builds' slot, 0x080FF000-0x080FFFFF. The stock image carries the block elsewhere (0x080FA26E in PRO V1.21, 0x080FA35A in V2.08); every stock build's block has the same hash |
| RAM | the stock image uses 0x20000000-0x20017180. It reads no option byte, so the SRAM size EOPB0 sets on a radio is not known; RadioFoundry takes 128 KB, the least any setting leaves |
| Clock | PWC LDOOV = 1, flash divider (0x40023C60) = 1, then HEXT, PLL MS 1, NS 20, FR /2 (12 MHz to 120 MHz), AHB /1, APB1 and APB2 /2, the switch with the automatic step on (MISC2 bits 5:4), which is turned off after |
| USB | OTGFS1 on PA11/PA12 (function 10), clock from the 48 MHz HICK: MISC1 bits 12, 14, 13 set in that order (inferred), the ACC trimming it from the start of frame (C1 7980, C2 8000, C3 8020). GCCFG bit 16 (transceiver on) and bit 21 (VBUS ignored), forced device mode. Stock VID:PID 2E3C:5740 |
| Timers | TMR2 (the PE6 burst tail), TMR4 CH4 on PB9 function 2 (the beeper PWM), TMR5, TMR6, TMR7: the GD32 builds' TIMER1..6 at the same addresses |
| EXINT | PD0 on line 0 and PC8 on line 8 through SCFG, both edges |
| DAC | channel 1 on PA4 (the stock image writes it with the output buffer off and a software trigger) |
| ADC | ADC1, HCLK / 4 (inferred), PA6, PB0, PB1, PC0, PC1, PC2, PC5; calibration after the ready flag |
| Serial | USART1 PA9/PA10 115200, USART2 PA2/PA3 9600 (GPS), UART4 PC10/PC11 115200; functions 7, 7, 8 |
| Watchdog | WDT at 0x40003000, used by the stock image only to force a reset |
| Debug | SWD; the debug pins are plain GPIO once a pin's mode is set, with no separate switch as on the GD32 |
| Flash controller | 0x40023C00. The stock image's flash library: unlock with 0x45670123, 0xCDEF89AB (bank 1 at +0x04, bank 2 at +0x44); sector erase with CTRL bit 1, the address at +0x14 and bit 6; halfword program with CTRL bit 0; STS bits 0 (busy), 2 and 4 (errors) read; CTRL bit 7 locks. It takes bank 2's registers (+0x40) for 0x08200000-0x083EFFFF, so the part has 4032 KB with a 2 MB bank 1. It erases in 2 KB steps (0x082F5800 on, 21 steps) and reads no user system data |
| Flash stub | `CONFIG_STUB_ENTRY` on: the AT32F43x stub ([stub-at32f43x.md](stub-at32f43x.md)) with board record id 7, application base 0x08006000; `stub/stubtool.py --board d578uva` drives it. Host tested, untested on the radio |
| APRS receive | as on the GD32 builds (3.4): TMR6 (IRQ 54, shared with the DAC) and ADC1's preempted group, PSQ's PSN4 with PCLEN 0, started with CTRL2 PCSWTRG (bit 22, the external trigger off), the result in PDT1. Untested on the radio, including that a preempted conversion interrupts and resumes an ordinary one as on the GD32 |
| USB network | TinyUSB's NCM class on the same OTGFS1 port driver (TinyUSB's dwc2) as the serial mode; nothing in the port glue is mode specific. Untested on the radio |

Untested on the radio for the flash stub: the hand-over (frame buffer staging, the copy, the WDT stretch), the adopted
and the re-enumerated OTGFS1 start, the erase (whether this part's sectors are 2 KB or 4 KB; the stub works with
either), word programming, and that the zero-wait area follows an erase or a program. The interlock's question of the
GD32 builds (section 9: the bootloader with the application's first word erased) applies here too.

The 128 KB take what the GD32 builds leave out for RAM (`boards/d578uva/config.mk`):

| Feature | RAM |
| --- | --- |
| APRS receive and the iGate | about 11.9 KB, with the sample ring |
| The USB network adapter and the DMR IP gateway | about 8.8 KB |
| mDNS, TFTP and the ID database over TFTP | about 0.6 KB |
| SNMP | about 3.4 KB |
| The control station mode | under 0.1 KB |
| Zone sharing | about 4.7 KB |

That leaves about 8.4 KB of static RAM free. TCP and the services on it (the HTTP server, the web control, rigctld,
KISS and APRS-IS) are off: TCP alone takes about 4.7 KB. USRP isn't written on either build: it needs PCM both ways,
and the voice stays in the baseband.

RadioFoundry differs from the stock image where a choice doesn't change the hardware: it trims no timer for the
5 ms tick (its tick is 1 ms), feeds the watchdog, runs the DAC buffered and untriggered as on the GD32 builds, and
reads the ADC one channel at a time (the APRS receive sampling aside). Not yet known: the flash size register and unique ID addresses (the image reads
neither; the platform uses 0x1FFFF7E0 and 0x1FFFF7E8, unverified), and the drive strength codes the image writes for
every pin (RadioFoundry leaves the reset value).
