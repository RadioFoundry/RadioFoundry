<!-- SPDX-License-Identifier: MIT -->
# First stages

A first stage is a small program in front of the application, for a radio whose loader starts the application
directly. Today there is one: `db25d/`, for the Radioddity DB25-D. A board turns its stage on in `board.mk`
(`BOOT_STAGE`, `BOOT_SIZE`, `BOOT_DEFS`, `BOOT_SIGNATURE`); `boot.mk` builds it with the board's compiler, and `make`
writes `build/<board>/odr_<board>_install.bin`: the stage, padded to its sector, followed by the application.

## DB25-D (`db25d/`)

The DB25-D's vendor IAP loader (0x03000000-0x0300BFFF) jumps to 0x0300C000. Its UART0, where the HR_C7000 boot ROM
offers a serial download, isn't reachable, so without the stage there would be no way into a stub but the loader.

| Address | Content |
|---|---|
| 0x0300C000 | The stage, one 4 KB sector |
| 0x0300D000 | The application (`boards/db25d/db25d.ld`) |

What the stage does at every power-on:

1. Starts with the 6 bytes of the stock entry (`2C 11 01 C0 20 64`). The loader starts an application on a normal
   power-on only if its first 4 bytes are `2C 11 01 C0`; otherwise it stays in update mode. `pad.py` fails the build
   if the stage loses them.
2. Drives the power latch (PTB13) high, so the radio stays on once the power key is let go, and the transmitter
   off: TX supplies (PTA27, PTB2) and PA selects (PTA26, PTB3) low, the receive supply (PTA23) high. A soft reset
   in the middle of a transmission may leave them as they were ([crash-log.md](../docs/spec/crash-log.md) 8).
3. Sets UART1 up as the loader does in its update mode (pins, clocks, 115200 8N1 at the loader's 36 MHz APB), unless
   the loader already did.
4. Offers the HR_C7000 boot ROM's handshake (`02 24 00 03`) ten times, about 50 ms apart. If `stub/stubtool.py`
   answers, it takes the stub in 256-byte blocks and starts it, exactly as the boot ROM does on a DM-32UV.
5. Otherwise jumps to the application at 0x0300D000. A UART that doesn't work skips straight to this step.

Install: write `odr_db25d_install.bin` from 0x0300C000, through the loader's update mode (P1 and power) or with a
running stub (`stubtool --write-app`). The loader erases only as far as the image reaches.

The protocol is covered by `boards/db25d/tests/bootDb25dTest.c`. The facts behind it are in
[hardware-db25d.md](../docs/spec/hardware-db25d.md) 2 and 7.
