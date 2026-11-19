<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Flash stubs

A stub is a small RAM monitor that a host tool loads into a radio to read and write its flash. This folder holds two
stubs and their host tools:

- The universal stub serves the HR_C7000 radios (DM-32UV, DM-1801 Pro, DB25-D). `stubtool.py` drives it.
- The STM32F4 stub serves the STM32F405 radios behind the AnyRoad / TYT bootloader. `f4stub.py` drives it.

To use the tools, see the [stub user guide](USER-GUIDE.md). For how a running RadioFoundry hands over to a stub, see
the [stub entry spec](../docs/spec/stub-entry.md) and [debug link spec](../docs/spec/debuglink.md) 3.18.

## Files

| File | Contents |
|---|---|
| `stubtool.py` | The HR_C7000 host tool, with the stub images embedded. It needs Python 3 and pyserial. `--self-test` runs without a radio |
| `embed_stub.py` | Embeds a stub image in `stubtool.py` |
| `build_db25d_stubs.sh` | Builds the older position-dependent DB25-D stubs |
| `db25d_update.sh` | Updates a DB25-D: enter, write the install image, restart, check |
| `common/` | The protocol engine every stub target shares: framing, ops, safety rules, SUMS and READ. `stubeng.h` documents the port a target supplies. Host tested |
| `stm32f4/` | The STM32F4 target: entry, USB, flash controller, SPI flash, the board record check, linker script and Makefile |
| `f4stub.py` | The STM32F4 host tool, with the stub build embedded. `--self-test` runs without a radio |
| `USER-GUIDE.md` | How to use the tools |

## Build

The STM32F4 stub builds from this folder:

```
make -C stub/stm32f4                 # writes build/stub-stm32f4/f4stub.bin (at most 8 KB, built with -flto)
python3 stub/f4stub.py --embed build/stub-stm32f4/f4stub.bin   # puts the new build in f4stub.py
```

`f4stub.py` checks its embedded build by SHA-256, and CI fails while a fresh build differs from it.

The universal stub's source (C, about 1,400 lines, written for this project) and its protocol document are kept
outside this repository. It builds with the csky-elf GCC 14 toolchain (`-Os`). To embed a rebuilt image, run:

```
python3 stub/embed_stub.py stub/stubtool.py UNIV stub_univ.bin
```

`stubtool.py --self-test` decodes every embedded image and checks its layout. The web flasher serves the images
embedded in `stubtool.py` and `f4stub.py`, never a copy.

## Ways in

`stubtool.py` loads the universal stub into the radio's RAM in one of three ways:

- Through the chip's serial boot ROM: switch the radio on with the cable in.
- Through the DB25-D's [first stage](../boot/README.md).
- From a running RadioFoundry over the debug link, with `CONFIG_STUB_ENTRY=y`.

A running RadioFoundry built with `CONFIG_STUB_ENTRY=y` loads the STM32F4 stub over the USB debug link.

The port can be a device or a pyserial URL. `rfc2217://HOST:PORT` reaches a radio on a networked serial server, so the
server keeps running while the tool writes through it.

## The universal stub

One image (7,784 bytes, protocol version 7) serves every HR_C7000 radio and every way in. It is linked to run at
0x30000. It copies itself there from wherever it was loaded, then zeroes its .bss and sets its own stack.

Board details aren't built in. After HELLO, the host sends CONFIG: the clock rates it measured, the UART to keep, the
power latch and key pins, LEDs, board id, and NAND flag. The host checks the echo in the next HELLO before it writes
anything, and a stub that wasn't configured refuses every write. The profiles are `C7_BOARDS` in `stubtool.py`. The
board comes from the running RadioFoundry's VERSION `board=`, or from `--board` at power-on.

HELLO v6 also reports the RAM footprint (run address, image length, end of .bss, stack bounds) and the deepest stack
use so far, measured from a painted stack. `stubtool` prints it as `stub RAM:`.

### The watchdog

RadioFoundry's `enter_stub` leaves the watchdog running with a 32 s timeout. The stub feeds it with the knock sequence
on every pass of its receive wait and before every SPI read burst, so a stub that hangs resets the radio.

`--boot reset` writes RadioFoundry's restart-cause mark (`CRASH_SOFT_MARK`, [crash log spec](../docs/spec/crash-log.md)
1.3) through POKE, then turns the watchdog off just before the soft reset, so RadioFoundry reports a software restart.
Flashing and backups never trip the watchdog unless the stub itself hangs ([watchdog spec](../docs/spec/watchdog.md) 3).

### Writes

Protocol 7 adds WRITE (op 0x0E). One request carries a whole 4 KB sector. The stub erases it, programs its non-blank
pages, waits on the flash itself, and replies with the CRC-32 of the sector read back. The host compares that CRC with
its own and writes the sector again, up to 3 times, on a mismatch. A CRC pass over the whole range follows.

Before protocol 7, every page took four or more requests (write enable, status, program, busy polls), so a write was
bound by turnarounds, not bytes. Both hosts (`stubtool.py` and the web flasher's `stub.js`) use WRITE when HELLO says 7
or more, and the per-page SPI requests otherwise. The host checks the write range before it sends.

On a DM-32UV at 115200 baud over an FTDI cable, WRITE changes the times as follows:

| Write | Per-page requests (v6) | WRITE (v7) |
|---|---|---|
| One changed sector | 1.37 s | 0.41 s |
| A 505 KB image, all 127 sectors changed | About 174 s (estimate) | 51.9 s |

At 115200, a WRITE frame (4,106 bytes) takes about 360 ms on the wire, so the rest is bandwidth.

### Serial rate

Protocol 7 lets the host raise the serial rate without help from the user. The stub returns to its home rate by itself
once a raised link has carried no good request for one BAUD window, so the host can always get home by going quiet.

`stubtool.py` picks the rate for writes and backups as follows:

1. It runs a binary search over the rates the UART makes (clock / (16 * d), 200000 to 1100000, fastest first).
1. It proves each rate with WRITE-size frames to the radio and full-size reads back.
1. It remembers the rate per port and radio in `~/.cache/rfoundry/stub-baud.json`.

If a raised rate starts damaging frames mid-run, the tool drops it (+10 per damaged frame, -1 per clean one, give up at
30) and carries on at 115200. `--baud off` keeps 115200, and `--baud N` asks for one rate. The web flasher stays at
115200.

On a DM-32UV with an FTDI FT232R cable, the search gives these results:

| Rate | Result |
|---|---|
| 350000, 291666, 276315 | Fail the proof (no reply, or bad request checksums on 4 KB frames to the radio) and fall back by themselves |
| 262500 | Holds. The search picks it in about 16 s, then at once from the cache |
| A 517 KB image, 126 sectors changed, at 262500 | 27 s writing, 34 s including the stub upload |

The limit is the cable and the radio's jack, not the divisor: 276315 passed full-size reads but damaged long frames
towards the radio, which is why the proof sends both.

The rates need a measured clock. `timer_hz` times each sample as its request goes out, since the reply can wait up to
the cable's latency timer (16 ms on an FTDI). Timing to the reply reads an 84 MHz clock as about 80 MHz, and puts
every divisor but the first 5% off. Both hosts round the measured clock to whole MHz, since every APB clock here is
24, 36, or 84 MHz, so the same rates come out every run.

The latency timer is most of each request's round trip at 115200: a 64-byte debug link ping takes 32 ms at 16 ms and
17 ms at 1 ms. `stubtool.py` and `f4stub.py` set it to 1 ms on Linux (pyserial's low latency mode). On macOS and
Windows, it stays at the driver's setting (on Windows, Device Manager, the port's Advanced settings). The web flasher
sets 2 ms where it drives the cable itself (WebUSB, `usbserial.js`). Web Serial can't set it.

### RAM placement

The HR_C7000's RAM is IRAM 0x10000-0x57FFF (288 KB). The 32 KB SRAM at 0x18000000 is shared with the modem, and the
stub doesn't use it. The chip has no general DMA controller (only the USB and SDIO blocks have their own, unused on
these radios), and the modem writes only its own SRAM, so after the stub masks every interrupt nothing else writes
IRAM.

| Region | Address | Size |
|---|---|---|
| Entry and copy loop (position independent, no stack) | Load address | 0x80 |
| Image (text, parameter block at +0x80, data) | 0x30000-0x31E68 after the copy | 7,784 B |
| .bss (receive buffer, sector buffers) | To 0x36C90 | 20,008 B |
| Stack (4 KB, painted) | 0x36C90-0x37C90 | 4,096 B |
| Whole footprint | 0x30000-0x37C90 | 31,888 B (32 KB budget, checked by the linker) |

The stack use is 168 bytes on the DM-32UV and DM-1801 Pro, and 252 bytes on the DB25-D with NAND reads. Two
126-sector WRITE installs on the DM-32UV used 200 bytes.

The entry stops in place, writing nothing, when the loaded copy and 0x30000 would overlap. Before it sends the image,
the host checks the same rule and that the load lies in IRAM (`check_univ_load`).

| Way in | Load address | What is in RAM at the hand-over | Why 0x30000 is safe |
|---|---|---|---|
| Boot ROM, power-on (all three radios) | 0x10000 (28 blocks of 256 B) | Nothing: the ROM uses no RAM (registers only) | All IRAM is free |
| DB25-D first stage, power-on | 0x10000 | The first stage (accepts blocks below 0x4E000) | Nothing returns to it |
| Running RadioFoundry, DM-32UV, DM-1801 Pro | 0x17000 (span 96 KB) | RadioFoundry, about to be dropped | Nothing returns to RadioFoundry; the reset restarts it |
| Running RadioFoundry, DB25-D without DMR | 0x30000 (span 128 KB) | RadioFoundry | Loaded at the run address, no copy |
| Running RadioFoundry, DB25-D with DMR | 0x1D000 (span 72 KB, codec window) | RadioFoundry | The copy goes up to 0x30000, which doesn't overlap the source |
| Factory DB25-D firmware 018 or 020, console | 0x54000 (window 0x54000-0x58000) | The factory firmware | The window is above the RAM that firmware uses, and nothing else runs once the stub masks interrupts |

Every route reaches 0x30000 without moving anything still in use. The running RadioFoundry spans start at 0x17000,
0x1D000, or 0x30000, and all cover 0x30000-0x37978. The ROM and the first stage leave all IRAM free. Placing .bss and
stack at the run address, not after the load address, means no route has to assume free RAM above where it loaded the
stub.

On the factory firmware console route, the stub hands the program flash back from execute-in-place to the SSI and
masks the interrupt controller before it touches the flash. It leaves the clocks as they are. The host measures timer
2 and sends the rates in CONFIG, so the same `db25d` profile serves both routes (both use UART1 on the K1 jack).

### The first stage

On the DB25-D, 0xC000 holds RadioFoundry's [first stage](../boot/README.md) and the application starts at 0xD000.
`--write-app` writes at 0xC000, so the DB25-D takes the `_install.bin`, which starts with the stage.

The tool reads the radio's word at 0xC000 first. When the word is the stage's and the image doesn't start with the
stage, the tool refuses the write: the application alone there leaves the radio in its update loader (white screen,
red LED) until an IAP install. No board list is involved, so the check covers any radio with the stage, and a radio
whose stage is already gone takes either image. `--replace-stage` overrides the check.

`stub/db25d_update.sh IMAGE [PORT]` runs a whole DB25-D update.

### Older stubs

The position-dependent stubs stay embedded: one linked for the ROM load at 0x10000, one for RadioFoundry at 0x17000,
and the DB25-D's 0x30000 and 0x1D000 builds. `--legacy-stub` selects them, with `--load-base` where the tool can't
tell the base. `--stub FILE` and `--rfoundry-stub FILE` load an image built elsewhere.

The tool reads the addresses an image was linked with and refuses one that doesn't match before anything is sent,
because jumping into a stub linked for another base needs a power cycle. A stub older than version 6 (a HELLO shorter
than 19 bytes) gets no CONFIG.

## The STM32F4 stub

The STM32F4 stub installs builds (changed sectors only, keeping the codec), backs up and verifies the internal flash,
and reads and writes the external SPI flash. It also backs up and puts back the SPI flash's security registers
(`--secreg`, `--write-secreg`), never their one-time lock bits. Its protocol is in the
[STM32F4 stub spec](../docs/spec/stub-stm32f4.md).

One image serves every STM32F4 radio. After HELLO, the host sends a CONFIG with the radio's board record: the SPI
flash bus, pins, mode and chip select, the other chip selects to hold high, the crystal, and the bootloader's end. The
host checks the echo in the next HELLO, and the stub writes nothing before that. The profiles are `BOARDS` in
`f4stub.py`, built from the board registry (`board.toml` `[flash.stub]` and `pins.h`). The board comes from
RadioFoundry's VERSION `board=`, or from `--board` with `--attach`. `stubtool.py` hands every board with an `f4stub`
`[flash.stub]` to `f4stub.py` (`--rfoundry-enter`, or `--attach --board <board>`).

The stub is linked for the SRAM base. RadioFoundry stages it in an 8 KB buffer and copies it there, so
`--rfoundry-enter` checks it differently from the HR_C7000 stubs: it must start with the STM32F4 stub's entry code,
and the base RadioFoundry reports must be 0x20000000.

The IWDG can't be stopped once RadioFoundry starts it (`CONFIG_WATCHDOG`), so the stub feeds it at start-up, in every
wait loop (through `stub_ms()`, including the flash erase busy wait), at every request, and once per 4 KB block of a
long read or checksum.

The image has a 16-byte header at offset 0x10: magic, stub version 5, and flags with bit 0 "feeds the IWDG".
`stm32f4/f4stub_header.h` is the one definition the stub and RadioFoundry share, and the
[STM32F4 stub spec](../docs/spec/stub-stm32f4.md) 3.1 gives the layout. A watchdog build of RadioFoundry refuses ENTER
unless the header has the magic, a version of at least 3, and the flag. An older `f4stub.py` or web page then gets a
refusal before anything is written, never a reset part way through ([watchdog spec](../docs/spec/watchdog.md) 3.1).
`--rfoundry-enter` prints the version it read from the header.

## Protected regions

Both host tools refuse to touch a board's protected regions (`board.toml` `[[flash.protect]]`, see
[Board registry](../docs/dev/board-registry.md#protected-regions)). The stubs themselves don't know the list.

- `stubtool.py` checks every erase and program command it sends, and refuses a chip erase.
- `f4stub.py` checks installs, sector erases, and programs. It carves the regions out of an SPI flash restore
  (`--write-q128`), and refuses a security register write to a protected register.
