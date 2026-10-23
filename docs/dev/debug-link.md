<!-- SPDX-License-Identifier: MIT -->

# Debug Link

A small framed protocol on the radio's host port for bench automation and debugging. The full spec, with every
command's payload, is [docs/spec/debuglink.md](../spec/debuglink.md).

## What it does

Read the version, the diagnostics counters, the radio's state and a log ring; reset counters; take screenshots; press
keys; key PTT with a firmware time-out; send radio requests (channel, zone, VFO, frequency, power, privacy key, USB
mode and network settings); send DMR data and emergencies; APRS and SNMP settings and tests; the split duplex view;
read the chip's identity and read-only memories; inspect the settings region; reboot.

It shares the port with the MMDVM hotspot and the CPS shim (`core/hostlink` dispatches on the first byte) and never
changes the port's mode. A debug frame starts with `A5 5A`, then command, sequence number, a 16-bit length, the
payload and a CRC-16/CCITT-FALSE. Requests carry at most 64 bytes, replies at most 1024.

| Part | Where |
|---|---|
| Protocol and commands | `core/debuglink/debuglink.c`, one `debuglink_<area>.c` per command family, view sections in `debuglink_view*.c` (switch `CONFIG_DEBUGLINK`, on wherever the host link is) |
| Key injection | `core/input/input_task.c` |
| Hook into the host port | `core/hostlink/hostlink.c` |
| Chip commands | `core/debuglink/dl_chip.c`, `hal/hal_system.h`, `platform/stm32f4/sysboot.c` |
| Host tool and Python module | `tools/rfoundry_debug.py` |
| Host tests | `tests/host/debuglinkTest.c` |

## tools/rfoundry_debug.py

Needs Python 3 and pyserial. It finds the radio by its USB ID (1FC9:0094); `--port` or `$RFOUNDRY_PORT` picks one when
there are several. `rfoundry_debug.py --help` and the module docstring list everything. The commands used most:

| Command | Does |
|---|---|
| `rfoundry_debug.py version` | Firmware version, board, feature switches |
| `rfoundry_debug.py diag --watch 1` | Radio summary, voice path and board counters, with rates, once a second |
| `rfoundry_debug.py view --section radio` | The radio state as key=value (`--json` for scripts) |
| `rfoundry_debug.py screenshot -o shot.png --scale 3` | The display as PNG |
| `rfoundry_debug.py key ok`, `key quick --long`, `key enc --steps 2` | Inject a key press, a hold, encoder steps |
| `rfoundry_debug.py ptt --seconds 3` | Key PTT for at most 10 s (the firmware times out too) |
| `rfoundry_debug.py radio channel +1`, `radio freq 162.550` | Radio requests |
| `rfoundry_debug.py radio privacy moto KEY` | Load a basic privacy key (settings slot 1, kept across restarts) |
| `rfoundry_debug.py data sms ID TEXT` | Send DMR data: text, call alert, radio check, emergency; read the inbox |
| `rfoundry_debug.py aprs ...`, `snmp ...`, `split view` | APRS, the SNMP agent, the split duplex repeater |
| `rfoundry_debug.py log --follow` | The firmware's `dlog()` ring |
| `rfoundry_debug.py storage info` | Whether the settings region is ours or foreign |
| `rfoundry_debug.py reboot` | Restart (needs the chip's UID as a token; refused while transmitting) |

`chip` and `dump` read the chip's identity, option bytes and read-only memories. Dumps are written outside any git
tree: the stock bootloader is proprietary.

The HIL tests use the `DebugLink` class from the same module.

## Safety limits

Injected PTT ends after 10 s at most, and the radio's own transmit checks still apply. One injected key at a time,
each held at most 10 s, 40 ms apart. Radio requests are rate limited. Nothing in the debug link writes internal flash
or option bytes. See [debuglink.md](../spec/debuglink.md) section 4.

## Adding a command

1. Add the number to `core/debuglink/debuglink_proto.h`.
2. Add a handler in `core/debuglink/debuglink.c`, or a `debuglink_<area>.c` for a family of operations: short,
   non-blocking, reply at most 1024 bytes.
3. Add a row to the command table in [debuglink.md](../spec/debuglink.md) section 3, and a section for it.
4. Add a method and a subcommand to `tools/rfoundry_debug.py`.
5. Add a case to `tests/host/debuglinkTest.c`.

A new key in a view section needs only the renderer (`core/debuglink/debuglink_view*.c`) and the table in
debuglink.md 3.7.
