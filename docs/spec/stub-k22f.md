<!-- SPDX-License-Identifier: MIT -->

# K22F flash stub

A small RAM monitor for the Kinetis K22F radios behind their own loader: today the GD-77. It installs, checks and
backs up the internal flash, and reads and writes the external SPI NOR. A running ODR loads it through the debug link
([debuglink.md](debuglink.md) 3.18) and jumps to it with no power cycle. It is the STM32F4 stub's sibling
([stub-stm32f4.md](stub-stm32f4.md)): the same wire protocol, the same protocol engine, the same host tool class, the
same install order. This page covers only what differs.

Status (2026-10-21): built and host tested. **Nothing here has run on a radio.** Every K22F register fact in the
stub and the entry code is unverified.

| Part | Where |
|---|---|
| ODR side: staging buffer, quiesce, USB park, copy, jump | `platform/k22f/stub_entry.c`, `stub_handoff.h`; `hal_system_t.stub_region / enter_stub / stub_run_addr` in `platform/k22f/system.c` |
| The stub | `stub/k22f/` (`make -C stub/k22f` writes `build/stub-k22f/k22stub.bin`) |
| Shared with the STM32F4 stub | `stub/common/`: `stubeng` (protocol engine), `stubrt` (runtime, main loop, reset), `stubcdc` (CDC-ACM descriptors and control requests), `stubnor` (SPI NOR commands) |
| The K22F target's facts and board record check (portable) | `stub/k22f/k22stub.h`, `k22stub_board.c`; host test `tests/host/k22stubTest.c` |
| Host tool and the board profile | `stub/k22stub.py` (`BOARDS`), on top of `stub/f4stub.py`; called by `stub/stubtool.py`; `tests/host/k22stub_wire_check.py` runs it against the real engine and checks the profile against the GD-77's sources |
| Switch | `CONFIG_STUB_ENTRY` (default off; builds on for the GD-77) |

## 1. Chip facts

| Item | Value |
|---|---|
| Flash | 512 KB at 0 (`SIM_FCFG1` PFSIZE), 2 KB sectors, 8-byte program units (FTFA "program longword" twice per unit) |
| Flash commands | FTFA erase sector (0x09), program longword (0x06), run from the stub's RAM with no flash access, 300 ms / 5 ms time-outs; refused in HSRUN; the flash cache invalidated after each |
| Never written | sector 0 always (the engine's `min_app_base` 0x800: vector table and the flash configuration field at 0x400); on the GD-77 also everything below 0x4000 and the last sector 0x7F800 (board record) |
| Run address | 0x1FFF0000, SRAM_L's base; stack top 0x20000000; image at most 8 KB, stack 4 KB (both checked by the linker script) |
| Clock | what ODR left, in RUN; r2 carries the core clock in Hz for the cycle counter timing |
| Float | soft float: the stub has none, and ODR clears the FPU context flag before the jump |
| HELLO | name `K2STUB`, protocol 3, magic `K2SB` 0x4B325342; byte 11 is `FTFA_FSEC`; flags bit 0: USB adopted |
| Serial number | ODR's: the low 96 bits of the unique ID (UIDMH, UIDML, UIDL) as 24 hex digits |
| PEEK | SRAM 0x1FFF0000..0x2000FFFF, the SIM's ID registers 0x40048024..0x40048063 (one word at a time), the FTFA registers 0x40020000..0x40020013, the internal flash. Never the USB controller (its status registers change when read) |

## 2. Entry

`k22f_enter_stub` (ODR side), in order:

1. Check the entry lies in the run span and the staging buffer (8 KB in ODR's `.bss`, SRAM_L) lies above the run
   address, so a forward word copy is safe.
2. `board_stub_prepare` (weak): the transmitter off. The power hold and the display stay as they are.
3. Step the clock down from HSRUN to RUN (`k22f_clock_step_down`): the FTFA runs no command in HSRUN.
4. Interrupts off, SysTick off, every NVIC source disabled and cleared.
5. eDMA: no new requests (`DMA_ERQ` = 0), then wait while a channel is active. I2S0 transmit and receive off. DMAMUX
   channels 0-15 off. Each only if its clock gate is on.
6. USB, if its clock gate is on: park the controller (below), then decide the hand-over flags.
7. Copy the staging buffer to 0x1FFF0000, invalidate the flash cache, `CONTROL` = 0, jump with r0, r1, r2.

| Register | Meaning |
|---|---|
| r0 | bit 0: adopt the USB device (ODR's device was configured in serial mode) |
| r1 | USB data toggles: bit 0 IN 0x81 next DATA1, bit 1 IN 0x82, bit 2 OUT 0x01; bit 31 valid |
| r2 | the core clock, Hz |

### 2.1 Parking the USB controller

The controller writes received packets wherever its buffer descriptor table points, and ODR's table and buffers are in
the SRAM_L the copy overwrites. Before the copy, ODR points `BDTPAGE1-3` at a zeroed 512-byte table in SRAM_U
(`ODR_CPU_RAM_SECTION`). The CPU owns every descriptor in it, so the controller answers each token with NAK and the
host retries. ODR then waits 1 ms for a packet already in flight to finish.

The toggles come from the USB driver's state, read from the old table before the copy (`k22f_stub_toggles`). The
driver keeps 8 bytes per endpoint and direction 512 bytes after the table: the packet size in bits 0-10 and the next
half (even or odd) in bit 16. Each descriptor's DATA1 bit is fixed when the endpoint opens, so the next toggle is the
DATA1 bit of the next half's descriptor. The layout is checked (packet sizes 64, 64, 64, 8); anything else, or a
token completed but not handled, leaves bit 31 clear. In network mode r0 is 0: the host bound CDC-NCM, so the stub must
enumerate as a serial port again.

## 3. USB in the stub

A polled driver for the full-speed controller (`stub/k22f/k22stub_usb.c`), written from the reference manual. It
keeps its own table and buffers, double-buffered on endpoint 1. Descriptors and control requests are the shared
`stubcdc`: the same IDs (1FC9:0094), product "ODR flash stub", the same endpoints as on the STM32F4 stub.

| Start | When | What the host sees |
|---|---|---|
| Adopt | r0 bit 0 set and the controller still has an address | nothing: the same port keeps working. `CTL.ODDRST` puts every endpoint to its even half; the address and endpoint registers are kept |
| Enumerate again | otherwise | the port disappears briefly and comes back with the same IDs and serial number |

Toggles at adoption: with r1 bit 31 set, the stub carries on with ODR's. Without it, IN starts at DATA0 (if wrong,
the host drops one reply and the tool asks again) and OUT takes either toggle (DTS off). A bus reset at any time
restarts enumeration. The address from SET_ADDRESS is applied after the status stage.

## 4. The board record

The same CONFIG op as on the STM32F4 stub, with a K22F layout of 24 bytes (`stub/k22f/k22stub.h`). All u32 big
endian. The stub writes nothing before a record is accepted; the host checks the echo in the next HELLO.

| Bytes | Field | Checked |
|---|---|---|
| 0 | board id (GD-77 1) | not 0 |
| 1 | layout, 1 | equal |
| 2 | SPI flash: 0 none, 1 NOR on GPIO pins | at most 1 |
| 3 | chip select mode: 0 driven high when idle, 1 released to its pull-up | at most 1 |
| 4-7 | application base, the commit word | a sector start, at or above 0x800 (the engine) |
| 8-11 | application end: no ERASE or PROGRAM at or above it (0: the flash end) | a sector start above the base, inside the flash (the engine) |
| 12-15 | SPI write floor: no XERASE or XPROGRAM below it | a 4 KB multiple |
| 16-19 | SCK, MOSI, MISO, CS pins: port × 32 + pin (port A = 0 to E = 4) | ports A-E, all four different (only with an SPI flash) |
| 20-23 | 0 | equal |

| Radio | Record (hex) | Source |
|---|---|---|
| GD-77 | `01 01 01 01 00004000 0007F800 00100000 85 84 86 13 00000000` | [hardware-gd77.md](hardware-gd77.md) 6.1, 6.2 |

The GD-77's NOR is mode 0 on PTE5 (SCK), PTE4 (MOSI), PTE6 (MISO), chip select PTA19, which is released to its
pull-up when idle (the pin is also XTAL0). Its first megabyte is the radio's own data: the floor keeps it read only.
The self-tests and host tests use a test profile with the same layout and id 0xFE (`k22stub.test_board()`).
`app_end` and `xfloor` are engine fields; the STM32F4 stub leaves them 0 (no limit).

## 5. Host side

`stub/k22stub.py` defines `K22Chip` (a `f4stub.Chip`) and `K22`, passed as `chip=` to `f4stub.F4Radio`, `run`,
`load_segments` and `board_from_version`. `stubtool.py` routes a board in `k22stub.BOARDS` to it, found from ODR's
VERSION `board=` with `--odr-enter`, or `--board gd77` with `--attach`.

| Profile key (`BOARDS["gd77"]`) | Value |
|---|---|
| `owned` | 0x4000..0x55D00: what a plain build replaces |
| `codec`, `codec_image_end` | 0x55D00..0x7B800; a merged image ends at 0x7B780 (the descriptor's end) |
| `app_end` | 0x7F800 |
| `spi`, `spi_floor` | the pins above; 0x100000 |

The install order is the STM32F4 stub's ([stub-stm32f4.md](stub-stm32f4.md) 7): the commit sector at 0x4000 first,
its first 8-byte unit last. Sector 171 (0x55800) holds both the application's end and the codec's start; the tool reads
it back, saves it to the backup directory and keeps the codec bytes. `spi_write` refuses a file that differs from the
radio below the floor before anything is sent.

For a web flasher: `k22stub.APP_BASE`, `BOARDS`, `K22STUB_SHA256`, `stub_image()` and `config_record(board_id)` mirror
`f4stub`'s names.

## 6. Open points

- Every K22F fact: run on a GD-77 with the debug link and a backup of the whole flash first.
- Whether the radio's loader checks anything at 0x4000 besides the stack pointer and reset vector.
- The toggle hand-over against TinyUSB's driver on the radio.
- The FTFA time-outs with the clock ODR leaves.

Sources: Kinetis K22F reference manual (FTFA, USB OTG, SIM, eDMA, DMAMUX, I2S chapters), the K22F data sheet (flash
timing), USB 2.0 chapter 9, USB CDC 1.2, [hardware-gd77.md](hardware-gd77.md) 6, 13 and 15.1.
