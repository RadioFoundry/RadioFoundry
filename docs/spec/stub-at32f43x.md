<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# AT32F43x flash stub

A small RAM monitor that installs, checks and backs up the internal flash of the AT32F43x radios behind a vendor
bootloader, one image for all of them. It is the AT32F43x counterpart of the GD32F30x stub
([stub-gd32f30x.md](stub-gd32f30x.md)) and speaks the same protocol through the same engine (`stub/common/stubeng.c`),
with the same 8-byte board record. A running RadioFoundry loads it through the debug link ([debuglink.md](debuglink.md)
3.18) and jumps to it with no power cycle. Which radios are on this platform: [feature-matrix.md](../feature-matrix.md).

| Part | Where |
|---|---|
| RadioFoundry side: staging in the frame buffer, quiesce, copy, jump | `core/stubentry/stub_entry.c`, `platform/at32f43x/stub_entry.c`, `hal_system_t.enter_stub_staged / stub_run_addr / stub_image_ok` |
| The stub | `stub/at32f43x/` (`make -C stub/at32f43x` writes `build/stub-at32f43x/at32stub.bin`) |
| The target's facts and the board record check (portable) | `stub/at32f43x/at32stub.h`, `at32stub_board.c`, `at32stub_header.h` |
| Host test | `tests/host/at32stubTest.c`: the sector map, the banks, the record, the image header (the other targets' images refused), the engine's rules at a bootloader end that is not on an STM32F4 sector |
| Host tool | `stub/f4stub.py` (through `stub/stubtool.py`) and the web flasher's `f4stub.js`, shared with the other stubs; each embeds or serves this image (section 8) |
| Host tool test | `tests/host/at32stubSim.c` with `f4stub_wire_check.py --at32`: the host tool against the real engine; `f4stub.py --self-test`; `site/tests/f4stub.test.mjs` |
| Switch | `CONFIG_STUB_ENTRY`, on by default (`config/features.mk`) |

Status (2026-10-31): built and host tested, host tools included. Nothing has run on a radio.

## 1. Differences from the GD32F30x stub

| | GD32F30x stub | AT32F43x stub |
|---|---|---|
| Name in HELLO, magic | `GDSTUB`, `0x47445342` ('GDSB') | `ATSTUB`, `0x41545342` ('ATSB') |
| Header magic | `0x48534447` ("GDSH") | `0x48535441` ("ATSH") |
| Flash controller | FMC, 2 KB pages in bank 0, 4 KB in bank 1 | 2 KB or 4 KB sectors by part, two banks on parts of 1 MB and more (section 2) |
| Watchdog fed | FWDGT | WDT (section 3.3) |
| USB core | USBD, its own packet memory | OTGFS1, a Synopsys DWC2 core like the STM32F4's OTG_FS (section 4) |
| DMA stopped at entry | DMA0, DMA1 | DMA1, DMA2 (7 channels each) |

The record, the 4 KB engine sectors, the 1 MB cut, the frame buffer staging and the entry registers are the GD32F30x
stub's.

## 2. Flash map

The AT32F435/437 flash controller erases a sector at a time and programs a word (or a halfword or byte) at a time
(AT32F435/437 reference manual, flash memory controller). Each bank has its own unlock, status, control and address
registers; bank 2's are 0x40 above bank 1's.

| Fact | Value | Source |
|---|---|---|
| Unlock | keys 0x45670123 then 0xCDEF89AB to UNLOCK (0x40023C04; bank 2 0x40023C44); OPLK (CTRL bit 7) set relocks | reference manual; a vendor image on this chip does the same |
| Sector erase | CTRL bit 1 (SECERS), the address to ADDR (+0x14), then CTRL bit 6 (ERSTR); wait while STS bit 0 (OBF) | reference manual; a vendor image |
| Program | CTRL bit 0 (FPRGM), then the write to the flash address; wait while OBF | reference manual; a vendor image (halfwords) |
| Errors | STS bit 2 (PRGMERR), bit 4 (EPPERR), written 1 to clear; bit 5 (ODF) done | reference manual; a vendor image reads bits 0, 2, 4 |
| Bank 2 on parts over 1 MB | from 0x08200000: bank 1 is the first 2 MB | a vendor image's flash library selects bank 2's registers for 0x08200000 to 0x083EFFFF |
| Bank 2 on 1 MB parts | from 0x08080000 (two 512 KB banks) | reference manual, not checked against an image |
| Sector size | 4 KB on parts over 1 MB, 2 KB on the others | reference manual, not checked on a part. A vendor image on a 4032 KB part erases in 2 KB steps |

The engine's view:

| | Value |
|---|---|
| Sector size | 4 KB everywhere |
| Erase of one engine sector | each 2 KB half that does not read blank gets a sector erase at its address, then must read blank. On a 4 KB part the first erase blanks both halves, so the second is skipped; on a 2 KB part each half is its own sector. Either way nothing outside the 4 KB is erased, and the stub needs no sector size fact |
| Sectors | the part's flash (the flash size register, 0x1FFFF7E0) cut to 1 MB: at most 256 |
| Why 1 MB | the ERASE request carries the sector as one byte. RadioFoundry's image and every vendor area a board's registry names lie below 1 MB |
| Program unit | 32-bit words; a request that crosses into bank 2 goes to each bank's registers in turn |
| Sector 0 | never writable: no record may name it as the application base (`AT32_MIN_APP_BASE`) |
| Bootloader | below the record's application base, never erased or programmed (engine rule) |
| User system data (option bytes) | never touched: the stub has no code that unlocks or writes it |

A bootloader whose end is on a 2 KB sector but not on a 4 KB boundary cannot be described; the record check refuses it.

## 3. Entry

The GD32F30x entry ([stub-gd32f30x.md](stub-gd32f30x.md) 3): the image is staged in the display's frame buffer (3.1
there), checked, copied to 0x20000000 and entered with the same registers (r0 bit 0 adopt the USB device, r1 the core
clock, r2 the soft-restart mark's address). `platform/at32f43x/stub_entry.c` stops the DMA channels of each controller
whose clock is on. RadioFoundry runs the OTGFS1 core without DMA, so its FIFOs need nothing.

The stub uses at most 32 KB of RAM from 0x20000000 and accepts a mark address only above its own stack and below the
end of the largest SRAM setting, so the kept RAM at the top of SRAM survives.

### 3.1 The watchdog

The WDT cannot be stopped once started. Before the copy RadioFoundry sets its slowest divider and longest reload
(about 26 s at the typical 40 kHz LICK, at least 17 s) so the copy and the jump can't trip it. The stub feeds it first
thing and then in every loop that takes time (each read of its millisecond clock). A reset the stub asks for writes the
soft mark first, so RadioFoundry reports it as a requested restart, not a watchdog reset.

### 3.2 Image header

16 bytes at offset 0x10, little endian: magic `0x48535441` ("ATSH"), layout 1, size 16, version, flags (bit 0: feeds
the WDT). The header is required whether or not the watchdog runs, as on the GD32F30x: the image is copied to a fixed
address, and only an image built for it and this chip may go there. The other stubs' images are refused.

## 4. USB in the stub

The stub polls the OTGFS1 core; it never enables an interrupt. The core is the STM32F4's OTG_FS design, so the driver
is the STM32F4 stub's ([stub-stm32f4.md](stub-stm32f4.md) 4) on this chip's addresses. Descriptors: CDC-ACM, bulk 0x01
/ 0x81, 64 bytes, notification 0x82, with RadioFoundry's serial number (the UID as hex), so the host keeps its port
name.

| Start | When | What |
|---|---|---|
| Adopt | r0 bit 0 set, the device has an address and the bulk pair and the notification endpoint are active | keeps the address and the configuration; flushes the TX FIFOs and rearms endpoint 0. HELLO reports it (flag 0x01) |
| Cold | anything else (USB off, network mode, an unexpected state) | PA11 / PA12 to function 10, the 48 MHz HICK to the USB with the ACC trimming it (as `platform/at32f43x/usb.c`), soft disconnect, core reset, forced device mode, transceiver on, VBUS ignored, the FIFOs sized, 100 ms detached, then enumerate again |

## 5. Protocol and the board record

The protocol and the 8-byte CONFIG record are the GD32F30x stub's ([stub-gd32f30x.md](stub-gd32f30x.md) 5): board id
(nonzero, echoed by HELLO), layout 1, two reserved zero bytes, the application base (the bootloader's end, on a 4 KB
sector, above sector 0), big endian.

PEEK reads the flash size and UID words (0x1FFFF7E0-0x1FFFF7F3), the user system data (0x1FFFC000-0x1FFFC03F) and the
first 128 KB of SRAM, which every EOPB0 setting leaves as SRAM. Everything else, the peripherals included, is refused.

## 6. Safety rules

| Area | Rule |
|---|---|
| Below the application base | never erased or programmed (engine) |
| Sector 0 | never writable (record check) |
| User system data | never written (no code for it) |
| Vendor areas above the application base | the host's protect list (`[[flash.protect]]`, `tools/rfoundry`), as for the other stubs |
| Kept RAM | above the stub's 32 KB; the mark address is checked |
| A failed erase | the engine reports it; the half that did not read blank is never programmed over |

## 7. Size

| | Bytes |
|---|---|
| Image (at most 8,192) | about 7,100 |
| RAM at run time (image, `.bss`, 4 KB stack) | about 28 KB of the 32 KB it may use |
| RadioFoundry side (`CONFIG_STUB_ENTRY`) | about 1.3 KB of flash and 64 bytes of RAM (the loader's state; the frame buffer is borrowed) |

## 8. Host tools

`stub/f4stub.py` drives every target of the engine, this one included. The target is a property of the chip, never of
a radio: an image names it by its header's magic (3.2), a running stub by HELLO's name (`ATSTUB`), a board by its
platform's stub (`platform.toml` `stubs`, `board_stub` with `tool = "f4stub"`). A board on this platform gets a profile
from its `board.toml` `[flash.stub]` (the record's board id) and its linker script (the application base). The tool
refuses an image, a running stub or a board of another target before it sends anything that writes. The hand-over, the
configure step, the install order and the protect list are the GD32F30x stub's ([stub-gd32f30x.md](stub-gd32f30x.md)
8); `make -C stub/at32f43x check` fails while the image `f4stub.py` embeds differs from a fresh build.

The web flasher's driver (`site/static/js/flasher/f4stub.js`) has the same target table and `data/f4-stub.json`
carries this image under `targets.at32f43x`, but no flasher route offers the stub on a radio of this platform yet.

## 9. What remains

| Item | Why it matters |
|---|---|
| Hardware | nothing has run on a radio: the hand-over, the adopted and the cold USB start, the erase and program |
| Sector size | the erase works with 2 KB or 4 KB sectors, but which one a part has is not confirmed on a part |
| The zero-wait area | the first part of the flash is read through a zero-wait copy in SRAM. That the controller keeps the copy in step with an erase or a program is taken from the manual and not checked: the stub reads back each erased half and the host verifies every write by CRC, so a stale copy would show as a failed install, not a silent one |
| The flash size and UID words | the stub reads them at 0x1FFFF7E0 and 0x1FFFF7E8, the platform's addresses, which no vendor image confirms. A wrong size only narrows the map (it is cut to 1 MB either way) |
| The vendor bootloader with an erased application | the interlock assumes a bootloader that finds the first application word erased stays in its update mode, as on the GD32F30x radios |
| A flasher route | the web flasher's update backends for these radios use the vendor update mode only |

The switch is on: the code is written. What is untested is recorded in each radio's hardware page.
