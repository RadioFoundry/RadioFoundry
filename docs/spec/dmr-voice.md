<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR voice calls

This page describes the voice side of the DMR stack, above the modem. The air interface and the modem contract are in
[dmr-modem.md](dmr-modem.md). Protocol references are ETSI TS 102 361-1 and -2.

| Code | Role |
|---|---|
| `core/dmr/call` | The call stack |
| `core/dmr/dmrPrivacy.c` | Basic privacy |
| `core/radio/radio_sm.c` | The radio core: channel, contacts, settings |
| `core/codeplug` | Contacts, settings, the DMR ID database |

## 1. Calls the radio plays

A voice call on the radio's timeslot and color code starts from a voice LC header or by late entry (section 4). The
radio plays the call when it passes the filters in the next table, and shows it either way.

Digital monitor (hear all) also takes calls on any color code and either timeslot (`dmr_listen_filter_t.any_cc`,
`.both_ts`). The modem then gets no color code filter and, through a repeater, no slot lock. The first call heard
keeps the voice until it ends; bursts on the other slot meanwhile are dropped. Turning digital monitor on or off
changes only the modem's filters: the call being heard and the reply window stay, and a call heard as not the
radio's plays at once.

The view gives the call's color code and slot (`rx_cc`, `rx_ts`) beside the channel's (`cc`, `ts`), which PTT always
uses. `rx_ts` is 0 when the slot is unknown. `rx_cc` comes from the first burst that carries one, never burst A.

| Call | Played when |
|---|---|
| Group (FLCO 0) | Its talkgroup is the talk target's, all call (0xFFFFFF), in the channel's RX group list (listen list), or the reply target's during the reply window; or hear all is on |
| Private (FLCO 3) | It is addressed to the radio's ID (the channel's DMR ID override, else the radio's), or to all call; or hear all is on |
| The radio's own ID as source | Not while the radio transmits, nor for `DMR_CALL_OWN_ECHO_MS` (700 ms) after its call ended or was cut short (the repeater's echo of it). Later it is judged as any other call, so a parrot's playback of the call plays (#294). A terminator from the radio's ID ends only the call with its LC, never another station's |
| Encrypted | Only with a privacy key, and never after a PI header (section 5) |

Hear all is "digital monitor" in the UI. It is a latch:

- The MONITOR key's press toggles it on a DMR channel; its release does nothing (#19). Quick 5 and
  `radio_req_dmr_hear_all` set it too.
- It lasts until the channel changes, DMR stops (a mode change), or BACK on Home. The end of a call doesn't end it.
- A zone or VFO scan started with it on keeps it on at every stop, and latches it on the channel the scan ends on.
  Turning it off during the scan ends it for the scan ([radio-features.md](radio-features.md) 2).

A call that doesn't pass is shown as busy ("not played") while it lasts, and counts for polite channel access.

The radio shows the caller by name when one is known. It looks in this order:

1. The channel's talk target and listen list contacts.
2. Every codeplug contact with that ID. Group and private IDs are separate name spaces.
3. For a private ID, the DMR ID database text (callsign and name, cut to the 16-character name field).

The radio core keeps the last four answers, including "no name", so it reads flash once per new ID.

The emergency service option (bit 7) marks the call (`emergency` in the view, the call screen's warning color). With
`CONFIG_DMR_EMERGENCY`, a played emergency call also raises the emergency alert and tone, and the radio's own
emergency calls carry the option ([dmr-emergency.md](dmr-emergency.md)).

## 2. Talk target

PTT sends to the first of these that applies:

0. A one-shot target: the row with the focus in the talk target picker or a list (`radio_req_dmr_focus`, handed over
   by the UI as the focus moves). PTT talks to it once. After the call, the radio follows it (plays its calls and
   sends to it on PTT) for the reply window, then goes back to the channel's target. A channel change, Reply cancel,
   a new manual target, or the focus moving to the talk target itself ends it (`dmr.one_shot`, `dmr.focus_id`).
1. The reply target, while the reply window runs. After a call the radio played, it is that call's talkgroup (group
   call) or caller (private call to the radio). After the radio's own call, it is that call's target again. The
   window is the codeplug's group or private call hang time (rounded up to whole seconds, at most 30 s), or 10 s when
   the codeplug has none. The reply time setting (`reply_s`, 0 to 30 s, Settings > Talk & listen) replaces the
   codeplug's for both. BACK on Home ends the window.
2. A temporary talk target: a typed ID or talkgroup, a codeplug contact (with its timeslot override), or "call back",
   which takes the last call heard (its caller for a private call, its talkgroup for a group call). It lasts until the
   channel changes or it is cleared.
3. The channel's TX contact. With none, the first talkgroup of its RX group list (private calls skipped). With
   neither, talkgroup 9. `codeplug_channel_talk_contact()` is the one place this is decided.

The call is unit to unit (FLCO 3) for a private target, else group (FLCO 0).

Temporary changes (Quick settings, P7) leave the channel record alone:

| Request | Changes | Temporary bit |
|---|---|---|
| `radio_req_dmr_timeslot` | The timeslot. It overrides a contact's timeslot | 0x20 |
| `radio_req_dmr_color_code` | The color code | 0x40 |
| `radio_req_mode` | Listens in FM or DMR on the same frequencies | 0x80 |

The radio core refuses them while transmitting or busy. BACK on Home (`radio_req_clear_temporary`) or a channel change
undoes them. Save to channel writes them into the channel. In the VFO the same requests change the VFO's own settings
instead (section 2.1).

TX time-out: the channel's codeplug TOT (15 s units) when it has one, else the radio's (180 s by default). The
transmission ends at the next superframe boundary with its terminator, and PTT shows "timed out" until released.

Stalled modem: PTT release and the time-out both wait for a superframe boundary, which only comes while the modem
sends bursts. When the modem takes no burst for `DMR_CALL_TX_STALL_MS` (1.5 s; a burst counts when it is queued or its
slot starts), the stack stops the transmission and returns to receive with `DMR_TX_ERR_STALLED`
(`dmr.tx_error=stalled` on the debug link). With PTT still held, the radio shows "Modem refused to send" until
release. Without this limit, a modem without slot timing (a modem that stops answering) keeps the radio keyed until
the battery comes out ([gotchas](gotchas.md) 3.12).

Through a repeater, before the first burst's slot, the wait is 300 ms longer (`DMR_CALL_TX_ALIGN_GRACE_MS`). The
modem's own aligned-start timeout (1.5 s and a slot) must come first, so that its refusal wakes the repeater again
([dmr-modem.md](dmr-modem.md) 2.7). With an encoder slow to start, the headers are the last bursts queued, so without
the margin the stall cut comes first.

### 2.1 The DMR VFO

With `CONFIG_VFO_DMR`, the VFO has a mode, FM or DMR, as a setting of its own, next to its frequency, bandwidth and
tones. In DMR it works like a DMR channel that has no codeplug record behind it:

| What | In the DMR VFO |
|---|---|
| Frequency | The VFO's. RX and TX are the same (simplex, direct mode). A VFO A record written by a CPS with a TX offset keeps it, and then the VFO is a repeater-mode channel by the channel rule (RX and TX differ) |
| Color code | 0 to 15, default 1 |
| Timeslot | 1 or 2, default 1. A contact's timeslot override doesn't apply: the VFO's slot is the slot |
| Own ID | The radio's, as on a channel without an ID of its own |
| Talk target | A typed ID or talkgroup, or a codeplug contact, private or group. Default: talkgroup 9, as on a channel with no contact and no RX group list |
| Heard | The talk target's group, all call, private calls to the radio's ID, the reply window, and everything with digital monitor. The VFO has no RX group list, so no other talkgroup plays (it shows as busy), as on a DMR channel whose list is empty |
| Power, TOT, privacy key | The radio's, as on a channel (the VFO's power level, the radio's TOT, the radio-wide privacy key) |

The DMR VFO has no talker alias (a channel's setting), roaming, "hear all by default" setting, or save to a channel.

These are the VFO's own settings, not temporary changes. No dot shows on Home, BACK on Home doesn't undo them, they
stay while the radio is on a channel, and they're kept across power cycles with the VFO's frequency
([settings-store.md](settings-store.md) 3). Squelch has no VFO value: a squelch change in the VFO becomes the radio's
default squelch at once, so it isn't temporary either.

The requests are the ones a channel uses, and the radio core gives them the VFO meaning:

- `radio_req_mode`. It needs both `CONFIG_VFO_FM` and `CONFIG_VFO_DMR`. On a channel the mode isn't a quick menu
  change; Copy to VFO (2.2) is the way in.
- `radio_req_dmr_color_code` and `radio_req_dmr_timeslot`.
- `radio_req_dmr_target` and `radio_req_dmr_contact` for the talk target. A typed or chosen target is the VFO's from
  then on (`dmr.target_manual` stays 0). "Call back" is temporary, as on a channel, and `radio_req_dmr_target_clear`
  returns to the VFO's target.

The radio core refuses all of them while transmitting.

A mode change retunes the same frequency. FM to DMR starts the call stack and sets the board to DMR. DMR to FM stops
it, and the VFO's FM bandwidth and tones come back. No sub-audio squelch is set while in DMR, whatever tone the VFO
holds. A build with only `CONFIG_VFO_DMR` has a DMR VFO and no mode change; a build with only `CONFIG_VFO_FM` has an
FM VFO.

`radio_req_vfo_tx` (`RREQ_VFO_TX`, the quick tile TX freq, [information-architecture.md](../ux/information-architecture.md)
5) sets a split VFO's TX frequency: any frequency in a transmit band, or 0 for simplex. It works in the VFO only, and
not while transmitting or in hotspot mode. The receiver isn't retuned. The frequency is saved with the VFO's settings
(`vfo_tx_hz`).

### 2.2 Copy to VFO

From a channel (FM or DMR), Copy to VFO (`radio_req_vfo_copy`, `RREQ_VFO_COPY`) makes the channel's settings the VFO's
own and opens the VFO. It needs `CONFIG_VFO_FM` or `CONFIG_VFO_DMR`. These start it:

- Tile 4 ("To VFO") of the channel's quick menu.
- The assignable key function "Copy to VFO" (`KEYFN_COPY_VFO`, short or long press).
- The debug link (RADIO op 32).

The radio copies what is in effect, so a temporary change on the channel (bandwidth, tone, mode, color code,
timeslot, power, a temporary talk target) goes along. The channel itself is unchanged: leaving the VFO brings back the
channel as it was, without the temporary changes, as any channel change does.

| Channel | VFO |
|---|---|
| RX and TX frequency | Both, so a repeater offset stays (the VFO is then a split VFO; a new RX frequency, typed or stepped, keeps the offset while RX stays in its transmit band and TX lands there too, `board_offset_carries`; a move to another band, or TX off the band, makes the VFO simplex). Direct-mode channels (DMO) copy RX only |
| Mode, bandwidth | Both |
| Power | The level in effect; a channel on "default" stays on the radio's default |
| CTCSS / DCS, RX and TX (FM) | Both. A DMR channel has none: the VFO keeps its own for its FM mode |
| Color code, timeslot (DMR) | Both. The slot is the one in use: a temporary timeslot, else the talk contact's override, else the channel's |
| Talk target (DMR) | The ID and the group / private flag of what the channel would send: a temporary target, else its contact, else the first talkgroup of its RX group list, else talkgroup 9 (the channel default). A one-shot or reply target isn't copied |
| FM channel into a VFO that has DMR values | Color code, timeslot and talk target stay as the VFO had them |

The VFO has nothing to hold these, so they aren't copied: the RX group list, the privacy key, roaming, the data and
emergency settings, the talker alias, TOT and the squelch level (a radio setting, not the VFO's). The VFO uses the
radio-wide values.

The VFO's values are saved as its own settings ([settings-store.md](settings-store.md) 3), and the VFO's temporary
marks are cleared. In these cases nothing changes, and the UI says why (`radio_vfo_copy_check`):

| Reason | Toast | Debug link status |
|---|---|---|
| No VFO in the build | "VFO: not in this build" | UNAVAILABLE |
| A DMR channel without `CONFIG_VFO_DMR`, or an FM channel without `CONFIG_VFO_FM` | "VFO: no DMR in this build" / "VFO: no FM in this build" | REFUSED |
| RX or TX frequency outside the board's bands | "VFO: frequency outside its bands" | REFUSED |
| Already in the VFO | "Already in VFO" | UNAVAILABLE |

The radio core also ignores the request while transmitting and in hotspot mode. A scan in progress ends first, and
the channel it stopped on is the one copied.

## 3. Talker alias

Receive: the radio takes the header (FLCO 4) and blocks 1-3 (FLCO 5-7) in the embedded LC, in any order, in all four
formats.

- 7-bit text starts at the header's first data bit; ISO 8-bit, UTF-8 and UTF-16 (big endian) at its second.
- Characters outside printable ASCII show as one `?` each. A UTF-8 multi-byte sequence or a UTF-16 surrogate pair is
  one character.
- The announced length counts characters, but some senders count UTF-8 bytes or overstate it. So an alias also counts
  as complete at the first zero character (padding) or once all four parts are in.
- A different header (format or length) starts a new alias. A new talker resets it.

Transmit (`CONFIG_DMR_TALKER_ALIAS`): the radio sends the radio name (the codeplug callsign, 8 characters) in ISO
8-bit when the channel's talker alias setting asks for text on its timeslot. From the second superframe, every other
superframe carries one alias LC instead of the voice LC (LC, header, LC, block 1, LC, header, ...), so late entry
still finds the voice LC within two superframes. The modem takes the new embedded LC at the next burst A it sets up;
the stack hands it over before queueing that burst A.

### 3.1 GPS Info

`CONFIG_DMR_GPS` (= DMR). The GPS Info LC (TS 102 361-2 7.1.1, FLCO 8, FID 0) rides in the embedded LC like a talker
alias block. After the FLCO and FID octets it holds:

| Field | Bits | Encoding |
|---|---|---|
| Reserved | 4 | |
| Position error | 3 | Below 2 m, 20 m, 200 m, 2 km, 20 km, 200 km, more, 7 = unknown |
| Longitude | 25 | Two's complement, 360 / 2^25 degree steps, east positive |
| Latitude | 24 | Two's complement, 180 / 2^24 degree steps, north positive |

One step is about 1.2 m. `dmr_lc.c` (`dmr_gps_encode`, `dmr_gps_decode`) packs and unpacks it. A manufacturer FID, or
0, 0 (no fix), isn't a position.

Receive: `dmr_call` decodes every GPS Info LC on the call into `dmr_caller_t.gps` / `gps_valid` (counter `rx_gps`). A
new talker clears it, and `last_heard` keeps it after the call. The radio core adds the distance and bearing from the
radio's own position (the position service, [position.md](position.md)) in `radio_dmr_call_t`. The receive screen
shows "12.3 km NE", or the coordinates when the radio has no position of its own. Recent keeps the distance with the
entry. In `CONFIG_APRS_RX` builds, the caller goes into the APRS station list, named by its alias's callsign, else its
DMR ID.

Transmit: the radio sends GPS Info when either of these asks for it.

| Setting | Where | Scope |
|---|---|---|
| "Send in DMR calls" | Settings > Location (`position_cfg_t.dmr_gps`, off by default, P7) | Every DMR call |
| The channel's talker alias field for its timeslot: APRS or Both | The codeplug ([open-firmware-cps.md](open-firmware-cps.md) 7.3.3, offset 0x30; `channel_t.alias_tx` `CODEPLUG_ALIAS_TS1_GPS` / `_TS2_GPS`), so a CPS sets it | Calls on that channel; not the VFO |

At PTT, the radio core takes the position in use (`position_now`: GPS on boards with one, the host's NMEA, else the
fixed position; position ambiguity applied) and hands it to the stack (`dmr_call_set_tx_gps`). With no position, the
radio sends no GPS Info. A radio without a GPS sends its fixed position.

The GPS Info LC is one more entry at the end of the talker alias rotation in the odd superframes: LC, header, LC,
block 1, LC, block 2, LC, GPS, ... with an alias, or LC, GPS, LC, GPS without one (counter `tx_gps_lcs`). The
position error says 20 m for GPS and host fixes, 200 m for a typed position, or the ambiguity's blur. It needs no
APRS: the setting and the position belong to the position service, built with `CONFIG_DMR_GPS`.

The hotspot passes FLCO 4-8 embedded LCs through to the host (`hotspot.c`). In its own transmissions it relays the
host's GPS Info and adds none of its own.

## 4. Late entry

The radio acquires a call heard without its header from the embedded LC once it has seen two consecutive voice bursts
(a lone burst is noise), and either the LC differs from the previous call's (the modem's stale LC, gotcha 1.4) or the
same LC came in two superframes. Playback starts at the burst that completed the LC. A new talker on a running call
(a repeater passing overs without a header) switches the caller from the embedded LC. The host tests cover joining at
burst C, the lone burst, the stale LC that needs confirmation, a fresh LC taken at once, and a talker change.

A call lost for a moment, with no terminator heard, ends and must be joined again. Where the modem can prove the same
call is back, that takes one superframe instead of two, and it counts as one call ([dmr-modem.md](dmr-modem.md) 2.4).

## 5. Privacy

Basic privacy (`CONFIG_DMR_PRIVACY`, `dmrPrivacy.c`) has two schemes: basic privacy with keys 1-255
(`DMR_PRIVACY_MOTOROLA_BP`) and 16-bit basic privacy with keys 0x0000-0xFFFF (`DMR_PRIVACY_ANYTONE_BP`). The keystream is XORed onto the 49 AMBE+2 parameter bits of each frame by superframe
position:

- On TX, as the burst is queued (the encoder's bits as they are).
- On RX, before the voice sink (after the FEC decode, `core/dmr/ambe/ambeFec.c`).

With a key loaded, the radio's calls carry FID 0x10 and the privacy service option (0x40), and the radio descrambles
received calls with the privacy service option (or PI). Without a key, such bursts never play; they count as
`rx_enc_dropped`. The radio doesn't support enhanced privacy.

The same key covers packet data where its scheme defines a data form (Motorola BP encrypts it, Anytone BP blocks
data TX): [dmr-data.md](dmr-data.md) 13.

The key is set over the debug link (RADIO op 19, `radio_req_dmr_privacy`). The radio core saves it in settings slot 1
and loads the active slot at boot ([settings-store.md](settings-store.md) 3.1). The open-firmware codeplug has no key
storage, so qdmr can't set it.

PI header (data type 0, CRC-CCITT masked 0x6969): heard between the voice LC header and burst A, it marks the call
encrypted. Its first two octets are kept for display (algorithm and key ID in the common layout; not interpreted). A
PI header heard just before a late entry is attached to that call. A PI header means an MI-based scheme the static
keystream can't undo, so such a call never plays, key or not. The radio never sends PI headers.

The HR_C6000 reports the EMB PI bit unreliably for voice, so its driver clears it
([modem-hrc6000.md](modem-hrc6000.md)). On that modem the radio recognizes privacy from the service options and PI
headers only.

## 6. Encoding the microphone

The vocoder encodes each 20 ms of microphone audio (`core/dmr/ambe/ambeVoice.c`, in the audio task). Before that, the
samples are scaled by the mic gain (Settings > Talk & listen, 2 dB a step). The codec itself can also clean up the
input. These are generic encode options in `hal_vocoder_t`; the board maps them to its codec, and `core/` never sees
codec bits.

| Option | Effect | Default | Setting |
|---|---|---|---|
| `VOCODER_OPT_NOISE_REDUCTION` | Suppresses steady background noise, also between words. It also lowers a steady tone held into the mic | on | Settings > Talk & listen > Mic noise reduction |
| `VOCODER_OPT_TONE_FRAMES` | A steady tone or tone pair (DTMF) is sent as a tone frame and comes out at its frequency on the other side. Speech isn't affected | on | none |
| `VOCODER_OPT_AGC` | Input level control: quiet speech raised (about 4x), loud speech lowered. The codec's own parameters | off | Settings > Talk & listen > Mic AGC |

- `options()` says which options the board's codec takes. A board without them leaves `options`, `set_options` and
  `encode_flags` NULL. The settings then show dimmed ("Not on this radio"), and nothing changes.
- Any task can call `ambeVoiceSetEncodeOptions`. The audio callback hands the options to the vocoder before the next
  frame it encodes, so every codec call of a frame uses the same options. The board keeps them across `reset()`, so a
  new transmission starts with the options in force.
- The radio reads the options at start-up from the settings and on every change (`ui_svc_apply`). With AGC on, the
  mic gain is step 5 and the board's levels apply (FM too, on a radio whose board sets the gain); Mic gain is dimmed
  and shows "AGC".
- Voice is simplex: while a transmission runs, nothing is decoded. A call still being heard goes on as silence, and
  the decoder starts fresh once the transmission ends. A call that begins meanwhile resets nothing until then. A
  platform can keep the encoder's and the decoder's state in one block (`hal_vocoder_t`): a decode during the
  transmission would cost it its encoder.
- Boards with an AMBE+2 host share the encode flag mapping in `drivers/vocoder/ambe2_flags.h`. For the codec each
  board hosts, see [codec.md](../dev/codec.md).

### 6.1 A tone in place of the microphone

`ambeVoiceTxTone` (`ambeVoice.h`, radio task) hands the audio callback a tone sequence of up to `SIG_AIR_STEPS` steps
(`SIG_STEPS` and a guard of silence, `SIG_AIR_GUARD_MS`; [radio-features.md](radio-features.md) 4.3). From the next
block of a transmission (the first, if it hasn't started), the encoder gets the generated tone instead of the
microphone, after the mic gain. While the tone plays, the encode options are `VOCODER_OPT_TONE_FRAMES` set and
`VOCODER_OPT_NOISE_REDUCTION` cleared. The options in force come back after it.

- The encoder waits for room in its queue (`AMBE_VOICE_QUEUE`) before each block of the tone, instead of letting the
  queue overflow, so a tone longer than the headers' time isn't cut.
- `final`: the microphone stays muted after the tone until the transmission ends.
- `dmr_voice_source_t.busy` tells the call stack that the tone's frames aren't all pulled yet. After PTT is released,
  the stack then goes on at each superframe boundary, for at most `DMR_CALL_TX_TAIL_HOLDS` superframes, before it ends
  the over with the terminator. A time-out ends it at once.
- `ambeVoiceTxToneCancel` drops a request, and the source's `end` does it too: a request for a transmission that never
  went on air doesn't play in the next one. `busy` is false while no transmission runs.
- The tone's amplitude is fixed (`TONE_AMP_Q15`).

## 7. Observing it

Debug link ([debuglink.md](debuglink.md)):

| Where | Keys and ops |
|---|---|
| `core` view | `mode`, `vfo` |
| `dmr` view | `dmr.cc`, `dmr.ts`, `dmr.target_id`, `dmr.target_private`, `dmr.target_manual`, `dmr.privacy`, `dmr.alias_tx`, `dmr.gps_tx_channel` (the channel's own position setting) |
| RADIO ops | 31 sets the mode, color code and timeslot (the VFO's own in the VFO); 15 sets the talk target; 32 copies the channel to the VFO (2.2) |
| `dmr_call` view | `emergency`, `pi_header`, `alias_complete`, `fid`, `svc`, `pi_alg`, `pi_key` |
| Counters | `rx_private_calls`, `rx_emergency_calls`, `rx_encrypted_calls`, `rx_pi_headers`, `rx_alias_complete`, `tx_alias_lcs`, `rx_gps`, `tx_gps_lcs` |
| GPS Info | `call.gps_valid gps_lat gps_lon gps_err gps_dist_m gps_bearing` (and `last.`) |

Tests: `dmrCallTest` (host). HIL: `tests/hil/test_dmr_call_features.py` (`test_gps_info`: the bench sends a call with
a GPS block, a `bench_tx` `voice --gps LAT,LON[,ERR]` job) and `tests/hil/test_dmr_tx.py` (`test_dmr_gps_tx`: the
bench modem's listener decodes the radio's GPS Info).

### 7.1 Receive level

A DMR receiver decodes 4FSK well only while the demodulated level at its modem sits in a window. Too high, and the
outer symbol threshold is crossed (corrected bits on every frame, even far above sensitivity). Too low, and sync is
lost. That level is the receiver's audio gain times the transmitter's deviation, and transmitters differ by 10-20 % in
deviation. So the gain a board sets must sit in the middle of the window, not at its best point for an exact
transmitter.

A carrier offset shifts the same level. A gain with no headroom clips on one side of the channel first, and a fixed
receive offset moves the whole window off the channel. Transmitters a kHz or two off frequency are common, so a board
keeps its window centered on the channel.

| Tool | What it does |
|---|---|
| `tools/dmr_rx_sweep.py` | Steps a chip register (debug link CHIP_REG), or one bit field of it (`--field HI:LO`), through values while the SDR bench sends speech, over bench levels, deviations and carrier offsets (`--detune` makes the offset by tuning the radio away instead); ranks the values by their worst cell. About three minutes a grid |
| `test_dmr_audio_rx_strong` | The bench's simplex speech at -75 and -55 dBm at the radio: 95 % of the frames and at most 0.2 corrected bits a frame at each (a level the bench can't reach is left out) |
| `test_dmr_audio_rx_deviation` | The bench's speech at 0.85 and 1.15 of the standard deviation, at -100 dBm at the radio: 95 % of the frames and at most 0.8 corrected bits a frame, both ways |
| `test_dmr_audio_rx_offset_wide` | The bench's speech with its carrier 2.5 kHz below and above the channel, at -100 dBm at the radio: 90 % of the frames and at most 2 corrected bits a frame, both ways |

To measure a board's window, follow these rules:

- Measure near sensitivity (-90 to -105 dBm at the radio) for the level window: there a fraction of a dB shows.
- Measure strong simplex signals too (-75 to -45 dBm). A simplex transmitter keys one 30 ms burst every 60 ms, so the
  receiver's AGC sees the carrier come and go with every burst, and one that settles too slowly overloads the start
  of each burst. That costs a bit or more a frame however strong the signal, and only on simplex: a repeater's
  continuous carrier hides it. A clean receiver corrects almost nothing at those levels.
- Use speech or another varied recording, never AMBE silence: its errors land in bits with no FEC.
- Let the radio drop the last call before the next one starts (about 2 s). A call that begins while the radio still
  follows the last one's slot timing can be decoded at that timing to its end.

## 8. Codeplug values

The radio reads these codeplug values as follows:

| Value | Meaning |
|---|---|
| Group and private call hang time (general settings 0x18, 0x19) | 500 ms steps (`codeplug.c`, `GEN_HANG_UNIT_MS`), so a stored 6 is 3 s. The reply time setting (`rs_reply_s`) overrides it ([open-firmware-cps.md](open-firmware-cps.md) 7.3.2) |
| DMR ID database plain format, 4-byte BCD ID | Read as a little-endian word |
| Channel TOT of 0 | Falls back to the radio's TOT |

Emergency alarm and emergency call TX are in [dmr-emergency.md](dmr-emergency.md).
