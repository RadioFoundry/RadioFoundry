<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Hardware watchdog

`CONFIG_WATCHDOG` (default on). The platform starts the SoC's watchdog in `main()` and the FreeRTOS idle task feeds
it. The idle task runs only when every other task waits, so a feed says the whole system still gets through its work.
A task that spins, a critical section that never ends, an interrupt storm or a hang with interrupts off stops the
feeds, and the chip resets within a few seconds. Off: no watchdog runs, as before.

With the crash log, the hang check in the tick interrupt keeps a record of the task that kept the idle task out, a
quarter of the timeout before the reset would come, and restarts the radio itself ([crash-log.md](crash-log.md) 1.1).
Only a hang with interrupts off still reaches the hardware reset.

A watchdog reset is a chip reset: the radio starts as at power-on. The transmitter must be off by then, which is the
early-boot RF-off step's job (`board_rf_safe`, [crash-log.md](crash-log.md)), not the watchdog's.

Code: `hal/hal_watchdog.h`, `core/app/watchdog.c` (the feed), `platform/stm32f4/watchdog.c`,
`platform/hrc7000/watchdog.c`, `core/debuglink/debuglink_watchdog.c` (bench tests).

## 1. The feed

`vApplicationIdleHook` (`configUSE_IDLE_HOOK` follows `CONFIG_WATCHDOG` in every platform's `FreeRTOSConfig.h`) calls
`watchdog_idle(hal_millis())` on every pass of the idle loop. It feeds the watchdog and notes how long the idle task
was away since its last pass: the longest such gap since start is `watchdog.max_gap_ms` on the debug link (6). That
number shows how close legitimate work comes to the timeout.

| Phase | Who feeds |
|---|---|
| `main()`, before the scheduler | `main()` after the start, after `board_early_init()` and after `app_start()`; `hal_delay_ms()` once a millisecond while it busy-waits before the scheduler runs |
| Scheduler running | the idle task |
| A long busy operation that keeps the idle task out on purpose | the operation, once per block or step, by `watchdog_kick()`, so the hang check counts from that feed too (4) |
| A CPU fault with the fault trace | `hrc_fault` once, before the 3 s trace ([crash-log.md](crash-log.md) 2.3) |

## 2. Timeout

| Platform | Watchdog | Timeout | Start |
|---|---|---|---|
| STM32F4 | IWDG on the LSI RC oscillator, prescaler 64, reload 2500 | 5 s at the typical 32 kHz; 3.4 s at the data sheet's fastest LSI (47 kHz), 9.4 s at its slowest (17 kHz) | right after `stm32_sysboot_check()` |
| HR_C7000 | the SoC watchdog at 0x14010000 (`WDT_BASE`, `platform/hrc7000/platform.h`) | 4 s (the 24 MHz crystal, measured; 2.1) | right after `hrc_clock_init()`, which turns off whatever the loader left |

### 2.1 The HR_C7000 counter

OT_LOAD_H (bits 2:0) and OT_LOAD_L are one 35-bit start value. The counter counts up from it and the radio resets
when it overflows at 2^35, so the timeout is (2^35 - load) / f and a small
load is the longest timeout. f is the 24 MHz crystal: on one HR_C7000 board, H = 7, L = 0xF0000000 (2^28 counts) reset the
radio after 11.1 s (6.2, 2026-10-21). `WDT_LOAD` is 2^35 - 4 s x 24 MHz (H = 7, L = 0xFA472800). RST_LOAD is left at
its reset value (0x03F80000: about 20 ms from the overflow to the reset, per the manual). The timeout must stay
comfortably above 3 s: the fault trace takes that long with no feeds.

## 3. Leaving the application

Code that never feeds the watchdog must not run with it on.

**The guarantee.** Flashing and backing up a radio, with `stub/stubtool.py`, `stub/f4stub.py` or the web flasher, never
trips the watchdog unless the stub itself hangs. Every stub RadioFoundry can enter feeds the watchdog in every loop that can
take time, and a watchdog build of RadioFoundry refuses to enter a stub that doesn't (3.1).

| Exit | HR_C7000 | STM32F4 |
|---|---|---|
| Reboot (`hal_system_t.reboot`, the debug link's REBOOT, a crash restart) | `hrc_soft_reset` turns it off first: whether a soft reset clears it is unknown, and the vendor bootloader's update mode never feeds it. If the soft reset doesn't happen, it turns the watchdog back on as the last way out | `NVIC_SystemReset`: a system reset stops the IWDG |
| ROM DFU entry (REBOOT mode 1, STM32 boards) | not offered | the request resets first; `stm32_sysboot_check()` jumps to the ROM before `main()` starts the IWDG |
| Runtime stub entry (STUB ENTER, [debuglink.md](debuglink.md) 3.18) | `hrc_watchdog_stretch()` gives it 32 s and a feed, and the stub feeds it from its start-up on (3.2) | the IWDG can't be stopped: ENTER is refused for a stub whose image header doesn't say it feeds; `stm32_watchdog_stretch()` sets the longest timeout (about 32 s, at least 22 s) for the jump, and the stub feeds it from its first instruction on (3.1) |
| Power off | the supply goes; until it does the idle task feeds as usual (a knob that keeps the radio on leaves it running normally) | the same |

### 3.1 The STM32 stub and the IWDG

The IWDG runs until the next reset once started, so a stub entered at run time inherits it. Choices considered:

| Option | Why not, or why |
|---|---|
| Don't start the IWDG on builds with `CONFIG_STUB_ENTRY` | the radios that update through the stub would lose the watchdog |
| Start it late, only after a check | nothing at start-up knows a stub session will come |
| Only stretch the timeout at the jump | 22-62 s is shorter than a whole application write |
| **The stub feeds it** (chosen), **and RadioFoundry enters only a stub that does** | below |

The stub (`stub/stm32f4`) feeds the IWDG:

| Where | How often |
|---|---|
| `stub_main`, first | once, before anything else: RadioFoundry's stretch covered the copy and the jump |
| `stub_ms()`, which every wait loop reads: the main loop's receive poll, the flash erase and program busy waits (a 128 KB sector: up to 2 s), the SPI flash's status polls, the USB transmit waits, the delays | every pass |
| The protocol engine (`stubeng_port_t.wdog_feed`, stub/common/stubeng.c) | at every request, before each erase or program, once per 4 KB block in SUMS and READ (a 1 MB SUMS is 256 blocks) |

The loops left without a feed are bounded to microseconds (the SPI byte transfers, the FIFO copies) or are the stub
hanging, which is what the watchdog is for.

Every such image starts with a header at offset 0x10 ([stub-stm32f4.md](stub-stm32f4.md) 3.1): a magic, the stub
version and a flags word whose bit 0 says the stub feeds the IWDG. The first version that feeds is 3
(`F4STUB_VERSION_FEEDS_IWDG`). While the IWDG runs, RadioFoundry checks the loaded image before ENTER fires
(`stm32_stub_image_ok`, through `stub_load_t.image_ok`) and again in `stm32_enter_stub`, with one rule: the magic, a
version of at least 3 and the feed flag, all three. A stub built before has no header (garbage where the header would
be): ENTER answers refused (7) with reason 1
(`STUB_REFUSE_WATCHDOG`, [debuglink.md](debuglink.md) 3.18), RadioFoundry keeps running, and nothing is written. So an old
`stubtool.py` or a web flasher page that hasn't been redeployed gets a clear refusal, never a reset part way through a
write. `stubtool.py` says to use a current stub; the page says to reload.

The web flasher has no stub of its own: the site's build takes the image from `stub/f4stub.py` (`f4_stub()` in
`site/rfoundrysite/repo.py`, served as `data/f4-stub.json`), so the page and the repository can't drift apart. The site's
tests and `f4stub.py --self-test` check the served and embedded images' headers (magic, version at least 3, the
flag), and that their copies of the numbers match `stub/stm32f4/f4stub_header.h`, the one definition the stub and RadioFoundry
both include.

### 3.2 The HR_C7000 stub

The watchdog keeps running while the stub runs, as on the STM32 radios, so a stub that hangs resets the radio. Before
the jump, `enter_stub` calls `hrc_watchdog_stretch()` (RAM code, interrupts off): a load for 32 s, the STM32 IWDG's
longest timeout, and a feed. The universal stub leaves the watchdog as it finds it (off under the boot ROM, running
after RadioFoundry's entry) and feeds it with the same knock sequence on every pass of its receive wait and before every SPI read
burst. An idle stub sits in its receive wait, so it feeds for as long as the host leaves it there. The flash erases and
programs are host-driven SPI commands, one frame at a time, so the stub passes through its receive wait between them.
A knock while the watchdog is off does nothing.

The stub's reset (`--boot reset`, the web flasher's `resetChip`) turns the watchdog off over the stub's register write
before `SYS_SOFT_RSTN`, as `hrc_soft_reset` does: a soft reset may keep it, and the bootloader never feeds it. RadioFoundry starts
it again in `main()`.

RadioFoundry needs no version check here. Every older HR_C7000 stub turns the watchdog off at its start-up, so it runs as before,
without the watchdog: never reset part way through a write. A watchdog build with no stub that feeds is the STM32 case
(3.1) only. The web flasher takes this stub from
`stub/stubtool.py` (`c7000_stub()` in `site/rfoundrysite/repo.py`); the site's tests check it carries the feed.

## 4. Long operations

What keeps the idle task out on purpose, and how each is handled. Everything that waits by `hal_delay_ms()` or a
FreeRTOS call after the scheduler starts lets the idle task run and needs nothing.

| Operation | Where | Handling |
|---|---|---|
| Start-up before the scheduler (clocks, board early init, display, the radio chips' reset waits) | `platform/*/main.c`, `board_early_init` | `main()` feeds between steps; `hal_delay_ms()`'s busy wait feeds every millisecond before the scheduler runs |
| SPI NOR erase and program (settings, journal, codeplug, CPS writes, TFTP) | `drivers/flash/spi_nor` | none needed: the busy polls are `hal_delay_ms(1)`, which sleeps once the scheduler runs |
| SPI NAND mount, garbage collection, wear leveling (boards with SPI NAND storage) | `drivers/flash/nand_sectors` | busy waits of 10 µs: the layer calls `nand_dev_t.busy` once per block or sector, which such a board points at `watchdog_kick()` |
| A single NAND page read, program or block erase | `drivers/flash/spi_nand` | at most a few ms each, fed through the above around them |
| Settings saves, codeplug sector rewrites, CPS sessions, fast codeplug commits | `core/settings`, `core/storage`, `core/cps` | through the flash drivers above |
| The HR_C7000 codec load into RAM at the first DMR call | `platform/hrc7000/vocoder.c` | a memcpy of tens of KB: milliseconds |
| Internal flash erase and program | the stubs only (`stub/stm32f4`, the HR_C7000 stubs) | RadioFoundry never writes its own program flash; the stubs run after the exits in 3 |
| Critical sections, `vTaskSuspendAll` | `core/input`, `core/radio`, `core/debuglink/dlog.c`, the audio drivers | microseconds; none loops |
| A CPU fault: the fault trace, then the record and the restart | `platform/hrc7000/irq.c` | one feed before the 3 s trace; the restart turns the watchdog off |
| A deliberate stop without the crash log (stack overflow or malloc-failed hook, a failed `configASSERT`) | `platform/*/main.c`, `FreeRTOSConfig.h` | none: they spin with interrupts off and the watchdog resets the radio, instead of leaving it stopped |

## 5. Unverified

- The HR_C7000 counter's clock and load units (2.1): until measured, the real timeout is somewhere between 4 s and
  much longer.
- Whether the HR_C7000 soft reset, or a watchdog reset itself, leaves the watchdog running into the bootloader. The
  soft reset turns it off first; after a watchdog reset the bootloader's update mode must still be checked to stay up.
- The STM32 LSI on these radios (the IWDG's timeout scales with it).
- The longest legitimate idle gap on each radio, in particular the start-up of a SPI NAND board with a full codeplug.
- A whole STM32 stub write on a watchdog build, and the refusal of a stub without the header (version 2), on the
  radio.

## 6. Bench tests

The debug link's watchdog view section and command `0x1D` ([debuglink.md](debuglink.md) 3.21);
`tools/rfoundry_debug.py watchdog` drives them. Every test ends in a watchdog reset, and none starts while the radio
transmits.

### 6.1 The reset

| Test | Starves the idle task by | Checks |
|---|---|---|
| `watchdog test starve` | the idle task no longer feeds; everything else runs, the link answers until the reset | the reset, and its timeout: the tool polls the view until the link goes quiet and prints the last `watchdog.starved_ms` |
| `watchdog test spin` | the host link's task spins with interrupts on | starvation by a busy task |
| `watchdog test irq-off` | the host link's task spins with the kernel's interrupts masked | a hang with interrupts off |

After each: the radio restarts on its own, the boot is clean, the transmitter stays off, and `watchdog.feeds` counts
again from 0.

### 6.2 Measuring the HR_C7000 counter

`watchdog load H L [RST]` re-arms the watchdog with raw OT_LOAD_H, OT_LOAD_L (and RST_LOAD, left alone when not
given), stops the feeds and polls as `starve` does. The view's `watchdog.regs` shows the six
registers raw. Remember the count runs up to an overflow: a larger load is a shorter timeout. `load 7 0xF0000000`
should reset after about 11 s (2^28 counts at 24 MHz).

## 7. Tests

`tests/host/watchdogTest.c`: the idle feed, the longest gap (with the millisecond clock wrapping), the starve and its
time, the view's keys, and DL_CMD_WATCHDOG's guards (length, kind, token, transmitting, a PTT before the test
starts, LOAD where the platform has none) with the deferred start. `tests/host/nandSectorsTest.c`
(`test_busy_hook`): the sector layer's mount and garbage collection call the busy hook. `tests/host/f4stubTest.c`
(`test_feeds`): the STM32 stub's engine feeds at every request, before an erase or a program, and once per block of a
1 MB SUMS and a READ. `tests/host/stubEntryTest.c`: ENTER refused with reason 1 when the platform's image check says
no, the image kept; the header rule with the IWDG running (no header, a wrong magic, an old version, no flag:
refused; the feed version and later: entered) and not running (anything entered). `stub/f4stub.py --self-test` and
the site's `f4stub.test.mjs` / `c7000.test.mjs`: the stubs the tools and the page serve have the header and the feed. On the radio: 6.
