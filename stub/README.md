<!-- SPDX-License-Identifier: MIT -->
# stub: flash access for the HR_C7000 radios (DM-32UV, DM-1801 Pro, DB25-D)

A small RAM monitor and the host tool that drives it. The tool loads the monitor into the radio's RAM through the chip's
serial boot ROM (radio switched on with the cable in), through the DB25-D's first stage, or, with `CONFIG_STUB_ENTRY=y`,
from a running RadioFoundry over the debug link (`docs/spec/debuglink.md` 3.18, `docs/spec/hardware-dm32.md` 17.2). It then reads
the program flash, the data flash (the DB25-D's SPI NAND too) and installs RadioFoundry builds into the application area of the
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

One image (7,796 bytes, protocol version 7) serves every HR_C7000 radio and every way in. It is linked to run at 0x30000
and copies itself there from wherever it was loaded, then zeroes its .bss and sets its own stack. Board details are not
built in: after HELLO the host sends CONFIG (clock rates it measured, the UART to keep, the power latch and key pins,
LEDs, board id, NAND flag), and checks the echo in the next HELLO before it writes anything. The profiles are in
`stubtool.py` (`C7_BOARDS`) and come from the running RadioFoundry's VERSION `board=`, or from `--board` at power-on. A stub that
was not configured refuses every write.

The watchdog: RadioFoundry's `enter_stub` leaves it running with a 32 s timeout, and the stub feeds it with the knock sequence on
every pass of its receive wait and before every SPI read burst, so a stub that hangs resets the radio. `--boot reset`
turns it off just before the soft reset, after it writes RadioFoundry's restart-cause mark (`CRASH_SOFT_MARK`,
`docs/spec/crash-log.md` 1.3) through POKE, so RadioFoundry reports a software restart. No flashing or backup trips the watchdog unless the stub itself hangs (`docs/spec/watchdog.md` 3). The source of this image is kept outside the repository; a rebuild is
embedded with `python3 stub/embed_stub.py stub/stubtool.py UNIV stub_univ.bin`, and the site serves the image from
`stubtool.py`, never a copy.

HELLO v6 also reports the RAM footprint (run address, image length, end of .bss, stack bounds) and the deepest stack use
so far, measured from a painted stack. `stubtool` prints it as `stub RAM:`.

Protocol 7 adds WRITE (op 0x0E): one request carries a whole 4 KB sector, and the stub erases it, programs its non-blank
pages, waits on the flash itself and replies with the CRC-32 of the sector read back. The host compares that CRC with
its own and writes the sector again (up to 3 times) on a mismatch. The CRC pass over the whole range afterwards is
unchanged. Before protocol 7 every page took four or more requests (write enable, status, program, busy polls), so a
write was bound by turnarounds, not bytes. Both hosts (`stubtool.py`, the web flasher's `stub.js`) use WRITE when HELLO
says 7 or more and the per-page SPI requests otherwise. The host checks the write range before it sends, as before.

| DM-32UV, 115200 baud, FTDI cable on a Mac (2026-10-27) | Per-page requests (v6) | WRITE (v7) |
|---|---|---|
| One changed sector | 1.37 s | 0.41 s |
| A 505 KB image, all 127 sectors changed | about 174 s (estimate) | 51.9 s |

At 115200 a WRITE frame (4,106 bytes) takes about 360 ms on the wire, so what is left is bandwidth.

Protocol 7 also lets the host raise the serial rate without user help. The stub goes back to its home rate by itself
once a raised link has carried no good request for one BAUD window, so the host can always get home by going quiet
(`common/PROTOCOL.md`, "Going home on a lost link"). `stubtool.py` picks the rate for writes and backups: a binary
search over the rates the UART makes (clock / (16 * d), 200000 to 1100000, fastest first), each proved with
WRITE-size frames to the radio and full-size reads back, then remembered per port and radio in
`~/.cache/rfoundry/stub-baud.json`. A raised rate that starts damaging frames mid-run is dropped (+10 per damaged
frame, -1 per clean one, give up at 30) and the run carries on at 115200. `--baud off` keeps 115200, `--baud N` asks
for one rate. The web flasher stays at 115200.

| DM-32UV, FTDI FT232R cable on a Mac (2026-10-27) | Result |
|---|---|
| 350000, 291666, 276315 | Fail the proof (no reply, or bad request checksums on 4 KB frames to the radio); fall back by themselves |
| 262500 | Holds; picked by the search in about 16 s, then at once from the cache |
| A 517 KB image, 126 sectors changed, at 262500 | 27 s writing, 34 s including the stub upload |

The limit was the cable and the radio's jack, not the divisor: 276315 passed full-size reads but damaged long frames
towards the radio, which is why the proof sends both.

The rates take a measured clock. `timer_hz` times each sample as its request goes out, since the reply can wait up to
the cable's latency timer (16 ms on an FTDI). Timing to the reply read an 84 MHz clock as about 80 MHz, and every
divisor but the first came out 5 % off. Both hosts round the measured clock to whole MHz (every APB clock here is:
24, 36 or 84 MHz), so the same rates come out every run.

### RAM placement

The HR_C7000's RAM is IRAM 0x10000-0x57FFF (288 KB). The 32 KB SRAM at 0x18000000 is shared with the modem and is not
used. The chip has no general DMA controller (only the USB and SDIO blocks have their own, unused on these radios), and
the modem writes only its own SRAM, so after the stub masks every interrupt nothing else writes IRAM.

| Region | Address | Size |
|---|---|---|
| Entry and copy loop (position independent, no stack) | load address | 0x80 |
| Image (text, parameter block at +0x80, data) | 0x30000-0x31E74 after the copy | 7,796 B |
| .bss (receive buffer, sector buffers) | to 0x36C9C | 20,008 B |
| Stack (4 KB, painted) | 0x36CA0-0x37CA0 | 4,096 B |
| Whole footprint | 0x30000-0x37CA0 | 31,904 B (32 KB budget, checked by the linker) |

Measured stack use: 168 bytes on the DM-32UV and DM-1801 Pro, 252 bytes on the DB25-D (NAND reads included); 200 bytes on
the DM-32UV after two 126-sector WRITE installs (protocol 7, 7,796-byte build).

The entry stops in place, writing nothing, when the loaded copy and 0x30000 would overlap. The host checks the same
rule, and that the load lies in IRAM, before it sends the image (`check_univ_load`).

| Way in | Load address | What is in RAM at the hand-over | Why 0x30000 is safe |
|---|---|---|---|
| Boot ROM, power-on (all three radios) | 0x10000 (28 blocks of 256 B) | Nothing: the ROM uses no RAM (registers only) | All IRAM is free |
| DB25-D first stage, power-on | 0x10000 | The first stage (accepts blocks below 0x4E000) | It is never returned to |
| Running RadioFoundry, DM-32UV, DM-1801 Pro | 0x17000 (span 96 KB) | RadioFoundry, about to be dropped | RadioFoundry is not returned to; the reset restarts it |
| Running RadioFoundry, DB25-D without DMR | 0x30000 (span 128 KB) | RadioFoundry | Loaded at the run address, no copy |
| Running RadioFoundry, DB25-D with DMR | 0x1D000 (span 72 KB, codec window) | RadioFoundry | The copy goes up to 0x30000, which does not overlap the source |
| Stock DB25-D 018, console | 0x54000 (window 0x54000-0x58000) | Stock: data and .bss to 0x525E0, interrupt stack to 0x535E0, heap to 0x53DE0 | The window is above the heap; the copy target was stock .bss, unused once the stub has masked interrupts |
| Stock DB25-D 020, console | 0x54000 (same window) | Stock: data and .bss to 0x4F150, interrupt stack to 0x50150, heap to 0x50950 | As for 018 |

Why 0x30000: it is the one address every route can reach without moving anything still in use. Running RadioFoundry spans start
at 0x17000, 0x1D000 or 0x30000 and all cover 0x30000-0x37978. The ROM and the first stage leave all IRAM free. A stock
DB25-D keeps nothing the stub needs once interrupts are off, and the only part of the stub that must sit in free stock
RAM while stock still runs is the 7,004-byte image, which fits the 16 KB above 018's heap. Placing .bss and stack at
the run address and not after the load address means no route has to assume free RAM above where it loaded the stub.

On the stock routes the stub hands the program flash back from execute-in-place to the SSI and masks the interrupt
controller before it touches the flash. The clocks are left as stock set them; the host measures timer 2 and sends the
rates in CONFIG, so the same `db25d` profile serves stock and RadioFoundry (both use UART1 on the K1 jack).

### The first stage

On the DB25-D, 0xC000 holds RadioFoundry's first stage and the application starts at 0xD000. `--write-app` writes at 0xC000,
so the DB25-D takes the `_install.bin`, which starts with the stage. The tool reads the radio's word at 0xC000 first.
When it is the stage's and the image does not start with it, the write is refused: the application alone there leaves
the radio in its update loader (white screen, red LED) until an IAP install. No board list is involved, so any radio
with the stage is covered, and a radio whose stage is already gone takes either image. `--replace-stage` overrides
the check.

### Older stubs

The position-dependent stubs (linked for the ROM load at 0x10000, for RadioFoundry at 0x17000, and the DB25-D's 0x30000 and
0x1D000 builds) stay embedded. `--legacy-stub` selects them (with `--load-base` where the tool cannot tell the base),
`--stub FILE` / `--rfoundry-stub FILE` load an image built elsewhere. The tool reads the addresses an image was linked with
and refuses one that does not match before anything is sent, because jumping into a stub linked for another base needs
a power cycle. A stub older than version 6 (a HELLO shorter than 19 bytes) gets no CONFIG.

The universal stub is built outside this repository with the monitor's source; `stub/embed_stub.py` embeds it. `stub/db25d_update.sh IMAGE [PORT]` is the one command for a DB25-D update.

## STM32F4 radios

A second stub, with its source here, for the STM32F405 radios behind the AnyRoad / TYT bootloader. A running RadioFoundry built
with `CONFIG_STUB_ENTRY=y` loads it over the USB debug link; it then installs builds (changed sectors only, the codec
kept), backs up and verifies the internal flash, reads and writes the external SPI flash, and backs up and puts back the SPI
flash's security registers (`--secreg`, `--write-secreg`; never their one-time lock bits). Spec: [docs/spec/stub-stm32f4.md](../docs/spec/stub-stm32f4.md).

One image serves every STM32F4 radio, as the HR_C7000's universal stub does: after HELLO the host sends a CONFIG with the
radio's board record (SPI flash bus, pins, mode and chip select, other chip selects to hold high, the crystal, the
bootloader's end), and checks the echo in the next HELLO. The stub writes nothing before that. The profiles are
`BOARDS` in `f4stub.py`; the board comes from RadioFoundry's VERSION `board=`, or from `--board` with `--attach`.

The STM32F4 stub is linked for the SRAM base. RadioFoundry stages it in an 8 KB buffer and copies it there, so `--rfoundry-enter`
checks it differently from the HR_C7000 monitors: it must start with the STM32F4 stub's entry code, and the base RadioFoundry
reports must be 0x20000000.

The IWDG can't be stopped once RadioFoundry starts it (`CONFIG_WATCHDOG`), so the stub feeds it: at start-up, in every wait loop
(through `stub_ms()`, including the flash erase busy wait), at every request and once per 4 KB block of a long read
or checksum. Its image has a 16-byte header at offset 0x10 (magic, stub version 5, flags with bit 0 "feeds the
IWDG"; `stm32f4/f4stub_header.h`, the one definition the stub and RadioFoundry share, layout in
[stub-stm32f4.md](../docs/spec/stub-stm32f4.md) 3.1). A watchdog build of RadioFoundry refuses ENTER unless the header has the
magic, a version of at least 3 and the flag, so an older `f4stub.py` or web page gets a refusal before anything is
written, never a reset part way through. `--rfoundry-enter` prints the version it read from the header. The web flasher serves the image embedded in `f4stub.py`, never a copy (`docs/spec/watchdog.md` 3.1).

| File | What |
|---|---|
| `common/` | the protocol engine every stub target shares (`stubeng.h` documents the port a target supplies): framing, ops, safety rules, SUMS and READ. Host tested |
| `stm32f4/` | the STM32F4 target: entry, USB, flash controller, SPI flash, the board record check, linker script and Makefile: `make -C stub/stm32f4` writes `build/stub-stm32f4/f4stub.bin` (8,072 bytes, at most 8 KB, built with `-flto`) |
| `f4stub.py` | its host side, with the stub build embedded (checked by SHA-256; `--embed` puts a new build in, and CI fails while a fresh build differs); `stubtool.py` hands every board with an `f4stub` `[flash.stub]` to it (`--rfoundry-enter`, or `--attach --board md380`); the board profiles come from the board registry (`board.toml` `[flash.stub]` and `pins.h`, [board-registry.md](../docs/dev/board-registry.md)); `--self-test` runs without a radio |

Status (2026-10-25): built and host tested (`tests/host`: the protocol engine with the board record and the security
registers, and the host tool against the real engine through a pipe). Protocol 2 ran on the DM-1701
([stub-stm32f4.md](../docs/spec/stub-stm32f4.md) 11); protocol 4 (the board record), protocol 5 (the security registers)
and the MD-380 have not run on a radio yet.

## HR_C7000 status

Status (2026-10-17), universal stub, entry from a running RadioFoundry: on the DM-1801 Pro and the DM-32UV (its first stub run),
reads of both flash chips, the bootloader identical to earlier dumps, `--write-test` and the restart into RadioFoundry. On the
DB25-D (DMR build, so the copy from 0x1D000), `--write-app` of the running image (0 sectors written), NAND reads and
`--verify-nand`, and the restart. Not yet run: the power-on routes with the 28-block image and the stock console route.

Earlier (2026-10-15), with the older stubs: `--write-app` (a 93-sector build in 71 s, changed sectors only, both
directions) and the provisional baud switch on a DM-1801 Pro.
