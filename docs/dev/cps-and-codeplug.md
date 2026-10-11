<!-- SPDX-License-Identifier: MIT -->

# CPS, codeplug and settings

ODR doesn't have its own programming software. qdmr programs it through a shim that speaks the open-firmware
CPS protocol qdmr already supports, over the codeplug layout qdmr already writes.

## The CPS shim

| Part | Where |
|---|---|
| Protocol engine (no RTOS, no USB) | `core/cps/cps_openfw.c` |
| Hook to the host link and reboot | `core/cps/cps_port.c` |
| Dispatch on the host port | `core/hostlink` (first byte: `0xE0` MMDVM, `A5 5A` debug link, other bytes CPS) |
| Switch | `CONFIG_CPS_OPENFW` |
| Host tests | `tests/host/cpsTest.c`, and `tests/host/cpsSim` serving the engine on stdin and stdout |
| Test client | `tools/cps_client.py` (`info`, `read`, `write`, `verify`) |

The session ends at `C 5` or after 10 s without a byte. The shim refuses writes to the radio's own areas (settings,
journal) and keeps the radio's quick keys.

| Document | Covers |
|---|---|
| [cps-protocol.md](../spec/cps-protocol.md) | The shim: requests, reads, writes, control commands, what qdmr sends, counters, open decisions |
| [open-firmware-cps.md](../spec/open-firmware-cps.md) | The wire protocol and the codeplug layout in prose |

`tools/cps_client.py` transfers the same regions qdmr does, so it is the stand-in for qdmr in tests:

```
python3 tools/cps_client.py read backup.bin
python3 tools/cps_client.py --exec "tests/host/cpsSim --populate" info     # against the simulator
```

qdmr itself hasn't been run against ODR on a radio yet. When it is, record the qdmr version and radio model chosen in
[cps-protocol.md](../spec/cps-protocol.md) 2.5.

## The codeplug

The radio reads the open-firmware layout in place (`core/codeplug`): zones, channels, contacts, talkgroup lists, the DMR ID
database. Edits made on the radio (save a temporary change, rename, add a contact) go through the editing model in
[codeplug-native.md](../spec/codeplug-native.md) section 2 and are written by the housekeeping task as storage jobs.
A native codeplug format is planned in the same document, not built.

## Settings and safe writes

Radio settings (not the codeplug) live in a ring of records with a sector journal for power-safe writes:
[settings-store.md](../spec/settings-store.md).

| Topic | Section |
|---|---|
| The flash areas on each radio | 1 |
| Record format, versions, defaults | 2 |
| Adding a field: append only | 3 |
| Who writes and when | 3.0 |
| Privacy key slots | 3.1 |
| The sector journal | 4 |
| The foreign region guard (a radio coming from stock firmware) | 5 |

A new setting is appended to the record, gets a default, and goes into the settings reference of the user manual,
[Settings](../user/12-settings.md), with its status.
