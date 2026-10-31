<!-- SPDX-License-Identifier: MIT -->

# DMR voice calls

What the voice side of the DMR stack does above the modem (`dmr-modem.md` covers the air interface and the modem
contract). Code: `core/dmr/call` (the call stack), `core/dmr/dmrPrivacy.c` (basic privacy), `core/radio/radio_sm.c`
(the radio core: channel, contacts, settings), `core/codeplug` (contacts, settings, the DMR ID database). Protocol
references are ETSI TS 102 361-1 and -2.

## 1. Who we hear

A voice call on our timeslot and color code starts from a voice LC header or by late entry (section 4). It is played
when it passes the filters, and shown either way:

| Call | Played when |
|---|---|
| Group (FLCO 0) | its talkgroup is the talk target's, all call (0xFFFFFF), in the channel's RX group list (listen list), the reply target's during the reply window, or hear all, "digital monitor" in the UI (the hear-all latch: the MONITOR key's press toggles it on a DMR channel, its release does nothing (#19); Quick 5; `radio_req_dmr_hear_all`; until the channel changes, DMR stops (a mode change), or BACK on Home; the end of a call doesn't end it) |
| Private (FLCO 3) | addressed to our ID (the channel's DMR ID override, else the radio's), or all call, or hear all |
| Our own ID as source | never (our call echoed by a repeater) |
| Encrypted | only with a privacy key, and never after a PI header (section 5) |

A call that doesn't pass is shown as busy ("not played") while it lasts, and counts for polite channel access.

The caller is shown by name when one is known: the channel's talk target and listen list contacts first, then every
codeplug contact with that ID (group and private IDs are separate name spaces), then, for a private ID, the DMR ID
database text (callsign and name, cut to the 16-character name field). The radio core keeps the last four answers
(including "no name"), so flash is read once per new ID.

The emergency service option (bit 7) marks the call (`emergency` in the view, the call screen's warning color). With
`CONFIG_DMR_EMERGENCY` a played emergency call also raises the emergency alert and tone, and the radio's own
emergency calls carry the option (`dmr-emergency.md`).

## 2. Who we talk to

PTT sends to, in order:

0. a one-shot target: the row with the focus in the talk target picker or a list (`radio_req_dmr_focus`, handed over
   by the UI as the focus moves). PTT talks to it once; after the call the radio follows it (plays its calls and
   sends to it on PTT) for the reply window, then goes back to the channel's target. A channel change, Reply cancel,
   a new manual target, or the focus moving to the talk target itself end it (`dmr.one_shot`, `dmr.focus_id`).
1. the reply target while the reply window runs: after a call we played, its talkgroup (group call) or its caller
   (private call to us); after our own call, its target again. The window is the codeplug's group or private call
   hang time (rounded up to whole seconds, at most 30 s), or 10 s when the codeplug has none; the reply time setting
   (`reply_s`, 0 to 30 s, Settings > Talk & listen) replaces the codeplug's for both. BACK on Home ends it.
2. a talk target chosen for now: a typed ID or talkgroup, a codeplug contact (with its timeslot override), or "call
   back", which takes the last call heard (its caller for a private call, its talkgroup for a group call). It lasts
   until the channel changes or it is cleared.
3. the channel's TX contact; with none, the first talkgroup of its RX group list (private calls skipped); with neither,
   talkgroup 9. `codeplug_channel_talk_contact()` is the one place this is decided.

The call is unit to unit (FLCO 3) for a private target, else group (FLCO 0).

For now changes (Quick settings, P7): `radio_req_dmr_timeslot` and `radio_req_dmr_color_code` set the timeslot
(temp bit 0x20; it overrides a contact's timeslot) and the color code (0x40) without touching the channel;
`radio_req_mode` listens in FM or DMR on the same frequencies (0x80). They are refused while transmitting or busy, and
BACK on Home (`radio_req_clear_temporary`) or a channel change undoes them. Save to channel writes them into the
channel. In the VFO the same requests change the VFO's own settings instead (section 2.1).

TX time-out: the channel's codeplug TOT (15 s units) when it has one, else the radio's (180 s by default). The
transmission ends at the next superframe boundary with its terminator, and PTT shows "timed out" until released.

Stalled modem: PTT release and the time-out both wait for a superframe boundary, which only comes while the modem
sends bursts. When the modem takes no burst for `DMR_CALL_TX_STALL_MS` (1.5 s; a burst counts when it is queued or its
slot starts), the stack aborts the transmission and returns to receive with `DMR_TX_ERR_STALLED` (`dmr.tx_error=stalled`
on the debug link). With PTT still held the radio shows "Modem refused to send" until release. A modem without slot
timing (a modem that stops answering) otherwise kept the radio keyed until the battery came out
([gotchas](gotchas.md) 3.12).

### 2.1 The DMR VFO

With `CONFIG_VFO_DMR` the VFO has a mode, FM or DMR, as a setting of its own, next to its frequency, bandwidth and
tones. In DMR it works like a DMR channel that has no codeplug record behind it:

| What | In the DMR VFO |
|---|---|
| Frequency | The VFO's. RX and TX are the same (simplex, direct mode). A VFO A record written by a CPS with a TX offset keeps it, and then the VFO is a repeater-mode channel by the channel rule (RX and TX differ) |
| Color code | 0 to 15, default 1 |
| Timeslot | 1 or 2, default 1. A contact's timeslot override is not applied: the VFO's slot is the slot |
| Own ID | The radio's, as on a channel without an ID of its own |
| Talk target | A typed ID or talkgroup, or a codeplug contact, private or group. Default: talkgroup 9, as a channel with no contact and no RX group list |
| Heard | The talk target's group, all call, private calls to our ID, the reply window, and everything with digital monitor. The VFO has no RX group list, so no other talkgroup is played (it is shown as busy), as on a DMR channel whose list is empty |
| Power, TOT, privacy key | The radio's, as on a channel (the VFO's power level, the radio's TOT, the radio-wide privacy key) |

Not in the VFO: talker alias (a channel's setting), roaming, the "hear all by default" setting, saving to a channel.

These are the VFO's own settings, so they are not temporary changes: no dot on Home, BACK on Home doesn't undo them,
they stay while the radio is on a channel, and they are kept across power cycles with the VFO's frequency
(`settings-store.md` 3). The requests are the ones a channel uses and the radio core gives them the VFO meaning:
`radio_req_mode` (needs both `CONFIG_VFO_FM` and `CONFIG_VFO_DMR`; on a channel the mode is no longer a quick menu change, Copy to VFO (2.2) is the way in), `radio_req_dmr_color_code`,
`radio_req_dmr_timeslot`, and `radio_req_dmr_target` / `radio_req_dmr_contact` for the talk target. A typed or
chosen target is the VFO's from then on (`dmr.target_manual` stays 0); "call back" is for now as on a channel and
`radio_req_dmr_target_clear` returns to the VFO's target. All are refused while transmitting.

A mode change retunes the same frequency. FM to DMR starts the call stack and sets the board to DMR; DMR to FM stops
it, and the VFO's FM bandwidth and tones come back. No sub-audio squelch is set while in DMR, whatever tone the VFO
holds. A build with only `CONFIG_VFO_DMR` has a DMR VFO and no mode change; one with only `CONFIG_VFO_FM` is as
before.

A split VFO's TX frequency is set by `radio_req_vfo_tx` (`RREQ_VFO_TX`, the quick tile Split, [information-architecture.md](../ux/information-architecture.md) 5): any frequency in a transmit band, 0 for simplex, in the VFO only, not while transmitting or in hotspot mode. The receiver isn't retuned. It is saved with the VFO's settings (`vfo_tx_hz`).

### 2.2 Copy to VFO

From a channel (FM or DMR), Copy to VFO (`radio_req_vfo_copy`, `RREQ_VFO_COPY`) makes the channel's settings the VFO's
own and opens the VFO. It is in the channel's quick menu (tile 4, "To VFO"), on the assignable key function "Copy to
VFO" (`KEYFN_COPY_VFO`, short or long press) and on the debug link (RADIO op 32). It needs `CONFIG_VFO_FM` or
`CONFIG_VFO_DMR`.

What is copied is what is in effect now, so a temporary change on the channel (bandwidth, tone, mode, color code,
timeslot, power, a talk target chosen for now) goes along. The channel itself is unchanged: leaving the VFO brings
back the channel as it was, without the temporary changes, as any channel change does.

| Channel | VFO |
|---|---|
| RX and TX frequency | Both, so a repeater offset stays (the VFO is then a split VFO; a new RX frequency, typed or stepped, keeps the offset while RX stays in its transmit band and TX lands there too, `board_offset_carries`; a move to another band, or TX off the band, makes the VFO simplex). Direct-mode channels (DMO) copy RX only |
| Mode, bandwidth | Both |
| Power | The level in effect; a channel on "default" stays on the radio's default |
| CTCSS / DCS, RX and TX (FM) | Both. A DMR channel has none: the VFO keeps its own for its FM mode |
| Color code, timeslot (DMR) | Both. The slot is the one in use: a timeslot chosen for now, else the talk contact's override, else the channel's |
| Talk target (DMR) | The ID and the group / private flag of what the channel would send now: a target chosen for now, else its contact, else the first talkgroup of its RX group list, else talkgroup 9 (the channel default). A one-shot or reply target is not copied |
| FM channel into a VFO that has DMR values | Color code, timeslot and talk target stay as the VFO had them |

Not copied, because the VFO has nothing to hold them: the RX group list, the privacy key, roaming, the data and
emergency settings, the talker alias, TOT and the squelch level (a radio setting, not the VFO's). They apply to the VFO
as the radio-wide values.

The VFO's values are saved as its own settings (`settings-store.md` 3) and the VFO's temporary marks are cleared.
Nothing changes, and the UI says why (`radio_vfo_copy_check`), when:

| Reason | Toast | Debug link status |
|---|---|---|
| No VFO in the build | "VFO: not in this build" | UNAVAILABLE |
| A DMR channel without `CONFIG_VFO_DMR`, or an FM channel without `CONFIG_VFO_FM` | "VFO: no DMR in this build" / "VFO: no FM in this build" | REFUSED |
| RX or TX frequency outside the board's bands | "VFO: frequency outside its bands" | REFUSED |
| Already in the VFO | "Already in VFO" | UNAVAILABLE |

The radio core also ignores the request while transmitting and in hotspot mode. A scan in progress ends first (the
channel it stopped on is the one copied).

## 3. Talker alias

Receive: header (FLCO 4) and blocks 1-3 (FLCO 5-7) in the embedded LC, in any order, all four formats. 7-bit text
starts at the header's first data bit; ISO 8-bit, UTF-8 and UTF-16 (big endian) at its second. Characters outside
printable ASCII show as one `?` each (a UTF-8 multi-byte sequence or a UTF-16 surrogate pair is one character). The
announced length counts characters, but some senders count UTF-8 bytes or overstate it, so an alias also counts as
complete at the first zero character (padding) or once all four parts are in. A different header (format or length)
starts a new alias; a new talker resets it.

Transmit (`CONFIG_DMR_TALKER_ALIAS`): the radio name (the codeplug callsign, 8 characters) in ISO 8-bit, when the
channel's talker alias setting asks for text on its timeslot. From the second superframe, every other superframe
carries one alias LC instead of the voice LC (LC, header, LC, block 1, LC, header, ...), so late entry still finds the
voice LC within two superframes. The modem takes the new embedded LC at the next burst A it sets up; the stack hands
it over before queueing that burst A.

### 3.1 GPS Info

`CONFIG_DMR_GPS` (= DMR). The GPS Info LC (TS 102 361-2 7.1.1, FLCO 8, FID 0) rides in the embedded LC like a talker
alias block. After the FLCO and FID octets: 4 reserved bits, the position error (3 bits: below 2 m, 20 m, 200 m,
2 km, 20 km, 200 km, more, 7 unknown), the longitude (25 bits, two's complement, 360 / 2^25 degree steps, east
positive) and the latitude (24 bits, two's complement, 180 / 2^24 degree steps, north positive). One step is about
1.2 m. `dmr_lc.c` (`dmr_gps_encode`, `dmr_gps_decode`) packs and unpacks it; a manufacturer FID or 0, 0 (no fix) is
not a position.

Receive: `dmr_call` decodes every GPS Info LC on the call into `dmr_caller_t.gps` / `gps_valid` (counter `rx_gps`);
a new talker clears it, and `last_heard` keeps it after the call. The radio core adds the distance and bearing from
our own position (the APRS position source, `aprs.md` 7) in `radio_dmr_call_t`, the receive screen shows "12.3 km NE"
(or the coordinates when we have no position of our own), Recent keeps the distance with the entry, and in
`CONFIG_APRS_RX` builds the caller goes into the APRS station list (named by its alias's callsign, else its DMR ID).

Transmit: Tools > APRS > "Position in DMR calls" (radio-wide, off by default, `aprs_cfg_t.dmr_gps`, P7). At PTT the
radio core takes the APRS position in use (GPS on boards with one, the host's NMEA, else the fixed position;
position ambiguity applied) and hands it to the stack (`dmr_call_set_tx_gps`); no position, no GPS Info. The GPS Info
LC is one more entry at the end of the talker alias rotation in the odd superframes: LC, header, LC, block 1, LC,
block 2, LC, GPS, ... with an alias, or LC, GPS, LC, GPS without (counter `tx_gps_lcs`). The position error says 20 m
for GPS and host fixes, 200 m for a typed position, or the ambiguity's blur. Without `CONFIG_APRS` there is no
position source, so nothing is sent (receive still works).

The hotspot already passes FLCO 4-8 embedded LCs through to the host (`hotspot.c`).

## 4. Late entry

A call heard without its header is acquired from the embedded LC once two consecutive voice bursts were seen (a lone
burst is noise) and the LC differs from the previous call's (the modem's stale LC, gotcha 1.4), or the same LC came
in two superframes. Playback starts at the burst that completed the LC. A new talker on a running call (a repeater
passing overs without a header) switches the caller from the embedded LC. The host tests cover joining at burst C,
the lone burst, the stale LC needing confirmation, a fresh LC taken at once, and a talker change.

## 5. Privacy

Basic privacy (`CONFIG_DMR_PRIVACY`): Motorola BP (key 1-255) and Anytone 16-bit BP (`dmrPrivacy.c`). The keystream
is XORed onto the 49 AMBE+2 parameter bits of each frame by superframe position: on TX as the burst is queued (the
encoder's bits as they are), on RX before the voice sink (after the FEC decode, `core/dmr/ambe/ambeFec.c`). With a key
loaded our calls carry FID 0x10 and the privacy service option (0x40), and received calls with the privacy service
option (or PI) are descrambled. Without a key such bursts are never played; they count as `rx_enc_dropped`.

The key is set over the debug link (RADIO op 19, `radio_req_dmr_privacy`). The radio core saves it in settings slot 1
and loads the active slot at boot (`settings-store.md` 3.1); the open-firmware codeplug has no key storage, so qdmr can't
set it.

PI header (data type 0, CRC-CCITT masked 0x6969): heard between the voice LC header and burst A, it marks the call
encrypted and its first two octets are kept for display (algorithm and key ID in the common layout; not interpreted).
A PI header heard just before a late entry is attached to that call. A PI header means an MI-based scheme the static
keystream can't undo, so such a call is never played, key or not. We never send PI headers.

The HR_C6000 reports the EMB PI bit unreliably for voice, so its driver clears it (`modem-hrc6000.md`); on that modem
privacy is recognized from the service options and PI headers only.

## 6. Encoding the microphone

The vocoder encodes each 20 ms of microphone audio (`core/dmr/ambe/ambeVoice.c`, in the audio task). Before that the
samples are scaled by the mic gain (Settings > Talk & listen, 2 dB a step). The codec itself can also clean up the
input. These are generic encode options in `hal_vocoder_t`; the board maps them to its codec, and `core/` never sees
codec bits.

| Option | Effect | Default | Setting |
|---|---|---|---|
| `VOCODER_OPT_NOISE_REDUCTION` | Suppresses steady background noise, also between words. It also lowers a steady tone held into the mic | on | Settings > Talk & listen > Mic noise reduction |
| `VOCODER_OPT_TONE_FRAMES` | A steady tone or tone pair (DTMF) is sent as a tone frame and comes out at its frequency on the other side. Speech is not affected | on | none |
| `VOCODER_OPT_AGC` | Input level control: quiet speech raised (about 4x), loud speech lowered. The codec's own parameters; not yet checked on air | off | Settings > Talk & listen > Mic AGC |

* `options()` says which options the board's codec takes. A board without them leaves `options`, `set_options` and
  `encode_flags` NULL. Then the settings are shown dimmed ("Not on this radio") and nothing changes.
* `ambeVoiceSetEncodeOptions` may be called from any task. The audio callback hands the options to the vocoder before
  the next frame it encodes, so every codec call of a frame uses the same options. The board keeps them across
  `reset()`, so a new transmission starts with the options in force.
* The options are read at start-up from the settings and on every change (`ui_svc_apply`). With AGC on, the mic gain
  is step 5, the board's levels (FM too on a radio whose board sets the gain), and Mic gain is dimmed, showing "AGC".
* Each board's codec mapping is on its `codec-<id>.md` page ("Encode flags"). Boards with an AMBE+2 host share
  `drivers/vocoder/ambe2_flags.h`.

## 7. Observing it

Debug link (`debuglink.md`): the `core` view's `mode` and `vfo`, the `dmr` view's `dmr.cc`, `dmr.ts`, `dmr.target_id`,
`dmr.target_private`, `dmr.target_manual`, `dmr.privacy`, `dmr.alias_tx`; RADIO op 31 sets the mode, color code and timeslot (the VFO's
own in the VFO), op 15 the talk target and op 32 copies the channel to the VFO (2.2); the `dmr_call` view's
`emergency`, `pi_header`, `alias_complete`, `fid`, `svc`, `pi_alg`, `pi_key`; the counters `rx_private_calls`,
`rx_emergency_calls`, `rx_encrypted_calls`, `rx_pi_headers`, `rx_alias_complete`, `tx_alias_lcs`, `rx_gps`,
`tx_gps_lcs`; GPS Info as `call.gps_valid gps_lat gps_lon gps_err gps_dist_m gps_bearing` (and `last.`). HIL tests:
`tests/hil/test_dmr_call_features.py` (`test_gps_info`: the bench sends a call with a GPS block, `tools/bench/
bench_tx.py voice --gps LAT,LON[,ERR]`), `tests/hil/test_dmr_tx.py` (`test_dmr_gps_tx`: our GPS Info decoded by the
bench modem's listener).

## 8. Open points

- The codeplug's hang time unit is not confirmed. The interop spec lists the group and private hang bytes
  (general settings 0x18, 0x19) only as vendor layout that the open-firmware layout does not use
  (`open-firmware-cps.md` 7.3.2), and neither the reference notes nor the vendor guide give a unit.
  Assumed: 500 ms steps (`codeplug.c`, `GEN_HANG_UNIT_MS`), so a stored 6 is 3 s. Confirm with the vendor CPS
  (set a known hang time, read the byte) on the bench; the reply time setting (`rs_reply_s`) overrides it meanwhile.
- DMR ID database plain format: the 4-byte BCD ID is read as a little-endian word (the spec doesn't say).
- A codeplug TOT of 0 means "off" in the vendor layout; here it falls back to the radio's TOT.
- Emergency alarm and emergency call TX are in `dmr-emergency.md`. No enhanced privacy. GPS Info is host-tested
  (`dmrCallTest`) and on air both ways against the SDR bench (`docs/testing/on-air-coverage.md`). Not done: GPS Info in the hotspot's own
  transmissions (it relays the host's), a separate Settings > Location toggle for it (it lives on the APRS page).
