<!-- SPDX-License-Identifier: MIT -->
# Anytone AT-D868UV / AT-D878UV / AT-D578UV hardware

Status: research, 2026-10-12, facts only. ODR has no Anytone board. The owner has an AT-D868UV (hardware version and
MCU not yet checked). This page collects what is known about the family, what an ODR port would need, and what is
still missing. The survey entry is in [radios.md](radios.md).

**Summary.** These radios are a GD32F303 (or, on late D578UVs, an Artery AT32F43x) MCU driving a Sicomm SCT3258 DMR
baseband over an 8-bit parallel host port. The SCT3258 runs DMR layers 1 and 2 and the AMBE+2 vocoder itself, and
voice audio goes codec ↔ SCT3258 over I2S without passing through the MCU. The RF chip is the AT1846S (or a
compatible QA4818N), which ODR already drives. Loading our firmware looks possible: native USB, a documented vendor
update mode, and SWD pads with no read-out protection on the units documented so far. The cost is the baseband.
ODR's burst-level modem and vocoder interfaces don't map onto it directly, and features that need the AMBE bits
(privacy, the hotspot voice bridge) aren't possible unless a vocoder bypass is found.

## Confidence tags

| Tag | Meaning |
|---|---|
| **DS** | Sicomm CT3258 / SCT3258 datasheet and "Packet Interface of CT3258" v1.0 (2015). Copies mirrored on hackaday.io; their license is unclear, so cite them, don't copy them into this repository |
| **PKG** | The vendor's public update packages (D868UVE 1.40/2.40, D878UV and D878UVII PLUS 3.08N, D578UV PLUS 2.08, D578 PRO 1.21): their instruction PDFs, release notes and file listing. The firmware images themselves have not been examined |
| **COM** | Community teardown and mod pages, public protocol notes from flash tools (prose only), GitHub issues |
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
| GPS | none | ATGM336H or T3RV1 (GPS + BDS) | ATGM336H | COM |
| Bluetooth | none | PLUS: Beken BK3260 module. 878UVII: a daughter board with Bluetooth and an FSK receiver for APRS, with its own firmware ("BHT_BOARD") | PLUS: the same daughter board; "BT01" module | COM, PKG |
| Display | not found | 1.77" color TFT, 160×128 (INF from the size); controller not found | not found | COM, INF |
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
ship it, so like the AMBE codec on other radios, it would have to come from the owner's own radio. Where the stock
firmware keeps it (internal flash or NAND) isn't known yet.

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
| Encryption, signing | Not documented. Community band-limit edits of .CDD files suggest no signature. The tool's grayed "Encrypt" option shows the format can carry encryption. Not yet checked on the images | COM, PKG |
| Bootloader | Anytone's own, internal flash 0x08000000–0x08003FFF; application from 0x08004000 | COM |
| SWD | Pads marked "AF" beside the MCU. A documented 868 → 878 conversion read and wrote internal flash with an ST-LINK, so read-out protection was off on those radios. Recovery from a bad write: hold NRST (MCU pin 14) low | COM |
| SCT3258 update | Separate "Boot Mode for Sct": hold # + the top key at power-on. Sicomm's SCT_PORT tool talks to the chip through the radio's COM port (9600 shown) and writes an Intel-hex file (`SCT3258TD_UpdateFlash_V2_01_07BF.hex` in the 578 package). The MCU firmware therefore has a pass-through mode to the chip | PKG, INF |
| MCU ROM bootloader | Whether BOOT0 is reachable on the board is not known (GD32F303 and AT32F43x both have a ROM USB/UART loader) | — |

## 4. What a port needs

| Part | ODR has | New work |
|---|---|---|
| MCU | `platform/stm32f4` (a different family) | `platform/gd32f30x` (clock tree, GPIO, EXMC or GPIO bus for the HPI, SPI, I2C, I2S, USB FS device). The AT32F43x for late 578s is a second platform |
| Baseband | none | `drivers/modem/sct3258`: HPI bus, boot block load, packet framing, layer 2 mode, 60 ms pacing, calibration |
| Boot block | none | A tool that takes the 1024 words from the owner's stock firmware (like the codec tools) and puts them where our firmware reads them |
| RF | `drivers/rf/at1846s` | Per-radio tuning tables; QA4818N compatibility on the 578 |
| Audio codec | none | TLV320AIC3204 setup over I2C (gains, routing, mic bias) |
| Storage | SPI NOR drivers | SPI NAND driver with bad-block handling (also needed by the DB25-D and GD-88) |
| Display | `st7735`, `st7789` | Depends on the controller (unknown) |
| Flashing | odrflash (DFU, UART loaders) | The Anytone update protocol over USB CDC, or SWD for the first bring-up |
| Core | — | A capability for "voice frames stay in the modem" that hides privacy and the hotspot voice bridge |

## 5. Open questions

1. **The owner's 868UV**: board version and MCU (VET6 with 64 KB RAM, or VGT6). Read the MCU marking, or read the
   ID code over SWD.
2. **Display controller and resolution** on each model, and the **pin map** (MCU to HPI, SPI NAND, AT1846S, codec,
   keys, LCD, PTT, LEDs, power). Board photos or continuity checks; the FCC internal photos first.
3. **The .CDD format**: plain or encrypted, load address, header and checksum, and how the identity check works.
   Needs the images examined as data. Then: could the stock update mode load our image, so no SWD is needed?
4. **Where the SCT3258 boot block lives** in the stock firmware or NAND.
5. **AMBE pass-through in DMR mode.** The v1.0 manual says dPMR only. Check the SCT3258 v2.0 datasheet, later
   packet-interface revisions and the TYPE bypass bits. It decides whether privacy and the hotspot are possible.
6. **Data types** that `DIGC_DATA_FRAME` accepts in layer 2 mode (CSBK, rate 1/2, 3/4, 1, UDT) and whether
   the chip filters any of them.
7. **Read-out protection** on current production radios, and whether BOOT0 is reachable for recovery.
8. **878UVII MCU**, and **the AT32F43x pin map** on late 578s.

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
