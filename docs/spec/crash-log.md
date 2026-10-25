<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

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
| Assert | 4 | `configASSERT` (the platforms' `FreeRTOSConfig.h`) | the line | the caller of the assertion |
| Hang | 5 | the hang check in the tick interrupt (1.1), with `CONFIG_WATCHDOG` | ms since the last watchdog feed | where the interrupted task was |

The capture runs with interrupts off, on a stack of its own (1 KB), and calls nothing that waits. It turns the
transmitter off first (8), with or without the crash log. It adds the running
task's name (empty in an interrupt handler: `CRASH_F_ISR`), the uptime, the 16 words at the stack pointer when that
points into RAM (`CRASH_F_STACK`), the other tasks (1.2), the commit, then seals the record with a CRC-32, stores it
and restarts the radio.

Not caught: a hang with interrupts off (the tick can't run, so the hardware watchdog resets the radio with no record,
[watchdog.md](watchdog.md)), a fault while the fault entry pushes its frame (2.2), and an unhandled interrupt on the STM32F4 (the start-up's default
handler spins until the watchdog resets the radio; no interrupt is enabled without a handler). The NMI on the STM32F4
takes the HardFault entry (`cause` 2).

### 1.1 The hang check

The watchdog ([watchdog.md](watchdog.md)) resets a radio whose idle task stops running, but that reset leaves nothing
to read. The hang check keeps a record first. Code: `watchdog_hung` and `watchdog_tick` in `core/app/watchdog.c`, the
platforms' `hal_crash_hang`.

| Item | Value |
|---|---|
| Where | the RTOS tick interrupt, before the kernel's tick: `vPortTickHandler` (HR_C7000), `SysTick_Handler` (STM32F4). Not the tick hook: `xTaskResumeAll` also calls the hook from a task, for ticks that came while the scheduler was suspended |
| Counts from | the last feed: the idle task's, or a long job's `watchdog_kick` (a board's long SPI NAND loops) |
| Fires | three quarters of the watchdog's timeout after it: 3 s on the HR_C7000 (4 s timeout), 3.75 s on the STM32F4 (5 s) |
| Never | before the idle task's first pass, with the watchdog stopped, or while a bench test starves it on purpose (the test times the hardware reset, [watchdog.md](watchdog.md) 5) |
| Record | kind 5, `cause` the ms since the last feed, the registers of the task the tick interrupted, its name in `task` |
| Then | as every capture: the transmitter off, the fault trace on the HR_C7000 (2.3, vector 0), the store, the restart |

What the record shows is the task that ran when the tick came, which on a hang is the one that kept the idle task out.
The tick runs at the kernel's priority, the lowest on the STM32F4, and HR_C7000 interrupts don't nest, so the tick
never interrupts another handler.

| Platform | Registers |
|---|---|
| HR_C7000 | `hrc_irq_entry` saved the task's frame (r0-r13, r15, EPSR, EPC, as the fault entry's) and wrote its address to the TCB's first word, `pxTopOfStack`. `hal_crash_hang` (`irq.c`) hands that frame to the same capture as a fault (`hrc_crash_frame`) |
| STM32F4 | the CPU's frame on the PSP: r0-r3, r12, LR, PC, xPSR. r4-r11 belong to the handler's callers by then and stay 0. Without EXC_RETURN the frame's size (FPU or not) isn't known, so `sp` is the frame's address and `stack[]` starts with the frame |

Not caught this way: a hang with interrupts off or in a critical section (the STM32F4 masks the tick there too), and
an interrupt storm that keeps the tick out. Those still end in the plain watchdog reset.

The debug link's CRASH TEST kind 3 ([debuglink.md](debuglink.md) 3.19) spins in the host link's task with interrupts
on: the check keeps a hang record of `host`.

### 1.2 The other tasks

The record's registers are the running task's. The task snapshot says where every other task was, so a hang or a
fault also shows what the rest of the radio waited for. Code: `put_tasks` in `core/crash/crash_capture.c`, the
platforms' `hal_crash_task_where`. Needs `CONFIG_TASK_STACKS` (on with the debug link): the tasks come from its table
(`core/app/task_stack.c`), in creation order. Without it the snapshot is empty.

| Item | Value |
|---|---|
| Left out | the running task, and the idle task (`IDLE`), which only waits for work |
| Entries | at most 8 (`CRASH_TASKS`); `more_tasks` counts the rest |
| Name | the first 4 letters (`CRASH_TAG_LEN`), NUL padded: `radi`, `ui`, `Tmr ` |
| `where`, bit 0 set | the task was waiting in the kernel. The rest is the first return address in the 24 words above its saved context (`CRASH_TASK_SCAN_WORDS`): the code that called the wait. No return address found: the saved pc |
| `where`, bit 0 clear | an interrupt took the task while it was ready. The rest is its pc |
| `where` 0 | its saved context isn't in RAM |

Each task's TCB starts with the stack pointer its last switch saved (`pxTopOfStack`), which points at its context.

| Platform | Waiting or ready |
|---|---|
| HR_C7000 | the 17-word frame (2.2). `vPortYield` stores its return address as both r15 and PC; the interrupt entry stores EPC as PC, which equals r15 only right after a call returns. r15 equal to PC counts as waiting |
| STM32F4 | r4-r11 and EXC_RETURN, s16-s31 when EXC_RETURN bit 4 is clear, then the CPU's frame (2.1). The instruction before the stacked pc is the yield's `ISB` (`portYIELD`) or the `MSR BASEPRI` of a critical section's exit, where a held-back yield is taken: waiting. Anything else: ready |

The return address is a candidate, as the stack words are (4): a code address left above the context by an earlier
call looks the same. A kernel task's own wait (the timer task's) can resolve to a kernel function. The packed report
(4) leaves the snapshot out; the debug link's view shows it (`crash.t<k>`).

### 1.3 The restart cause

A reset the capture never sees leaves no record: the hardware watchdog after a hang with interrupts off, a supply dip,
the reset pin. The restart cause says which it was. Code: `crash_init` and `crash_reset_cause` in
`core/crash/crash.c`, the platforms' `hal_crash_boot` and `hal_crash_reset_flags`.

Four words (`crash_boot_t`) sit next to the record, in the same kept memory (2):

| Word | Written by |
|---|---|
| `magic` (`BOOT`) | the start-up. Gone after a power loss |
| `soft` (`SOFT`) | every restart software asks for, first (`hal_crash_soft_mark`): a reboot, a crash's restart, the boot stub's reset. Cleared at each start |
| `uptime_ms` | the RTOS tick, with the hang check (`watchdog_tick`): the last run's uptime, to the tick |
| `warm` | the start-up: starts since the memory last lost its contents |

The chip's own reset flags come first where they say what happened. A chip without them leaves it to the words.

| Chip flag | Words | Cause (`reset.cause`) |
|---|---|---|
| watchdog, brown-out, power-on or software | any | that flag |
| none, or the reset pin | `soft` set | `software` |
| none, or the reset pin | `magic` missing | `power` |
| the reset pin | `magic` kept, no `soft` | `pin` |
| none | `magic` kept, no `soft` | `warm`: the watchdog, the reset pin, or a dip too short to lose memory |

| Platform | Chip flags | Words |
|---|---|---|
| STM32F4 | `RCC_CSR` (RM0090 7.3.21), read and cleared at start-up. Every reset also sets the pin flag, and a power-on sets the brown-out flag too, so the order is watchdog, software, power-on, brown-out, pin | the backup SRAM, after the record |
| HR_C7000 | none: the POR, the reset pin and the watchdog drive one global reset (manual 2, 4.1) | `HRC_CRASH_BOOT`, the 16 bytes below the record. `hrc_soft_reset` runs from RAM and writes `soft` by address |

The boot stubs mark their own reset, not their entry, so a stub that hangs and is reset by the watchdog reads as the
watchdog. The STM32 stub's RESET writes `soft` itself (`F4STUB_SOFT_MARK_ADDR` in `stub/stm32f4/f4stub_header.h`,
checked against the layout at build time). The HR_C7000 stub has no reset of its own: stubtool's `--boot reset` and
the web flasher poke `soft` (`CRASH_SOFT_MARK`) before the soft reset, which works with every stub version. The stub
runs in main SRAM only, so the words and the record survive it.

The debug link's view section `reset` (23) shows it with or without a record: `reset.cause`, `reset.flags` (the chip's
register as read, 0 without one), `reset.uptime_ms`, `reset.warm`.

## 2. Platforms

### 2.1 STM32F4

`platform/stm32f4/crash_stm32.c`. `HardFault_Handler` reads EXC_RETURN bit 2 to find the stack the CPU pushed the
frame on (MSP, or a task's PSP), moves to its own stack, pushes r4-r11 and fills the record: r0-r3, r12, LR, PC and
xPSR from the frame, the stack pointer before it (bit 4 of EXC_RETURN says whether the frame has the FPU registers,
bit 9 of the stacked xPSR whether the CPU padded it), CFSR, HFSR, MMFAR and BFAR. MemManage, BusFault and UsageFault
are not enabled, so they escalate to HardFault; CFSR still says which. If they are enabled later they take the same
entry. A frame the CPU couldn't push (a stacking error) leaves the registers 0 and `sp` the address it tried.

The store is the 4 KB backup SRAM at 0x40024000 (`PWR_CR.DBP`, `RCC_AHB1ENR.BKPSRAMEN`). A system reset leaves the
backup domain alone and the vendor bootloader, which runs in main RAM before the application, doesn't look at it
(the ROM DFU entry's RTC backup register survives the same way). The record is lost when
the supply goes. Restart: `NVIC_SystemReset`, then the vendor bootloader and RadioFoundry as at power-on.

### 2.2 HR_C7000

`platform/hrc7000/crash_hrc.c`, the entry in `freertos/portasm.S`. The CK803S pushes nothing on an exception, so
`hrc_fault_entry` stores r0-r13, r15, EPSR and EPC in a 68-byte frame below the stack pointer the fault left, as the
interrupt entry does, then calls `hrc_fault` (`irq.c`) on its own stack. `hrc_fault` keeps the vector, EPC and EPSR
in `hrc_fault_info` for a debugger, as before, and hands the frame to the capture. The CK803S has no fault address
register. `status[0]` is the PSR in the handler. A stack pointer that can't take the frame faults again with
exceptions off: an unrecoverable error, and the radio stays stopped as without the crash log.

The store is 256 bytes at 0x18007F00, the top of the SRAM shared with the modem. The codec's tables (from
0x18000800, at most to 0x18004F90 in any supported codec build) and the microphone PCM (0x18000000) don't reach it.
Main SRAM is not used: the vendor bootloader and the start-up use it. Restart: the chip's
soft reset (`hrc_soft_reset`, the same as a reboot). With the fault trace (2.3) the lines go out first; without the
crash log the fault ends in the same soft reset.

### 2.3 Fault trace

`CONFIG_FAULT_TRACE` (default on; the STM32 radios turn it off). A CPU fault prints its registers on the host UART
before the record and the restart, so a radio whose debug link is gone can still say where it crashed, with or
without the crash log. Code: `core/crash/crash_trace.c` (the line, portable), `hrc_fault` in
`platform/hrc7000/irq.c` (timing and output), `hrc_host_uart_put_polled` in `platform/hrc7000/uart.c`.

| Item | Value |
|---|---|
| Port | the host UART (the programming cable), at the rate it runs at (115200 8N1) |
| Lines | `CRASH_TRACE_LINES` (3), one a second, timed on the free-running clock timer (interrupts are off); then 50 ms for the UART's FIFO to empty, then the record and the restart |
| Format | `\r\n#RD0FAULT1 n=01 v=<vector, 0 for a hang> pc=<EPC> psr=<EPSR> lr=<r15> sp=<sp before the frame> r0=<..> .. r13=<..> *<crc>\r\n`; every value 8 lower-case hex digits, `n` the line's number as 2 hex digits |
| CRC | CRC-16/CCITT-FALSE (polynomial 0x1021, init 0xFFFF, no reflection, no final XOR) over the bytes from `#` up to and including the space before `*`, as 4 lower-case hex digits |
| Watchdog | fed once before the first line; the three seconds stay inside the timeout ([watchdog.md](watchdog.md) 2) |

The sync word `#RD0FAULT1` and the CRC let a reader pick the lines out of whatever else the port carries (MMDVM or
CPS traffic, the debug link). `tools/rfoundry_fault_trace.py PORT` reads the port raw for a while (the debug link must not
hold it), or `--file LOG` a saved capture, checks each line's CRC and prints the registers; `--selftest` decodes the
golden line `tests/host/crashTest.c` builds.

The STM32 radios have no UART a fault handler could print on: their host port is USB, which needs interrupts, and
their one UART is a GPS receive line. Their faults keep the record only.

Checked on hardware (2026-10-21, an HR_C7000 radio): `rfoundry_debug.py crash test trap` printed lines the tool decoded
with good CRCs, then the radio restarted with the record kept.

## 3. The record

`crash_record_t` (`hal/hal_crash.h`), 256 bytes, little endian, no padding. Version 2. Version 1 (188 bytes, without
the task snapshot) reads as no record.

| Offset | Field | Meaning |
|---|---|---|
| 0 | magic u32 | `0x48535243` (`"CRSH"`) |
| 4 | version u16, size u16 | 2, 256 |
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
| 184 | ntasks u8, more_tasks u8, reserved u16 | entries in `tasks`; the other tasks that didn't fit (saturates at 255); 0 |
| 188 | tasks[8] {name char[4], where u32} | the other tasks (1.2) |
| 252 | crc u32 | CRC-32 (IEEE, reflected, init and final XOR `0xFFFFFFFF`) of bytes 0-251 |

A store that fails any of magic, version, size or CRC holds no record (power-on contents). `crash_init()` copies a
valid record at start-up, before the tasks start; `crash_mark()` sets the seen and sent flags in both copies;
`crash_clear()` forgets it. A new crash overwrites the store.

## 4. The packed report

`crash_pack()`: the record in fewer bytes, for sending over the air. Little endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `'O' 'C'` |
| 2 | 1 | the report's version (1, `CRASH_PACK_VERSION`), its own: the record's version 2 didn't change it |
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

## 6. Checks on hardware

Checked with `tests/hil/test_crash.py` and `tests/hil/test_watchdog.py` under `--allow-reboot` (2026-10-17 to
2026-10-23) on the bench radios, at least one of each SoC family:

- The store survives the restart (backup SRAM on the STM32F4, modem SRAM on the HR_C7000, through the soft reset and
  the vendor bootloader). The record's CRC means a store that doesn't survive shows no record; it can't show a wrong
  one.
- The trap test records a fault of the expected kind and the task `host`.
- The restart cause (1.3): the watchdog tests read `watchdog` on the STM32F4 and `warm` on the HR_C7000, whose modem SRAM
  keeps the words through a watchdog reset. A reboot and the stub's reset read `software` on both. On the STM32F4
  `reset.flags` keeps `SFTRST` through the vendor bootloader, so it leaves `RCC_CSR` alone.
- The hang check and the task snapshot: CRASH TEST hang keeps kind 5, 3000 ms (3750 on a board with a slower
  timer), task `host`, pc in `debuglink_crash_service`; the snapshot names each waiting task's own wait.

Each radio's result is in its hardware spec and in `docs/testing/results/`. Not checked: the report on the air (no
receiver for port 4090 exists yet) and, on a radio that has not run `test_crash.py`, that its store survives the
restart. Check: `rfoundry_debug.py crash test trap`, then `rfoundry_debug.py crash` after the restart.

## 7. Tests

`tests/host/crashTest.c`: the fault trace's golden line and its CRC (and `tools/rfoundry_fault_trace.py --selftest` on the
same line), the CRC, sealing and the count in a row, the build ID, the start-up side on a fake store,
the packed report and the summary line (a hang's too); `ui_svc_crash.c` on fake settings, contacts and data service (the record for
the screen, seen, clear, the report to a private or group contact, its progress and the sent mark); and
`debuglink_crash.c` (view section 21, CRASH's argument checks, token and transmit refusals and the deferred test).
`tests/host/watchdogTest.c` (`test_hang_check`): when the hang check fires, a long job's kick, the starve and a
stopped watchdog, and the tick keeping the uptime. `crashTest.c` (`test_reset`): the restart cause's table and a run
of starts on fake kept words (power-on, an unasked reset, a reboot, chip flags, no words). `tests/hil/test_watchdog.py`:
after each bench watchdog reset the cause is `watchdog` (or `warm`) with the run's uptime; after a reboot, `software`.
`tests/hil/test_crash.py` (`test_crash_hang_kept`): CRASH TEST hang keeps a hang record of `host`
before the watchdog resets, with the UI and radio tasks in its snapshot and at least one of them waiting.
`tests/host/crashCaptureTest.c`: `crash_capture.c` on a stand-in RTOS; every way in (a fault record, a stack
overflow, out of heap, an assertion inside the capture) calls `board_rf_safe` first, with interrupts off, before the
store and the restart; the snapshot's order, the running and idle tasks left out, names cut, the count of the rest. A board with a first stage tests that it drives the transmitter pins off (`boards/<id>/tests/`).
`tests/host/dmrDataServiceTest.c` (`testUdpSend`): the UDP mailbox. `tests/host/radioCoreTest.c`
(`test_ui_settings`): the two settings through the UI service, their clamps, and that they leave the theme, Up/Down
and the accent alone. `tests/host/uiFlowTest.c` ("C crash ..."): the screen, Send to, the three modes after a
restart, a report that can't start or fails, no prompt off the Home screen. On the radio: the debug link's CRASH TEST
and view section 21 ([debuglink.md](debuglink.md) 3.19).

## 8. Transmitter off on any fault or reset

A board faulted while transmitting (2026-10-21). The capture stored its record and soft-reset the chip, and the radio
came back stopped before the display was lit with the PA keyed, drawing about 15 W, until a power cycle. Nothing on the
fault or reset paths turned the transmitter off, and the boards' pin set-up (`board_early_init`) ran only after the
clock, interrupt and timer set-up. Now every board has `board_rf_safe()` (`hal/board.h`): transmitter off with
register and GPIO writes only, callable with interrupts off and without the RTOS, any number of times. On the
HR_C7000 it is RAM code, so it doesn't depend on the XIP flash.

The pins each board forces are in its hardware spec: the TX supply and PA enables low, the band or VCO select at its
receive level and the power-control DAC at 0 (powered down where it can be).

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
| First stage (a board that has one) | | `boot/<id>/boot.c` drives the TX supplies and PA selects low and the RX supply high right after the power latch, before it offers the handshake (it may wait there for a host) |

Every reboot request (the debug link's REBOOT, the CPS's reset, the storage UI) goes through `board.system->reboot`, so
it is covered by the reset row. The time from power-on to the first `board_rf_safe` is the vendor bootloader's (and, on a board with a first stage, its handful of instructions), which RadioFoundry can't change.

Unverified on hardware: that each radio's transmitter actually stops (current and RF) at a crash while keyed, and that
the DAC B write before `hrc_clock_init` is harmless on every HR_C7000 radio (the vendor loaders leave the peripheral
clock gates on, as far as known). The debug link's crash test is refused while transmitting,
so a bench check needs a local test build that lets it through: keyed at the lowest power, the trap, then the supply
current and the SDR bench's capture drop to receive levels within a millisecond of the crash, and the radio restarts
receiving.
