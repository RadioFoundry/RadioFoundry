<!-- SPDX-License-Identifier: MIT -->

# Specifications

What the firmware does and what the hardware needs. Behavior specs are radio-independent; per-radio facts are on
the radio's hardware page. Past bugs, by subsystem: [gotchas.md](gotchas.md).

## Behavior

| Spec | Covers |
|---|---|
| [radio-features.md](radio-features.md) | Scan, the FM transmit extras (tones, burst, roger beep), beeps, battery and power management |
| [ui.md](ui.md) | The user interface as built: screens, keys, the drawing layer ([docs/ux](../ux/) has the design) |
| [dmr-voice.md](dmr-voice.md) | DMR voice calls |
| [dmr-data.md](dmr-data.md) | DMR data services: text messages, ARS, LRRP, call alert, radio check |
| [dmr-emergency.md](dmr-emergency.md) | DMR emergency |
| [dmr-roaming.md](dmr-roaming.md) | DMR repeater roaming |
| [sfr.md](sfr.md) | Single frequency repeater |
| [split-duplex.md](split-duplex.md) | Two radios as one duplex MMDVM modem |
| [aprs.md](aprs.md) | APRS on FM and over DMR |
| [airband-and-broadcast.md](airband-and-broadcast.md) | Air band AM receive and broadcast FM receive (parked) |
| [settings-store.md](settings-store.md) | Settings store and safe flash writes |
| [crash-log.md](crash-log.md) | The crash log: fault capture on each platform, the record kept across the restart, the packed report |
| [nand-sectors.md](nand-sectors.md) | NOR-like 4 KB sectors over NAND, for the settings store on NAND radios |
| [codeplug-native.md](codeplug-native.md) | Native codeplug (draft) |

## Host link and network

| Spec | Covers |
|---|---|
| [cps-protocol.md](cps-protocol.md) | The host port's protocols and how they share it; the CPS protocol |
| [cps-fast.md](cps-fast.md) | The fast codeplug protocol: block sums, LZ4, staged commit |
| [open-firmware-cps.md](open-firmware-cps.md) | The open-firmware codeplug format qdmr writes |
| [qdmr-yaml.md](qdmr-yaml.md) | qdmr's YAML codeplug file and how the Codeplug Editor maps it |
| [codeplug-stock-tyt.md](codeplug-stock-tyt.md) | Stock TYT-family codeplugs (DM-1701, MD-UV380, MD-380) in SPI flash: detection and conversion |
| [codeplug-port.md](codeplug-port.md) | Stock codeplug to ODR: the clean-up, contacts, CPS write shared by the migration tools |
| [debuglink.md](debuglink.md) | The debug link: commands, the view, key injection |
| [stub-stm32f4.md](stub-stm32f4.md) | The STM32F4 flash stub: entry from ODR, USB, protocol, safety rules, update order |
| [mmdvm-protocol.md](mmdvm-protocol.md) | The MMDVM modem protocol (hotspot mode) |
| [usb-network.md](usb-network.md) | USB network personality, the IP gateway, control station mode |
| [ppp.md](ppp.md) | PPP on the serial host link |
| [tcp-services.md](tcp-services.md) | TCP, HTTP, rigctld, KISS over TCP, APRS-IS |
| [tftp.md](tftp.md) | TFTP codeplug access |
| [mdns.md](mdns.md) | mDNS and DNS-SD |
| [snmp.md](snmp.md) | SNMP agent ([ODR-MIB.txt](ODR-MIB.txt)) |
| [usb-audio.md](usb-audio.md) | Analog and DMR voice over USB: design |

## DMR modems

| Spec | Covers |
|---|---|
| [dmr-modem.md](dmr-modem.md) | What any modem driver must do; the `hal_dmr_modem.h` contract; the chips compared |
| [modem-hrc6000.md](modem-hrc6000.md) | HR_C6000 as a DMR modem (also the HR_C5000's base) |
| [hrc6000-registers.md](hrc6000-registers.md) | HR_C6000 register reference |
| [modem-hrc7000.md](modem-hrc7000.md) | HR_C7000 as a DMR modem |

## Radios

[radios.md](radios.md) surveys the radios, their chips and the port order; [capabilities.md](capabilities.md) shows
what the aftermarket firmware does on each (ODR's own state: [feature matrix](../feature-matrix.md)). Where ODR places each radio's
stock codec: [codec-placement.md](codec-placement.md). Per radio:

| Radio | Hardware | Codec | Moving from stock |
|---|---|---|---|
| Baofeng DM-1701 | [hardware-dm1701.md](hardware-dm1701.md) | [codec-dm1701.md](codec-dm1701.md) | |
| TYT MD-UV380 | [hardware-mduv380.md](hardware-mduv380.md) | [codec-mduv380.md](codec-mduv380.md) | |
| TYT MD-380 | [hardware-md380.md](hardware-md380.md) | [codec-md380.md](codec-md380.md) | [md380-migration.md](md380-migration.md) |
| TYT MD-9600 | [hardware-md9600.md](hardware-md9600.md) | [codec-md9600.md](codec-md9600.md) | |
| Baofeng DM-32UV | [hardware-dm32.md](hardware-dm32.md) | [codec-dm32.md](codec-dm32.md) | [dm32-migration.md](dm32-migration.md), [dm32-stock-cps.md](dm32-stock-cps.md) |
| Radioddity DB25-D PRO | [hardware-db25d.md](hardware-db25d.md) | | |
| Radioddity GD-77 | [hardware-gd77.md](hardware-gd77.md) | | |
| Radioddity GD-88 | [hardware-gd88.md](hardware-gd88.md) | | |
| Anytone AT-D868UV / D878UV / D578UV | [hardware-anytone.md](hardware-anytone.md) | | |

## Plans

| Spec | Covers |
|---|---|
| [dmr-tier3-plan.md](dmr-tier3-plan.md) | DMR Tier III trunking: scope, architecture, work packages (planning only) |
