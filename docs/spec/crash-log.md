<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# Crash log

`CONFIG_CRASH_LOG` (default on). A crash keeps a record of where and why it happened in memory a reset doesn't
clear, and restarts the radio. The next start takes the record up, so a crash in the field leaves something to read
instead of a frozen radio. Code: `core/crash/`, `hal/hal_crash.h`, the platforms' `crash_*.c`. Off: a crash stops the
radio until a power cycle, as before.

## 1. What is caught

| Kind | `kind` | From | `cause` | `pc` |
|---|---|---|---|---|
| Fault | 1 | a CPU exception: the platform's fault entry (2) | the exception number (CM4: 3 HardFault; CK803: the vector, 1 misaligned access, 2 access error, 3 divide by zero, 4 illegal instruction, 7 breakpoint, 8 unrecoverable error) | the faulting instruction |
| Stack overflow | 2 | FreeRTOS's stack check (`configCHECK_FOR_STACK_OVERFLOW` 2), at a context switch | 0 | in the kernel; `task` names the task |
| Out of heap | 3 | FreeRTOS's malloc-failed hook | 0 | in the kernel |
| Assert | 4 | `configASSERT` (the boards' `FreeRTOSConfig.h`) | the line | the caller of the assertion |

The capture runs with interrupts off, on a stack of its own (1 KB), and calls nothing that waits. It turns the
transmitter off first (8), with or without the crash log. It adds the running
task's name (empty in an interrupt handler: `CRASH_F_ISR`), the uptime, the 16 words at the stack pointer when that
points into RAM (`CRASH_F_STACK`), the commit, then seals the record with a CRC-32, stores it and restarts the radio.

Not caught: a hang (no watchdog is enabled yet), a fault while the fault entry pushes its frame (2.2), and an
unhandled interrupt on the STM32F4 (the start-up's default handler still spins; no interrupt is enabled without a
handler). The NMI on the STM32F4 takes the HardFault entry (`cause` 2).

## 2. Platforms

### 2.1 STM32F4 (DM-1701, MD-380)

`platform/stm32f4/crash_stm32.c`. `HardFault_Handler` reads EXC_RETURN bit 2 to find the stack the CPU pushed the
frame on (MSP, or a task's PSP), moves to its own stack, pushes r4-r11 and fills the record: r0-r3, r12, LR, PC and
xPSR from the frame, the stack pointer before it (bit 4 of EXC_RETURN says whether the frame has the FPU registers,
bit 9 of the stacked xPSR whether the CPU padded it), CFSR, HFSR, MMFAR and BFAR. MemManage, BusFault and UsageFault
are not enabled, so they escalate to HardFault; CFSR still says which. If they are enabled later they take the same
entry. A frame the CPU couldn't push (a stacking error) leaves the registers 0 and `sp` the address it tried.

The store is the 4 KB backup SRAM at 0x40024000 (`PWR_CR.DBP`, `RCC_AHB1ENR.BKPSRAMEN`). A system reset leaves the
backup domain alone and the stock bootloader, which runs in main RAM before the application, doesn't look at it
(`hardware-dm1701.md` 15.4: the ROM DFU entry's RTC backup register survives the same way). The record is lost when
the supply goes. Restart: `NVIC_SystemReset`, then the stock bootloader and ODR as at power-on.

### 2.2 HR_C7000 (DM-32UV, DM-1801 Pro, DB25-D)

`platform/hrc7000/crash_hrc.c`, the entry in `freertos/portasm.S`. The CK803S pushes nothing on an exception, so
`hrc_fault_entry` stores r0-r13, r15, EPSR and EPC in a 68-byte frame below the stack pointer the fault left, as the
interrupt entry does, then calls `hrc_fault` (`irq.c`) on its own stack. `hrc_fault` keeps the vector, EPC and EPSR
in `hrc_fault_info` for a debugger, as before, and hands the frame to the capture. The CK803S has no fault address
register. `status[0]` is the PSR in the handler. A stack pointer that can't take the frame faults again with
exceptions off: an unrecoverable error, and the radio stays stopped as without the crash log.

The store is 256 bytes at 0x18007F00, the top of the SRAM shared with the modem. The codec's tables (from
0x18000800, at most to 0x18004F90 in any stock image) and the microphone PCM (0x18000000) don't reach it
(`hardware-dm32.md` 2). Main SRAM is not used: the Baofeng bootloader and the start-up use it. Restart: the chip's
soft reset (`hrc_soft_reset`, the same as a reboot).

## 3. The record

`crash_record_t` (`hal/hal_crash.h`), 188 bytes, little endian, no padding. Version 1.

| Offset | Field | Meaning |
|---|---|---|
| 0 | magic u32 | `0x48535243` (`"CRSH"`) |
| 4 | version u16, size u16 | 1, 188 |
| 8 | arch u8 | 1 Cortex-M4, 2 CK803S |
| 9 | kind u8 | section 1 |
| 10 | flags u8 | bit 0 in an interrupt handler, 1 the build had local changes, 2 `stack` is valid, 3 seen (shown or dismissed), 4 sent |
| 11 | count u8 | crashes in a row of this build nobody looked at: 1 the first; counts up while the record before is unseen and from the same build, saturates at 255 |
| 12 | cause u32 | section 1 |
| 16 | pc, lr, sp, psr u32 | where; the return address; the stack pointer before the exception; CM4 the stacked xPSR, CK803 EPSR |
| 32 | r[14] u32 | r0-r13 (CM4: r0-r12, `r[13]` = EXC_RETURN) |
| 88 | status[4] u32 | CM4: CFSR, HFSR, MMFAR, BFAR. CK803: the handler's PSR, 0, 0, 0 |
| 104 | uptime_ms u32 | time since start |
| 108 | build u32 | the firmware commit's first 8 hex digits (0: unknown) |
| 112 | task char[8] | the running task, NUL padded, not always terminated |
| 120 | stack[16] u32 | the words at `sp` |
| 184 | crc u32 | CRC-32 (IEEE, reflected, init and final XOR `0xFFFFFFFF`) of bytes 0-183 |

A store that fails any of magic, version, size or CRC holds no record (power-on contents). `crash_init()` copies a
valid record at start-up, before the tasks start; `crash_mark()` sets the seen and sent flags in both copies;
`crash_clear()` forgets it. A new crash overwrites the store.

## 4. The packed report

`crash_pack()`: the record in fewer bytes, for sending over the air. Little endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `'O' 'C'` |
| 2 | 1 | record version (1) |
| 3 | 1 | arch |
| 4 | 1 | kind |
| 5 | 1 | flags |
| 6 | 1 | count |
| 7 | 1 | n: return addresses at the end |
| 8 | 4 | build |
| 12 | 4 | uptime_ms |
| 16 | 4 | cause |
| 20 | 16 | pc, lr, sp, psr |
| 36 | 16 | status[4] |
| 52 | 8 | task |
| 60 | 16 | r0-r3 |
| 76 | 4 n | the stack words that point into code (flash, and RAM code on the HR_C7000), in stack order, at most 12 |

76 to 124 bytes. `tools/crash_decode.py` reads it (as received, as hex, or the debug link's crash view) and, with
the build's ELF, names the functions at `pc`, `lr` and the return addresses through addr2line. The stack words are
only candidates: a code address left on the stack by an earlier call looks the same as a live return address.

## 5. Sending the report

`CONFIG_CRASH_REPORT` (default on; needs the crash log's screen and DMR). The UI sends the packed report over DMR as one UDP
datagram. Code: `core/ui/ui_svc_crash.c`, the Crash log screen in `core/ui/ui_menus.c`, `dmr_data_send_udp()` in
`core/dmr/dmrDataService.c`.

### 5.1 On the air

| Item | Value |
|---|---|
| Transport | IPv4 + UDP, source and destination port 4090 (`CRASH_UDP_PORT`), the packed report as the payload |
| Data | unconfirmed, rate 1/2 ([dmr-data.md](dmr-data.md)) |
| Destination | a codeplug contact: a private contact's ID (individual) or a group contact's talkgroup (group) |
| Channel | the current one, when it is a DMR channel the radio may transmit on |
| Wait | `UDP_WAIT_MS` (120 s) queued without a channel it may go on, then failed; a busy channel waits in the transmit queue and fails when the queue gives up |

`dmr_data_send_udp()` takes one datagram at a time (at most `DMR_DATA_UDP_MAX`, 128 bytes) from any task, under the
service's lock; the radio task's tick sends it. `dmr_data_udp_state()` is queued, sending, OK (the bursts are out) or
failed. Nothing on the air says the report arrived. When a send ends OK, the record gets the sent and seen flags.

### 5.2 Settings and the screen

Two settings, in spare bits of the settings record ([settings-store.md](settings-store.md)): the target contact (the
codeplug contact number, 1..1024, 0 none) and what happens after a crash.

| After a crash | At the start after a crash |
|---|---|
| Off (default) | nothing; the report goes only from the menu |
| Ask | a prompt: "Restarted after a crash. Send the report to `<name>`?" with Send report and Not now |
| Auto | sent without a prompt; a toast says "Sending crash report", then "Crash report sent" or "Crash report not sent" |

Ask and Auto act once per record, 1.5 s after the start, on the Home screen, and only for a record that is neither
seen nor sent and with a target set. Showing the Crash log screen or the prompt marks the record seen.

**Settings > Advanced > Crash log** shows the summary line (`crash_summary`), "Report sent", the count in a row, the
uptime and the build. Its rows: Send report, Send to (the contacts list, type to find), After a crash (Off, Ask, Auto
in turn) and Clear. Without `CONFIG_CRASH_REPORT` only Clear is there. The screen is `CONFIG_CRASH_SCREEN` (on with the
crash log); without it the debug link still shows the record.

## 6. Unverified

- That the store survives the restart on the DM-32UV, the DB25-D and the MD-380. The record's CRC means a store
  that doesn't survive shows no record; it can't show a wrong one. Check: `odr_debug.py crash test trap`, then
  `odr_debug.py crash` after the restart.
- The report on the air: no receiver for port 4090 exists yet.

Checked on hardware (2026-10-17, both flashed through the stub), with `tests/hil/test_crash.py` passing under
`--allow-reboot` on each:

| Radio | Store | Trap test |
|---|---|---|
| DM-1701 | the backup SRAM kept the record through the stock bootloader | HardFault, CFSR UNDEFINSTR, task `host` |
| DM-1801 Pro | the modem SRAM kept the record through the soft reset and the Baofeng bootloader | exception 7 (breakpoint), task `host` |

## 7. Tests

`tests/host/crashTest.c`: the CRC, sealing and the count in a row, the build ID, the start-up side on a fake store,
the packed report and the summary line; `ui_svc_crash.c` on fake settings, contacts and data service (the record for
the screen, seen, clear, the report to a private or group contact, its progress and the sent mark); and
`debuglink_crash.c` (view section 21, CRASH's argument checks, token and transmit refusals and the deferred test).
`tests/host/crashCaptureTest.c`: `crash_capture.c` on a stand-in RTOS; every way in (a fault record, a stack
overflow, out of heap, an assertion inside the capture) calls `board_rf_safe` first, with interrupts off, before the
store and the restart. `tests/host/bootDb25dTest.c` (`test_tx_off`): the DB25-D first stage drives the transmitter
pins off.
`tests/host/dmrDataServiceTest.c` (`testUdpSend`): the UDP mailbox. `tests/host/radioCoreTest.c`
(`test_ui_settings`): the two settings through the UI service, their clamps, and that they leave the theme, Up/Down
and the accent alone. `tests/host/uiFlowTest.c` ("C crash ..."): the screen, Send to, the three modes after a
restart, a report that can't start or fails, no prompt off the Home screen. On the radio: the debug link's CRASH TEST
and view section 21 ([debuglink.md](debuglink.md) 3.19).

## 8. Transmitter off on any fault or reset

A DB25-D faulted while transmitting (2026-10-21). The capture stored its record and soft-reset the chip, and the radio
came back stopped before the display was lit with the PA keyed, drawing about 15 W, until a power cycle. Nothing on the
fault or reset paths turned the transmitter off, and the boards' pin set-up (`board_early_init`) ran only after the
clock, interrupt and timer set-up. Now every board has `board_rf_safe()` (`hal/board.h`): transmitter off with
register and GPIO writes only, callable with interrupts off and without the RTOS, any number of times. On the
HR_C7000 it is RAM code, so it doesn't depend on the XIP flash.

| Board | `board_rf_safe` |
|---|---|
| DM-1701 | PA_EN1 (TX supply) and PA_EN2 (APC loop) low, PA_BAND low, APC DAC (DAC1 channel 1) 0 |
| MD-380 | APC_EN and TX_5V (5TC) low, VCO_SEL high (receive VCO), APC DAC (DAC1 channel 1) 0 |
| DM-32UV | UHF_PA and VHF_PA low, DAC B 0 and powered down |
| DM-1801 Pro | TX_UHF and TX_VHF low, DAC B 0 and powered down |
| DB25-D | TX_SUPPLY_A, TX_SUPPLY_B, UHF_TX, VHF_TX low, DAC B 0 and powered down, RX_SUPPLY (PTA23) high |

Each pin is forced to an output at its level (the level first), so the call also works at start-up before
`board_early_init`. The STM32 DAC is written only when its clock is on. On the HR_C7000 the platform's `hrc_rf_safe()`
adds the modem's own transmit state: `FM_PTT` 0 and `LAYER2_TXRX_CTRL` 0 (neither slot direction). The RF chips (AT1846S,
SKY72310 and HR_C5000/C6000, FD6818, BK4819) are not touched: each needs a bus transaction, and with the TX supplies and
PA selects off their output reaches no keyed PA. The next start sets them up again.

Where it is called:

| Path | STM32F4 | HR_C7000 |
|---|---|---|
| CPU fault | `stm32_fault` first (HardFault, MemManage, BusFault, UsageFault, NMI); without the crash log, `HardFault_Handler` and the NMI | `hrc_fault`, right after interrupts go off, with or without the crash log |
| Crash capture | `crash_commit` and the soft capture (assert, stack overflow, out of heap), with interrupts off, before the store; `hal_crash_restart` | the same; `hal_crash_restart` is `hrc_soft_reset` |
| Hooks without the crash log | stack overflow and malloc-failed hooks (`main.c`) | the same |
| Software reset | `reboot()` (normal and ROM DFU) before `NVIC_SystemReset`; the ROM DFU entry at start-up (`stm32_sysboot_check`) | `hrc_soft_reset`, before the loader's clocks and the flash controller are put back |
| Stub entry | `stm32_enter_stub` (replaces `board_stub_prepare`) | `enter_stub`, before the PIC, watchdog and flash controller changes |
| Start-up | first thing in `main()`, before the ROM DFU check and the clocks | first thing in `main()`, before `hrc_clock_init` |
| DB25-D first stage | | `boot/db25d/boot.c` drives TX_SUPPLY_A/B and UHF_TX/VHF_TX low and RX_SUPPLY high right after the power latch, before it offers the handshake (it may wait there for a host) |

Every reboot request (the debug link's REBOOT, the CPS's reset, the storage UI) goes through `board.system->reboot`, so
it is covered by the reset row. The time from power-on to the first `board_rf_safe` is the vendor bootloader's (and on
the DB25-D the first stage's handful of instructions), which ODR can't change.

Unverified on hardware: that each radio's transmitter actually stops (current and RF) at a crash while keyed, and that
the DAC B write before `hrc_clock_init` is harmless on every HR_C7000 radio (the vendor loaders leave the peripheral
clock gates on, as far as known: `hardware-dm32.md` 1.1). The debug link's crash test is refused while transmitting,
so a bench check needs a local test build that lets it through: keyed at the lowest power, the trap, then the supply
current and the SDR bench's capture drop to receive levels within a millisecond of the crash, and the radio restarts
receiving.
