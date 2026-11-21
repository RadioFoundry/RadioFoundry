<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# HR_C7000 platform

| Item | Value |
|---|---|
| SoC | Hongruan HR_C7000, sold by Dahua as DH4570 |
| CPU | C-SKY CK803S (32-bit, little endian, 16/32-bit mixed instruction set); toolchain target `-mcpu=ck803s` |
| ELF e_flags that make objdump decode CK803 32-bit opcodes | 0x21002009 |
| Interrupt controller | Dahua PIC at 0x17000000, 64 sources, normal interrupts only (no fast interrupts on C7000) |
| DMR/FM baseband | on chip ("modem", registers at 0x11000000, buffers at 0x16000000) |
| Audio codec | on chip (byte registers at 0x160009C0) |

The [modem reference](modem-hrc7000.md) covers baseband registers and audio buffers.

## Microphone level

The codec ADC delivers 80 microphone samples every 10 ms. With DMR and VOX compiled in, the platform can measure
their level through `hrc_audio_pcm_mic_level`. The first read enables the PCM interrupt. After one second without a
read, the meter releases the interrupt unless the audio stream still needs it. A board opts in by supplying the
`hal_power_t.mic_level` hook. [PCM implementation](../../platform/hrc7000/audio_pcm.c)

## Data flash layout

For a board using this 16 MiB SPI NOR layout, the driver refuses writes below the board’s write floor. The board
supplies the end of the open-firmware region in `hal_flash_map_t.openfw_end`. It can reserve space before the native
codeplug. [Flash driver](../../platform/hrc7000/flash_spi0.c)

| Region | Start |
|---|---|
| Writable data and open-firmware EEPROM image | `0x700000` |
| Settings ring (8 sectors) | `0x711000` |
| Settings journal (2 sectors) | `0x719000` |
| Open-firmware flash image | `0x720000` |
| Native codeplug (1 MiB) | `0xF00000` |

## Factory serial loader

On boards that support factory update mode, the loader speaks 115200 baud, 8N1 on the programming cable.
The board’s installation instructions identify its supported write path. For a boot-ROM install the ROM loads a RAM stub. The host writes the application through the stub
and reads it back for comparison. [Universal stub](../../stub/README.md#the-universal-stub)

| Step | Host sends | Radio answers |
|---|---|---|
| 1 | 0x52 | 0x06 |
| 2 | 0x4D 00 00 00 01 | 0x4D 01 09 |
| 3 | 0x06 | model string |
| 4 | 0x45 00 00 00 00 | 0x06 |
| 5 | 0x31 | 0x43 |
| 6 | 0x01 | 0x43 |
| 7 | 0x00 0xFF, then a 128-byte block: name, 0x00, decimal size as ASCII, zero padded; then CRC-16/XMODEM of the block, big endian. The radio erases the app area (up to about 15 s) | 0x06 0x43 |
| 8 | per 1 KB block: 0x02, wait 0x43; block number n, 0xFF − n (n starts at 1, wraps 0xFF → 0x00); 1024 bytes (last block zero padded); CRC-16/XMODEM big endian | 0x06 |
| 9 | 0x04 | (reboots) |

Send the application payload after stripping any vendor file header. [Serial loader](../../tools/serial_loader.py)
