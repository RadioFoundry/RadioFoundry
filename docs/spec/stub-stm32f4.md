<!-- SPDX-License-Identifier: MIT -->

# STM32F4 flash stub

A small RAM monitor that installs, checks and backs up the internal flash of the STM32F405 radios behind the
AnyRoad / TYT bootloader: the DM-1701 first, then the MD-380. A running ODR loads it through the debug link
([debuglink.md](debuglink.md) 3.18) and jumps to it with no power cycle. It does for the STM32 radios what the HR_C7000
stub does for the DM-32UV and the DM-1801 Pro ([hardware-dm32.md](hardware-dm32.md) 17.2, `stub/README.md`).

| Part | Where |
|---|---|
| ODR side: staging buffer, quiesce, copy, jump | `platform/stm32f4/stub_entry.c`, `hal_system_t.stub_region / enter_stub / stub_run_addr`, `board_rf_safe` in `boards/dm1701/board.c` and `boards/md380/board.c` |
| The stub | `stub/stm32f4/` (`make -C stub/stm32f4` writes `build/stub-stm32f4/f4stub.bin`) |
| Its protocol engine (portable, host tested) | `stub/stm32f4/f4stub_proto.c`, `tests/host/f4stubTest.c` |
| Host tool | `stub/f4stub.py`, called by `stub/stubtool.py`; `tests/host/f4stub_wire_check.py` runs it against the real engine |
| Switch | `CONFIG_STUB_ENTRY`, on by default with the debug link; off on the DM-1701 (RAM, its `config.mk`) |

Status (2026-10-16): built, host tested and run on the DM-1701: section 11 has the hardware test and its results.

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
   Connected" is drawn with the backlight at full (the generic entry code in `core/debuglink/debuglink_stub.c`, so a
   dimmed or switched-off backlight does not hide it), and `enter_stub` runs in the host link task:

| Step | Why |
|---|---|
| Note whether USB is in serial mode (`usb_device_mode`) | the handoff flag; in network mode the host bound CDC-NCM to the same endpoint numbers |
| `board_rf_safe` ([crash-log.md](crash-log.md) 8): transmitter off (DM-1701 PA_EN1, PA_EN2, PA_BAND, APC DAC; MD-380 APC_EN, TX_5V, VCO_SEL, APC DAC) | nothing transmits while the stub runs. The backlight (already at full for the banner), power latch and USB pins stay |
| Interrupts masked, SysTick off, every NVIC enable and pending bit cleared | the stub polls; ODR's handlers live in flash it may erase |
| DMA1 and DMA2 reset | circular I2S, SPI and ADC streams write into SRAM, where the stub is about to run |
| Copy the staging buffer to 0x20000000 (8 KB, a forward word copy) | the stub is linked there. The copy runs from flash and its stack is the task stack in CCM, so it overwrites nothing it uses. The buffer lies at or above the target, so a forward copy is safe even if they overlap |
| CONTROL = 0, r0 = flags, branch to the entry | thread mode on the main stack. The stub sets SP to the top of SRAM first |

Clocks stay as ODR set them (72 MHz, USB 48 MHz; the reduced clock is never active while USB is configured,
[hardware-dm1701.md](hardware-dm1701.md) 1.3). GPIO levels stay, so the power latch holds. No watchdog runs (the option
bytes select the software watchdog and ODR does not start it).

Handoff flags in r0:

| Bit | Meaning |
|---|---|
| 0 | USB is ODR's configured CDC-ACM serial port: adopt it |

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
arriving for 200 ms is dropped. The largest payload is 4104 bytes (`F4STUB_MAX_PAYLOAD`).

| Op | Name | Request | Reply |
|---|---|---|---|
| `01` | HELLO | none | `F4STUB`, version u8 (2), max payload u16, flash KB u16, RDP byte u8, flags u8 (bit 0: USB adopted), application base u32 (0x0800C000), SPI flash size u32 (0: no chip answered), its JEDEC ID (3 bytes). Version 1 stopped after the application base |
| `02` | PEEK | addr u32, n u16 (1 to max) | n bytes. Allowed: flash, 0x1FFF0000-0x1FFF7A2F (system memory, OTP, UID, flash size), option bytes 0x1FFFC000-0x1FFFC00F, SRAM, CCM. Peripherals are refused |
| `07` | SUMS | addr u32 (4 KB aligned), count u16 (1 to 820), memory u8 (optional: 0 internal flash, the default; 1 SPI flash, addresses from 0) | count x (CRC-32 u32, blank u8) |
| `09` | READ | addr u32 (4 KB aligned), count u8 (1 to 255), compress u8, memory u8 (optional, as SUMS) | count frames, one per 4 KB block: CRC-32 u32, method u8 (0 raw, 2 blank), length u16, data. Compression is not offered; a compress request is answered raw |
| `0D` | RESET | magic u32 | OK, then a system reset once the reply has left |
| `10` | ERASE | sector u8, magic u32 | OK once erased |
| `11` | PROGRAM | addr u32 (word aligned), 4 to 4096 bytes (whole words) | OK once programmed and read back |
| `12` | XERASE | addr u32 (a 4 KB SPI flash sector), magic u32 | OK once erased |
| `13` | XPROGRAM | addr u32, 1 to 4096 bytes inside one 4 KB SPI flash sector | OK once programmed and read back |

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
| Transport | | USB CDC-ACM instead of a UART at 115200 |

`stubtool.Radio` holds the shared parts (frames, retries, the debug link loader, the READ record decoder) and
`f4stub.F4Radio` subclasses it. The web flasher splits the same way: `f4stub.js` extends `stub.js`, and the flasher
updates and backs up a radio running ODR with stub entry through it ([website.md](../dev/website.md)).

## 6. Safety rules

The stub enforces these, so a host bug can't break them:

| Rule | Effect |
|---|---|
| Sectors 0-2 (0x08000000-0x0800BFFF) are never erased or programmed | the bootloader always survives, on both radios |
| Nothing writes FLASH_OPTCR or its key | RDP and write protection never change |
| The interlock: ERASE of any sector but 3, and PROGRAM of anything outside sector 3, need the word at 0x0800C000 to read 0xFFFFFFFF | while any application sector is half written, the application looks invalid to the bootloader (section 7) |
| PROGRAM only goes to words that read erased; nothing is written if one doesn't | no silent AND with old data |
| Every programmed word is read back | a weak cell shows up as status `10` |
| ERASE and RESET carry the magic | a stray frame can't erase or reset |
| Caches and prefetch are off | reads after an erase or program see the flash, not a stale cache line |
| XERASE carries the magic; XPROGRAM only goes to bytes that read erased, inside one 4 KB sector, and every byte is read back | the same rules as the internal flash. The SPI flash holds no code, so there is no interlock: what goes there (a backup put back, a codeplug) is the host's choice |

### 6.1 The SPI flash

Both radios wire their W25Q128 the same way: SPI1 on PB3 (SCK), PB4 (MISO), PB5 (MOSI), alternate function 5, chip
select PD7, mode 0 ([hardware-dm1701.md](hardware-dm1701.md) 2, [hardware-md380.md](hardware-md380.md) 12). That is the
one wiring fact the stub carries. It sets the bus up from scratch at the slowest prescaler at or under 4.5 MHz (ODR's
rate), wakes the chip (0xAB), reads its JEDEC ID (0x9F) and takes the size from the ID's capacity byte, up to 16 MB
(3 address bytes). Reads use 0x03, erases 0x20 (4 KB), programs 0x02 in pages, each behind write enable (0x06) with
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

The host knows which flash the firmware owns (`f4stub.BOARDS`, from `tools/odrflash/odrflash/boards.py`'s maps):

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
| Stub image | 6,764 bytes (limit 8 KB, asserted by `f4stub.ld`) |
| Stub RAM after the jump | about 26 KB of SRAM (image, buffers, 8 KB receive ring) plus the stack |
| ODR with `CONFIG_STUB_ENTRY=y`, DM-1701 | the STUB state machine, the entry and the banner text, all in sectors 3-9 (720 KB); +8 KB of SRAM (the staging buffer): RAM 92.7 % used |
| ODR with `CONFIG_STUB_ENTRY=y`, MD-380 | the same code, in an application area of 464 KB (the default image is 320,236 bytes), RAM 78.8 %. The switch fits with no other switch off |
| ODR with the switch off | no change: the same image, byte for byte in size |

The switch stays off by default: the command hands the whole SoC to whatever image the host sends, guarded by the
magic, the length and the CRC-32. Turning it on for releases needs a signed or hash-pinned stub first (section 12).

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
| 2 | `python3 stub/stubtool.py PORT --odr-enter --peek 0x08000000:64` | "STM32F4 stub v1: 1024 KB flash, RDP byte 0x55, USB adopted from ODR" (or "enumerated again"), then 64 bytes; the screen shows "PC Loader Connected" |
| 3 | the same output | the bytes are the bootloader's vector table (not all 00 or FF): CPU reads from SRAM work at RDP 1 |
| 4 | `python3 stub/stubtool.py PORT --attach --board dm1701 --progmem ~/dm1701_flash.bin` | 1 MB, sha256 printed |
| 5 | `python3 stub/stubtool.py PORT --attach --board dm1701 --verify-progmem ~/dm1701_flash.bin` | identical |
| 6 | `python3 stub/stubtool.py PORT --attach --board dm1701 --write-app build/dm1701/odr_dm1701.bin` | "0 sector(s) written", every sector unchanged (the build already in the radio) |
| 7 | rebuild with a visible change (another `ODR_VERSION=`), then step 6 again | sectors 3 and the changed ones written, verified |
| 8 | `python3 stub/stubtool.py PORT --attach --board dm1701 --boot reset` | ODR starts; `odr_debug.py version` shows the new `git=` |
| 9 | `python3 stub/stubtool.py PORT --odr-enter --write-app ...` with yet another build, USB unplugged during the write, then a power cycle | the radio comes up in the bootloader with no keys held; odrflash restores it |
| 10 | `odr_debug.py radio usb network`, then `python3 stub/stubtool.py udp:RADIO_IP --odr-enter --peek 0x08000000:64` | the hand-over goes over UDP; the stub comes up as a serial port (`--stub-port`, default the one 1FC9:0094 port) and answers |
| 11 | `python3 stub/stubtool.py PORT --odr-enter --q128 ~/spi.bin`, then `--attach --verify-q128 ~/spi.bin` | "SPI flash 16384 KB (JEDEC …)" in the HELLO line; 16 MB read, identical |
| 12 | write a pattern to a blank 4 KB sector with `--write-q128 pat.bin --q128-range ADDR:ADDR+0x1000`, read it back, write 0xFF back the same way, then `--verify-q128 ~/spi.bin` | 1 sector written each time; the pattern reads back; the whole chip matches the first read; ODR starts with its codeplug |

Steps 4 and 5 write the dump outside the repository. Step 9 needs odrflash and the stock file at hand. The HIL suite
can gain a test for steps 2 and 3 (opt-in, reads only).

Results on the bench DM-1701 (2026-10-16, RDP level 1): steps 1 to 8 and 10 to 12 pass. The stub reads the bootloader's
vector table from SRAM (so code running from RAM reads flash at RDP 1), a 1 MB read takes 2 s and matches the image
flashed through the bootloader, an unchanged image writes 0 sectors, a changed one writes 5 sectors in 8 s, and the
codec sectors stay (the radio still finds its codec). Step 9 is still to do.

## 12. Open points

- Step 9 of section 11 (an interrupted write and the recovery) on hardware.
- MD-380: the bootloader's stack pointer check, and the stub on a real MD-380.
- odrflash: use the stub when the radio runs a `CONFIG_STUB_ENTRY` build, with the bootloader as the fallback.
- Release use: pin the stub image (the radio could refuse a stub whose SHA-256 is not built in), then turn the switch on
  by default.

Sources: STM32F405/407 reference manual (embedded flash interface: unlock, sector erase, programming parallelism,
read protection levels; OTG_FS device programming model), STM32F405 data sheet (flash erase and program times), USB 2.0
chapter 9, USB CDC 1.2 and PSTN 1.2, [hardware-dm1701.md](hardware-dm1701.md) 1.4, 15 and 16,
[hardware-md380.md](hardware-md380.md) 1.3.
