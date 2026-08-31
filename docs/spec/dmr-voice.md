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
| Group (FLCO 0) | its talkgroup is the talk target's, all call (0xFFFFFF), in the channel's RX group list (listen list), the reply target's during the reply window, or hear all (MONITOR held, or the hear-all latch: Quick 5 on a DMR channel, `radio_req_dmr_hear_all`, until the channel changes, DMR stops (a mode change), or BACK on Home; the end of a call doesn't end it) |
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
3. the channel's TX contact; with none, talkgroup 9.

The call is unit to unit (FLCO 3) for a private target, else group (FLCO 0).

For now changes (Quick settings, P7): `radio_req_dmr_timeslot` and `radio_req_dmr_color_code` set the timeslot
(temp bit 0x20; it overrides a contact's timeslot) and the color code (0x40) without touching the channel;
`radio_req_mode` listens in FM or DMR on the same frequencies (0x80). They are refused while transmitting or busy and
in the VFO (the VFO's own fields are the place for those), and BACK on Home (`radio_req_clear_temporary`) or a
channel change undoes them. Save to channel writes them into the channel.

TX time-out: the channel's codeplug TOT (15 s units) when it has one, else the radio's (180 s by default). The
transmission ends at the next superframe boundary with its terminator, and PTT shows "timed out" until released.

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
and loads the active slot at boot (`settings-store.md` 3.1); the legacy codeplug has no key storage, so qdmr can't
set it.

PI header (data type 0, CRC-CCITT masked 0x6969): heard between the voice LC header and burst A, it marks the call
encrypted and its first two octets are kept for display (algorithm and key ID in the common layout; not interpreted).
A PI header heard just before a late entry is attached to that call. A PI header means an MI-based scheme the static
keystream can't undo, so such a call is never played, key or not. We never send PI headers.

The HR_C6000 reports the EMB PI bit unreliably for voice, so its driver clears it (`modem-hrc6000.md`); on that modem
privacy is recognized from the service options and PI headers only.

## 6. Observing it

Debug link (`debuglink.md`): the `dmr` view's `target_manual`, `privacy`, `alias_tx`; the `dmr_call` view's
`emergency`, `pi_header`, `alias_complete`, `fid`, `svc`, `pi_alg`, `pi_key`; the counters `rx_private_calls`,
`rx_emergency_calls`, `rx_encrypted_calls`, `rx_pi_headers`, `rx_alias_complete`, `tx_alias_lcs`. HIL tests:
`tests/hil/test_dmr_call_features.py`.

## 7. Open points

- The codeplug's hang time unit is not confirmed. Checked: the interop spec lists the group and private hang bytes
  (general settings 0x18, 0x19) only as vendor layout the legacy firmware ignores ("only the callsign and the DMR ID
  matter", `legacy-cps-interop.md` 7.3.2), and neither the reference notes nor the DM-1701 vendor guide give a unit.
  Assumed: 500 ms steps (`codeplug.c`, `GEN_HANG_UNIT_MS`), so a stored 6 is 3 s. Confirm with the vendor CPS
  (set a known hang time, read the byte) on the bench; the reply time setting (`rs_reply_s`) overrides it meanwhile.
- DMR ID database plain format: the 4-byte BCD ID is read as a little-endian word (the spec doesn't say).
- A codeplug TOT of 0 means "off" to the legacy firmware; here it falls back to the radio's TOT.
- Emergency alarm and emergency call TX are in `dmr-emergency.md` (host-tested, not on air yet). No enhanced
  privacy, no APRS / GPS talker alias TX.
