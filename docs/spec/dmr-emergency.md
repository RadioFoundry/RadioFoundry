# DMR emergency

What the emergency key does, and what the radio shows when it hears someone else's emergency. Status: built and
host-tested on the DM-1701 (`dmrDataChainTest`, `dmrCallTest`, `radioCoreTest`); not yet checked on air
(`tests/hil/test_dmr_emergency.py`).

Sources: ETSI TS 102 361-2 (the service options octet of the voice LC, 7.2.1: bit 7 emergency), TS 102 361-1 (CSBK
layout and CRC), the public descriptions of Motorola's emergency alarm (a CSBK with the Motorola FID 0x10 that the
receiving radio does not acknowledge on a conventional channel). Code: `core/dmr/dmr_emergency.{h,c}` (the state
machine and the alerts), `core/dmr/dmrDataService.c` (the alarm CSBK), `core/radio/radio_sm.c` (the keying, the
call stack options, the tone, the settings), `core/ui/ui_svc_emergency.c` (the UI's key and banner).

## 1. Switch

`CONFIG_DMR_EMERGENCY` (default: follows `CONFIG_DMR`). It brings in the packet data core
(`CONFIG_DMR_DATA_CORE`), because the alarm is a CSBK sent by the data service. Off: `dmr_emergency.h` is inline
no-ops, the UI's emergency key says "Emergency is not set up on this radio" (the weak `ui_svc_emergency`), a heard
emergency call is still marked (`call.emergency`, the call screen's warning color) but raises no alert.

## 2. The emergency alarm (CSBK)

| Octet | Value |
|---|---|
| 0 | 0x80 \| 0x27 (last block, opcode 0x27) |
| 1 | FID: 0x10 (Motorola, the default) or 0x00 (ETSI form, setting `etsi_fid`) |
| 2 | bit 7: the target is a radio (individual), else a talkgroup |
| 3 | 0 |
| 4-6 | the target (talkgroup or radio) |
| 7-9 | the radio in emergency |
| 10-11 | CRC-CCITT, CSBK mask 0xA5A5 |

- Ours: sent `alarm_tries` times (default 3), at least `DMR_DATA_EMERGENCY_GAP_MS` (1 s) apart, each after the data
  service's usual channel-clear check (dmr-data.md 8). No acknowledgement is defined for the
  conventional alarm that we could verify, so none is waited for: event `EMERGENCY_SENT` when all tries have gone,
  `EMERGENCY_FAILED` when one couldn't be sent.
- Heard: an alarm to us, to all call (0xFFFFFF), or to a talkgroup we listen to (the channel's talk target, the
  listen list) posts the event `EMERGENCY_ALARM` and raises an alert (section 4). One to another
  group is only counted (`emergencyAlarmsHeard`). Our own alarm repeated back is ignored. Both FIDs are accepted.
  Nothing is sent back.

Open: the opcode, the use of octet 2 and the absence of an acknowledgement follow public descriptions, not a
standard text we have; to be checked against a commercial radio on the bench. The ETSI form (FID 0x00) is an
assumption.

## 3. The emergency key

`dmr_emergency_start()` (UI: hold the emergency key, `ui_svc_emergency(true)`; debug link DMR_DATA op 7). Per the
mode:

| Mode | What happens |
|---|---|
| `alarm` | the alarm, then the radio stays in emergency |
| `alarm_call` (default) | the alarm, then the emergency call |
| `call` | the emergency call at once |

The emergency call: with `hot_mic_s` > 0 (default 10 s) the radio keys itself ("hot mic") for that long, as the
1750 Hz burst keys the transmitter; a PTT press meanwhile takes the transmission over (it ends on release, not when
the hot mic runs out). With 0 the user keys it.

While in emergency (states `alarm`, `hot_mic`, `active`, until `dmr_emergency_cancel()`): every call carries the
emergency service option (LC octet 1 bit 7, in the headers, the embedded LC and the terminator), goes to the
emergency target (`dst`, or the channel's talk target when 0, following channel changes), and uses impolite channel
access. The radio core reconfigures the call stack when the emergency starts, ends or the target changes
(`dmr_emergency_config_serial`); it takes effect at the next call.

It can't start (state `failed`, until the next start) when the channel is not a DMR channel we may transmit on (the
data service's rule: not RX only, in band, not a VFO, not in hotspot mode, an own ID) or there is no target.

## 4. Alerts heard

An alarm (section 2) or a voice call with the emergency option that is played (to us or a group we listen to)
raises an alert: `dmr_emergency_alert()` gives the newest (kind alarm / call, source, destination, group, the
number not yet dismissed), and with `rx_tone` on the radio core plays `BEEP_EMERGENCY` (a 1500 / 900 Hz warble,
about 1 s). The UI gets `UI_EV_EMERGENCY` (from `ui_svc_poll`) and opens the emergency screen. The same radio's
emergency again within `DMR_EMERGENCY_REALERT_MS` (30 s) of its last activity (the repeated alarms, the call that
follows, the next over) adds no alert, dismissed or not. `dmr_emergency_dismiss()` clears the banner.

## 5. Settings

Kept in the settings store (`settings_t em_*`, `settings-store.md` 3); the radio core saves a change made through
`dmr_emergency_set_settings` (or the debug link's op 8). Not in the codeplug yet.

| Setting | Default | Range |
|---|---|---|
| `mode` | `alarm_call` | alarm, alarm_call, call |
| `dst` / `dst_private` | 0 (the channel's talk target) | a talkgroup or a radio |
| `alarm_tries` | 3 | 1-10 |
| `hot_mic_s` | 10 | 0 (off) - 60 |
| `etsi_fid` | off (Motorola FID 0x10) | |
| `rx_tone` | on | |

The hot mic default (10 s) and keying it at all are owner decisions (`docs/ux/principles.md`: no transmission the
user didn't ask for; holding the emergency key is that request).

## 6. Observing it

Debug link view section `dmr_ext` (`debuglink.md` 3.7): `em.state em.mode em.dst ...`, the newest alert
`em.alert*`, the counters `em.started ... em.tones`, and `data.emergency_alarms_heard / received / sent`.
`odr_debug.py data emergency start|cancel|dismiss`, `odr_debug.py data em-settings alarm|alarm_call|call [DST]
[private] [etsi] [no-tone] [tries=N] [hot=S]`.

## 7. Tests

- `dmrCallTest` `test_emergency_tx`: the service option in the headers, the embedded LC and the terminator, impolite
  access.
- `dmrDataChainTest`: alarms in (alert once for repeats, another group only counted, our own ignored, nothing sent)
  and out (3 CSBKs, opcode, FID, IDs, at least 1 s apart, `EMERGENCY_SENT`, state active, cancel).
- `radioCoreTest` `test_dmr_emergency`: the hot mic keys a call with the option to the talk target, released after
  the hot mic, the user's own calls keep the option until cancelled, PTT takes the hot mic over, a set target, an
  emergency call heard raises one alert and one tone, `UI_EV_EMERGENCY`, the UI's key.
- HIL (opt-in RF): `tests/hil/test_dmr_emergency.py`, pass criteria in `tests/hil/README.md`.
