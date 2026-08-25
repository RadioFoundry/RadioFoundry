<!-- SPDX-License-Identifier: MIT -->

# MMDVM implementation audit (2d45a21)

An audit of the MMDVM modem implementation at commit 2d45a21 against `docs/spec/mmdvm-protocol.md`: its section 7
test list and the per-client needs of MMDVMHost, RadioDesk rfgw, the bench tools and the behavior of the old
firmware. Done by reading the code; nothing was run on a radio.

Files: `core/hotspot/{mmdvm.c,mmdvm.h,hotspot.c,hotspot.h}`, the carried-over `hotspotUplink.c`, `hotspotData.c`,
`dmrDataFrame.c`, `core/hostlink/hostlink.c`, `drivers/usb/{usb_device.c,tusb_config.h}`, `platform/stm32f4/usb.c`,
`tests/host/mmdvmTest.c`, `tools/mmdvm_probe.py`, and TinyUSB 0.21.0 (`third_party/tinyusb`) where its behavior
matters.

Severity:
- **Blocker**: the feature can't work on the radio.
- **High**: a supported client fails or gets corrupt data.
- **Medium**: a client misbehaves, loses data now and then, or fills its log.
- **Low**: differs from the spec with little effect.
- **Info**: confirmed correct, or the spec should change.

## Status (2026-10-10, branch agent/hotspot-net)

The findings below are as written at 2d45a21. Where they stand now, checked against the code and the host tests
(nothing below has been run on a radio since):

| Finding | Status | Where |
|---|---|---|
| A-1 no board wires the hotspot | fixed: the radio core registers `radio_hs_ops` (`core/radio/radio_hotspot.c`) | `radioCoreTest` test_hotspot_session |
| A-2 hotspot left while DTR is clear | fixed: only a DTR drop (an edge) ends hotspot mode (`hostlink_policy.c`) | `mmdvmTest` test_hostlink_dtr |
| A-3 uplink EMB / embedded signalling | fixed | `mmdvmTest` test_uplink_emb |
| A-4 silence fill across a superframe | fixed (modulo 6) | test_uplink_wrapped_gap |
| A-5 DMR_SHORTLC NAK, I-2 DMR_START NAK | fixed: taken silently (ACKed in SFR mode) | test_config, `hotspotSfrTest` |
| A-6 SEND_CWID NAK | fixed: ACKed in idle; the ID itself is not keyed on the DM-1701 (no `cw_id` op, counted in `hs.cwid_unsent`) | test_cwid |
| A-7 rfgw doesn't know the description | open, on rfgw's side | |
| A-8 slot 1 space / SFR hook | the hook exists (`mmdvm_ext_t.space`); simplex keeps 0, which the spec allows | test_ext_space, `hotspotSfrTest` |
| A-9 no RSSI after uplink bursts | fixed (AT1846S raw scale) | test_uplink_rssi |
| A-10 no DMR_LOST on SET_FREQ mid-call | fixed | test_retune_lost |
| A-11 SET_MODE idle doesn't stop TX | fixed | test_mode_idle |
| A-12 POCSAG refused | fixed: accepted, never transmits | test_pocsag |
| A-13 NAK / accept mismatches | fixed | test_config |
| A-14 no space margin | fixed: `hotspot_space()` keeps 2 frames back (`SPACE_MARGIN`) | code |
| A-15 `enter` inside every frame | fixed: rate limited (1 s) and posted to the radio task | test_slow_enter, test_hotspot_session |
| T-1 coverage gaps | covered by the tests named above | |

Found since:

- **H-1 (HIL test, fixed): "hotspot entered through the debug link" failed with `hs.enter_attempts=2`,
  `hs.running=0`.** The test's MMDVM client opened the port with pyserial's default DTR set; closing it dropped DTR,
  and the host link leaves hotspot mode on that edge (spec 5.1). By the time the debug link looked, the session was
  over. `hs.enter_attempts` counts every session since boot, so 2 meant two sessions that had each taken the modem,
  not two refused attempts. The radio core path was reproduced on the host (DMR channel at 467.375 MHz, the radio
  task above and below the host link task in priority): the modem is taken on the first frame or on the retry 1 s
  later. Fix: `tests/hil/test_hotspot.py` opens with DTR clear (as MMDVMHost on Windows does) and checks the slot 2
  space, `hs.running` and `hs.starts`; new view keys `hostlink.left` (why the last session ended), `hs.starts`,
  `hs.stops`.
- **SFR mode**: built behind `CONFIG_SFR` (`docs/spec/sfr.md`): spec 6.1-6.4 answered by `core/hotspot/hotspotSfr.c`,
  host-tested by `hotspotSfrTest`. The on-air repeat needs per-burst RF switching in the driver (sfr.md 3).
- **Network transport**: MMDVM over UDP 3334 in the USB network personality (`docs/spec/usb-network.md`); the DTR
  rule does not apply there, the 20 s idle timeout does.

Updated per-client view: MMDVMHost on Linux and Windows and the rfgw roamer should work for voice and data in
simplex (rfgw still needs `radio.type: opengd77` until A-7); no CW ID on the DM-1701; rfgw SFR answers correctly
but repeats only what it heard before keying until the driver work lands.

## Summary

The protocol responder is sound:
- framing, long-frame skipping and resynchronisation;
- protocol 1 version and status layouts;
- SET_CONFIG and SET_FREQ ACKs, and SET_MODE accepted before SET_CONFIG;
- downlink classification from the burst itself when the control byte is 0;
- idle and PI bursts dropped without a NAK;
- one call per transmission, keyed on the host's terminator, with back-to-back calls held behind each other;
- NAK 5 with no wraparound when the buffer is full;
- every header sent up, late entry, DMR_LOST 500 ms after the last burst;
- the TX status bit;
- nothing sent before the host's first command.

What needs fixing:
- On the radio the hotspot can't start: no board registers the radio operations (A-1).
- A host that clears DTR (MMDVMHost on Windows) knocks the radio out of hotspot mode on every poll (A-2).
- Uplink voice bursts B-F have a malformed EMB and zeroed embedded signalling (A-3).
- The uplink silence fill doesn't handle a superframe boundary (A-4).
- DMR_SHORTLC, which MMDVMHost sends on every call even in simplex, gets a NAK (A-5).
- SEND_CWID gets a NAK, which breaks rfgw whenever a CW ID is configured (A-6).
- rfgw doesn't recognize the new description (A-7, a change on rfgw's side).

Structural copying scan: no copying from the GPL trees found (section 3).

## 1. Findings

### A-1 Blocker: no board wires the hotspot to the radio core

- **Where**: `core/hotspot/hotspot.h:43` (`hotspot_set_radio`). The only caller is `tests/host/mmdvmTest.c:332`. Without
  it, `hotspot_start()` returns false (`hotspot.c:270-272`), and `mmdvm.c:281-283` tries again on every frame.
- **Spec**: 2.4 (space), 2.8 (DMR data), 7.4 and 7.5. The hotspot must take the modem and run.
- **Code**: on the radio, GET_VERSION is answered, but status always reports slot 2 space 0 and every DMR_DATA2 gets
  NAK 2. MMDVMHost logs a NAK for every frame of every network call; rfgw never sees space and its calls stall until
  its stream timeout.
- **Fix**: the radio core implements the radio operations: enter, set_freq, set_power, leave, carrier and changed.
  `enter` stops FM and the DMR call stack and hands over the modem; `carrier` comes from the receiver's noise level.
  Register them at startup on every board that builds the hotspot. Until then, the description or the docs should
  not claim the hotspot works.

### A-2 High: hotspot mode is left whenever DTR is clear

- **Where**: `core/hostlink/hostlink.c:172` (`!port->connected()`). `connected` is TinyUSB's DTR test
  (`drivers/usb/usb_device.c:170-173`). `tools/mmdvm_probe.py:41` sets DTR for this reason.
- **Spec**: 5.1. Don't gate anything on DTR. MMDVMHost on Windows opens the port with DTR cleared. Test T-USB-2.
- **Code**: with DTR clear, the first frame enters hotspot mode and is answered, and then the next pass of the task
  calls `leave_hotspot()`. That resets the configuration (`mmdvm_end`) and gives the modem back. Every frame enters
  and leaves again, so SET_CONFIG and SET_FREQ never stick and calls can't work. Linux and macOS raise DTR on open,
  so MMDVMHost and pyserial there aren't affected.
- **Fix**: never require DTR. Leave hotspot mode only after 20 s without a frame (already there), on USB unmount or
  suspend, on the user's exit, or on a DTR drop from set to clear (a POSIX host closing the port). Never because DTR
  has not been set. TinyUSB 0.21.0 keeps sending while DTR is clear (its TX FIFO only becomes overwritable,
  `cdc_device.c:427-432`), so nothing else needs DTR.

### A-3 High: uplink voice B-F carry a wrong EMB and empty embedded signalling

- **Where**: `core/hotspot/hotspot.c:148-169` (voice_to_frame).
- **Spec**: 2.8, modem to host. The 33 bytes must be a well-formed burst: correct EMB (colour code, LCSS, QR(16,7)
  parity) and the embedded signalling for B-E made from the call's LC, as the old firmware did. Test U-10.
- **Code**: the EMB's 7 information bits are set, but its 9 parity bits and all 32 embedded-signalling bits are 0.
  MMDVMHost decodes the EMB with error correction, so with zero parity it "corrects" most bursts to a wrong colour
  code or LCSS. Then:
  - Its own late entry fails, because it requires the EMB colour code to match.
  - It collects embedded fragments in the wrong order.
  - For some colour codes the decoded LCSS sequence lines up, and the all-zero fragments form a valid embedded LC
    (all-zero rows are valid Hamming codewords, and the checksum of nine zero octets is 0). MMDVMHost then accepts
    an LC of group, TG 0, source 0 and puts it into later superframes going to the network.
  - Talker alias and in-call GPS from the sender never reach the host.
- **Fix**:
  - Build B-F the way a transmitter does. The EMB is colour code, PI 0 and the LCSS of the position, with QR(16,7,6)
    parity per ETSI TS 102 361-1 B.3.2 (not in `core/dmr/fec` yet: add it there, clean, from the standard).
  - The embedded bits of B-E are the four fragments of the call's LC (`dmr_emb_lc_encode` in `core/dmr/call/dmr_lc.h`
    already makes them). F carries null embedded signalling.
  - When the modem reports a decoded embedded LC that differs (talker alias, GPS: FLCO 4-7), send that superframe's
    fragments for it, so the alias reaches the host.
  - Add a host test that decodes the uplink EMB and embedded LC with the clean decoders.

### A-4 Medium: uplink silence fill skips bursts across a superframe boundary

- **Where**: `core/hotspot/hotspot.c:722-726`. The fill runs from the first missing position up to, but not
  including, the received burst's position, by increasing position only.
- **Spec**: 2.8 and gotchas 8.5. Missed bursts are filled with silence at their positions, so the B-F sequence
  MMDVMHost checks carries on. `hotspotUplink.h:48` and `hotspotUplink.c:228-238` give the fill range modulo 6.
- **Code**: when the range wraps (E received, then F and A missed, then B), nothing is filled. B arrives right after
  E, MMDVMHost drops it and the rest of the superframe (360 ms of audio), and the bench case `rf_voice8`, `rf_long`
  or `rf_late` can lose bursts.
- **Fix**: fill every position from the first missing one, stepping modulo 6, up to the received burst's position.
  An A in the fill gets the voice sync, as now. Add a host test with a wrapped gap.

### A-5 Medium: DMR_SHORTLC gets NAK 2

- **Where**: `core/hotspot/mmdvm.c:308-311`.
- **Spec**: 2.10. MMDVMHost sends DMR_SHORTLC at the start and end of every call, in simplex too (it isn't gated on
  duplex). Accept and ignore it, with no NAK. Test D-8.
- **Code**: NAK 2. MMDVMHost logs "Received a NAK from the MMDVM, command = 0x1C, reason = 2" at least twice per call,
  and Pi-Star style dashboards show those warnings.
- **Fix**: take DMR_SHORTLC of any length silently (no ACK needed).

### A-6 Medium: SEND_CWID gets NAK 2

- **Where**: `core/hotspot/mmdvm.c:312-313`.
- **Spec**: 2.11. In idle, ACK it and send the callsign in Morse on FM, with the TX bit set while keying. In any
  other state, NAK 5.
- **Code**: always NAK 2.
  - rfgw waits for an ACK to SEND_CWID. A NAK raises an error in its roamer and SFR loops, which drops the
    connection and reconnects with backoff. This happens every time a CW ID is due (default every 15 minutes) when
    `cwid` is configured, and the ID is never sent.
  - MMDVMHost with CW ID enabled logs a NAK every time, and no station ID goes out. The operator may count on that
    ID for licensing.
- **Fix**: implement CW ID per 2.11: accept only in idle, ACK, key FM, send the Morse (about 20 wpm), keep the TX bit
  set until done, then restore the DMR setup fully (gotchas 8.13). Until then, list it as a limitation in
  `capabilities.md` and the rfgw docs ("set no cwid").

### A-7 Medium: rfgw doesn't recognize the description (change on rfgw's side)

- **Where**: `core/hostlink/hostlink.c:77-89`. The description is `OpenDigitalRadio <board name> <git version>`, for
  example `OpenDigitalRadio DM-1701 2d45a21`.
- **Spec**: 2.3.2 and section 4, row 1.
- **Code**: the string itself meets the spec: printable ASCII, under 64 bytes, and none of the MMDVM_HS-type names.
  With no SFR in this build, the missing SFR suffix is right.
- **rfgw today**: its `detect()` (`radiodesk_rfgw/mmdvm/radio.py`) returns nothing, and with `radio.type: auto`
  rfgw stops with "can't tell what 'OpenDigitalRadio ...' is: set radio.type". The workaround is `radio.type:
  opengd77` in the rfgw config, which gives the 1-header / 1-terminator profile and simplex roaming. SFR stays
  impossible: `sfr_slots()` requires the old name in the description, and the config can't override that.
- **Fix, in rfgw**:
  - (a) `detect()`: a description containing `OpenDigitalRadio` (case-insensitive) gives the same radio kind as the
    old firmware, checked before the MMDVM names.
  - (b) `sfr_slots()`: accept `OpenDigitalRadio` wherever it accepts the old name. `sfr_framing()` needs no change.
  - (c) The `config.py` error texts that name the old firmware.
  - (d) rfgw's tests: a fake modem entry with the new description, plus `detect` and `sfr_slots` cases (in
    `tests/mmdvm_fakes.py`, `test_mmdvm_hardware.py`, `test_mmdvm_sfr.py`).
  - (e) rfgw's RSSI conversion for that kind assumes the AT1846S raw scale (see A-9).
- **Firmware side**: keep `OpenDigitalRadio` as the first word, unchanged. When SFR is ported, append ` SFR
  TS<in>>TS<out> <framing>` through the extension hook. Putting the old name in the description isn't allowed:
  `tools/license_grep.sh` rejects it in sources.

### A-8 Low: slot 1 space reported as 0 in simplex (spec says 10); no SFR hook for it

- **Where**: `core/hotspot/mmdvm.c:180`.
- **Spec**: 2.4 says the constant 10, as MMDVM, MMDVM_HS and the old firmware send. 6.2 says both space bytes carry
  the outbound slot's space in SFR mode.
- **Code**: 0.
  - MMDVMHost then never writes DMR_DATA1. That is harmless, even better, since it would only get NAK 2.
  - rfgw's roamer reads only slot 2's byte.
  - The bench tools only print it.
  - **Confirmed deviation, accepted**: the spec will say "0 or 10 in simplex".
- **Problem left for SFR**: the extension interface (`mmdvm.h:123-129`) can append bytes and OR flags, but can't set
  offset 7. When SFR is ported with an outbound TS1, rfgw writes DMR_DATA1 gated on offset 7 and would never send.
- **Fix**: keep 0 in plain simplex. When SFR is on, report the outbound slot's space at offsets 7 and 8 (an
  extension hook for the space values).

### A-9 Low: no RSSI after uplink bursts

- **Where**: `core/hotspot/hotspot.c:171-179`. Frames go up with length 37.
- **Spec**: 2.8 and 5.4. Optional. The old firmware sent 2 bytes (00, then the AT1846S register 0x1B high byte), and
  rfgw converts that scale to dBm (raw − 151 at 300 MHz and up, −164 + raw × 32 / 27 below). Test U-8.
- **Code**: none. rfgw and MQTT get no signal strength for anything the radio hears; MMDVMHost shows none.
- **Fix**: when the modem reports an RSSI for the burst (`rssi_dbm` in `dmr_burst_t` is non-zero), send two bytes
  after the burst (frame length 39), big endian: 00, then the raw value on the AT1846S scale. Use the raw reading
  directly if the board can pass it, otherwise invert the formula above for the band in use. Leave them off when
  the RSSI is unknown. On radios with another RF chip, document the scale.

### A-10 Low: no DMR_LOST when SET_FREQ interrupts a call being heard

- **Where**: `core/hotspot/hotspot.c:331-355`. The uplink call is reset with no DMR_LOST.
- **Spec**: 2.7 and test C-16. If an uplink call is open, send DMR_LOST2 before the SET_FREQ ACK. MMDVMHost has no
  RF watchdog, so it keeps such a call open until its 180 s timeout.
- **Code**: silent reset. MMDVMHost never retunes in operation; rfgw retunes only after 1 s of quiet, so the effect
  is small.
- **Fix**: if the uplink call is open when SET_FREQ arrives, send DMR_LOST2 first, then reset, retune and ACK.

### A-11 Low: SET_MODE idle doesn't stop a transmission

- **Where**: `core/hotspot/mmdvm.c:245`. Only the state byte changes.
- **Spec**: 2.6 and test D-12. Idle stops new traffic for TX, ends a call in progress with a terminator within a
  superframe, clears the queue, and the TX bit clears once off air.
- **Code**: a buffered or running call carries on. MMDVMHost sends idle only after its mode hang (3 s after network
  traffic), and rfgw only after the TX bit has cleared, so neither normally hits this.
- **Fix**: on SET_MODE idle, end a call in progress (our terminator), drop what's queued, then ACK.

### A-12 Low: SET_MODE and SET_CONFIG refuse POCSAG (06)

- **Where**: `core/hotspot/mmdvm.c:194` and `:237`.
- **Spec**: 2.5 and 2.6. Accept 06, keep receiving, report POCSAG space 0 or 1, never transmit pages.
- **Code**: NAK 4. MMDVMHost with POCSAG enabled (the Pi-Star default) logs a NAK each time a page arrives from
  DAPNET. Its behavior is otherwise the same: it drops RF DMR while in its POCSAG mode either way.
- **Fix**: ACK 06 and treat it like DMR for reception; leave POCSAG space at 0.

### A-13 Low: values the spec says to NAK are accepted, and vice versa

- **Colour code**: above 15 is masked to 4 bits instead of NAK 4 (`mmdvm.c:201`; test C-4). Fix: NAK 4.
- **DMR_DATA length**: payloads longer than 34 bytes are accepted (`mmdvm.c:255`; test D-6). Fix: NAK 4 unless the
  frame is exactly 37 bytes.
- **SERIAL and TRANSPARENT**: get NAK 1 (`mmdvm.c:332-336`). The spec says drop silently (2.12): MMDVMHost sends them
  for a Nextion on the modem port and for transparent data, and logs every NAK. Fix: drop both silently, as
  QSO_INFO already is.
- **FM_PARAMS1-4**: get NAK 2. A host configured with FM sends them during startup and stops on the NAK (MMDVM_HS
  behaves the same, so this is a config error either way). The spec says ACK and ignore (2.14). Fix: ACK and ignore.

### A-14 Low: no margin in the reported space

- **Where**: `core/hotspot/hotspot.c:797-807`. Space is free ring places, minus 1 while a call is open.
- **Spec**: 2.4. Report the free count minus at least 2 for frames already written while the status was on its way.
- **Code**: a margin of 1 (the open-call terminator place) plus the host's own rule of never using its last place.
  That is enough in steady state. At the start of a call the host writes up to the full reported count in one
  clump, and frames in flight can produce a NAK 5 (a lost frame) at the edge.
- **Fix**: subtract one more place. Test D-5 with a host that writes right up to its limit.

### A-15 Medium (latent): `enter` is attempted inside every frame's handling

- **Where**: `core/hotspot/mmdvm.c:281-283`.
- **Spec**: 2.4 and 3.1. Status replies within tens of milliseconds; the version reply within about 300 ms.
- **Code**: today `hotspot_start()` fails at once (A-1). Once wired, the radio core's `enter` (stopping FM and the
  DMR call stack, a retune) runs before every reply for as long as the radio refuses (transmitting, in a menu, DMR
  disabled). If it blocks, it delays every GET_STATUS and can trip MMDVMHost's 2 s reset.
- **Fix**: `enter` must not block (post a request and return). Try it again at most about once a second, not per
  frame. Always answer GET_VERSION and GET_STATUS first.

### I-1 Info: ZLP after replies that end on a 64-byte boundary

TinyUSB 0.21.0 sends the zero-length packet itself when a transfer ends on a packet boundary
(`third_party/tinyusb/src/class/cdc/cdc_device.c:521-522`), and `cdc_write` flushes after each frame. The spec's
requirement is met. Still run T-USB-1 on the radio.

### I-2 Info: DMR_START gets NAK 2

`mmdvm.c:308-311`. MMDVMHost sends DMR_START only in duplex, and rfgw doesn't send it in SFR, so no current client is
affected. The spec says ignore it. Low priority: take it silently along with DMR_SHORTLC (A-5).

### I-3 Info: DMR_LOST timing correct

DMR_LOST2 goes up 500 ms after a call's last burst of any kind (`hotspotUplink.h:13`, checked on every
`hotspot_poll`, which runs at least every 5 ms). It isn't sent during a header run. That meets 2.9 and the bench's
0.4-0.8 s window. The host test (`mmdvmTest.c`, uplink section) checks 0 before and 1 after.

### I-4 Info: TX status bit lifetime correct

`hotspot_transmitting()` (`hotspot.c:809-812`) is set from the first buffered frame, through the modem's transmission,
until `DMR_EV_TX_DONE`. It also covers queued data lists and the modem's own transmitting state, and is derived from
the modem state machine, not the PA (gotchas 3.1). Carrier detect is reported only while not transmitting.
Confirm on air that TX_DONE comes after the terminator slot (gotchas 3.2, test D-11).

### I-5 Info: confirmed against the test list

- **Framing**:
  - F-1 to F-8 by construction: resync on bytes other than 0xE0, length below 3 resynced, long frames skipped whole,
    unknown command NAK 1.
  - F-9: nothing goes up before the first frame (the uplink runs only while the hotspot runs).
- **Version and status**: V-1 and V-3. S-1: 13 bytes, byte 3 = 02, byte 12 = 0; byte 7 differs, see A-8. S-3 and S-4
  (overflow flags reported once). S-5.
- **Configuration**: C-1, C-2 (SET_MODE DMR before SET_CONFIG is ACKed), C-3 (other mode bits ignored), C-5, C-7, C-8,
  C-9 (POCSAG frequency ignored, only the board's range applies), C-10, C-12.
- **Downlink**:
  - D-1, D-2 and D-3 (control 0 classified from sync and slot type; idle and PI headers dropped without a NAK; one
    header or eight).
  - D-4 (the old call ends with a terminator, the new one keys again).
  - D-5 (NAK 5, no wraparound, TX overflow reported once).
  - D-7, D-9 and D-10 (data lists and lone CSBKs).
  - DMR_ABORT takes no reply and drops the queue.
- **Uplink**: U-1 (every header), U-2 (late-entry header made from the embedded LC, silence from A), U-4, U-6, U-7
  (data only within a data transmission), U-9.
- **Timing**: T-4 (20 s idle timeout, configuration forgotten).
- **Extensions**: X-4 and X-5 in a build without the extension (NAK 1), and through the hook.
- **Transport**: frames are written whole from the host-link task only, so replies and uplink frames never
  interleave.

### T-1 Test coverage gaps (`tests/host/mmdvmTest.c`, `tools/mmdvm_probe.py`)

Missing:
- A host test decoding the uplink EMB and embedded LC (A-3).
- A wrapped uplink gap (A-4).
- DMR_SHORTLC, SEND_CWID, SERIAL, TRANSPARENT and FM_PARAMS answers (A-5, A-6, A-13).
- DMR_LOST before the SET_FREQ ACK during a call (A-10).
- SET_MODE idle mid-call (A-11).
- RSSI bytes (A-9).
- A host link test with DTR clear (A-2).
- A slow `enter` (A-15).
- The probe always raises DTR. Add a `--no-dtr` run (T-USB-2) and an rfgw-order run (GET_VERSION, SET_MODE DMR,
  then SET_FREQ, SET_CONFIG, SET_MODE).

## 2. Per-client verdict at 2d45a21

| Client | Verdict | Blocking items |
| --- | --- | --- |
| MMDVMHost, Linux / Pi-Star | handshake and polling OK; no calls until A-1; then voice works, with a malformed uplink EMB, NAK warnings per call and lost bursts at wrapped gaps | A-1, A-3, A-5, A-4 |
| MMDVMHost, Windows | broken: config never sticks | A-2 (and the above) |
| rfgw roamer | needs `radio.type: opengd77` until rfgw accepts the new name; no CW ID; no RSSI | A-1, A-7, A-6, A-9 |
| rfgw SFR | not available (SFR not ported; rfgw refuses the description; offset 7 hook missing) | A-7, A-8 |
| Bench tools (`hs_normal.py`, copies) | UDP transport not in this build; over serial, `rf_*` cases are at risk from A-4 and `net_*` from A-1 | A-1, A-4 |

## 3. Structural copying scan

The audited files were compared with MMDVMHost, MMDVM and MMDVM_HS (all GPL-2) by token shingles: the repository's
`tools/similarity_scan.py` at 25, 15 and 10 tokens, plus a listing of the shared runs at 12 tokens. The scan found:

- `core/hotspot/mmdvm.c` shares nothing with them, even at 10 tokens. Its command dispatch is the shape the public
  protocol itself dictates.
- `core/hotspot/hotspot.c`, `core/hostlink/hostlink.c`, `drivers/usb/usb_device.c` and `platform/stm32f4/usb.c` share
  only generic idioms at 10-15 tokens: runs of include lines, zero-filled initializers, counted loops, and a
  shift-and-xor GF(256) multiply.
- `core/hotspot/hotspotUplink.c` (carried over, ours) shares one run at 15 tokens: a comparison listing the three DMR
  data-block types. That is protocol vocabulary, not structure.
- `tests/host/mmdvmTest.c` shares nothing at 15 tokens or more.

Conclusion: no structural copying from the GPL trees in the MMDVM implementation. Hits elsewhere in the tree at 10
tokens (FEC, UI and test files) are the same kind of idiom noise and are outside this audit. The pointers are in the
dirty-side notes, not here.
