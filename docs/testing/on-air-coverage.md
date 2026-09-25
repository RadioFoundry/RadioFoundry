<!-- SPDX-License-Identifier: MIT -->

# On-air test coverage

Which user-visible feature has an automated test on the DM-1701 and the SDR bench (`tests/hil`), and what the last
run showed. Features come from the README's "What it does" and area table, the manual's feature groups
(`docs/user/01-introduction.md`) and the build switches (`docs/build-config.md`). APRS has its own coverage notes
(`docs/spec/aprs.md` 13).

Status: **on air** = a test checks it over the air and passed; **radio** = a test checks it on the radio without
RF (or only the radio's side of an RF exchange); **partial** = only part of the feature can be checked on this
bench (why in the notes); **xfail** = the test runs and fails for a known, documented gap; **fails** = the test
runs and finds a real firmware problem (left failing on purpose); **none** = no automated test. Last run:
2026-10-10, firmware `5a13814` (main, every feature built), radio at its lowest power on 467.375 MHz.

| Feature | Test | Status | Notes |
|---|---|---|---|
| DMR group call receive, voice to the speaker | `test_dmr_rx`, `test_dmr_voice` | on air | every burst and frame counted |
| DMR late entry | `test_dmr_call_features::test_late_entry` | on air | |
| DMR private calls in | `test_dmr_call_features` | on air | to us played, to another ID not |
| DMR transmit (voice call) | `test_dmr_tx::test_dmr_tx_off_air` | on air | content strict; waveform checks not judged while the SDR clips (8 % at gain 0) |
| Talker alias receive | `test_dmr_call_features::test_talker_alias` | on air | |
| Talker alias transmit | `test_dmr_tx::test_dmr_tx_talker_alias` | on air | needs `--allow-codeplug-write`: the test channel's alias flag set over CPS, backed up and restored byte for byte; "N4NQV" decoded by the SDR and the bench modem |
| Basic privacy receive | `test_dmr_call_features::test_privacy_service_option` | on air | the bench can't scramble: drop without a key, play with one |
| Basic privacy transmit | `test_dmr_tx::test_dmr_privacy_tx` | on air (signaling) | service option 0x40 and FID 0x10 checked; the scrambling itself isn't (unknown microphone frames) |
| ETSI GPS Info in calls, in / out | `test_dmr_call_features::test_gps_info`, `test_dmr_tx::test_dmr_gps_tx` | on air | |
| Data, text messages, call alert, radio check in | `test_dmr_data` (rf) | on air | |
| Data, text messages, call alert, radio check out | `test_dmr_data` (radio_tx) | on air | 1 to 3 of 3 tries heard with a good CRC, content strict |
| ARS registration, LRRP answers | none on air | none | need a presence server / dispatcher on air; host-tested (`dmrDataServiceTest`, `dmrDataChainTest`) |
| Emergency alarm and call in | `test_dmr_emergency` (rf) | on air | |
| Emergency alarm and call out, the key | `test_dmr_emergency` (radio_tx) | on air | alarm tries counted like the data tries |
| Hotspot protocol | `test_hotspot` | radio | |
| Hotspot voice and data, both ways | `test_hotspot_voice` | on air | `hotspot_cases.py` rf_voice8, rf_data_unconf, net_voice, net_data |
| Hotspot CW ID | `test_hotspot_cwid` | on air | the Morse tone itself checked since the tone fix |
| SFR (hotspot SFR mode) | `test_hotspot_sfr` | partial, xfail | mode switching and the first repeat on TS2 pass; the whole call needs RX between our bursts (`docs/spec/sfr.md` 3) |
| Split duplex repeater (experimental) | `test_split_duplex` | fails | protocol passes; TX half: no same-slot spacing in the (clipped) capture; RX half: epoch residual 13.9 ms (limit 5 ms), 0 of 45 bursts labeled |
| USB network adapter | `test_usb_network` | radio | DHCP, ping and the debug link over UDP, back to serial (macOS 26). With `ff6268b` the Mac once never configured the adapter and the radio stayed in network mode; since `5a13814` it falls back to serial after 10 s |
| TCP services (HTTP, KISS over TCP, rigctld, APRS-IS) | `test_tcp_services` | radio | not run yet; host-tested against a scripted TCP peer (`netSvcTest`, `tcpTest`) |
| IP over DMR | none | none (rig) | needs a second station that speaks IP over DMR data |
| Roaming (settings, zone flag, roam now) | `test_roaming` radio tests | radio | |
| Roaming to the bench repeater, beacon surveys | `test_roaming` rf tests | none (rig) | need a repeater-pair channel and a second frequency; only 467.375 MHz is approved |
| FM receive | `test_fm::test_noaa_receive` | on air | |
| CTCSS / DCS encode | `test_fm_tx` | on air | |
| CTCSS / DCS decode | `test_fm::test_rx_tone_squelch` | partial | untoned NOAA stays muted with a tone required; a toned FM source would be needed for the positive case |
| 12.5 / 25 kHz deviation | `test_fm_tx::test_fm_tx_bandwidth` | fails | 25 / 12.5 kHz CTCSS deviation ratio 4.0 (nominally 2) |
| 1750 Hz burst | `test_fm_tx::test_fm_tx_tone_burst` | on air | 2 s of 1750.0 Hz at 1.43 kHz peak; silent before `ff6268b` (voice source in the wrong register bits), 3.3 kHz peak at `ff6268b` |
| Roger beep | `test_fm_tx::test_fm_tx_roger_beep` | on air | 1200 / 900 Hz at the end of the over, none when off; silent before `ff6268b` |
| TX inhibit, TOT warning and time-out | `test_radio_features` | radio | the radio's state, not the RF |
| Power levels | `test_fm_tx::test_fm_tx_power_levels` | none (rig) | skipped: the RTL-SDR clips at gain 0 even at 50 mW; needs an attenuator |
| VOX | `test_radio_features::test_vox_quiet` | partial | no key-up in silence, none from a beep in the speaker; a voice key-up needs sound at the microphone |
| Battery saver | `test_radio_features::test_battery_save*` | on air | cycles when idle, MONITOR and a bench call wake it |
| Zone scan stopping for calls | `test_radio_features::test_scan_stops_on_dmr_call` | on air | |
| VFO scan | `test_radio_features::test_vfo_scan*` | on air | holds on the bench's carrier |
| Dual watch | `test_radio_features::test_dual_watch*` | on air | holds on the channel with the call |
| qdmr reads | `test_cps` | radio | |
| qdmr writes | `test_cps::test_cps_write_dry_run`, `test_cps_write_restore` | radio | the dry run first; the write only with `--allow-codeplug-write` |
| RadioDesk alert call | `test_radiodesk_alert` | on air | the radio keys 1 s first so RadioDesk has a route; 198 of 198 frames played, none concealed |

## Before this round

| Feature | Before |
|---|---|
| DMR transmit | test failed on SDR clipping and the overloaded bench modem's FEC corrections |
| Data and emergency out | tests needed all 3 tries heard; the overloaded bench modem heard 1-2 (and read others with bit errors) |
| Talker alias transmit, privacy transmit | no test (the alias check was skipped: the test channel sends none) |
| Hotspot voice round trip | manual bench script only (`dmr_bench.py run H1`) |
| SFR, USB network, VOX, split duplex | not in the radio's build; tests skipped |
| Hotspot CW ID | passed on a keyed carrier with no tone in it |
| CTCSS / DCS decode | no test |
| 1750 Hz burst, roger beep | radio state only (`tone_burst=1`, `roger=1`): passed while nothing went on air |
| Power levels, battery saver | no test |
| VFO scan, dual watch | radio only (no signal to stop for) |
| qdmr writes | test never run; no dry run |
| RadioDesk alert | timed out waiting for the call to end: it waited for `call.src` to clear, which the view never does |
