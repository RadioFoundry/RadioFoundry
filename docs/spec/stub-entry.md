<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Stub entry

A running RadioFoundry build loads the host's flash-access RAM monitor (the *stub*) into free RAM and jumps to it.
This lets a host update the radio with no key held at power-on. The web flasher, `stub/stubtool.py`
(`--rfoundry-enter`), and `tools/rfoundry` use it. Every update after the first one depends on the stub entry, so it's
a feature of its own, separate from the debug link: a build without the debug link still offers it.

| Part | Where |
|---|---|
| Switch | `CONFIG_STUB_ENTRY`: on wherever `CONFIG_HOSTLINK` is (every radio); needs only `HOSTLINK` |
| STUB state machine (portable, host tested) | `core/stubentry/stub_load.c` |
| Firmware glue: the RAM span, the transmit checks, the banner, the jump | `core/stubentry/stub_entry.c` |
| Own listener for a build without the debug link (portable, host tested) | `core/stubentry/stub_link.c` |
| Framing shared with the debug link | `core/debuglink/dl_frame.{h,c}` (no RTOS, no board) |
| Platform: span, quiesce, and jump | `hal_system_t.stub_region / enter_stub / stub_run_addr / stub_image_ok`: the HR_C7000 platform code (`platform/hrc7000`), [stub-stm32f4.md](stub-stm32f4.md) (STM32F4) |
| Tests | `tests/host/stubEntryTest.c` (state machine), `tests/host/stubLinkTest.c` (wire bytes, both listeners) |

## 1. Wire format

The stub entry uses the debug link's framing ([debuglink.md](debuglink.md) 2) and its command `0x1A` STUB, with the
operations INFO, BEGIN, DATA, and ENTER ([debuglink.md](debuglink.md) 3.18). The request and reply bytes are the same
whichever listener answers. `stubLinkTest` replays recorded request frames and checks the replies byte for byte through
both listeners. A host can't tell the listeners apart by the STUB command; VERSION says which one answered (section 2).

## 2. Listeners

The host link has one side-protocol slot (`hostlink_side_t`, first byte `0xA5`). The build sets which listener takes
it:

| Build | Listener | Commands answered |
|---|---|---|
| `DEBUGLINK=y`, `STUB_ENTRY=y` (the default) | The debug link. Its dispatcher hands `0x1A` to `stub_entry_request` and calls `stub_entry_service` after every frame | Every debug link command |
| `DEBUGLINK=n`, `STUB_ENTRY=y` | The stub entry's own listener (`stub_entry_start`, `stub_link.c`), on the same framing | PING (echo), VERSION (the build's keys), STUB, CODEPLUG (with `CPS_FAST`; otherwise `DL_ST_UNAVAILABLE`); anything else gets `DL_ST_UNKNOWN_CMD` |
| `STUB_ENTRY=n` | The debug link, or none | STUB answers `DL_ST_UNAVAILABLE` |

With the debug link on, the wire is unchanged and no code is duplicated: the debug link's parser is the listener. With
the debug link off, the stub entry brings the parser (`dl_frame.c`, about 0.4 KB) and a reply buffer the size of the
debug link's (1 KB), which holds the VERSION text and a fast codeplug protocol block.

The debug link is the first thing a build short of flash drops. What the web tools need stays: identifying the radio
and its build (VERSION), the hand-over to the stub for updates and backups (STUB), and the Codeplug Editor's reads and
writes (CODEPLUG, [cps-fast.md](cps-fast.md)).

The own listener's VERSION reply carries the keys the flashing and web tools read, then `stub=1`, then the build's
start language, preset, and off list ([debuglink.md](debuglink.md) 3.1; `core/hostlink/build_info.c`, shared with the
debug link):

```
proto=1
git=5277f73a
git_time=1793318400
board=XR-100UV
features=FM,DMR,...,CPS_FAST,USB,HOSTLINK
codec=xr100uv-2501
stub=1
start=en
preset=full
off=APRS_IGATE,FONT_AA
```

The tools read these keys:

| Tool | Keys |
|---|---|
| `stubtool.py`, `f4stub.py` | `board=`, to pick the stub's profile |
| `rfoundry`, the web flasher's probe | `board=`, `git=`, `codec=` |
| The web flasher's update | `start=`, `preset=`, `off=` |
| The Codeplug Editor | `features=` (`CPS_FAST`) |

A key that doesn't fit the reply is left out whole. The own listener answers INFO_CHIP with `DL_ST_UNKNOWN_CMD`; every
tool treats that as older firmware and continues without the chip UID.

## 3. Coexistence on the host port

The listener sits where the debug link does ([debuglink.md](debuglink.md) 1, `hostlink.h`). It recognizes a frame
only at a request boundary: when idle, after the 3 ms gap in a CPS session, and between MMDVM frames in hotspot mode.
The CPS, hotspot, TNC, and PPP personalities work as they do with the debug link. In the USB network personality, the
host link feeds UDP serial bytes through the same dispatch, so STUB frames arrive there too. The STM32 entry then
tells the stub that the port isn't a serial device (`handoff_flags`), and the stub enumerates again as one.

The stub entry reads no pins and doesn't touch PTT. On an HR_C7000 board whose PTT shares the cable's serial line, the
input task's shared-PTT filter (`drivers/input/sharedptt`) works from the UART's receive time, whoever parses the bytes,
so a stub upload can't key the radio.

## 4. Safety

| Check | Where |
|---|---|
| The magic `'C7SB'` on BEGIN and ENTER, the exact length, and the CRC-32 of the whole image before the jump | `stub_load.c` |
| ENTER while the radio core transmits, or while the hotspot sends, answers busy before the image is checked | `stub_entry_request` |
| The platform's image check (STM32: the stub must state that it feeds the IWDG) | `hal_system_t.stub_image_ok`, [stub-stm32f4.md](stub-stm32f4.md) 3.1 |
| 100 ms after the ENTER reply, the jump is dropped if the radio is transmitting; otherwise `tx_off`, the banner, and the jump follow | `stub_entry_service` |

The transmit check reads only the radio core's state (`radio_get_state`), not the whole view, so it fits the host link
task's stack in a build without the debug link.

## 5. Cost

This table compares default builds (debug link on) with builds without the stub entry:

| Board family | Flash | RAM |
|---|---|---|
| STM32 with an HR_C6000 | +52 B | 0 |
| STM32 with an HR_C5000 | +232 B (LTO inlining moves) | 0 |
| HR_C7000 | +12 to +16 B | 0 |

Without the debug link (an STM32 board with an HR_C5000, `CONFIG_DEBUGLINK=n` with and without `CONFIG_STUB_ENTRY`),
the stub entry costs 2,356 B of flash and 8,456 B of RAM, of which 8 KB is the STM32 staging buffer. HR_C7000 boards
load the image into the codec window and add no staging buffer.
