<!-- SPDX-License-Identifier: MIT -->

# STM32F4 flash stub

A small RAM monitor that installs, checks and backs up the internal flash of the STM32F405 radios behind the
AnyRoad / TYT bootloader, one image for all of them. A running ODR loads it
through the debug link ([debuglink.md](debuglink.md) 3.18) and jumps to it with no power cycle. The host then sends
the radio's board record (section 5.2), and only then can the stub write. It does for the STM32 radios what the
HR_C7000 stub does for the DM-32UV and the DM-1801 Pro ([hardware-dm32.md](hardware-dm32.md) 17.2, `stub/README.md`).

| Part | Where |
|---|---|
| ODR side: staging buffer, quiesce, copy, jump | `platform/stm32f4/stub_entry.c`, `hal_system_t.stub_region / enter_stub / stub_run_addr`, `board_rf_safe` in each board's `board.c` |
| The stub | `stub/stm32f4/` (`make -C stub/stm32f4` writes `build/stub-stm32f4/f4stub.bin`) |
| Its protocol engine (portable, shared with other stubs, host tested) | `stub/common/stubeng.c` (section 13), `tests/host/f4stubTest.c` |
| The STM32F4 target's facts and board record check (portable) | `stub/stm32f4/f4stub.h`, `f4stub_board.c` |
| Host tool | `stub/f4stub.py`, called by `stub/stubtool.py`; `tests/host/f4stub_wire_check.py` runs it against the real engine |
| The board profiles | each board's `board.toml` `[flash.stub]` with the SPI flash pins of its `pins.h` (`PIN_FLASH_SCK`, `_MISO`, `_MOSI`, `_CS`), its crystal and image address from `board.mk`; `tools/boards.py` derives the profile and checks it ([board-registry.md](../dev/board-registry.md)), `stub/f4stub.py` `BOARDS` reads it |
| Switch | `CONFIG_STUB_ENTRY`, on by default on every radio including the DM-1701, with or without the debug link ([stub-entry.md](stub-entry.md)) |

Status (2026-10-22): built, host tested and run on the DM-1701 (section 11, with protocol 2). Protocol 4 (the board
record) is host tested only (`tested = false` in each board's `[flash.stub]`).

## 1. Why

The bootloader route ([hardware-dm1701.md](hardware-dm1701.md) 15) costs three things:

| Cost | With the stub |
|---|---|
| The user holds PTT + side key 1 while switching on | not needed for an update: ODR enters the stub on request |
| The image goes XOR-keyed (the key comes from the bootloader sectors or the stock file) | not needed: the stub writes plain bytes |
| The bootloader reads no flash, so a host can't back up or verify | the stub reads all of internal flash, CRC-checks it and compares it with a file |

## 2. Feasibility

| Question | Answer |
|---|---|
| Can code in SRAM erase and program the internal flash? | Yes. The flash interface takes sector erase and word program from any bus master once unlocked (the FLASH_KEYR key pair). Code fetched from SRAM keeps running while the flash is busy; only a flash access stalls. The stub runs entirely from SRAM, so it services USB during a 2 s erase |
| Does read protection level 1 stop it? | No. Level 1 locks flash out only while a debugger is connected, or when the chip booted from SRAM or system memory (the BOOT pins). After a normal boot from user flash, reads, erases and programs from user code are allowed, wherever that code runs. ODR enters the stub from a normal boot. Bench check: section 11, step 3 |
| Sector size | 16 KB (0-3), 64 KB (4), 128 KB (5-11). Erase is per sector, so an update rewrites whole sectors. The stub CRCs 4 KB blocks so the host can tell which sectors changed |
| Erase and program time | 128 KB: about 1 s, at most 2 s; 16 KB: at most 0.5 s; one word: 16 us, at most 100 us (data sheet, x32 parallelism, 2.7 to 3.6 V). An update erases only the sectors the image covers (DM-1701: at most 3-9), about 1 s per 128 KB sector |
| USB | The stub carries its own polled CDC-ACM device on the OTG_FS core (section 4). It adopts the device ODR already enumerated when it can |
| RAM | ODR's 128 KB SRAM and 64 KB CCM are nearly full (DM-1701: 86 % and 97 %). ODR keeps an 8 KB staging buffer for the image (switch on only). After the jump the stub owns all of SRAM |

### 2.1 Options compared

| Option | For | Against | Verdict |
|---|---|---|---|
| A. A separate stub with its own minimal USB device | Independent of ODR's state once entered; reads and writes every application sector, sector 3 included; the same model as the HR_C7000 stub, so the host tools share framing and flow | Needs its own USB code (about 2 KB); entry reaches the stub only while ODR runs | **Chosen** |
| B. The stub keeps using ODR's USB stack (TinyUSB) | No USB code in the stub | TinyUSB runs from flash, and the stub erases flash. Its state lives in SRAM the stub overwrites | Not possible as such. Option A takes the useful part: it adopts the configured device (address and endpoints) without re-enumerating |
| C. ODR writes flash itself (in-app updater, RAM-resident routine) | No second program to load | ODR runs from the sectors an update rewrites (DM-1701: 3-9), so it can't erase them while it runs. It would have to stage the image elsewhere (the external SPI flash has room: `hardware-dm1701.md` 13.2) and copy it with a RAM routine that has no USB. That copier is the stub without the host link, with less checking (no verify after the copy) | Rejected. A later "stage over the network, copy at reboot" path could reuse the stub's engine |

## 3. Entry

1. The host sends STUB INFO, BEGIN, DATA and ENTER ([debuglink.md](debuglink.md) 3.18). INFO reports the run address
   0x20000000 and the 8 KB span. The data goes into ODR's staging buffer (`stub_region`), which lies anywhere in ODR's
   `.bss`.
2. ENTER is refused with busy (3) while the radio transmits. Otherwise the reply goes out, the banner "PC Loader
   Connected" is drawn with the backlight at full (the generic entry code in `core/stubentry/stub_entry.c`, so a
   dimmed or switched-off backlight does not hide it), and `enter_stub` runs in the host link task:

| Step | Why |
|---|---|
| Note whether USB is in serial mode (`usb_device_mode`) | the handoff flag; in network mode the host bound CDC-NCM to the same endpoint numbers |
| `board_rf_safe` ([crash-log.md](crash-log.md) 8): transmitter off (DM-1701 PA_EN1, PA_EN2, PA_BAND, APC DAC; MD-380 APC_EN, TX_5V, VCO_SEL, APC DAC) | nothing transmits while the stub runs. The backlight (already at full for the banner), power latch and USB pins stay |
| `stm32_stub_image_ok` (`CONFIG_WATCHDOG`, also checked at ENTER): with the IWDG running, the image header (3.1) must say the stub feeds the IWDG, or ODR refuses it | a stub that doesn't feed the IWDG would be reset part way through a write |
| `stm32_watchdog_stretch` (`CONFIG_WATCHDOG`): the IWDG to its longest timeout (prescaler 256, reload 4095: about 32 s, at least 22 s), then a feed | the IWDG can't be stopped; the stub feeds it from here on ([watchdog.md](watchdog.md) 3.1) |
| Interrupts masked, SysTick off, every NVIC enable and pending bit cleared | the stub polls; ODR's handlers live in flash it may erase |
| DMA1 and DMA2 reset | circular I2S, SPI and ADC streams write into SRAM, where the stub is about to run |
| Copy the staging buffer to 0x20000000 (8 KB, a forward word copy) | the stub is linked there. The copy runs from flash and its stack is the task stack in CCM, so it overwrites nothing it uses. The buffer lies at or above the target, so a forward copy is safe even if they overlap |
| CONTROL = 0, r0 = flags, branch to the entry | thread mode on the main stack. The stub sets SP to the top of SRAM first |

Clocks stay as ODR set them (72 MHz, USB 48 MHz on every STM32F4 radio; the reduced clock is never active while USB is configured,
[hardware-dm1701.md](hardware-dm1701.md) 1.3). GPIO levels stay, so the power latch holds. The option bytes select the software
watchdog, so the IWDG runs only when ODR started it (`CONFIG_WATCHDOG`). Once started only a reset stops it, so the
stub feeds it: first thing in `stub_main`, in `stub_ms()`, which every wait loop reads (the main loop, the flash and
SPI busy waits, the USB waits, the delays), and from the protocol engine at every request, before each erase or
program and once per 4 KB block of SUMS and READ (`stubeng_port_t.feed`). A feed does nothing while the IWDG isn't
running. The image header (3.1) says so; ODR with its IWDG running refuses a stub whose header doesn't
([watchdog.md](watchdog.md) 3.1).

Handoff flags in r0:

| Bit | Meaning |
|---|---|
| 0 | USB is ODR's configured CDC-ACM serial port: adopt it |

### 3.1 Image header

16 bytes at offset 0x10 of the image (0x20000010 once copied), right after the 12-byte entry code; the linker script
puts section `.stub_header` there and fails the build if the entry code grows past it. Little endian. One definition,
`stub/stm32f4/f4stub_header.h`, shared by the stub and ODR (`platform/stm32f4/stub_entry.c`); `f4stub.py` and the web
flasher (`f4stub.js`) read the same layout, and their self-tests check their numbers against that file.

| Offset | Size | Field | Value |
|---|---|---|---|
| 0x10 | 4 | magic | `0x48533446` (bytes `F4SH`) |
| 0x14 | 2 | layout | 1; a later layout only appends fields |
| 0x16 | 2 | size | 16 |
| 0x18 | 4 | stub version | 4 (`F4STUB_VERSION`, also in HELLO) |
| 0x1C | 4 | flags | bit 0: feeds the IWDG (`F4STUB_FLAG_FEEDS_IWDG`) |

Versions: 1 the first stub, 2 the SPI flash, 3 the header and the IWDG feeds (`F4STUB_VERSION_FEEDS_IWDG`), 4 the
shared engine with CONFIG and the board record (section 5). Stubs
before version 3 have no header: those bytes are code. While its IWDG runs, ODR enters a stub only when all three hold:
the magic, a version of at least 3 and the feed flag (`f4stub_image_ok`). Otherwise ENTER is refused with reason 1
([debuglink.md](debuglink.md) 3.18). With the IWDG not running any stub is entered. `stubtool.py --odr-enter` and the
web flasher print the version from the header before they send the image.

## 4. USB in the stub

A CDC-ACM device on the OTG_FS core in slave mode (no DMA, no interrupt line): bulk OUT 0x01 and IN 0x81 (64 bytes),
interrupt IN 0x82 (8 bytes), VID 0x1FC9, PID 0x0094, the serial number ODR uses (the 96-bit UID in hex, lowest byte
first), product "ODR flash stub".

| Start | When | What the host sees |
|---|---|---|
| Adopt | flag bit 0 set, the device has an address, and endpoints 1 IN and OUT are active | nothing: the same port keeps working. An IN transfer TinyUSB left open is cut off and the FIFOs flushed |
| Enumerate again | otherwise | the port disappears for about 0.1 s and comes back with the same IDs and serial number (on macOS the same `/dev/cu.usbmodem…` name) |

After a fresh enumeration Linux may give the port the next number (`/dev/ttyACM1`) while it still holds the old name.
`stubtool.py` waits up to 20 s for the stub. It tries the port name it was given for the first 10 s (a `/dev/serial/by-path/…`
name stays valid across this). After that it also tries a port with the stub's USB ID, taken only from the same USB
location as the port ODR was on, or, if the location is unknown, the one port with ODR's serial number or USB ID. Two
candidates are never guessed between: it stops and says so.

The stub handles a bus reset at any time, so a host that resets the port gets a fresh enumeration. FIFO RAM in cold
start: RX 128 words, EP0 TX 16, EP1 TX 64, EP2 TX 16. A transfer that ends on a full 64-byte packet gets a
zero-length packet after it. HELLO's flags byte says which start happened.

None of this is a board fact. OTG_FS on PA11 / PA12 is the STM32F405's only full speed port, and ODR has set its
pins up before the jump. Every STM32F4 radio runs it with VBUS sensing off and gives ODR's serial number from the UID, so the stub carries no USB settings per radio.

Web Serial note: after a re-enumeration the page has to reopen the port. Chrome keeps the permission for the same
USB IDs and serial number, so `navigator.serial.getPorts()` returns it without a new prompt.

## 5. Protocol

The HR_C7000 stub's framing, unchanged, so the host tools share it:

| Direction | Bytes |
|---|---|
| request | `02` op, length u16, sum u8, `03`, payload |
| reply | `02` status, length u16, sum u8, `03`, payload |

All numbers are big endian (as in the HR_C7000 stub; the debug link is little endian). The sum is the low byte of the
payload's byte sum. USB has its own CRC per packet; every flash block also carries a CRC-32. A request that stops
arriving for 200 ms is dropped. The largest payload is 4104 bytes (`STUBENG_MAX_PAYLOAD`).

| Op | Name | Request | Reply |
|---|---|---|---|
| `01` | HELLO | none | `F4STUB`, version u8 (4), max payload u16, flash KB u16, RDP byte u8, flags u8 (bit 0: USB adopted, bit 7: configured), application base u32 (from the board record; 0 before one), SPI flash size u32 (0: none configured or no chip answered), its JEDEC ID (3 bytes), board id u8 (0 before a record), record length u8, the record (the echo, section 5.2). Versions 2 and 3 stopped after the JEDEC ID and always reported 0x0800C000; version 1 stopped after the application base |
| `02` | PEEK | addr u32, n u16 (1 to max) | n bytes. Allowed: flash, 0x1FFF0000-0x1FFF7A2F (system memory, OTP, UID, flash size), option bytes 0x1FFFC000-0x1FFFC00F, SRAM, CCM. Peripherals are refused |
| `07` | SUMS | addr u32 (4 KB aligned), count u16 (1 to 820), memory u8 (optional: 0 internal flash, the default; 1 SPI flash, addresses from 0) | count x (CRC-32 u32, blank u8) |
| `09` | READ | addr u32 (4 KB aligned), count u8 (1 to 255), compress u8, memory u8 (optional, as SUMS) | count frames, one per 4 KB block: CRC-32 u32, method u8 (0 raw, 2 blank), length u16, data. Compression is not offered; a compress request is answered raw |
| `0D` | RESET | magic u32 | OK, then a system reset once the reply has left. It first writes ODR's restart-cause mark (`F4STUB_SOFT_MARK_ADDR`, [crash-log.md](crash-log.md) 1.3), so ODR reports a software restart |
| `10` | ERASE | sector u8, magic u32 | OK once erased |
| `11` | PROGRAM | addr u32 (word aligned), 4 to 4096 bytes (whole words) | OK once programmed and read back |
| `12` | XERASE | addr u32 (a 4 KB SPI flash sector), magic u32 | OK once erased |
| `13` | XPROGRAM | addr u32, 1 to 4096 bytes inside one 4 KB SPI flash sector | OK once programmed and read back |
| `14` | CONFIG | the board record (24 bytes, section 5.2) | OK once checked and applied (SPI bus set up, chip woken). A refused record answers `0D` and leaves the stub unconfigured, even if an earlier record was in force |

The magic is `0x46345342` ("F4SB").

| Status | Meaning |
|---|---|
| `05` | OK |
| `0A` | bad request sum: send again |
| `0B` | unknown op |
| `0D` | bad argument: length, alignment or range |
| `0E` | payload longer than the maximum |
| `0F` | refused by a safety rule (section 6) |
| `10` | flash error: the controller flagged one, the read-back differs, or the SPI flash stayed busy |
| `11` | not configured: ERASE, PROGRAM, XERASE and XPROGRAM before a board record was accepted |

### 5.1 Shared with the HR_C7000 stub, and what differs

| Item | Same | Different |
|---|---|---|
| Framing, status OK / BADSUM / BADOP / TOOLONG | yes | |
| HELLO | the 6-byte magic, version, max payload at the same offsets | magic `F4STUB` instead of `C7STUB`; bytes 9-16 hold flash size, RDP, flags and the application base |
| PEEK | layout | refuses peripherals |
| SUMS, READ | the reply records, 4 KB units | the request is a flash address, not an SPI controller, chip select and offset |
| Writes | | the HR_C7000 stub passes SPI commands through (write enable, sector erase, page program); this one has ERASE and PROGRAM for the internal flash, with the rules in section 6 |
| Baud rate, LED, CALL | | not present (USB) |
| The SPI flash | | a memory byte on SUMS and READ, and XERASE / XPROGRAM, instead of the HR_C7000 stub's SPI pass-through. The stub drives the chip itself |
| Entry | the debug link STUB command and its magic `C7SB` | the platform copies the image to its run address; INFO reports the run address |
| Board configuration | a CONFIG after HELLO, echoed by the next HELLO; no writes before it | op `14` (the HR_C7000's `0D` is RESET here), and the record holds the SPI flash wiring and the bootloader's end instead of clocks and power pins |
| Transport | | USB CDC-ACM instead of a UART at 115200 |

### 5.2 The board record

The stub image is the same for every radio; what differs goes in a 24-byte record the host sends with CONFIG (layout
in `stub/stm32f4/f4stub.h`, the values from the board registry, read into `stub/f4stub.py` `BOARDS`). The host picks the profile from the running ODR's
VERSION `board=` (or `--board`), sends it, reads HELLO again and checks that the echo is byte for byte what it sent and
that the application base is 0x0800C000. Only then does it write.

| Bytes | Field | Checked by the stub |
|---|---|---|
| 0 | board id (`[flash.stub]` `id`, unique among the STM32F4 boards) | not 0 |
| 1 | layout, 1 | equal |
| 2 | SPI flash bus: 0 none, 1-3 SPI1-SPI3 | at most 3 |
| 3 | SPI mode, CPOL << 1 \| CPHA | at most 3 |
| 4-7 | crystal (HSE) Hz | 4 to 26 MHz |
| 8-11 | application base: the end of the bootloader, the commit word | a sector start, not sector 0, at or above 0x08004000 (the engine) |
| 12-15 | fastest SPI clock, Hz | at least 100 kHz |
| 16-18 | SCK, MISO, MOSI pins: port << 4 \| pin (port A = 0 to I = 8) | ports A-I |
| 19 | their alternate function | at most 15 |
| 20 | flash chip select pin | ports A-I, none of the other pins |
| 21-22 | other chip selects on the bus, held high (0xFF: none) | ports A-I or 0xFF |
| 23 | 0 | equal |

Where each field comes from:

| Field | Source |
|---|---|
| board id | `[flash.stub]` `id` |
| bus, mode, fastest clock | `[flash.stub]` `spi = { bus, mode, hz }` |
| SCK, MISO, MOSI, chip select | `pins.h` `PIN_FLASH_SCK`, `PIN_FLASH_MISO`, `PIN_FLASH_MOSI`, `PIN_FLASH_CS` |
| alternate function | the bus: `platform/stm32f4/platform.toml` `board_stub.spi_af` (AF5 for SPI1 and SPI2, AF6 for SPI3) |
| held high | `[flash.stub]` `idle_cs`: `pins.h` names, at most two |
| crystal | `board.mk` `HSE_VALUE` |
| application base | `board.mk` `APP_BASE` |

`tools/boards.py get <board> flash.stub` prints a board's values; the radio's hardware page says where they come
from. The crystal only sets the stub's own timing (time-outs, the 200 ms idle rule): the stub reads the clock tree ODR
left and needs the HSE to turn it into Hz. Until a record arrives it assumes 8 MHz.

The record matters most on a radio whose flash is not on the SPI1 wiring a protocol 2 stub had built in
(`stub/f4stub.py` `V2_WIRING`): there those pins may reach another chip, and a stub with that wiring built in would
send flash commands to it. A stub before protocol 4 serves only a board whose record matches `V2_WIRING`.

`tools/boards.py check` holds each profile to the board's sources: every pin it names must be in `pins.h` in the
`GPIOx, GPIO_PIN_n` form, the bus must have an alternate function, and the crystal must be set.

#### Why the host sends the record

The other way considered: ODR passes a pointer to a board record in a register at the jump, as it passes the USB flag.

| | Host CONFIG (chosen) | Record from ODR at the hand-over |
|---|---|---|
| Entry routes | every one: ODR's hand-over, `--attach` to a running stub, any later route that has no ODR behind it (a bootloader or ROM loader, as the K22F radios may need) | only ODR's hand-over |
| Where the facts live | one place on the host, next to the flash maps the host already keeps per radio (`BOARDS`), checked against the board sources by a host test | in every board's firmware; a fix needs a new ODR build in the radio first |
| Radios in the field | an ODR build already running with `CONFIG_STUB_ENTRY` serves a new stub unchanged | each needs an ODR update before the new stub can serve it |
| Lifetime | the record is in the stub's own RAM | ODR's RAM and flash are what the stub overwrites and erases; the record would have to be copied before the first write |
| Mistakes | the host checks the echo before any write; a wrong or missing record means no writes, not a wrong write | the stub would trust whatever the register held |

The HR_C7000 stub made the same choice.

`stubtool.Radio` holds the shared parts (frames, retries, the debug link loader, the READ record decoder) and
`f4stub.F4Radio` subclasses it. The web flasher splits the same way: `f4stub.js` extends `stub.js`, and the flasher
updates and backs up a radio running ODR with stub entry through it ([website.md](../dev/website.md)). It sends the
records `config_record()` makes, carried in the site's `data/flash-boards.json`, and builds none itself.

## 6. Safety rules

The stub enforces these, so a host bug can't break them:

| Rule | Effect |
|---|---|
| Nothing is erased or programmed before a board record is accepted (status `11`) | a host that skipped CONFIG, or sent a record the stub refused, writes nothing |
| Nothing below the record's application base is erased or programmed; no record can put that base in sector 0, below 0x08004000 or off a sector start | sectors 0-2 (0x08000000-0x0800BFFF), the bootloader, always survive on every radio, whose `APP_BASE` is 0x0800C000 |
| Nothing writes FLASH_OPTCR or its key | RDP and write protection never change |
| The interlock: ERASE of any sector but the one holding the application base (sector 3), and PROGRAM of anything outside it, need the word at the application base (0x0800C000) to read 0xFFFFFFFF | while any application sector is half written, the application looks invalid to the bootloader (section 7) |
| PROGRAM only goes to words that read erased; nothing is written if one doesn't | no silent AND with old data |
| Every programmed word is read back | a weak cell shows up as status `10` |
| ERASE and RESET carry the magic | a stray frame can't erase or reset |
| Caches and prefetch are off | reads after an erase or program see the flash, not a stale cache line |
| XERASE carries the magic; XPROGRAM only goes to bytes that read erased, inside one 4 KB sector, and every byte is read back | the same rules as the internal flash. The SPI flash holds no code, so there is no interlock: what goes there (a backup put back, a codeplug) is the host's choice |

### 6.1 The SPI flash

The wiring comes from the board record (section 5.2). At CONFIG the stub drives the chip select and the held-high
pins high as outputs, puts SCK, MISO and MOSI on their alternate function, and sets the SPI block up from scratch
(master, the record's mode, DMA requests off) at the slowest prescaler at or under the record's rate, from the APB
clock of that block (APB2 for SPI1, APB1 for SPI2 and SPI3). It wakes the chip (0xAB), reads its JEDEC ID (0x9F),
takes the size from the ID's capacity byte, up to 16 MB (3 address bytes), and waits while the chip reports busy, in
case ODR handed over during an erase. Reads use 0x03, erases 0x20 (4 KB), programs 0x02 in pages, each behind write enable (0x06) with
the status register (0x05) polled. A chip that doesn't answer gives size 0, and every SPI request is then refused.

On the bench DM-1701 (2026-10-16): JEDEC `ef4018`, the whole 16 MB read in 59 s (about 280 KB/s) and verified, and a
pattern written to a blank sector, read back and erased again, after which the whole chip matched the first read.

## 7. Update order and recovery

The DM-1701 bootloader stays in DFU mode when the application's initial stack pointer at 0x0800C000 fails
`(SP & 0x2FFE0000) == 0x20000000`; an erased word does
([hardware-dm1701.md](hardware-dm1701.md) 15.4). The host uses that as the commit flag:

1. HELLO; the board from the radio's VERSION reply before the entry.
2. SUMS over every sector that holds firmware-owned flash; plan the new content (section 8). Stop if nothing changed.
3. Save any sector that also holds bytes the firmware doesn't own (the MD-380's sector 6) to a file outside git.
4. ERASE sector 3. From here on the bootloader stays in DFU at every power-on.
5. ERASE and PROGRAM every other changed sector (4 KB pieces, blank pieces skipped).
6. PROGRAM sector 3 from 0x0800C004.
7. SUMS over everything written, with the first word expected erased.
8. PROGRAM the first word (the commit). SUMS of its block.
9. RESET, or leave it to a power cycle.

| Interrupted at | Next power-on | Recovery |
|---|---|---|
| before step 4 | ODR as before | none needed |
| steps 4 to 8 | the bootloader in DFU, with no keys held | `odrflash flash` (it needs the key: from the stock file, since the radio can't run ODR to read it from the bootloader) |
| after step 8 | the new ODR | none needed |

The bootloader (sectors 0-2) is never touched, so the bootloader route always stays.

MD-380: whether the TYT bootloader checks the stack pointer the same way is **unverified**. If it does not, an
interrupted update makes the radio jump to an erased vector. The power-on key combination still reaches DFU, so
recovery is the same with the keys held.

## 8. The codec and the other bytes in a sector

The host knows which flash the firmware owns (`f4stub.BOARDS`, from the board registry: the linker script's application region):

| Radio | Owned | Sectors touched by an update | Codec |
|---|---|---|---|
| DM-1701 | 0x0800C000-0x080BFFFF (sectors 3-9) | those of 3-9 the image covers | sectors 10 and 11: written only from an image merged with the codec, otherwise never erased, so the installed codec stays (odrflash's keep mode) |
| MD-380 | 0x0800C000-0x0807FFFF (sectors 3-7) | those of 3-7 the image covers | sectors 8-11: written only from an image merged with the codec, otherwise never erased |

Inside an owned range, bytes past the image end become 0xFF, so an older, longer build leaves nothing behind. An image
merged with the codec (`odr_<board>_codec.bin`) runs to the end of flash; the tool splits it into the application and
the codec sectors and writes both whole ([codec-placement.md](codec-placement.md), "STM32 radios"). Any other image
that reaches outside the owned range (the wrong board) is refused before anything is erased.

No radio shares a sector between the firmware and the codec. The rule for one that would stays in the code: a sector
that is rewritten and holds bytes the firmware doesn't own is read back and written again with those bytes unchanged.

The stub only runs on a radio that already runs ODR. A first install, on a radio still on stock firmware, goes through
the bootloader with odrflash's merge mode.

## 9. Backups and read protection

| Case | What the stub gives |
|---|---|
| A radio running ODR with `CONFIG_STUB_ENTRY` | a full 1 MB internal flash image (bootloader, ODR, codec) and the whole external SPI flash (16 MB: codeplug, calibration), every block CRC checked, with no button press. The SPI flash can be written back the same way |
| A stock radio | nothing yet. It can't run our code without the bootloader, and the bootloader reads no flash. A first install still goes through the bootloader, blind |
| After the first install | everything above. The bootloader and the codec that came with the radio can be saved before any later update |

So the stub buys updates and post-install backups. It does not buy a pre-install backup of a stock radio. A dump
holds the proprietary bootloader and codec: the tool refuses to write one inside a git work tree, and dumps are
never committed.

## 10. Size and cost

| Item | Size |
|---|---|
| Stub image | 7,968 bytes (limit 8 KB, ODR's staging buffer, asserted by `f4stub.ld`) |
| Stub RAM after the jump | 28.6 KB of SRAM (image, engine buffers, 8 KB receive ring), then the stack: at least 4 KB is left for it (asserted by `f4stub.ld`); the deepest call chain needs under 1 KB (`-fstack-usage`) |
| ODR with `CONFIG_STUB_ENTRY=y`, DM-1701 | the STUB state machine, the entry and the banner text, all in sectors 3-9 (720 KB); +8 KB of SRAM (the staging buffer): RAM 95.4 % used, the default build |
| ODR with `CONFIG_STUB_ENTRY=y`, MD-380 | the same code, in an application area of 464 KB (the default image is 320,236 bytes), RAM 78.8 %. The switch fits with no other switch off |
| ODR with the switch off | no change: the same image, byte for byte in size |

The switch is on by default. The command hands the whole SoC to whatever image the host sends, guarded by the magic,
the length and the CRC-32; a signed or hash-pinned stub would close that (section 12).

## 11. First hardware test (DM-1701)

Owner-run. Nothing below transmits. Build, then flash once through the bootloader:

```
make BOARD=dm1701 CONFIG_STUB_ENTRY=y -j8
make -C stub/stm32f4
python3 tools/release.py --board dm1701 --version v0.0.0-stub --out build/dist
(radio in the bootloader: PTT + side key 1 at power-on)
cd tools/odrflash && python3 -m odrflash flash ../../build/dist/odr-dm1701-v0.0.0-stub.bin --yes   (key or --stock as usual)
```

Then, with ODR running (PORT is its USB serial port, `/dev/cu.usbmodem…` on macOS):

| Step | Command | Pass |
|---|---|---|
| 1 | `python3 tools/odr_debug.py version` | `board=DM-1701`, `git=` the build just flashed |
| 2 | `python3 stub/stubtool.py PORT --odr-enter --peek 0x08000000:64` | "STM32F4 stub v4: 1024 KB flash, RDP byte 0x55, not configured yet, USB adopted from ODR" (or "enumerated again"), "stub configured for the Baofeng DM-1701: SPI flash 16384 KB (JEDEC ef4018)", then 64 bytes; the screen shows "PC Loader Connected" |
| 3 | the same output | the bytes are the bootloader's vector table (not all 00 or FF): CPU reads from SRAM work at RDP 1 |
| 4 | `python3 stub/stubtool.py PORT --attach --board dm1701 --progmem ~/dm1701_flash.bin` | 1 MB, sha256 printed |
| 5 | `python3 stub/stubtool.py PORT --attach --board dm1701 --verify-progmem ~/dm1701_flash.bin` | identical |
| 6 | `python3 stub/stubtool.py PORT --attach --board dm1701 --write-app build/dm1701/odr_dm1701.bin` | "0 sector(s) written", every sector unchanged (the build already in the radio) |
| 7 | rebuild with a visible change (another `ODR_VERSION=`), then step 6 again | sectors 3 and the changed ones written, verified |
| 8 | `python3 stub/stubtool.py PORT --attach --board dm1701 --boot reset` | ODR starts; `odr_debug.py version` shows the new `git=` |
| 9 | `python3 stub/stubtool.py PORT --odr-enter --write-app ...` with yet another build, USB unplugged during the write, then a power cycle | the radio comes up in the bootloader with no keys held; odrflash restores it |
| 10 | `odr_debug.py radio usb network`, then `python3 stub/stubtool.py udp:RADIO_IP --odr-enter --peek 0x08000000:64` | the hand-over goes over UDP; the stub comes up as a serial port (`--stub-port`, default the one 1FC9:0094 port) and answers |
| 11 | `python3 stub/stubtool.py PORT --odr-enter --q128 ~/spi.bin`, then `--attach --verify-q128 ~/spi.bin` | "SPI flash 16384 KB (JEDEC …)" in the "configured" line; 16 MB read, identical |
| 12 | write a pattern to a blank 4 KB sector with `--write-q128 pat.bin --q128-range ADDR:ADDR+0x1000`, read it back, write 0xFF back the same way, then `--verify-q128 ~/spi.bin` | 1 sector written each time; the pattern reads back; the whole chip matches the first read; ODR starts with its codeplug |

Steps 4 and 5 write the dump outside the repository. Step 9 needs odrflash and the stock file at hand. The HIL suite
can gain a test for steps 2 and 3 (opt-in, reads only).

Results on the bench DM-1701 (2026-10-16, RDP level 1): steps 1 to 8 and 10 to 12 pass. The stub reads the bootloader's
vector table from SRAM (so code running from RAM reads flash at RDP 1), a 1 MB read takes 2 s and matches the image
flashed through the bootloader, an unchanged image writes 0 sectors, a changed one writes 5 sectors in 8 s, and the
codec sectors stay (the radio still finds its codec). Step 9 is still to do.

## 12. Open points

- Step 9 of section 11 (an interrupted write and the recovery) on hardware.
- Protocol 4 (the board record) on hardware: section 11 again on the DM-1701.
- MD-380: the bootloader's stack pointer check, and the stub on a real MD-380.
- odrflash: use the stub when the radio runs a `CONFIG_STUB_ENTRY` build, with the bootloader as the fallback.
- Release use: pin the stub image (the radio could refuse a stub whose SHA-256 is not built in), then turn the switch on
  by default.

## 13. The shared engine

The protocol engine (`stub/common/stubeng.h`, `stubeng.c`) knows no chip. It holds the framing, every op, the safety
rules of section 6, CRC-32 SUMS and block READ, and the board record's copy for the HELLO echo. A target supplies a
`stubeng_port_t` and its glue: the STM32F4 target is `stub/stm32f4` (USB, flash controller, SPI, entry); a Kinetis
K22F target for the GD-77 family would be another directory with the same shape. The wire protocol (section 5) is the
same for every target; only the HELLO name, the op magic and the board record's layout are the target's.

| `stubeng_port_t` field | What the target gives | STM32F4 |
|---|---|---|
| `name`, `version`, `magic` | HELLO's first 6 bytes, the protocol version, the u32 ERASE / XERASE / RESET carry | `F4STUB`, 3, 0x46345342 |
| `flash` (`stubeng_flash_t`) | internal flash: `base`, `size` (sectors that don't fit wholly don't exist), `runs` (the sector map as runs of equal sectors), `prog_unit` (PROGRAM's alignment and length unit: 4 or 8 bytes), `min_app_base` (no record opens anything below it) | 0x08000000, the flash size register, 4 x 16 KB + 64 KB + 7 x 128 KB, 4, 0x08004000 |
| `peek`, `npeek` | address ranges PEEK may read besides the internal flash; never peripherals | system memory and OTP, option bytes, SRAM, CCM |
| `security`, `flags` | HELLO bytes 11 and 12 (the target's bits below 0x80; the engine adds bit 7, configured) | RDP byte; USB adopted |
| `write` | send one reply frame | USB bulk IN |
| `read` | copy memory the engine has checked | `memcpy` |
| `erase`, `program` | erase one sector by index; program words at an address the engine has checked erased (aligned to `prog_unit`) | the flash controller, x32 |
| `reset` | after the RESET reply | `NVIC_SystemReset` |
| `xread`, `xerase`, `xprogram` | the external SPI flash, or NULL: read, erase one 4 KB sector, program inside one sector | `f4stub_spi.c` |
| `configure` | check and apply the board record (1 to 32 bytes, first byte the nonzero board id); fill `stubeng_config_t`: `app_base`, and the SPI flash's `xsize` and `xid` | `f4_board_parse`, then the clock and `f4spi_init` |

The engine checks the application base `configure` returns: a sector start, not sector 0, at or above
`min_app_base`. The commit word is at that base, and the commit sector (the interlock's) is the sector holding it. The
engine keeps 2 x 4 KB frame buffers and a 4 KB word buffer (`stubeng_t`, about 12.3 KB of RAM) and needs no heap. It
calls nothing but the port, `memcpy`, `memmove`, `memset` and `memcmp`.

A new target needs: the port, a receive loop that feeds bytes with `stubeng_feed` and calls `stubeng_idle` after
200 ms of silence, its board record layout and check (host tested, as `f4stub_board.c`), its host-side profiles, and
the host tests: `tests/host/f4stubTest.c` already runs the engine with a uniform 2 KB sector map and 8-byte program
units.

Sources: STM32F405/407 reference manual (embedded flash interface: unlock, sector erase, programming parallelism,
read protection levels; OTG_FS device programming model), STM32F405 data sheet (flash erase and program times), USB 2.0
chapter 9, USB CDC 1.2 and PSTN 1.2, [hardware-dm1701.md](hardware-dm1701.md) 1.4, 15 and 16,
[hardware-md380.md](hardware-md380.md) 1.3.
