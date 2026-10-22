<!-- SPDX-License-Identifier: MIT -->
# stub: flash access for the HR_C7000 radios (DM-32UV, DM-1801 Pro, DB25-D, GD-88)

A small RAM monitor and the host tool that drives it. The tool loads the monitor into the radio's RAM through the chip's
serial boot ROM (radio switched on with the cable in), through the DB25-D's first stage, or, with `CONFIG_STUB_ENTRY=y`,
from a running ODR over the debug link (`docs/spec/debuglink.md` 3.18, `docs/spec/hardware-dm32.md` 17.2). It then reads
the program flash, the data flash (the DB25-D's SPI NAND too) and installs ODR builds into the application area of the
program flash (`--write-app`), with every block checked.

| File | What |
|---|---|
| `stubtool.py` | the host tool; needs Python 3 and pyserial; has the stub images embedded; `--self-test` runs without a radio |
| `build_db25d_stubs.sh`, `embed_stub.py` | build the older position-dependent DB25-D stubs, and embed an image in `stubtool.py` |
| `db25d_update.sh` | DB25-D: enter, write the install image, restart, check |
| `USER-GUIDE.md` | how to use the tool, in plain language |

The binaries are built from the monitor's source (C, about 1,400 lines, written for this project) with the csky-elf GCC 14
toolchain (`-Os`). The source and its protocol document are kept with the reverse-engineering notes outside this
repository for now. `stubtool.py --self-test` decodes every embedded image and checks its layout.

The port can be a device or a pyserial URL: `rfc2217://HOST:PORT` reaches a radio on a networked serial server, so the
server keeps running while the tool writes through it (`--write-app` took 111 s for 24 changed sectors that way).

## The universal stub

One image (7,004 bytes, protocol version 6) serves every HR_C7000 radio and every way in. It is linked to run at 0x30000
and copies itself there from wherever it was loaded, then zeroes its .bss and sets its own stack. Board details are not
built in: after HELLO the host sends CONFIG (clock rates it measured, the UART to keep, the power latch and key pins,
LEDs, board id, NAND flag), and checks the echo in the next HELLO before it writes anything. The profiles are in
`stubtool.py` (`C7_BOARDS`) and come from the running ODR's VERSION `board=`, or from `--board` at power-on. A stub that
was not configured refuses every write.

The watchdog: ODR's `enter_stub` leaves it running with a 32 s timeout, and the stub feeds it with the knock sequence on
every pass of its receive wait and before every SPI read burst, so a stub that hangs resets the radio. `--boot reset`
turns it off just before the soft reset. No flashing or backup trips the watchdog unless the stub itself hangs (`docs/spec/watchdog.md` 3). The source of this image is kept outside the repository; a rebuild is
embedded with `python3 stub/embed_stub.py stub/stubtool.py UNIV stub_univ.bin`, and the site serves the image from
`stubtool.py`, never a copy.

HELLO v6 also reports the RAM footprint (run address, image length, end of .bss, stack bounds) and the deepest stack use
so far, measured from a painted stack. `stubtool` prints it as `stub RAM:`.

### RAM placement

The HR_C7000's RAM is IRAM 0x10000-0x57FFF (288 KB). The 32 KB SRAM at 0x18000000 is shared with the modem and is not
used. The chip has no general DMA controller (only the USB and SDIO blocks have their own, unused on these radios), and
the modem writes only its own SRAM, so after the stub masks every interrupt nothing else writes IRAM.

| Region | Address | Size |
|---|---|---|
| Entry and copy loop (position independent, no stack) | load address | 0x80 |
| Image (text, parameter block at +0x80, data) | 0x30000-0x31B5C after the copy | 7,004 B |
| .bss (receive buffer, sector buffer) | to 0x36974 | 19,992 B |
| Stack (4 KB, painted) | 0x36978-0x37978 | 4,096 B |
| Whole footprint | 0x30000-0x37978 | 31,096 B (32 KB budget, checked by the linker) |

Measured stack use: 168 bytes on the DM-32UV and DM-1801 Pro, 252 bytes on the DB25-D (NAND reads included).

The entry stops in place, writing nothing, when the loaded copy and 0x30000 would overlap. The host checks the same
rule, and that the load lies in IRAM, before it sends the image (`check_univ_load`).

| Way in | Load address | What is in RAM at the hand-over | Why 0x30000 is safe |
|---|---|---|---|
| Boot ROM, power-on (all three radios) | 0x10000 (28 blocks of 256 B) | Nothing: the ROM uses no RAM (registers only) | All IRAM is free |
| DB25-D first stage, power-on | 0x10000 | The first stage (accepts blocks below 0x4E000) | It is never returned to |
| GD-88 A or B first stage | 0x10000 | As the DB25-D's | As the DB25-D's |
| Running ODR, DM-32UV, DM-1801 Pro | 0x17000 (span 96 KB) | ODR, about to be dropped | ODR is not returned to; the reset restarts it |
| Running ODR, DB25-D without DMR | 0x30000 (span 128 KB) | ODR | Loaded at the run address, no copy |
| Running ODR, DB25-D with DMR | 0x1D000 (span 72 KB, codec window) | ODR | The copy goes up to 0x30000, which does not overlap the source |
| Stock DB25-D 018, console | 0x54000 (window 0x54000-0x58000) | Stock: data and .bss to 0x525E0, interrupt stack to 0x535E0, heap to 0x53DE0 | The window is above the heap; the copy target was stock .bss, unused once the stub has masked interrupts |
| Stock DB25-D 020, console | 0x54000 (same window) | Stock: data and .bss to 0x4F150, interrupt stack to 0x50150, heap to 0x50950 | As for 018 |

Why 0x30000: it is the one address every route can reach without moving anything still in use. Running ODR spans start
at 0x17000, 0x1D000 or 0x30000 and all cover 0x30000-0x37978. The ROM and the first stage leave all IRAM free. A stock
DB25-D keeps nothing the stub needs once interrupts are off, and the only part of the stub that must sit in free stock
RAM while stock still runs is the 7,004-byte image, which fits the 16 KB above 018's heap. Placing .bss and stack at
the run address and not after the load address means no route has to assume free RAM above where it loaded the stub.

On the stock routes the stub hands the program flash back from execute-in-place to the SSI and masks the interrupt
controller before it touches the flash. The clocks are left as stock set them; the host measures timer 2 and sends the
rates in CONFIG, so the same `db25d` profile serves stock and ODR (both use UART1 on the K1 jack).

### The first stage

On the DB25-D, 0xC000 holds ODR's first stage and the application starts at 0xD000. `--write-app` writes at 0xC000,
so the DB25-D takes the `_install.bin`, which starts with the stage. The tool reads the radio's word at 0xC000 first.
When it is the stage's and the image does not start with it, the write is refused: the application alone there leaves
the radio in its update loader (white screen, red LED) until an IAP install. No board list is involved, so any radio
with the stage is covered, and a radio whose stage is already gone takes either image. `--replace-stage` overrides
the check.

### Older stubs

The position-dependent stubs (linked for the ROM load at 0x10000, for ODR at 0x17000, and the DB25-D's 0x30000 and
0x1D000 builds) stay embedded. `--legacy-stub` selects them (with `--load-base` where the tool cannot tell the base),
`--stub FILE` / `--odr-stub FILE` load an image built elsewhere. The tool reads the addresses an image was linked with
and refuses one that does not match before anything is sent, because jumping into a stub linked for another base needs
a power cycle. A stub older than version 6 (a HELLO shorter than 19 bytes) gets no CONFIG.

The universal stub is built outside this repository with the monitor's source; `stub/embed_stub.py` embeds it. `stub/db25d_update.sh IMAGE [PORT]` is the one command for a DB25-D update.

## STM32F4 radios

A second stub, with its source here, for the STM32F405 radios behind the AnyRoad / TYT bootloader. A running ODR built
with `CONFIG_STUB_ENTRY=y` loads it over the USB debug link; it then installs builds (changed sectors only, the codec
kept), backs up and verifies the internal flash, and reads and writes the external SPI flash. Spec: [docs/spec/stub-stm32f4.md](../docs/spec/stub-stm32f4.md).

One image serves every STM32F4 radio, as the HR_C7000's universal stub does: after HELLO the host sends a CONFIG with the
radio's board record (SPI flash bus, pins, mode and chip select, other chip selects to hold high, the crystal, the
bootloader's end), and checks the echo in the next HELLO. The stub writes nothing before that. The profiles are
`BOARDS` in `f4stub.py`; the board comes from ODR's VERSION `board=`, or from `--board` with `--attach`.

The STM32F4 stub is linked for the SRAM base. ODR stages it in an 8 KB buffer and copies it there, so `--odr-enter`
checks it differently from the HR_C7000 monitors: it must start with the STM32F4 stub's entry code, and the base ODR
reports must be 0x20000000.

The IWDG can't be stopped once ODR starts it (`CONFIG_WATCHDOG`), so the stub feeds it: at start-up, in every wait loop
(through `stub_ms()`, including the flash erase busy wait), at every request and once per 4 KB block of a long read
or checksum. Its image has a 16-byte header at offset 0x10 (magic, stub version 4, flags with bit 0 "feeds the
IWDG"; `stm32f4/f4stub_header.h`, the one definition the stub and ODR share, layout in
[stub-stm32f4.md](../docs/spec/stub-stm32f4.md) 3.1). A watchdog build of ODR refuses ENTER unless the header has the
magic, a version of at least 3 and the flag, so an older `f4stub.py` or web page gets a refusal before anything is
written, never a reset part way through. `--odr-enter` prints the version it read from the header. The web flasher serves the image embedded in `f4stub.py`, never a copy (`docs/spec/watchdog.md` 3.1).

| File | What |
|---|---|
| `common/` | the protocol engine every stub target shares (`stubeng.h` documents the port a target supplies): framing, ops, safety rules, SUMS and READ. Host tested |
| `stm32f4/` | the STM32F4 target: entry, USB, flash controller, SPI flash, the board record check, linker script and Makefile: `make -C stub/stm32f4` writes `build/stub-stm32f4/f4stub.bin` (7,968 bytes, at most 8 KB) |
| `f4stub.py` | its host side, with the stub build embedded (checked by SHA-256; `--embed` puts a new build in, and CI fails while a fresh build differs); `stubtool.py` hands every board with an `f4stub` `[flash.stub]` to it (`--odr-enter`, or `--attach --board md380`); the board profiles come from the board registry (`board.toml` `[flash.stub]` and `pins.h`, [board-registry.md](../docs/dev/board-registry.md)); `--self-test` runs without a radio |

Status (2026-10-22): built and host tested (`tests/host`: the protocol engine with the board record, and the host tool
against the real engine through a pipe). Protocol 2 ran on the DM-1701 ([stub-stm32f4.md](../docs/spec/stub-stm32f4.md)
11); protocol 4 (the board record) and the MD-380 have not run on a radio yet.

## HR_C7000 status

Status (2026-10-17), universal stub, entry from a running ODR: on the DM-1801 Pro and the DM-32UV (its first stub run),
reads of both flash chips, the bootloader identical to earlier dumps, `--write-test` and the restart into ODR. On the
DB25-D (DMR build, so the copy from 0x1D000), `--write-app` of the running image (0 sectors written), NAND reads and
`--verify-nand`, and the restart. Not yet run: the power-on routes with the 28-block image and the stock console route.

Earlier (2026-10-15), with the older stubs: `--write-app` (a 93-sector build in 71 s, changed sectors only, both
directions) and the provisional baud switch on a DM-1801 Pro.
