<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Specifications

The specs say what the firmware does, one subsystem a page. They describe behavior that holds on every radio: what a
board must provide is stated as `board_t` fields, `hal/` interfaces and `CAP_*` bits. For which radios have a feature,
see the [feature matrix](../feature-matrix.md). For process (building, testing, releases), see the
[contributor guide](../dev/README.md).

Code cites these pages by file name and section number, for example `docs/spec/aprs.md 4.2`. Section numbers are
stable: a rewrite keeps them.

## Radio and user interface

- [Capabilities](capabilities.md): every capability by area, with its build switch and spec.
- [Radio features](radio-features.md): settings of the radio core, scan and dual watch, FM transmit extras, beeps,
  volume, battery, LEDs, transmit power and the reference trim.
- [Air band and broadcast](airband-and-broadcast.md): AM receive in the air band and the broadcast FM receiver.
- [APRS](aprs.md): AFSK on FM, positions over DMR, the KISS TNC, and iGate rules.
- [Position](position.md): the position service and the protocols that read it.
- [User interface](ui.md): how `core/ui` is built: screens, keys, data owners, and the simulator.
- [Radios](radios.md): the radio survey: each radio's chips and port state.

## Digital modes

- [Air interface](air-interface.md): the layer shared by the digital mode engines.
- DMR: [voice](dmr-voice.md), [data](dmr-data.md), [zone sharing](zone-share.md), [emergency](dmr-emergency.md),
  [roaming](dmr-roaming.md), and [Tier III trunking](dmr-trunking.md).
- [P25](p25.md) and [P25 trunking](p25-trunking.md).
- [NXDN](nxdn.md).
- Repeating: [single-frequency repeater](sfr.md) and [split duplex](split-duplex.md).

## Modem chips

- [DMR modem](dmr-modem.md): the driver contract and how the chips compare.
- HR_C6000: [as a modem](modem-hrc6000.md) and its [registers](hrc6000-registers.md).
- [HR_C7000 as a modem](modem-hrc7000.md).
- [PA key-up](pa-keyup.md): power amplifier control for each DMR burst.
- [Second receiver link](rx2-link.md): the protocol to a receiver on a second SoC.

## Host link and network

- [Debug link](debuglink.md), [CPS protocol](cps-protocol.md), and the [fast codeplug protocol](cps-fast.md).
- [MMDVM protocol](mmdvm-protocol.md) for hotspot mode.
- [USB network](usb-network.md) and [PPP](ppp.md), with the services on top: [TCP services](tcp-services.md),
  [TFTP](tftp.md), [mDNS](mdns.md), and [SNMP](snmp.md) ([MIB](RFOUNDRY-MIB.txt)).
- [USB audio](usb-audio.md): USRP voice over the network link, and the shared audio stream.
- [Compact ID database](iddb-compact.md).

## Codeplug and storage

- [Open-firmware codeplug](open-firmware-cps.md) and [qdmr YAML](qdmr-yaml.md).
- [Native codeplug](codeplug-native.md), [codeplug settings](codeplug-settings.md), and
  [channel modes](channel-modes.md).
- Mode records: [DMR Tier III](dmr-trunking-codeplug.md), [P25](p25-codeplug.md), and [NXDN](nxdn-codeplug.md).
- [Codeplug conversion](codeplug-port.md) from a manufacturer codeplug.
- [Settings store](settings-store.md) and [NAND sectors](nand-sectors.md).

## Flashing and reliability

- [Stub entry](stub-entry.md) and the [STM32F4 stub](stub-stm32f4.md).
- [Crash log](crash-log.md) and [watchdog](watchdog.md).
- [Gotchas](gotchas.md): past bugs and the rules they left, by subsystem.
