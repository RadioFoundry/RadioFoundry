<!-- SPDX-License-Identifier: MIT -->

# Hardware-in-the-loop tests

pytest tests that drive a real radio over its USB debug link (`docs/spec/debuglink.md`, `tools/odr_debug.py`) and,
when allowed, the SDR bench as the other station. They are meant to be run again and again: every test leaves the
radio as it found it, and nothing transmits unless the run asks for it.

## Running

Needs Python 3 with `pytest` and `pyserial` (nothing else), and a radio running firmware built with
`CONFIG_DEBUGLINK` (the default) on USB.

```
python3 -m pytest tests/hil -v -rs                    # radio only, never transmits
python3 -m pytest tests/hil -v -rs --radio-port /dev/cu.usbmodem2101
python3 -m pytest tests/hil -v -rs --allow-rf --allow-service-stop          # adds the RF tests
python3 -m pytest tests/hil -v -rs --allow-rf --allow-service-stop --ambe ~/recordings/speech.ambe
python3 -m pytest tests/hil -k ui --shots /tmp/odr-shots                    # keep the screenshots
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-service-stop -k "dmr_tx or fm_tx"   # radio TX off air
python3 -m pytest tests/hil -v -rs -s --allow-rf --allow-radiodesk -k radiodesk         # RadioDesk alert call
python3 -m pytest tests/hil -v -rs -s -k cps --allow-codeplug-write --allow-reboot      # CPS write + restart
```

Without a radio every test is skipped with the reason (no failure). The radio is found by its USB ID 1FC9:0094;
`--radio-port` or `$ODR_PORT` picks one when there are several.

### Opt-in flags

| Flag | Allows |
|---|---|
| (none) | radio-only tests: version, screenshots, keys, FM receive, MMDVM requests that don't transmit |
| `--allow-rf` | tests that transmit (marker `rf`: the SDR bench's transmitter; marker `radio_tx`: the radio itself) |
| `--allow-usb-switch` | tests that switch the radio's USB personality to the network adapter and back (marker `usb_switch`) |
| `--allow-service-stop` | tests that stop the SDR bench's gateway service (marker `bench_service`) to use its transmitter and receiver |
| `--allow-codeplug-write` | tests that write the codeplug over CPS (marker `codeplug_write`): the qdmr regions are read and saved to the test's tmp directory first, one channel name is changed, and the backup is written back and verified in a `finally` block |
| `--allow-reboot` | tests that restart the radio (marker `reboot`) through the debug link's REBOOT (mode 0) |
| `--allow-radiodesk` | tests that publish to the RadioDesk sandbox broker (marker `radiodesk`), whose gateway then calls the radio; also needs `--allow-rf` and `$ODR_RADIODESK_MQTT_*` |

The RF tests need both flags. The gateway service is stopped through `tools/bench/dmr_bench.py`'s lease: a detached
process on the SDR bench stops it, waits while a hold file exists (at most 300 s) and starts it again whatever
happens to pytest or the SSH link; the test removes the hold file in a `finally` block and checks that the service
is back.

Other options: `--tx-power` (bench transmitter level 0-255, default 40), `--dmr-freq` / `--dmr-cc` / `--dmr-tg`
(DMR test channel, default 461.6875 MHz, color code 2, talkgroup 9), `--noaa-mhz` / `--rssi-floor` (FM test),
`--ambe` / `$ODR_HIL_AMBE` (speech recording for the voice test), `--shots DIR`, `--sdr-gain` (RTL-SDR gain for
raw captures, default 5 dB; `dmr_bench.py run C0` picks it), `--sdr-ppm` (the SDR's own frequency error: carrier
offsets are corrected by it), `--max-foff` (largest carrier offset the TX tests accept, default 500 Hz), `--fm-mhz`
(FM TX test frequency, default 461.6875), `--radiodesk-freq` / `--radiodesk-src` / `--radiodesk-text` (the alert
test's channel, RadioDesk's caller ID to check, the text it speaks).

SDR bench names come from the environment (`docs/testing/dmr-bench.md` 1): `$ODR_SDR_BENCH_HOST` (ssh host or
alias, default `sdr-bench`), `$ODR_SDR_BENCH_SERVICE`, `$ODR_SDR_BENCH_DIR`, `$ODR_SDR_BENCH_PY`. The RadioDesk
sandbox credentials only ever come from `$ODR_RADIODESK_MQTT_HOST`, `_PORT`, `_USER`, `_PASSWORD`, `_TLS`; never
put them in a file in the repository.

## Rig assumptions

- The radio on USB, with a codeplug whose current zone holds a DMR channel on the DMR test frequency (the tests
  look for it by frequency and mode, and put the radio back on its channel afterwards).
- The SDR bench reachable by SSH without a password (passwordless sudo for `systemctl` there), with the bench tools
  installed (`tools/bench/dmr_bench.py sync`; the FM tests copy `tools/sdr_fm_check.py` there themselves). Its
  gateway configuration has Ch 1 at 467.375 MHz and Ch 15 at 461.6875 MHz, color code 2; the tests use Ch 15.
- For the TX tests: the radio has a DMR ID, a DMR channel on the test frequency with a talk target, and may
  transmit on `--fm-mhz` in its FM VFO; it runs on its lowest power, into an antenna or a dummy load at bench
  distance.
- For the RadioDesk test: a RadioDesk sandbox whose gateway (on the SDR bench) calls on `--radiodesk-freq`, and
  `pip install paho-mqtt`.
- An antenna or attenuator so the bench transmitter at power 40 doesn't overload the radio.
- For `test_fm`: NOAA weather radio audible at the bench (162.550 MHz by default; change it with `--noaa-mhz`).
- Speech recordings for `test_dmr_voice` stay outside the repository; give the path with `--ambe` or
  `$ODR_HIL_AMBE`. Never commit a recording.

## Tests

| Module | What | Needs |
|---|---|---|
| `test_dm32.py` | DM-32UV bring-up: display geometry and features, data flash JEDEC ID and factory calibration, battery reading, FD6818 reads alive, DMR timeslot interrupts, stock codec merged (docs/testing/dm32-bringup.md); skipped on other boards | DM-32 on its programming cable: `--radio-port /dev/cu.usbserial-...` |
| `test_boot.py` | version / features, ping latency, screenshot geometry, firmware clock vs wall clock (no stalls), view sections, diagnostics clock, error replies (unknown command, bad CRC, oversized request), log ring | radio |
| `test_ui.py` | UP / DOWN change the channel on Home; OK opens the Hub (6 rows), digits open its destinations, BACK goes up, long BACK goes Home from depth 3; Quick settings open and close (BACK and QUICK); digits on Home start Find (`ui.query`); long QUICK locks and unlocks; long # opens Recent; diagnostics through Tools (Hub 5, 7); screenshots `ui_quick.png` and `ui_find.png`; injected keys count as activity; the Quick tiles for now (DMR: slot `dmr.ts` + temp 0x20, color code `dmr.cc` + 0x40, mode + 0x80, hear all `dmr.hear_all_latch`; FM: monitor latch, VOX `vox_on`), each undone by BACK on Home or a second press; the picker's focus row (`dmr.focus_id`, cleared by long BACK); Settings long press time (`input.long_ms`, set back) and auto lock 30 s (`ui.locked` after 30 s, set back to Off); opt-in: the timeslot saved to the channel and back (`storage.job_busy`, `temporary`), deleting the newest message (`$ODR_HIL_DELETE_MESSAGE=1`), factory reset with a restart (`$ODR_HIL_FACTORY_RESET=1`). State is read from the link section (`ui.screen`, `ui.depth`, `ui.count`, `ui.locked`, `ui.query`, `ui.toast`) | radio; `--allow-codeplug-write`, `--allow-reboot` |
| `test_fm.py` | VFO on NOAA weather: squelch opens or RSSI above the floor within 10 s (receive only) | radio with the FM VFO |
| `test_hotspot.py` | MMDVM GET_VERSION / GET_STATUS answered (client with DTR clear, so its close doesn't end the session), the hotspot takes the modem (slot 2 space, `hs.running`, `hs.starts`), the debug link answers in hotspot mode, hotspot left through the debug link (no transmission) | radio with the hotspot |
| `test_hotspot_sfr.py` | MMDVM SFR mode on the radio's channel: the version suffix, duplex with RX = TX turns it on (121-byte status, both space bytes 16, `sfr.on`), command 0xD5 ACK / NAK 4, simplex turns it off; sends nothing and expects a quiet channel (`sfr.tx_starts` unchanged) | `--allow-rf` (a call heard would be repeated), firmware with `CONFIG_SFR` |
| `test_usb_network.py` | USB network personality: re-enumerates as CDC-NCM, DHCP gives the host 12.<ID>, ping and the debug link over UDP 3334 at the radio's address, back to serial | `--allow-usb-switch`, firmware with `CONFIG_DMR_IP_GATEWAY` |
| `test_dmr_rx.py` | bench sends 2 group calls (10 superframes, 8 headers, 3 terminators each); counter deltas | `--allow-rf --allow-service-stop` |
| `test_dmr_voice.py` | bench sends a speech recording; every frame reaches the speaker path | the same, and `--ambe` |
| `test_dmr_tx.py` | radio keys 3 s on DMR (debug-link PTT); SDR capture + sdrmon analysis and the bench modem's listener check src ID, TG, CC, headers / voice / terminator, timing, carrier offset | `--allow-rf --allow-service-stop` |
| `test_fm_tx.py` | radio FM VFO keys 3 s with CTCSS 100.0, DCS D023N, and at 12.5 / 25 kHz; `tools/sdr_fm_check.py` on an SDR capture | `--allow-rf --allow-service-stop` |
| `test_radiodesk_alert.py` | a RadioDesk sandbox alert (MQTT `ESS/RadioDesk/Alert/<id>/voice`) becomes a private call; the radio shows and plays it; `AlertStatus` reports | `--allow-rf --allow-radiodesk`, `$ODR_RADIODESK_MQTT_*` |
| `test_cps.py` | legacy CPS over USB with `tools/cps_client.py`: radio info (type 8 / 10, struct 3), two reads of the qdmr regions identical, the current channel's record matches the view, a channel change saved by the settings store within 2 s (the `storage` view section); opt-in: one channel renamed, read back and the backup restored and verified; the channel position survives a restart | radio; `--allow-codeplug-write`, `--allow-reboot` |
| `test_dmr_call_features.py` | bench sends a private call to us and to another ID, an emergency call, a talker alias, a late entry (no header, 8 bursts left out), a call with the privacy service option (without and with a key), a call with the PI bit; plus the talk target / privacy key requests (radio only) | `--allow-rf --allow-service-stop` (the last test: radio) |
| `test_radio_features.py` | the `features` view section, settings round trip, beep counters (does PCM play in this mode), zone scan + cancel, nuisance delete, VFO band scan, dual watch (radio only); TX inhibit, TOT warning and time-out, roger beep, 1750 Hz burst on the FM VFO at lowest power (`radio_tx`); the scan holds on a bench DMR call (`rf`) | radio; `--allow-rf` (radio_tx); `--allow-rf --allow-service-stop` (the last) |
| `test_dmr_emergency.py` | DMR emergency: the bench sends an emergency alarm CSBK and an emergency voice call to the channel's talkgroup (alerts raised); the radio sends its alarm (3 CSBKs) and an emergency call with the hot mic (the bench listens); the emergency settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio), firmware with `CONFIG_DMR_EMERGENCY` |
| `test_hotspot_cwid.py` | MMDVM SEND_CWID in hotspot mode: the radio keys the callsign in Morse on FM on the hotspot TX frequency (the SDR bench records), then the hotspot's DMR set-up is back | `--allow-rf --allow-service-stop` |
| `test_dmr_data.py` | DMR data: bench sends unconfirmed and confirmed data, a TMS and an ETSI text message, a call alert and radio checks to us (the radio's answers listened for); the radio sends a call alert, a radio check (Motorola and ETSI forms) and a text message (TMS and ETSI) to the bench (the bench modem listens); the data settings request (radio only) | `--allow-rf --allow-service-stop` (the first test: radio) |

### Pass criteria and tolerances

`test_dmr_rx`: voice bursts exactly 120; voice LC headers 14 to 16 (the first header of a call can fall in the
receiver's acquisition); terminators 5 to 6; CRC errors 0; the call stack saw exactly 2 calls.

`test_dmr_voice`: frames into the AMBE voice layer = the frames sent (the recording padded to whole superframes of 18
frames); frames played at least sent - 3 (`AMBE_VOICE_PRIME`) and at most sent; no late entry; zero concealed,
muted, lost or overflowed frames in the voice layer, zero lost frames in the call stack, zero audio overruns and
dropped blocks. When it fails, the printed deltas show which layer lost frames: modem (`modem.voice`, `modem.crc`),
call stack (`call.rx_voice_bursts`, `call.rx_lost_frames`), voice layer (`voice.rx_in`, `voice.rx_frames`,
`voice.rx_concealed`, `voice.rx_underruns`, `voice.rx_overflows`) or audio (`audio.ov`, `audio.dr`).

`test_dmr_tx`, `test_fm_tx`, `test_radiodesk_alert`, `test_radio_features`: the criteria are in each module's
docstring (dmr_bench.py case T1's checks for DMR TX; CTCSS within 0.5 Hz and 200-900 Hz deviation at 12.5 kHz, DCS
with the right polarity, the 25 / 12.5 kHz deviation ratio 1.5-2.6 for FM; a private call to us on Home, played with
at most 5 % concealed frames, for the alert; for the radio features: warning 2.5-4.5 s and time-out 5.5-7.5 s with
tot_s 6 / tot_warn_s 3, roger=1 seen after release while still on air, beep.blocks grows when beep.played does).

Screenshots: every SCREENSHOT request freezes the UI's redraws for a moment (`docs/spec/debuglink.md` 3.3), so a
capture taken right after a key can still show the old screen. `Radio.screenshot()` waits out the previous
capture's hold, and `Radio.shot_until(pred)` repeats until the picture is what the test waits for (and not blank);
UI tests use `shot_until`.

`test_dmr_call_features`: one call heard per RF test (`call.rx_calls` delta 1). Private to us: played, `last.to_us=1`,
the reply target is the bench ID. Private to another ID: counted, 0 bursts played. Emergency: `last.emergency=1`.
Talker alias: `call.alias` equals the text sent, `call.rx_alias_complete` 1. Late entry: `call.rx_late_entries` 1,
18 to 28 voice bursts played of the 28 sent after the gap. Privacy option without a key: 0 bursts played, 20 or more
dropped as encrypted; with a key: none dropped, 22 or more played. PI bit: only that one call is heard (the
DM-1701's modem driver clears PI on voice, so it plays as clear).

`test_dmr_data` (counters from the `dmr_data` section, `docs/spec/debuglink.md` 3.7): unconfirmed data:
`data.rx_headers` 1, `data.rx_packets` 1, `data.rx_for_us` 1, `data.rx_blocks` 4 or more, CRC errors and stale
blocks 0. Confirmed: `data.rx_confirmed` 1, `data.responses_sent` 1, `data.tx_failed` 0. Text message:
`data.sms_received` 1, `data.tms_acks_sent` 1, inbox entry 0 is the text from the bench ID, unread until
MARK_READ. Call alert: `data.call_alerts_received` 1, `data.csbk_answers_sent` 1, `data.event=call_alert` from the
bench ID. Radio check: with `radio_check_reply` off (the default) counted and not answered; on, answered
(`data.event=radio_check_answered`). Radio TX: call alert and radio check end `failed` after exactly 3 tries heard
by the bench modem (the bench doesn't answer); the text message (no ACK asked) ends `ok` and its data header (to
the bench, from us) is heard. An answer to the bench that the bench modem misses only warns: the radio sends it
within a few frames of the bench's last burst, inside the bench modem's TX to RX turnaround.
ETSI forms (`dmr_ext` section): an ETSI text message in: `data.sms_received` 1, `data.etsi_sms_received` 1,
`data.responses_sent` 1, no TMS ACK, inbox entry 0 with format `etsi`. ETSI text out (no response asked): `ok`,
`data.etsi_sms_sent` 1, a data header with DPF 13 and SAP 10 to the bench heard. Radio check in the ETSI form: 3
tries heard, each with FID 0x00.

`test_dmr_emergency` (`dmr_ext` section): an alarm in (opcode 0x27, FID 0x10, to the channel's talkgroup):
`data.emergency_alarms_received` 1 or more, `em.alert=1`, `em.alert_kind=alarm` from the bench ID, `data.event=
emergency_alarm`, nothing transmitted (`data.tx_lists` unchanged). An emergency voice call in (another source):
`em.alerts_call` +1, `em.alert_kind=call`, `last.emergency=1`. Alarm out (alarm mode, 3 tries):
`data.emergency_alarms_sent` +3, `em.state=active`, 3 CSBKs heard with FID 0x10, the talkgroup in octets 4-6 and us
in 7-9, at least 0.9 s apart. Emergency call out (call mode, 3 s hot mic, needs the codec): the radio keys by itself
and unkeys, the LCs heard to the talkgroup from us all carry service options bit 0x80, `em.hot_mic_keyed` 1 or more.

`test_hotspot_cwid` (DM-1701 or another board with `tx_tone_key`): SEND_CWID "N0CALL" (73 units) ACKed, the
GET_STATUS TX flag set while keying and clear after, `hs.cw_sent` +1, `hs.cw_wpm=20`, `hs.cw_last_ms` within 10 %
of 150 + 73 x 60 + 150 = 4680 ms, one keyed segment off air.

Counter names: `Radio.counters()` merges the diagnostics lines (`modem.*` from the DM-1701's HR_C6000 lines, `call.*`
and `voice.*` short names, `audio.*`) with the `dmr_stats` and `hotspot` view sections (full names such as
`voice.rx_frames`, `hs.net_frames`).

## Writing a test

- Use the `radio` fixture (an `odr_hil.Radio`); the autouse `restore_radio` fixture snapshots the radio's channel or
  VFO, zone, power, squelch and monitor before the test and puts them back afterwards, and releases PTT. A test that
  changes the VFO's frequency wraps that in `with radio.vfo_session():` (the VFO frequency isn't visible while on a
  channel).
- Drive the radio with `radio.press("up")`, `radio.long_press("back")`, `radio.link.key_hold("monitor", 2000)`,
  radio requests (`radio.link.vfo_set(hz)`, `radio.link.zone_pos(n)`, ...) and `radio.ptt_for(seconds)` (at most 10 s;
  the firmware releases PTT by itself at its time-out). Call `radio.go_home()` first for UI tests.
- Check state with `radio.wait_for(lambda v: ..., timeout)` on the view (key=value strings, `docs/spec/debuglink.md`
  3.7), never with fixed sleeps alone; counters with `radio.reset_counters()` then `radio.counters()` deltas.
- Anything that transmits gets `@pytest.mark.rf` (the radio: `radio_tx`); anything that uses the SDR bench's
  transmitter or receiver also gets `@pytest.mark.bench_service` and runs inside `with sdr_bench.lease():`
  (`transmit` / `voice_call`, `listen_start` / `listen_result`, `capture_start` / `capture_wait`, then
  `analyze_dmr` / `analyze_fm` after the lease, with the gateway back up).
- Never sleep and capture: use `radio.shot_until(...)` for screenshots that should show a new screen.
- Document the pass criteria and tolerances in the module docstring and in the table above.
