<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Architecture

One radio-independent core runs on every radio. Each radio adds a board directory that describes the radio and drives
its chips. The core never includes a board or platform header and never tests a radio define. What differs by radio
reaches the core at run time through the board descriptor, `board_t` in [hal/board.h](../../hal/board.h).

## Layers

The following diagram shows the layers, from the core down to the third-party code.

```
 core/        DMR stack, radio core, UI, codeplug, settings, storage, hotspot, host link, CPS, debug link,
              APRS, IP services (TCP, HTTP, mDNS, TFTP, SNMP)
   |  calls only hal/*.h and board.<field>
 hal/         interfaces: board_t, radio, DMR modem, audio, vocoder, display, input, flash, power, stream, netif,
              system, AFSK, broadcast receiver
   |  implemented by
 boards/<b>/  the radio: board_t, pins, key map, how its build differs from the platform's, codec hosting, chip glue;
              radios of one board design share it in a family directory (adding-a-radio.md)
   |  uses
 drivers/     chips shared by several radios: AT1846S, BK4819, FD6818, SKY72310, HR_C6000, HR_C7000, ST7735, ST7789,
              RDA5807M, SPI NOR, key matrix, encoder, NMEA GPS, USB; and the math they share (mic level, battery)
 platform/<p>/ the SoC: start-up, clocks, interrupts, time base, FreeRTOS port glue, and the build defaults its
              boards share (stm32f4, hrc7000)
 third_party/ FreeRTOS, TinyUSB, ST HAL and CMSIS (submodules, permissive licenses)
```

The following table gives the rule for each layer.

| Layer | Rule |
|---|---|
| `core/` | Portable C. Tests every radio difference through `board` (bands, power levels, display geometry and insets, key map, optional hooks that can be NULL). Host tests compile it unchanged |
| `hal/` | Headers only: structs of function pointers and the data types between core and board |
| `drivers/` | One chip each, no knowledge of the radio it sits in beyond what its init call is given. Also header-only helpers the boards share, such as `audio/miclevel` (the VOX level) and `power/battery` (the battery average and charge estimate) |
| `boards/` | The only place that knows the radio. Fills in `board_t`; maps physical keys to logical actions |
| `platform/` | The SoC, shared by every radio on it (the STM32F405 radios share `platform/stm32f4`), and in subdirectories the glue for a chip on that SoC that several radios share (`platform/stm32f4/hrc`, `gps_uart`, `spi_flash`, `fsmc_keypad`), built for the boards that list it in `BOARD_DIRS`. Also the build defaults: `platform.mk` (compiler, part defines, HAL modules), `config.mk` (feature defaults), `common.ld` (the memory layout), `FreeRTOSConfig.h`. A board's files list only how it differs |

Besides `board_t`, every board provides three functions (`hal/board.h`):

- `board_early_init`: pins and clocks, before the RTOS starts.
- `board_init`: called from the first task.
- `board_rf_safe`: turns the transmitter off with register writes only.

The platform calls `board_rf_safe` first in `main()`, in its fault handlers, and before every software reset or jump
out of the application. The crash capture in `core/crash` calls it too. It must work with interrupts off, without the
RTOS, and before the clocks are set up ([crash-log.md](../spec/crash-log.md) 8).

RF chip drivers share three interfaces in `drivers/rf`, as the following table shows.

| Interface | What it is |
|---|---|
| `rfbus` | A chip's register bus: write, read, a millisecond wait, and a write count. `rfbus_3w_t` is the bit-banged 3-wire bus; a board fills in its pin functions. A driver takes an `rfbus_t *` and never touches pins |
| `subaudio` | The CTCSS, tone and DCS words every chip family uses, rounded or truncated as the caller needs |
| `superhet` | The two roles of a superheterodyne: main transceiver and LO, as tables of ops. A driver that can fill a role exports its ops; the board detects its chips, binds each to its ops and calls only those. Steps that depend on the board (band pins, calibration lookups) stay in the board |

## Tasks

[core/app/app.c](../../core/app/app.c) creates the FreeRTOS tasks in the following table. Its header comment has the
priorities and stack sizes.

| Task | Owner | Does |
|---|---|---|
| start / housekeeping | `core/app` | Start-up, then the power switch poll, settings saves and codeplug writes (storage jobs) |
| radio | `core/radio` | The radio state machine: requests, status poll, squelch, PTT, time-out, the DMR call stack tick, the data service |
| dmr | board | Modem events to the DMR call stack or the hotspot |
| audio | board | PCM blocks; the vocoder runs here |
| input | `core/input` | Key scan, gestures; PTT and monitor go straight to the radio core |
| host | `core/hostlink` | The host port: MMDVM hotspot, CPS shim, debug link; in network mode the IP gateway and the radio's own services (`core/net`, `core/snmp`) |
| usb | `drivers/usb` | TinyUSB device stack |
| ui | `core/ui` | Frame pacing, drawing, the UI's behavior |
| gps | board | NMEA from the GPS module, on radios that have one |

Other tasks talk to the radio core through requests (`radio_req_*`, queued, never waiting) and read a copy of its
state (`radio_view_t`). The UI never calls the RTOS from its behavior code, so the host simulator runs that code
unchanged.

## The core, by directory

The following table lists each core directory, the spec that describes it, and what it holds.

| Directory | Spec | Contents |
|---|---|---|
| `core/radio` | [radio-features](../spec/radio-features.md), [dmr-roaming](../spec/dmr-roaming.md), [airband-and-broadcast](../spec/airband-and-broadcast.md) | The radio state machine (`radio_sm.c`), its task, scan, beeps, battery, Morse, roaming, air band and broadcast receive |
| `core/dmr/call` | [dmr-voice](../spec/dmr-voice.md), [dmr-modem](../spec/dmr-modem.md) | The DMR call stack: talk targets, filters, late entry, talker alias |
| `core/dmr/fec`, `core/dmr/ambe` | [dmr-modem](../spec/dmr-modem.md) | ETSI FEC, AMBE frame FEC and the voice path around the codec |
| `core/dmr/*.c` | [dmr-data](../spec/dmr-data.md), [dmr-emergency](../spec/dmr-emergency.md), [sfr](../spec/sfr.md), [usb-network](../spec/usb-network.md) | Data service (SMS, CSBK signaling, ARS, LRRP, confirmed data, SAP 3 compression), emergency, privacy, SFR, IP gateway |
| `core/hotspot` | [mmdvm-protocol](../spec/mmdvm-protocol.md), [split-duplex](../spec/split-duplex.md) | MMDVM modem protocol, the hotspot, MMDVM SFR and the split duplex repeater |
| `core/net` | [tcp-services](../spec/tcp-services.md), [mdns](../spec/mdns.md), [tftp](../spec/tftp.md) | The minimal TCP and the radio's services on the USB network link: KISS, HTTP, rigctld, APRS-IS, mDNS, TFTP |
| `core/snmp` | [snmp](../spec/snmp.md) | SNMPv2c agent, the RadioFoundry MIB, notifications |
| `core/aprs` | [aprs](../spec/aprs.md) | The APRS engine and its two backends: AX.25 / AFSK (beacons, messages, KISS TNC, iGate rules) and positions over DMR |
| `core/position` | [position](../spec/position.md) | Where the radio is (board GPS, host NMEA, fixed position) for DMR GPS Info, LRRP and APRS; NMEA parsing |
| `core/usbaudio` | [usb-audio](../spec/usb-audio.md) | USRP and the jitter buffer |
| `core/hostlink` | [cps-protocol](../spec/cps-protocol.md) 1 | Dispatch of the host port by first byte; the USB network side |
| `core/cps` | [cps-protocol](../spec/cps-protocol.md) | The open-firmware CPS shim |
| `core/codeplug` | [open-firmware-cps](../spec/open-firmware-cps.md), [codeplug-native](../spec/codeplug-native.md) | Codeplug reading and editing, the DMR ID database |
| `core/settings`, `core/storage` | [settings-store](../spec/settings-store.md) | The settings ring, the sector journal, the foreign region guard |
| `core/ui` | [ui](../spec/ui.md), [docs/ux](../ux/) | Drawing, screens, navigation, Find, the services the UI calls |
| `core/input` | [ui](../spec/ui.md) 3 | Gesture engine (press, long, repeat), injected keys |
| `core/debuglink` | [debuglink](../spec/debuglink.md) | The debug protocol and its view sections |
| `core/app` | | Start-up and housekeeping |

## The UI's seams

The UI asks for data that other packages own through `core/ui/ui_svc.h`. Each call has a weak "not in this build"
stub in `ui_svc_stub.c`, and the owning package's strong definition replaces it at link time. A screen whose feature
is missing is dimmed and says why. For details, see [ui.md](../spec/ui.md) 4.

## Build switches

Features that cost flash sit behind `CONFIG_` switches (`config/features.mk`, `core/config.h`). Code tests them with
`#if CONFIG_X`, and the Makefile leaves out whole directories. For every switch, see
[Build configuration](../build-config.md).

## Gotchas

[Gotchas](../spec/gotchas.md) collects the lessons from earlier firmware work, by subsystem: modem timing, hotspot
buffering, data retries, and storage. Read the section for the subsystem you touch.

## Source tree

The following table lists the top-level directories.

| Path | What |
|---|---|
| `core/` | Radio-independent code: the DMR stack (`core/dmr`), the radio state machine (`core/radio`), the UI (`core/ui`), the hotspot, the host link, the codeplug, settings, CPS, and the debug link |
| `hal/` | The interfaces the core uses (modem, RF, audio, codec, flash, display, input), and the `board_t` descriptor |
| `drivers/` | Chip drivers shared across radios |
| `platform/` | Per-SoC start-up, clocks, interrupts, and RTOS glue (`stm32f4`, `hrc7000`) |
| `boards/` | One directory per radio: the board descriptor, pins, feature defaults, linker script, and codec hosting |
| `boot/` | First stages in front of the application, for radios whose loader starts it directly |
| `config/` | Build-time feature switches |
| `third_party/` | Permissively licensed upstream code, as submodules (FreeRTOS, ST HAL and CMSIS, TinyUSB) |
| `tools/` | Flashing (`rfoundry`), release packaging, codec merge, the CPS client, the debug link client (`rfoundry_debug.py`), CI, and SDR bench tools |
| `stub/` | The flash stubs and `stubtool.py` |
| `tests/host/` | Unit and SIL tests, and the UI simulator (`tests/host/sim`) |
| `tests/hil/` | HIL tests: pytest against a radio and the SDR bench |
| `site/` | The web site, the web flasher, and the Codeplug Editor |
| `docs/` | The documentation; [the documentation map](../README.md) lists every page |
