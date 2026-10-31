<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# GD32F30x flash stub

A small RAM monitor that installs, checks and backs up the internal flash of the GD32F30x radios behind a vendor
bootloader, one image for all of them. It is the GD32F30x counterpart of the STM32F4 stub
([stub-stm32f4.md](stub-stm32f4.md)) and speaks the same protocol through the same engine (`stub/common/stubeng.c`). A
running RadioFoundry loads it through the debug link ([debuglink.md](debuglink.md) 3.18) and jumps to it with no power
cycle. Which radios are on this platform: [feature-matrix.md](../feature-matrix.md).

| Part | Where |
|---|---|
| RadioFoundry side: staging in the frame buffer, quiesce, copy, jump | `core/stubentry/stub_entry.c`, `platform/gd32f30x/stub_entry.c`, `hal_system_t.enter_stub_staged / stub_run_addr / stub_image_ok` |
| The stub | `stub/gd32f30x/` (`make -C stub/gd32f30x` writes `build/stub-gd32f30x/gd32stub.bin`) |
| The target's facts and the board record check (portable) | `stub/gd32f30x/gd32stub.h`, `gd32stub_board.c`, `gd32stub_header.h` |
| Host test | `tests/host/gd32stubTest.c`: the sector map, the record, the image header, the engine's rules at a bootloader end that is not on an STM32F4 sector |
| Host tool | `stub/f4stub.py` (through `stub/stubtool.py`) and the web flasher's `f4stub.js`, shared with the STM32F4 stub; each embeds or serves this image (section 8) |
| Host tool test | `tests/host/gd32stubSim.c` with `f4stub_wire_check.py --gd32`: the host tool against the real engine; `f4stub.py --self-test`; `site/tests/f4stub.test.mjs` |
| Switch | `CONFIG_STUB_ENTRY`, on by default (`config/features.mk`) |

Status (2026-10-31): built and host tested, host tools included. Nothing has run on a radio.

## 1. Differences from the STM32F4 stub

| | STM32F4 stub | GD32F30x stub |
|---|---|---|
| Name in HELLO, magic | `F4STUB`, its own | `GDSTUB`, `0x47445342` ('GDSB') |
| Sectors the engine sees | the chip's sectors (16 to 128 KB) | 4 KB logical sectors over the first 1 MB (section 2) |
| Program unit | 4 or 8 bytes (the engine's rule) | 32-bit words, the FMC's unit |
| SPI flash, security registers | yes, from the board record | none: the record holds only the bootloader's end (section 5) |
| Watchdog fed | IWDG | FWDGT (section 3.3) |
| USB core | OTG_FS | USBD, the full-speed device core with its own packet memory (section 4) |
| Staging on the RadioFoundry side | a static 8 KB buffer | the display's frame buffer (section 3.1) |

## 2. Flash map

The GD32F30x FMC has two banks. Bank 0 is the first 512 KB with 2 KB pages; bank 1 follows on larger parts with 4 KB
pages (GD32F30x user manual, FMC). Each bank has its own key, status, control and address registers.

| Engine's view | Value |
|---|---|
| Sector size | 4 KB everywhere: two 2 KB pages in bank 0, one page in bank 1 |
| Sectors | the part's flash (the flash size register) cut to 1 MB: at most 256 |
| Why 1 MB | the ERASE request carries the sector as one byte. RadioFoundry's image and every vendor area a board's registry names lie below 1 MB |
| Sector 0 | never writable: no record may name it as the application base (`GD32_MIN_APP_BASE`) |
| Bootloader | below the record's application base, never erased or programmed (engine rule) |
| Option bytes | never touched: the stub has no code that unlocks or writes them |

A bootloader whose end is on a 2 KB page but not on a 4 KB boundary cannot be described; the record check refuses it.
Program writes split at the bank boundary, so one request may span both banks.

## 3. Entry

The STM32F4 entry sequence ([stub-stm32f4.md](stub-stm32f4.md) 3) with three changes.

### 3.1 Staging in the frame buffer

These radios have no 8 KB of RAM to spare. A platform that sets `hal_system_t.enter_stub_staged` (and leaves
`stub_region` and `enter_stub` NULL) has the core stage the image in the display's frame buffer instead:

| Step | What happens |
|---|---|
| Each STUB request | `gfx_hold` for 5 s from now. The first one waits up to 200 ms for the UI task to finish a frame, then draws the banner. The panel keeps showing it from its own memory while the image overwrites the buffer |
| Load | into the frame buffer (`GFX_FB_BYTES`, at least the 8 KB image); no display, no stub |
| ENTER | the image's CRC and header are checked; the core calls `enter_stub_staged(image, length, entry)` |
| Host stops part way | the hold runs out and the UI draws again. The image is then damaged and ENTER's CRC check refuses it |

### 3.2 Copy and jump

`platform/gd32f30x/stub_entry.c`:

1. Check the image fits 8 KB, the entry lies in it, the source lies above the run address (a forward word copy is then
   safe) and the calling task's stack lies above the copy.
2. Check the image header (3.4).
3. `board_rf_safe()`, then stretch the watchdog (3.3).
4. Mask interrupts, stop SysTick, disable and clear every NVIC source, stop every DMA channel.
5. Copy the image to 0x20000000, its link address. RadioFoundry's `.data` and `.bss` are gone from here on; code runs
   from flash and the stack is above the copy.
6. CONTROL = 0 (main stack, no FPU context), then jump with the entry registers:

| Register | Value |
|---|---|
| r0 | flags: bit 0 = the USB core is RadioFoundry's configured CDC-ACM device (adopt it) |
| r1 | the core clock in Hz (the stub times by the cycle counter, so it needs no crystal fact) |
| r2 | where RESET writes the soft-restart mark (RadioFoundry's restart-cause word in the kept RAM, [crash-log.md](crash-log.md) 1.3), 0 for none |

The stub uses at most 32 KB of RAM from 0x20000000 and accepts a mark address only above its own stack, so the kept
RAM at the top of SRAM survives.

### 3.3 The watchdog

The FWDGT cannot be stopped once started. Before the copy RadioFoundry sets its slowest prescaler and longest reload
(about 26 s) so the copy and the jump can't trip it. The stub feeds it first thing and then in every loop that takes
time (each read of its millisecond clock). A reset the stub asks for writes the soft mark first, so RadioFoundry reports
it as a requested restart, not a watchdog reset.

### 3.4 Image header

16 bytes at offset 0x10, little endian: magic `0x48534447` ("GDSH"), layout 1, size 16, version, flags (bit 0: feeds
the FWDGT). Unlike the STM32F4 rule the header is required whether or not the watchdog runs: the image is copied to a
fixed address, and only an image built for it and this chip may go there. The STM32F4 stub's image is refused.

## 4. USB in the stub

The stub polls the USBD core; it never enables an interrupt. Its descriptors are the STM32F4 stub's (CDC-ACM, bulk
0x01 / 0x81, 64 bytes, notification 0x82) with RadioFoundry's serial number (the UID as hex), so the host keeps its port
name.

| Start | When | What |
|---|---|---|
| Adopt | r0 bit 0 set and the endpoint registers hold the expected bulk pair and interrupt endpoint, single buffered | keeps the device address and the configuration; takes the buffer addresses from the BTABLE. HELLO reports it (flag 0x01) |
| Cold | anything else (USB off, network mode, an unexpected layout) | USBD clock off, D+ driven low 100 ms to make the host see a detach, clock on, core reset, enumerate again |

The D+ pull-up is left as RadioFoundry had it: the stub drives no board pin, so it needs no board fact.

## 5. Protocol and the board record

The protocol is the shared engine's ([stub-stm32f4.md](stub-stm32f4.md) 5 and 13): HELLO, PEEK, CRC, READ, ERASE,
PROGRAM, CONFIG, RESET, with the interlock on the commit sector (the application's first sector is erased first and its
first word programmed last).

The CONFIG record is 8 bytes, big endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | board id, nonzero (echoed by HELLO) |
| 1 | 1 | layout, 1 |
| 2 | 2 | reserved, 0 |
| 4 | 4 | application base: the first writable address, the bootloader's end, on a 4 KB sector, above sector 0 |

PEEK reads the chip's device information (flash size, UID), the option bytes and SRAM.

## 6. Safety rules

| Area | Rule |
|---|---|
| Below the application base | never erased or programmed (engine) |
| Sector 0 | never writable (record check) |
| Option bytes | never written (no code for it) |
| Vendor areas above the application base | the host's protect list (`[[flash.protect]]`, `tools/rfoundry`), as for the other stubs |
| Kept RAM | above the stub's 32 KB; the mark address is checked |

## 7. Size

| | Bytes |
|---|---|
| Image (at most 8,192) | about 7,200 |
| RAM at run time (image, `.bss`, 4 KB stack) | about 22 KB of the 32 KB it may use |
| RadioFoundry side (`CONFIG_STUB_ENTRY`) | about 1.5 KB of flash and 64 bytes of RAM (the loader's state; the frame buffer is borrowed) |

## 8. Host tools

`stub/f4stub.py` drives every target of the engine (the AT32F43x stub, [stub-at32f43x.md](stub-at32f43x.md), is
another). The target is a property of the chip, never of a radio:

| What | Says which target by |
|---|---|
| An image | its header's magic (section 3.4) |
| A running stub | HELLO's name, `GDSTUB` |
| A board | its platform's stub (`platform.toml` `stubs`, `board_stub` with `tool = "f4stub"`) |

A board on this platform gets a profile from its `board.toml` `[flash.stub]` (the record's board id) and its linker
script (the application base, the record's last field). The tool refuses an image, a running stub or a board of the
other target before it sends anything that writes.

| Step | What the tool does |
|---|---|
| Hand-over | sends the image `f4stub.py` embeds (`make -C stub/gd32f30x check` fails while it differs from a fresh build), enters it at its run address (section 3.2), checks the stack top its entry code loads |
| Configure | sends the 8-byte record (section 5) and checks the echo in the next HELLO |
| Install | erases the sector at the application base first, then every other changed sector the firmware owns, programs the commit sector except its first word, verifies all by CRC, then programs the first word |
| Protect list | every erase and program is checked against the board's `[[flash.protect]]` before it is sent |

The flash after the application (a board's codec slot, if any) is written only from an image merged with it, as on
the STM32F4 radios. The web flasher's driver (`site/static/js/flasher/f4stub.js`) has the same target table and
`data/f4-stub.json` carries each target's image (`targets.<target>`), but no flasher route offers the stub on a radio of
this platform yet.

## 9. What remains

| Item | Why it matters |
|---|---|
| The vendor bootloader with an erased application | the interlock assumes a bootloader that finds the first application word erased stays in update mode. Not yet confirmed on these bootloaders; until it is, an install cut short may leave a radio that needs the bootloader's own update entry |
| Hardware | nothing has run on a radio: the adopt and cold USB starts, the FMC erase and program, the entry itself |
| A flasher route | the web flasher's update backends for these radios use the vendor update mode only |

The switch is on: the code is written. What is untested is recorded in each radio's bring-up page.
