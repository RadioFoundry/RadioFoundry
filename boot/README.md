<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# First stages

A first stage is a small program in front of the application, for a radio whose loader starts the application directly.
Each stage has its own folder, named for its board: `db25d/` serves the Radioddity DB25-D. A board turns its stage on in
`board.mk` (`BOOT_STAGE`, `BOOT_SIZE`, `BOOT_DEFS`, `BOOT_SIGNATURE`); `boot.mk` builds it with the board's compiler,
and `make` writes `build/<board>/rfoundry_<board>_install.bin`: the stage, padded to its sector, followed by the
application.

## DB25-D (`db25d/`)

The DB25-D's IAP loader (0x03000000-0x0300BFFF) jumps to 0x0300C000. Its UART0, where the HR_C7000 boot ROM
offers a serial download, isn't reachable, so without the stage there would be no way into a stub but the loader.

| Address | Content |
|---|---|
| 0x0300C000 | The stage, one 4 KB sector |
| 0x0300D000 | The application (`boards/db25d/fm_only.ld`) |

At every power-on, the stage does the following:

1. Starts with the 6 bytes `2C 11 01 C0 20 64`. The loader starts an application on a normal
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

To install, write `rfoundry_db25d_install.bin` from 0x0300C000, through the loader's update mode (P1 and power) or with
a running stub (`stubtool --write-app`). The loader erases only as far as the image reaches.

`boards/db25d/tests/bootDb25dTest.c` tests the stage's protocol. For the stub it loads, see
[stub/README.md](../stub/README.md).
