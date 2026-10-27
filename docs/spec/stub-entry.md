<!-- SPDX-License-Identifier: MIT -->

# Stub entry

A running RadioFoundry loads the host's flash-access RAM monitor (the "stub") into free RAM and jumps to it, so a radio is
updated with no key held at power-on. The web flasher, `stub/stubtool.py` (`--rfoundry-enter`) and `tools/rfoundry` use
it. Because every update after the first one depends on it, the stub entry is its own small feature, apart from the
debug link: a build without the debug link still offers it.

| Part | Where |
|---|---|
| Switch | `CONFIG_STUB_ENTRY`: on wherever `CONFIG_HOSTLINK` is (every radio), needs only `HOSTLINK` |
| The STUB state machine (portable, host tested) | `core/stubentry/stub_load.c` |
| Firmware glue: the RAM span, the transmit checks, the banner, the jump | `core/stubentry/stub_entry.c` |
| Own listener for a build without the debug link (portable, host tested) | `core/stubentry/stub_link.c` |
| Framing shared with the debug link | `core/debuglink/dl_frame.{h,c}` (no RTOS, no board) |
| Platform: span and quiesce-and-jump | `hal_system_t.stub_region / enter_stub / stub_run_addr / stub_image_ok`: the HR_C7000 platform code (`platform/hrc7000`), [stub-stm32f4.md](stub-stm32f4.md) (STM32F4) |
| Tests | `tests/host/stubEntryTest.c` (state machine), `tests/host/stubLinkTest.c` (wire bytes, both listeners) |

## 1. Wire format

The stub entry speaks the debug link's framing ([debuglink.md](debuglink.md) 2) and its command `0x1A` STUB with the
operations INFO, BEGIN, DATA and ENTER ([debuglink.md](debuglink.md) 3.18). The request and reply bytes are the
same whichever listener answers, and `stubLinkTest` replays recorded request frames and checks the replies byte for byte through both listeners. A
host can't tell the listeners apart by the STUB command; VERSION says which one answered (section 2).

## 2. Listeners

The host link has one side protocol slot (`hostlink_side_t`, first byte `0xA5`). Who takes it depends on the build:

| Build | Listener | Commands answered |
|---|---|---|
| `DEBUGLINK=y`, `STUB_ENTRY=y` (the default) | the debug link. Its dispatcher hands `0x1A` to `stub_entry_request` and calls `stub_entry_service` after every frame | every debug link command |
| `DEBUGLINK=n`, `STUB_ENTRY=y` | the stub entry's own (`stub_entry_start`, `stub_link.c`), on the same framing | PING (echo), VERSION (short), STUB; anything else `DL_ST_UNKNOWN_CMD` |
| `STUB_ENTRY=n` | the debug link, or none | STUB answers `DL_ST_UNAVAILABLE` |

So with the debug link on, nothing changes on the wire and no code is duplicated: the debug link's parser is the
listener. With it off, the stub entry brings the parser (`dl_frame.c`, about 0.4 KB) and an 89-byte reply buffer.

The own listener's VERSION carries the keys the flashing tools read, then `stub=1`:

```
proto=1
git=5277f73a
board=XR-100UV
codec=xr100uv-2501
stub=1
```

`stubtool.py` and `f4stub.py` take `board=` from it to pick the stub's profile, `rfoundry` and the web flasher's probe
take `board=`, `git=` and `codec=`. Their INFO_CHIP request gets `DL_ST_UNKNOWN_CMD`, which every tool already treats
as "older firmware" and goes on without the chip UID.

## 3. Coexistence on the host port

The listener sits where the debug link does ([debuglink.md](debuglink.md) 1, `hostlink.h`): a frame is recognized
only at a request boundary (idle; in a CPS session after the 3 ms gap; in hotspot mode between MMDVM frames), so the
CPS, hotspot, TNC and PPP personalities work as with the debug link. In the USB network personality the host link
feeds UDP serial bytes through the same dispatch, so the STUB frames arrive there too. The STM32 entry then tells
the stub that the port is not a serial device (`handoff_flags`), and the stub enumerates again as one.

The stub entry reads no pins and does not touch PTT. On the HR_C7000 radios whose PTT shares the cable's serial line,
the input task's shared-PTT filter (`drivers/input/sharedptt`) works from the UART's receive time, whoever parses the
bytes, so a stub upload can't key the radio.

## 4. Safety

| Check | Where |
|---|---|
| The magic `'C7SB'` on BEGIN and ENTER, the exact length and the CRC-32 of the whole image before the jump | `stub_load.c` |
| ENTER while the radio core is transmitting, or the hotspot is sending, answers busy before the image is looked at | `stub_entry_request` |
| The platform's image check (the STM32 IWDG: the stub must say it feeds it) | `hal_system_t.stub_image_ok`, [stub-stm32f4.md](stub-stm32f4.md) 3.1 |
| 100 ms after the ENTER reply, the jump is dropped if the radio is transmitting; otherwise `tx_off`, the board's `stub_entering` (optional: status LEDs the monitor can't drive), the banner, the jump | `stub_entry_service` |

The transmit check reads only the radio core's state (`radio_get_state`), not the whole view, so the check fits the
host link task's stack in a build without the debug link.

## 5. Cost

Default builds (debug link on), against a build without the stub entry:

| Board family | Flash | RAM |
|---|---|---|
| STM32 with an HR_C6000 | +52 B | 0 |
| STM32 with an HR_C5000 | +232 B (LTO inlining moves) | 0 |
| HR_C7000 | +12 to +16 B | 0 |

The stub entry without the debug link (an STM32 board with an HR_C5000, `CONFIG_DEBUGLINK=n` with and without `CONFIG_STUB_ENTRY`): 2,356 B of
flash and 8,456 B of RAM, of which 8 KB is the STM32 staging buffer. The HR_C7000 boards load the image into the codec window and add no staging buffer.
