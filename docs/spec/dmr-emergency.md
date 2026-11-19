<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR emergency

This page describes what the emergency key does, and what the radio shows when it hears another radio's emergency.

Sources:

- ETSI TS 102 361-2: the service options octet of the voice LC (7.2.1: bit 7 emergency).
- ETSI TS 102 361-1: CSBK layout and CRC.
- The public descriptions of the commercial emergency alarm: a CSBK with the manufacturer FID 0x10 that the receiving
  radio doesn't acknowledge on a conventional channel.

| Code | Role |
|---|---|
| `core/dmr/dmr_emergency.{h,c}` | The state machine and the alerts |
| `core/dmr/dmrDataService.c` | The alarm CSBK |
| `core/radio/radio_sm.c` | The keying, the call stack options, the tone, the settings |
| `core/ui/ui_svc_emergency.c` | The UI's key and banner |

## 1. Switch

`CONFIG_DMR_EMERGENCY` (default: follows `CONFIG_DMR`). It brings in the packet data core (`CONFIG_DMR_DATA_CORE`),
because the alarm is a CSBK sent by the data service. With the switch off:

- `dmr_emergency.h` is inline no-ops.
- The UI's emergency key says "Emergency is not set up on this radio" (the weak `ui_svc_emergency`).
- A heard emergency call is still marked (`call.emergency`, the call screen's warning color) but raises no alert.

## 2. The emergency alarm (CSBK)

| Octet | Value |
|---|---|
| 0 | 0x80 \| 0x27 (last block, opcode 0x27) |
| 1 | FID: 0x10 (the default) or 0x00 (ETSI form, setting `etsi_fid`) |
| 2 | Bit 7: the target is a radio (individual), else a talkgroup |
| 3 | 0 |
| 4-6 | The target (talkgroup or radio) |
| 7-9 | The radio in emergency |
| 10-11 | CRC-CCITT, CSBK mask 0xA5A5 |

The opcode, the use of octet 2 and the absence of an acknowledgment follow the public descriptions. The ETSI form
(FID 0x00) is the same CSBK with the ETSI FID.

- Sending: the radio sends the alarm `alarm_tries` times (default 3), at least `DMR_DATA_EMERGENCY_GAP_MS` (1 s)
  apart, each after the data service's usual channel-clear check ([dmr-data.md](dmr-data.md) 8). The conventional
  alarm has no acknowledgment, so the radio waits for none. Event `EMERGENCY_SENT` follows when all tries have gone,
  and `EMERGENCY_FAILED` when one couldn't be sent.
- Receiving: an alarm to the radio, to all call (0xFFFFFF), or to a talkgroup the radio listens to (the channel's talk
  target, the listen list) posts the event `EMERGENCY_ALARM` and raises an alert (section 4). An alarm to another
  group is only counted (`emergencyAlarmsHeard`). The radio ignores its own alarm repeated back. It accepts both FIDs
  and sends nothing back.

## 3. The emergency key

`dmr_emergency_start()` starts an emergency. The UI calls it when the user holds the emergency key
(`ui_svc_emergency(true)`); the debug link's DMR_DATA op 7 calls it too. What follows depends on the mode:

| Mode | What happens |
|---|---|
| `alarm` | The alarm, then the radio stays in emergency |
| `alarm_call` (default) | The alarm, then the emergency call |
| `call` | The emergency call at once |

The emergency call: with `hot_mic_s` > 0 (default 10 s), the radio keys itself ("hot mic") for that long, as the
1750 Hz burst keys the transmitter. A PTT press meanwhile takes the transmission over: it ends on release, not when
the hot mic runs out. With 0, the user keys it.

While in emergency (states `alarm`, `hot_mic`, `active`, until `dmr_emergency_cancel()`), every call:

- Carries the emergency service option (LC octet 1 bit 7, in the headers, the embedded LC and the terminator).
- Goes to the emergency target (`dst`, or the channel's talk target when 0, following channel changes).
- Uses impolite channel access.

The radio core reconfigures the call stack when the emergency starts or ends, or the target changes
(`dmr_emergency_config_serial`). The change takes effect at the next call.

An emergency can't start (state `failed`, until the next start) when there is no target, or when the channel isn't a
DMR channel the radio can transmit on. The data service's rule decides: not RX only, in band, not in hotspot mode, an
own ID set; a DMR VFO counts.

On a Tier III trunked system, the alarm and its cancel are statuses 126 and 124, and the call is an emergency request
([dmr-trunking.md](dmr-trunking.md) 7.8).

## 4. Alerts heard

An alarm (section 2), or a played voice call with the emergency option (to the radio or a group it listens to),
raises an alert:

- `dmr_emergency_alert()` gives the newest alert: kind (alarm or call), source, destination, group, and the number not
  yet dismissed.
- With `rx_tone` on, the radio core plays `BEEP_EMERGENCY` (a 1500 / 900 Hz warble, about 1 s).
- The UI gets `UI_EV_EMERGENCY` (from `ui_svc_poll`) and opens the emergency screen. On a radio with a vibration motor,
  it vibrates ([radio-features.md](radio-features.md) 4.1).
- The same radio's emergency again within `DMR_EMERGENCY_REALERT_MS` (30 s) of its last activity (the repeated
  alarms, the call that follows, the next over) adds no alert, dismissed or not.
- `dmr_emergency_dismiss()` clears the banner.

## 5. Settings

The settings store keeps the settings (`settings_t em_*`, [settings-store.md](settings-store.md) 3). The radio core
saves a change made through `dmr_emergency_set_settings` or the debug link's op 8. The codeplug has no emergency
settings.

| Setting | Default | Range |
|---|---|---|
| `mode` | `alarm_call` | alarm, alarm_call, call |
| `dst` / `dst_private` | 0 (the channel's talk target) | A talkgroup or a radio |
| `alarm_tries` | 3 | 1-10 |
| `hot_mic_s` | 10 | 0 (off) - 60 |
| `etsi_fid` | off (FID 0x10) | |
| `rx_tone` | on | Settings > Sound > Emergency tone |

The hot mic keys the radio without PTT. This follows the rule of no transmission the user didn't ask for
([principles.md](../ux/principles.md)): holding the emergency key is that request.

## 6. Observing it

Debug link view section `dmr_ext` ([debuglink.md](debuglink.md) 3.7): `em.state em.mode em.dst ...`, the newest alert
`em.alert*`, the counters `em.started ... em.tones`, and `data.emergency_alarms_heard / received / sent`.

Commands:

```sh
rfoundry_debug.py data emergency start|cancel|dismiss
rfoundry_debug.py data em-settings alarm|alarm_call|call [DST] [private] [etsi] [no-tone] [tries=N] [hot=S]
```

## 7. Tests

- `dmrCallTest` `test_emergency_tx`: the service option in the headers, the embedded LC and the terminator; impolite
  access.
- `dmrDataChainTest`: alarms in (alert once for repeats, another group only counted, the radio's own ignored, nothing
  sent) and out (3 CSBKs, opcode, FID, IDs, at least 1 s apart, `EMERGENCY_SENT`, state active, cancel).
- `radioCoreTest` `test_dmr_emergency`: the hot mic keys a call with the option to the talk target and releases after
  the hot mic; the user's own calls keep the option until canceled; PTT takes the hot mic over; a set target; an
  emergency call heard raises one alert and one tone, `UI_EV_EMERGENCY`; the UI's key.
- HIL (opt-in RF): `tests/hil/test_dmr_emergency.py`; pass criteria in [tests/hil/README.md](../../tests/hil/README.md).
