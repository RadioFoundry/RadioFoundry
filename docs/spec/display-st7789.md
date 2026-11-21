<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# ST7789-class panel on the i8080 bus

The driver writes RGB565 pixels, two bytes per pixel. The panel configuration supplies the dimensions, memory access
direction and inversion. The board supplies its bus pins and usable display area.
[Display driver](../../drivers/display/st7789/st7789.c)

## Reset and initialization

Drive NRESET high, low and high again, waiting 120 ms after each level. Then send the commands in order.

| # | Command | Parameters | Meaning |
|---|---|---|---|
| 1 | 0x11 | — | sleep out, then wait 120 ms |
| 2 | 0xB2 | 0C 0C 00 33 33 | porch control |
| 3 | 0x35 | 00 | tearing effect line on |
| 4 | 0x36 | panel MADCTL | memory access direction |
| 5 | 0x3A | 05 | 16 bits per pixel |
| 6 | 0xB7 | 56 | gate control |
| 7 | 0xBB | 17 | VCOM |
| 8 | 0xC0 | 2C | LCM control |
| 9 | 0xC2 | 01 | VDV/VRH from command |
| 10 | 0xC3 | 0B | VRH |
| 11 | 0xC6 | EF | frame rate control (normal mode) |
| 12 | 0xD0 | A7 | power control 1 (first, short form) |
| 13 | 0xD0 | A4 A1 | power control 1 |
| 14 | 0xD6 | A1 | (undocumented) |
| 15 | 0xE0 | 70 03 08 07 08 15 30 54 47 3A 15 14 2B 2E | positive gamma |
| 16 | 0xE1 | 70 06 0A 09 06 03 2F 44 47 38 12 15 29 2F | negative gamma |
| 17 | 0x21 or 0x20 | — | set the inversion specified by the panel |
| 18 | 0x2A | first and last columns | set the column window |
| 19 | 0x2B | first and last rows | set the row window |
| 20 | 0x2C | RGB565 pixels | clear the panel to black |
| 21 | 0x29 | — | display on |
