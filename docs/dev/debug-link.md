<!-- SPDX-License-Identifier: MIT -->

# Debug link tool

The debug link is a small framed protocol on the radio's host port for bench automation and debugging. This page
covers the host tool, `tools/rfoundry_debug.py`, and where the code lives. For the frame format, every command's
payload, and the limits the firmware enforces, see [debuglink.md](../spec/debuglink.md).

With the debug link, you can do the following:

- Read the version, the diagnostics counters, the radio's state, and a log ring, and reset the counters.
- Take screenshots, press keys, and key PTT with a firmware time-out.
- Send radio requests: channel, zone, VFO, frequency, power, privacy key, USB mode, and network settings.
- Send DMR data and emergencies, run the APRS and SNMP settings and tests, and read the split duplex view.
- Read the chip's identity and read-only memories, inspect the settings region, and restart the radio.

The debug link shares the host port with the MMDVM hotspot and the CPS shim (`core/hostlink` dispatches on the first
byte) and never changes the port's mode.

| Part | Where |
|---|---|
| Protocol and commands | `core/debuglink/debuglink.c`, one `debuglink_<area>.c` per command family, view sections in `debuglink_view*.c` (switch `CONFIG_DEBUGLINK`, on wherever the host link is) |
| Key injection | `core/input/input_task.c` |
| Hook into the host port | `core/hostlink/hostlink.c` |
| Chip commands | `core/debuglink/dl_chip.c`, `hal/hal_system.h`, `platform/stm32f4/sysboot.c` |
| Host tool and Python module | `tools/rfoundry_debug.py` |
| Host tests | `tests/host/debuglinkTest.c`; `debuglinkDmrDataTest.c` for the DMR data IP operations |

## tools/rfoundry_debug.py

The tool needs Python 3 and pyserial. It finds the radio by its USB ID (1FC9:0094); `--port` or `$RFOUNDRY_PORT`
picks one when there are several. `rfoundry_debug.py --help` and the module docstring list every command. The
following are the ones used most.

| Command | Does |
|---|---|
| `rfoundry_debug.py version` | Firmware version, board, feature switches |
| `rfoundry_debug.py diag --watch 1` | Radio summary, voice path and board counters, with rates, once a second |
| `rfoundry_debug.py view --section radio` | The radio state as key=value (`--json` for scripts) |
| `rfoundry_debug.py screenshot -o shot.png --scale 3` | The display as PNG |
| `rfoundry_debug.py key ok`, `key quick --long`, `key enc --steps 2` | Inject a key press, a hold, encoder steps |
| `rfoundry_debug.py ptt --seconds 3` | Key PTT for at most 10 s (the firmware times out too) |
| `rfoundry_debug.py radio channel +1`, `radio freq 438.250` | Radio requests |
| `rfoundry_debug.py radio privacy moto KEY` | Load a basic privacy key (settings slot 1, kept across restarts) |
| `rfoundry_debug.py data sms ID TEXT` | Send DMR data: text, call alert, radio check, emergency; read the inbox |
| `rfoundry_debug.py aprs ...`, `snmp ...`, `split view` | APRS, the SNMP agent, the split duplex repeater |
| `rfoundry_debug.py log --follow` | The firmware's `dlog()` ring |
| `rfoundry_debug.py storage info` | Whether the settings region is ours or foreign |
| `rfoundry_debug.py reboot` | Restart (needs the chip's UID as a token; refused while transmitting) |

`chip` and `dump` read the chip's identity, option bytes, and read-only memories. Write dumps outside any git tree:
the bootloader is proprietary, and dumps never go in the repository (see [Contributing](contributing.md#rules)
rule 2).

The HIL tests use the `DebugLink` class from the same module.

## Safety limits

The firmware limits what the debug link can do: injected PTT ends after 10 s at most, injected keys and radio
requests are rate limited, and nothing in the debug link writes internal flash or option bytes. The radio's own
transmit checks still apply. For the limits, see [debuglink.md](../spec/debuglink.md) 4.

## Adding a command

To add a debug link command, follow these steps:

1. Add the number to `core/debuglink/debuglink_proto.h`.
2. Add a handler in `core/debuglink/debuglink.c`, or a `debuglink_<area>.c` for a family of operations: short,
   non-blocking, reply at most 1024 bytes.
3. Add a row to the command table in [debuglink.md](../spec/debuglink.md) 3, and a section for it.
4. Add a method and a subcommand to `tools/rfoundry_debug.py`.
5. Add a case to `tests/host/debuglinkTest.c`.

A new key in a view section needs only the renderer (`core/debuglink/debuglink_view*.c`) and the table in
debuglink.md 3.7.
