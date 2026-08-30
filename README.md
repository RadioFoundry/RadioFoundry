# OpenDigitalRadio

OpenDigitalRadio (ODR) is free, MIT-licensed firmware for inexpensive DMR radios. One radio-independent core handles
the DMR stack, the UI, the codeplug and the host tools, and each radio adds a thin board layer underneath. The goal
is firmware that's good to use, does the DMR features stock firmware usually skips, and can be used by anyone for
anything, commercial products included.

It's early. Read the status section before you flash anything.

<p>
<img src="docs/ux/screens/160x120i5/boot.png" width="180" alt="Boot screen">
<img src="docs/ux/screens/160x120i5/home_dmr.png" width="180" alt="Home screen on a DMR channel">
<img src="docs/ux/screens/160x120i5/hub.png" width="180" alt="The hub">
<img src="docs/ux/screens/160x120i5/rx_group.png" width="180" alt="Receiving a group call">
</p>

<sub>DM-1701 screens, rendered by the UI simulator (`make -C tests/host ui-gallery`).</sub>

## Radios and status

| Radio | Status |
|---|---|
| Baofeng DM-1701 | First target, on the bench now |
| Baofeng DM-32UV | Builds and passes its host tests; hasn't run on a radio yet |
| TYT MD-UV380, MD-9600, MD-380, Radioddity GD-77, GD-88 | Planned |

On the DM-1701, these are verified on the radio: boot, display, keys, calibration, FM receive and transmit, DMR voice
receive, and DMR transmit (decoded by an MMDVM receiver on the SDR bench). Most everything else (data, emergency,
privacy, hotspot, USB network, SFR, CPS) is built and passes the host tests, but is still waiting on hardware tests.
"Host-tested" means it works against simulated chips, not that anyone has heard it on air yet.

The DM-32UV port has a [bring-up checklist](docs/testing/dm32-bringup.md) for the first time it meets a radio.

There are no release binaries. You build it yourself and supply the codec (see below).

## What it does

- **DMR voice:** group and private calls, talker alias, basic privacy ([dmr-voice](docs/spec/dmr-voice.md))
- **DMR data:** text messages, ARS, LRRP, call alert, radio check ([dmr-data](docs/spec/dmr-data.md))
- **Emergency:** alarm, emergency call with hot mic, alerts ([dmr-emergency](docs/spec/dmr-emergency.md))
- **Hotspot:** the radio works as an MMDVM modem over USB ([mmdvm-protocol](docs/spec/mmdvm-protocol.md))
- **SFR:** single frequency repeater mode on the hotspot, off by default ([sfr](docs/spec/sfr.md))
- **USB network mode:** the radio becomes a USB network adapter and routes IP over DMR, off by default
  ([usb-network](docs/spec/usb-network.md))
- **FM:** CTCSS/DCS, roger beep, 1750 Hz burst, scan, dual watch ([radio-features](docs/spec/radio-features.md))
- **qdmr as the CPS:** program the radio with qdmr ([cps-protocol](docs/spec/cps-protocol.md))
- **A UI designed from first principles**, not copied from anyone's menus, with press budgets checked by the host
  tests ([principles](docs/ux/principles.md), [screens](docs/ux/screens/))

Each feature that costs flash has a build switch. See [docs/build-config.md](docs/build-config.md).

## The AMBE codec

DMR voice needs DVSI's proprietary AMBE+2 vocoder. ODR never ships it, not in this repo and not in any binary.

Each radio runs exactly the codec its own stock firmware came with. You download the stock firmware for your radio,
and a tool here pulls the codec out of it and merges it into your ODR image on your own machine. The tool checks the
stock file by SHA-256 and refuses anything it doesn't recognize.

```
make BOARD=dm1701
tools/codec_extract.py --stock "DM-1701(CSV)-V02.06.bin" --key dm1701.key
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

To flash a DM-1701, hold PTT and the side key above it while powering on, then run `tools/stm32_dfu.py image.bin
--model dm1701 --key dm1701.key`. The bootloader key isn't in this repo either; you supply it. The DM-32UV image gets
wrapped with `tools/dm32_pack.py` for Baofeng's updater.

More: [build-config](docs/build-config.md), [testing](docs/testing/README.md),
[DM-1701 hardware and bootloader](docs/spec/hardware-dm1701.md).

## Debug link and hardware tests

Default builds answer a debug protocol on USB for diagnostics, screenshots and key/PTT injection
([debuglink](docs/spec/debuglink.md), `tools/odr_debug.py`). The pytest suite in `tests/hil` drives a real radio
with it, and with opt-in flags uses the SDR bench as the other station. Without a radio, every test just skips. See
[tests/hil/README.md](tests/hil/README.md).

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
of `core/`, put anything that costs flash behind a `CONFIG_` switch, and make sure `tools/ci.sh` passes.

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
| `tools/` | Flashing, codec merge, CPS client, debug link, CI, SDR bench tools |
| `tests/host/`, `tests/hil/` | Host tests and hardware-in-the-loop tests |
| `docs/spec/`, `docs/ux/`, `docs/testing/` | Specs, UI design, test procedures |
