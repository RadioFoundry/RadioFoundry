<!-- SPDX-License-Identifier: MIT -->
# First stages

A first stage is a small program in front of the application, for a radio whose loader starts the application
directly. There are three, all on HR_C7000 radios: `db25d/` (Radioddity DB25-D), `gd88/` and `gd88b/` (the GD-88's two
SoCs). A board turns its stage on in `board.mk` (`BOOT_STAGE`, `BOOT_SIZE`, `BOOT_DEFS`, `BOOT_SIGNATURE`); `boot.mk`
builds it with the board's compiler, and `make` writes `build/<board>/odr_<board>_install.bin`: the stage, padded to
its sector, followed by the application.

The common part is in `hrc7000/`: the entry (`entry.S`), the linker script and the boot ROM's protocol (`stage.c`).
Each radio's `boot.c` names its UART and its pins, makes the radio safe and includes `stage.c`.

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

The protocol is covered by `tests/host/bootDb25dTest.c`. The facts behind it are in
[hardware-db25d.md](../docs/spec/hardware-db25d.md) 2 and 7.

## GD-88 (`gd88/`, `gd88b/`)

Each SoC has its own vendor IAP loader below 0x0300C000 and its own program flash, with the DB25-D's layout: the stage
in the sector at 0x0300C000, the application from 0x0300D000. Both stages start with the same signature.

SoC A (`gd88/`) is the DB25-D's stage with A's pins:

1. The power latch (PTB13) high.
2. The transmitter off: PTB2, A's TX line, low.
3. The 64-bit serial output chain (PTB0 clock, PTB1 data, PTB3 latch; audio routing and the TX route) in its power-on
   state: the stage shifts out 64 zero bits and latches them. PTC20 goes low.
4. UART1 (the programming cable, PTA9 / PTA10) as the loader sets it up, then ten handshakes, then the application.

SoC B (`gd88b/`) has no cable. Its UART3 (PTA13 / PTA14) is the link to A:

1. The transmitter off: PTB20 / PTB21 (TX enables), PTB6 / PTB3 (TX tune), PTA17 / PTB10 (TX band switches) low.
2. UART3 set up, then ten handshakes there, then the application.
3. If B's application wrote `STAY` (0x53544159) at 0x0004FFF8 before it jumped to the stage, the stage offers for up to
   a minute (1,200 handshakes). ODR on A asks for this with `RX2_STUB` and then bridges the cable to UART3, so the host
   loads the stub into B through A ([rx2-link.md](../docs/spec/rx2-link.md) 4). The stage clears the word either way.

`tests/host/bootGd88Test.c` covers both.
