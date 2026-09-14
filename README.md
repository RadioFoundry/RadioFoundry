# OpenDigitalRadio

OpenDigitalRadio (ODR) is free, MIT-licensed firmware for inexpensive DMR radios. One radio-independent software core handles
the DMR stack, the UI, the codeplug and the host tools, and each radio adds a thin board layer underneath. 

The goal is a nearly-universal software framework that can be used to build firmware for any available radio, 
with an intuitive user interface and powerful feature set, including the advanced Analog/DMR features low-cost 
radio stock firmwares usually omit. This project can be used by anyone for anything, commercial products included.

It's early. Read the status section before you flash anything.

<p>
<img src="docs/ux/screens/160x120i5/boot.png" width="180" alt="Boot screen">
<img src="docs/ux/screens/160x120i5/home_dmr.png" width="180" alt="Home screen on a DMR channel">
<img src="docs/ux/screens/160x120i5/hub.png" width="180" alt="The hub">
<img src="docs/ux/screens/160x120i5/rx_group.png" width="180" alt="Receiving a group call">
</p>

<sub>DM-1701 screens, rendered by the UI simulator (`make -C tests/host ui-gallery`).</sub>

## Radios and status

| Radio | Builds | Host tests | On the radio | Support |
|---|:---:|:---:|:---:|---|
| Baofeng DM-1701 | ● | ● | ◐ | First target. FM and DMR voice work on the radio; the other features wait on hardware tests (below) |
| Baofeng DM-32UV | ● | ● | ○ | Builds and passes its host tests; hasn't run on a radio yet ([bring-up checklist](docs/testing/dm32-bringup.md)). Air band AM and broadcast FM receive are in the core, parked until bring-up ([airband-and-broadcast](docs/spec/airband-and-broadcast.md)) |
| TYT MD-UV380 / MD-UV390, Retevis RT3S | ○ | ○ | ○ | Planned. Same board family as the DM-1701 (STM32F405, HR_C6000, AT1846S), so it should be the next port |
| TYT MD-9600, Retevis RT90 | ○ | ○ | ○ | Planned. Mobile radio on the same STM32F405 and HR_C6000 pair, with SKY72310 synthesizers (older boards) and a 128x64 mono LCD |
| TYT MD-380 / MD-390, Retevis RT3 (and the GPS models) | ● | ● | ○ | Builds (UHF and VHF images) and passes its host tests; hardware needed: it hasn't run on a radio yet ([bring-up checklist](docs/testing/md380-bringup.md)). FM only until the codec is located in the stock firmware ([hardware-md380](docs/spec/hardware-md380.md) 14.4) |
| Radioddity GD-77, TYT MD-760, Baofeng DM-1801 (v1), RD-5R | ○ | ○ | ○ | Planned. Different MCU (NXP MK22FN512), same HR_C6000 and AT1846S, 128x64 mono LCD |
| Radioddity GD-88 | ○ | ○ | ○ | Planned. Dual receiver on the HR_C7000 platform (chip count not confirmed yet); should reuse much of the DM-32UV port |

● done&nbsp;&nbsp; ◐ partly&nbsp;&nbsp; ○ not yet

The chips in each radio, their sources and the planned port order: [docs/spec/radios.md](docs/spec/radios.md).

On the DM-1701, these are verified on the radio: boot, display, keys, calibration, FM receive and transmit, DMR voice
receive (including late entry), DMR transmit (decoded by an MMDVM receiver on the SDR bench), receiving the DMR
calls and data the SDR bench can send (group and private calls, text messages, call alert, radio check), codeplug
storage, qdmr reads and zone scan (stopping for DMR calls). Transmitting data, emergency, privacy, hotspot, USB
network, SFR, roaming with beacon surveys, APRS, ETSI GPS Info in calls, the split duplex repeater and CPS writes are
built and pass the host tests, but are still waiting on hardware tests.
"Host-tested" means it works against simulated chips, not that anyone has heard it on air yet.

Releases carry the DM-1701 firmware (everything except the codec), `odrflash` to flash it on Windows, macOS and
Linux, and the user manual as a PDF. You don't build anything; see [Installing](docs/user/02-installing.md).

## What it does

- **DMR voice:** group and private calls, talker alias, basic privacy ([dmr-voice](docs/spec/dmr-voice.md))
- **DMR data:** text messages, ARS, LRRP, call alert, radio check ([dmr-data](docs/spec/dmr-data.md))
- **Emergency:** alarm, emergency call with hot mic, alerts ([dmr-emergency](docs/spec/dmr-emergency.md))
- **Hotspot:** the radio works as an MMDVM modem over USB ([mmdvm-protocol](docs/spec/mmdvm-protocol.md))
- **SFR:** single frequency repeater mode on the hotspot, off by default ([sfr](docs/spec/sfr.md))
- **USB network mode:** the radio becomes a USB network adapter and routes IP over DMR, off by default
  ([usb-network](docs/spec/usb-network.md))
- **Roaming:** moves to the strongest repeater of a zone when the one it's on gets weak, surveying the other sites
  during the repeaters' idle beacons; turned on per zone
  ([dmr-roaming](docs/spec/dmr-roaming.md))
- **APRS:** position, status and messages on FM (1200 baud AFSK), positions over DMR to an APRS gateway, KISS TNC;
  transmit only on the DM-1701 ([aprs](docs/spec/aprs.md))
- **Split duplex repeater (experimental):** two radios on one MMDVM host as a full duplex repeater, off by default
  ([split-duplex](docs/spec/split-duplex.md))
- **FM:** CTCSS/DCS, roger beep, 1750 Hz burst, dual watch ([radio-features](docs/spec/radio-features.md))
- **Scan:** zone, VFO and dual watch scan; on DMR it stops for calls in progress, using late entry
  ([radio-features](docs/spec/radio-features.md))
- **qdmr as the CPS:** program the radio with qdmr ([cps-protocol](docs/spec/cps-protocol.md))
- **A UI designed from first principles**, not copied from anyone's menus, with press budgets checked by the host
  tests ([principles](docs/ux/principles.md), [screens](docs/ux/screens/))

Each feature that costs flash has a build switch. See [docs/build-config.md](docs/build-config.md).

## Interoperability

Interoperability is a priority, in two directions.

**Across hardware.** One core runs on every radio. A radio is a board file that picks its chips (DMR modem, RF
chip, display, keypad or encoder, flash) and describes its bands, power levels and screen. The core never checks
which radio it's on. The same UI lays itself out on a 128x64 monochrome LCD or a 240x320 color panel, and works
with a full keypad or just a knob and a couple of buttons. Today that's two different MCUs (STM32F405 and C-SKY
CK803S), two DMR modems (HR_C6000 and HR_C7000) and two RF chips (AT1846S and FD6818) sharing all the code above
them.

**With everything else on the air and on your desk.** A radio is only useful if it talks to the radios and software
people already have. We support the formats other radios use, not just our favorite:

| Area | Supported now | Planned |
|---|---|---|
| Air interface | ETSI DMR Tier II: simplex, repeater, SFR (single frequency repeater), repeater roaming | ETSI Tier III trunking ([plan](docs/spec/dmr-tier3-plan.md)) |
| Text messages | Motorola TMS; ETSI short data in 7-bit, ISO 8859, UTF-8 and UTF-16; ETSI UDT text | Hytera text format |
| Location | Motorola LRRP location answers and ARS registration; ETSI GPS Info in voice calls (the caller's position shown with distance and bearing, ours sent from the APRS position); APRS position, status and messages on FM (Bell 202 AFSK) and positions over DMR to an APRS gateway ID; KISS TNC on USB | NMEA from the radio's GPS receiver, ETSI UDT location reports, APRS receive and iGate on radios that can sample received audio |
| Signaling | call alert, radio check and emergency, each in the Motorola and ETSI forms; talker alias in all four ETSI formats | DTMF and 5-tone on FM |
| Privacy and encryption | Motorola Basic Privacy, Anytone 16-bit Basic Privacy | ARC4 and AES-256 per the DMR Association interoperability profiles |
| Programming | qdmr, over the GD-77-style CPS protocol | an open, published native codeplug format with its own qdmr driver; qdmr YAML and CSV import/export; RadioID user database |
| Hotspot and network | MMDVM modem protocol (MMDVMHost, Pi-Star, WPSD and the networks behind them); IP over DMR with the radio as a USB network adapter; two radios as one full duplex repeater (experimental, build switch; the HR_C6000's CACH still needs a bench test) | the split duplex host side in RadioDesk |
| Audio to a computer | | USRP over UDP (AllStarLink, DVSwitch), USB sound card with serial-line PTT ([usb-audio](docs/spec/usb-audio.md)) |

"Supported now" means built and passing its tests; see [Radios and status](#radios-and-status) for what has been
checked on the air. Specs for every format are in [docs/spec](docs/spec), written so other projects can implement
the same thing.

## The AMBE codec

DMR voice needs DVSI's proprietary AMBE+2 vocoder. ODR never ships it, not in this repo and not in any binary.

Each radio runs exactly the codec its own stock firmware came with. Release images leave it out, and
[odrflash](tools/odrflash/README.md) handles it when you flash: if the radio already holds the right codec (it runs
ODR with DMR voice working, or the matching stock firmware), odrflash writes only ODR's own flash sectors and the
codec stays where it is. Otherwise it pulls the codec out of the stock firmware file you downloaded, on your own
machine, and writes it to the radio. It checks the stock file by SHA-256 and refuses anything it doesn't recognize.

```
odrflash flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
```

The DM-32UV uses `tools/dm32_codec.py`. Details, including which stock files work:
[codec-dm1701](docs/spec/codec-dm1701.md), [codec-dm32](docs/spec/codec-dm32.md). Without a codec the firmware runs FM
only.

## Building and flashing

You need make, Python 3, and `arm-none-eabi-gcc` (DM-1701) or a `csky-elf` GCC (DM-32UV, built by
`tools/build-csky-toolchain.sh`).

```
make BOARD=dm1701           # or BOARD=dm32
make -C tests/host          # host tests
tools/ci.sh                 # full release gate: tests, build matrix, license checks
```

To flash your own DM-1701 build, run `odrflash flash build/dm1701/odr_dm1701.bin --board dm1701` (add `--stock FILE`
to write the codec too) and hold PTT and the side key above it while powering the radio on when it asks. The
bootloader key isn't in this repo; odrflash derives it from the stock firmware file. If you put the stock firmware
file in [codecs/](codecs/README.md), every `make` also writes an image with the codec merged in. Releases are built by
`.github/workflows/release.yml` ([releases](docs/dev/release.md)). The DM-32UV image gets wrapped with
`tools/dm32_pack.py` for Baofeng's updater.

More: [build-config](docs/build-config.md), [testing](docs/testing/README.md),
[DM-1701 hardware and bootloader](docs/spec/hardware-dm1701.md).

## Documentation

- [User manual](docs/user/README.md): installing, the keys and screens, FM, DMR, messages, hotspot, settings,
  troubleshooting. Each release also carries it as a PDF.
- [Developer documentation](docs/dev/README.md): architecture, building, adding a radio, testing, the debug link,
  CPS, the codec, contributing, releases.
- Specs and design: [docs/spec](docs/spec/), [docs/ux](docs/ux/), [docs/testing](docs/testing/README.md).

## How it's tested

Radio firmware is usually tested by hand: flash it, push buttons, key up, listen. That's slow, and it means nobody
wants to touch old code. We test at four levels instead, and most of it runs on any laptop with no radio at all.

- **Unit tests.** Small pieces checked on their own: the error-correction math, a message encoder, the settings
  format. Feed in known inputs, compare against known answers (worked out from the standards, or recorded from
  real radios).
- **Software-in-the-loop (SIL).** The real firmware code for whole features, compiled for your computer and run
  against simulated chips. A fake DMR modem plays recorded or generated bursts into the real call handling, a fake
  flash chip takes the codeplug writes, a fake CPS talks to the real CPS code. If a call, a text message or a
  codeplug write breaks, this catches it without a radio.
- **UI simulator.** Part of SIL: the real screen and menu code drawn into an image instead of an LCD, at every
  display size we support. Scripted key presses walk through the menus and check where they end up, and the same
  tool renders every screenshot in these docs.
- **Hardware-in-the-loop (HIL).** The same kind of scripted tests, run against a real radio over USB. A debug
  protocol lets the tests press keys, read what the radio thinks is going on and take screenshots
  ([debuglink](docs/spec/debuglink.md), `tools/odr_debug.py`), and an SDR bench plays the other radio: it sends
  calls and texts to the radio and checks what the radio transmits. See [tests/hil/README.md](tests/hil/README.md).

On top of that, every change builds every radio with each feature switched on and off, checks the image still fits,
and runs the license and clean-room checks (`tools/ci.sh`). How to run each level and where a new test goes:
[developer docs, testing](docs/dev/testing.md) and [docs/testing](docs/testing/README.md).

What that buys: you can change the DMR stack, the UI or the storage code and know within a couple of minutes, on
your own machine, whether you broke something. You don't need our exact radio or our bench to contribute, and a bug
found on the bench usually turns into a SIL test so it stays fixed.

## FAQ

**Why allow commercial use?**

Why not? Commercial use is the reason we can buy $40 DMR radios. If everything DMR were ham-only, we would still be
paying $500 for a decent radio. Nobody "loses" here, because the project authors aren't trying to make any money off
of this. If someone can figure out a way to use it in their product and make money, great! We're not in the business
of selling DMR firmware, we just want everyone to have access to a decent one.

**Why reinvent the wheel?**

There is no current truly open source effort to make a firmware for most of these radios, and definitely not one
that tries to share this much code across all makes and models. Any prior major "open" firmware efforts have ended up
with source code being taken down by authors, licensing issues, arguments and fights, or key authors closed-sourcing
their additions to the project.

**Why another radio firmware?**

DMR is filled with cool features that cheaper radios almost never implement. These are incredibly useful for
commercial applications, and offer very powerful, useful capabilities for amateur use as well.

**How can I help?**

Open an issue or pull request, or donate a new radio model for testing.

## Clean room and licensing

ODR is MIT licensed ([LICENSE](LICENSE)). It's a clean-room project, independent of earlier radio firmwares: code is
written from prose specs, datasheets and public standards. No GPL code, no non-commercial code, no vendor SDKs. The
rules and the checks behind them are in [docs/CLEANROOM.md](docs/CLEANROOM.md).

Third-party code in `third_party/` (FreeRTOS, TinyUSB, ST's HAL and CMSIS) keeps its own permissive license; see
[LICENSES/](LICENSES/) and each submodule.

## Contributing

Issues and pull requests are welcome. Read [docs/CLEANROOM.md](docs/CLEANROOM.md) first, keep radio-specific code out
of `core/`, put anything that costs flash behind a `CONFIG_` switch, and make sure `tools/ci.sh` passes. Details: [contributing](docs/dev/contributing.md).

## Layout

| Path | What |
|---|---|
| `core/` | Radio-independent code: DMR stack, hotspot, host link, codeplug, settings, UI, CPS, debug link |
| `hal/` | Interfaces the core uses (modem, RF, audio, codec, flash, display, input), and the `board_t` descriptor |
| `drivers/` | Chip drivers shared across radios |
| `platform/` | Per-SoC startup, clocks, interrupts and RTOS glue (`stm32f4`, `hrc7000`) |
| `boards/` | One directory per radio: board descriptor, pins, feature defaults, linker script, codec hosting |
| `config/` | Build-time feature switches |
| `third_party/` | Permissively licensed upstream code, as submodules |
| `tools/` | Flashing (`odrflash`), release packaging, codec merge, CPS client, debug link, CI, SDR bench tools |
| `tests/host/`, `tests/hil/` | Host tests and hardware-in-the-loop tests |
| `docs/user/`, `docs/dev/` | User manual, developer documentation |
| `docs/spec/`, `docs/ux/`, `docs/testing/` | Specs, UI design, test procedures |
