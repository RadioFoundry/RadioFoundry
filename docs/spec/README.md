<!-- SPDX-License-Identifier: MIT -->

# Specifications

What the firmware does and what the hardware needs: one page per subsystem, written before the code and kept true to
it. Behavior specs are radio-independent. A radio's own facts (pins, chips, quirks, measurements) are on its pages,
named by the board id: `hardware-<id>.md`, `codec-<id>.md` and, for some, `<id>-migration.md`. The radios and their
chips are in [radios.md](radios.md); which radio has which feature is in [feature-matrix.md](../feature-matrix.md).
Past bugs, by subsystem: [gotchas.md](gotchas.md). Contributors read [docs/dev](../dev/README.md) for process.

## Radio features and the user interface

| Spec | Covers |
|---|---|
| [capabilities.md](capabilities.md) | What a radio can do, by area, with the switch and spec behind each |
| [radio-features.md](radio-features.md) | Scan, the FM transmit extras (tones, burst, roger beep), beeps, battery and power management |
| [airband-and-broadcast.md](airband-and-broadcast.md) | Air band AM receive and broadcast FM receive, chosen by the frequency |
| [aprs.md](aprs.md) | APRS on FM and over DMR |
| [ui.md](ui.md) | The user interface as built: screens, keys, the drawing layer ([docs/ux](../ux/) has the design) |

## DMR

| Spec | Covers |
|---|---|
| [dmr-voice.md](dmr-voice.md) | DMR voice calls |
| [dmr-data.md](dmr-data.md) | DMR data services: text messages, ARS, LRRP, call alert, radio check |
| [zone-share.md](zone-share.md) | Zone and channel sharing over DMR data: the record, fragments, offers, the import |
| [dmr-emergency.md](dmr-emergency.md) | DMR emergency |
| [dmr-roaming.md](dmr-roaming.md) | DMR repeater roaming |
| [sfr.md](sfr.md) | Single frequency repeater |
| [split-duplex.md](split-duplex.md) | Two radios as one duplex MMDVM modem |

## DMR modem chips

| Spec | Covers |
|---|---|
| [dmr-modem.md](dmr-modem.md) | What any modem driver must do; the `hal_dmr_modem.h` contract; the chips compared |
| [modem-hrc6000.md](modem-hrc6000.md) | HR_C6000 as a DMR modem (also the HR_C5000's base) |
| [hrc6000-registers.md](hrc6000-registers.md) | HR_C6000 register reference |
| [modem-hrc7000.md](modem-hrc7000.md) | HR_C7000 as a DMR modem |
| [rx2-link.md](rx2-link.md) | The link to a second receiver on another SoC: frames, messages, timing |

## Host link and network

| Spec | Covers |
|---|---|
| [debuglink.md](debuglink.md) | The debug link: commands, the view, key injection |
| [cps-protocol.md](cps-protocol.md) | The host port's protocols and how they share it; the CPS protocol |
| [cps-fast.md](cps-fast.md) | The fast codeplug protocol: block sums, LZ4, staged commit |
| [iddb-compact.md](iddb-compact.md) | The compact DMR ID database (format C): dictionary, pages, compatibility |
| [mmdvm-protocol.md](mmdvm-protocol.md) | The MMDVM modem protocol (hotspot mode) |
| [usb-network.md](usb-network.md) | USB network personality, the IP gateway, control station mode |
| [ppp.md](ppp.md) | PPP on the serial host link |
| [tcp-services.md](tcp-services.md) | TCP, HTTP, rigctld, KISS over TCP, APRS-IS |
| [tftp.md](tftp.md) | TFTP codeplug access |
| [mdns.md](mdns.md) | mDNS and DNS-SD |
| [snmp.md](snmp.md) | SNMP agent ([RFOUNDRY-MIB.txt](RFOUNDRY-MIB.txt)) |
| [usb-audio.md](usb-audio.md) | Analog and DMR voice over USB: design |

## Codeplug and storage

| Spec | Covers |
|---|---|
| [open-firmware-cps.md](open-firmware-cps.md) | The open-firmware codeplug format qdmr writes |
| [qdmr-yaml.md](qdmr-yaml.md) | qdmr's YAML codeplug file and how the Codeplug Editor maps it |
| [codeplug-native.md](codeplug-native.md) | The editing model and the native codeplug (draft) |
| [settings-store.md](settings-store.md) | Settings store and safe flash writes |
| [nand-sectors.md](nand-sectors.md) | NOR-like 4 KB sectors over NAND, for the settings store on NAND radios |
| [spi-nand-lut.md](spi-nand-lut.md) | The stock firmware's block map over an SPI NAND, for reading its partitions |

## Codecs

DMR voice needs each radio's own codec; the process is in [docs/dev](../dev/codec.md).

| Spec | Covers |
|---|---|
| [codec-placement.md](codec-placement.md) | Where RadioFoundry places the codec on each SoC family, descriptors, slots, codec maps |
| `codec-<id>.md` | One radio's codec: the stock files accepted, their format, the code and table windows, entry points |

## Migrating from stock

Reading a vendor codeplug and moving a radio from the vendor firmware. User steps are in
[docs/user](../user/README.md).

| Spec | Covers |
|---|---|
| [codeplug-stock-tyt.md](codeplug-stock-tyt.md) | The vendor codeplug layout of the TYT-family radios in SPI flash: detection and conversion |
| [codeplug-port.md](codeplug-port.md) | Stock codeplug to RadioFoundry: the clean-up, contacts, CPS write shared by the migration tools |
| `<id>-migration.md` | One radio's flashing and migration procedure, where a migration tool exists |
| `<id>-stock-cps.md` | One radio's vendor programming protocol, for interoperability |

## Flashing and updates

| Spec | Covers |
|---|---|
| [stub-entry.md](stub-entry.md) | The stub entry: loading the flashing tools' RAM monitor at run time, with or without the debug link |
| [stub-stm32f4.md](stub-stm32f4.md) | The STM32F4 flash stub: entry from RadioFoundry, USB, protocol, safety rules, update order |

## Reliability

| Spec | Covers |
|---|---|
| [crash-log.md](crash-log.md) | The crash log: fault capture on each platform, the record kept across the restart, the fault trace, the packed report |
| [watchdog.md](watchdog.md) | The hardware watchdog: the idle task's feed, timeouts, long operations, leaving the application, bench tests |
| [gotchas.md](gotchas.md) | Past bugs and traps, by subsystem |

## Radios

| Spec | Covers |
|---|---|
| [radios.md](radios.md) | The hardware survey: every radio's chips and the port order |
| `hardware-<id>.md` | One radio: MCU and memory map, pins, chips, keys, display, power, bootloader and image format, calibration |

The bring-up records for each radio are in [docs/testing](../testing/README.md).
