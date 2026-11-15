<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# DMR AES-256 voice: clean-room research

## 1. Status and provenance

RadioFoundry implements AES-256 voice behind `CONFIG_DMR_AES256`, alongside
[basic privacy](dmr-voice.md#5-privacy). This document records the clean-room protocol input and implementation
limits. It is not an official DMRA specification and does not establish DMRA conformance. Hardware and vendor
interoperability checks are deferred to manual testing.

A separate spec writer reviewed the user-selected
[reference repository](https://github.com/dondch/OpenGD77-AES256/tree/925d1cbe51efbf5aa5821cfcbd0d9acf38fe9b7a)
at revision `925d1cbe51efbf5aa5821cfcbd0d9acf38fe9b7a`, on 2026-11-14. Review covered its encryption module,
voice integration, privacy-header transmission, and published project description. No reference program was
executed. No hardware was accessed. Source was kept outside this repository. The implementation agent must not
read that source; see [clean-room roles](../CLEANROOM.md#roles).

The reference's repository license includes a non-commercial restriction. It is not a source dependency suitable
for this MIT project. Its encryption module credits another decoder for protocol behavior and coding matrices.
That dependency was not inspected. No source, comments, coding matrices, provisioning constants, firmware, or
captures are carried over here. This newly written prose records protocol facts and identifies open questions;
the MIT header applies to this prose, not to the reference source. Maintainers should retain the separate
provenance record required by [the clean-room policy](../CLEANROOM.md#records-and-checks).

The reference claims bidirectional hardware interoperability with one stock-firmware family and decoder checks.
Those are author reports, not independently reproduced results. Motorola and AnyTone compatibility remains an
acceptance target, not a verified result. Model support belongs in the [feature matrix](../feature-matrix.md).

## 2. Scope and conventions

This research covers encrypted voice only. Do not infer a packet-data encryption mode from the voice mode.
Provisioning, flash storage, key wrapping, USB commands, and user interface choices in the reference are outside
this protocol description.

An octet is eight bits. Multi-octet integers are most-significant octet first unless stated otherwise. Bits in an
octet are numbered by transmission order: bit 0 is the most significant. A voice superframe has bursts A through F;
each burst carries three 72-bit coded vocoder frames. Frame index within the superframe is 0 through 17. The
encryption input is the 49 decoded AMBE+2 parameter bits, in the parameter order already used by basic privacy.

## 3. Call signaling

The voice LC retains the ordinary group or private call format. The reference uses manufacturer identifier
`0x10` and sets service-options bit `0x40` for encrypted voice. Other service-option bits retain their meaning.

The reference sends a Privacy Indicator header after the voice LC preamble and before voice burst A. The decoded
PI information is 80 bits, followed by the PI CRC and ordinary data-burst coding. The PI CRC mask is `0x6969`;
the standard air-interface coding and CRC definition must come from ETSI TS 102 361-1, not reference matrices.

| Information octets, zero-based | Meaning in the reviewed reference |
|---|---|
| 0 | Algorithm identifier: `0x25` for AES-256 |
| 1 | Manufacturer identifier: `0x10` |
| 2 | Key identifier, eight bits |
| 3 through 6 | Initial Message Indicator (MI), 32 bits |
| 7 through 9 | Call destination, 24 bits |

The reference recognizes `0x24` as AES-128 when parsing a header, but its cipher path is AES-256. Parser acceptance
does not establish AES-128 support. A RadioFoundry AES-256 implementation must reject unsupported algorithms.

The initial MI describes the first voice superframe. Re-reading a cached initial PI during an established call
must not reset the current superframe's cipher position. A PI must be associated with its actual call and data
type, with valid length and CRC. Parsing arbitrary LC bytes as a PI based only on matching octet values is unsafe.

The existing RadioFoundry display interpretation of the first two PI octets as algorithm and key does not match
this layout: octet 1 is the manufacturer identifier, and octet 2 is the key identifier.

## 4. MI, IV, and keystream

Let the current superframe MI be a 32-bit unsigned value. The first four IV octets are its big-endian encoding.
The remaining 96 IV bits are successive feedback bits of a left-shifting 32-bit linear feedback shift register
initialized from that MI. The feedback bit is the exclusive-or of the old state bits numbered 31, 21, 1, and 0,
where bit 0 is the least significant bit. A shift drops the old bit 31 and inserts the feedback at bit 0. Feedback
bits are appended to the IV in generation order, most-significant bit first in each octet.

The next superframe MI is the big-endian value of IV octets 4 through 7. Equivalently, it is the register state
after the first 32 feedback shifts. IV construction continues for 96 shifts, but that final state is not the next
MI. Repeat this mapping once per superframe.

Use the standard AES-256 block cipher with a 32-octet key and 128-bit OFB feedback. The initial OFB feedback value
is the derived 16-octet IV. Each output block is AES encryption of the preceding feedback value. Discard the first
16-octet output block. Begin voice keystream at the following block, with bits read most significant first.

For superframe frame index j and parameter index i, with j from 0 through 17 and i from 0 through 48, the XOR bit
is at bit offset `128 + 56j + i` in the full OFB output before discarding. Thus each frame occupies seven octets:
49 keystream bits are applied and seven are unused. Do not pack successive frames at 49-bit intervals. A
superframe reserves 126 octets after the initial discard; unused tail bits are not carried into the next one.

On TX, encrypt parameter bits before generating their AMBE error-correction codeword. On RX, correct and decode
the codeword before applying the XOR. Encrypting the 27 coded octets of a burst directly is a different format.
The reference encrypts every transmitted frame, including silence. The four signaling positions below replace
parameter bits 45 through 48 in this AMBE ordering. The first 45 parameters remain recoverable; those four
low-priority parameters cannot be compared to the original after signaling insertion.

The reference's receiver exempts recognizable clear silence and comfort-noise patterns from XOR while preserving
position. This is a receiver heuristic, not established DMRA behavior. Its necessity and false-positive effect
need independent testing; it must not justify sending clear voice on an encrypted channel.

## 5. In-band late-entry MI

The MI conveyed during a superframe is the MI for the following superframe. Using it for the superframe that
carried it creates a one-superframe displacement. The initial PI supplies the starting MI before this look-ahead
information becomes available.

The 32 MI bits, most significant first, are followed by four check bits. The check is the remainder of the
32-bit message polynomial multiplied by x^4 and divided by x^4 + x + 1, complemented across its four bits. Split
the resulting 36 bits into three consecutive 12-bit information words. Each is protected by systematic extended
Golay(24,12,8) coding, with its information bits preceding its 12 parity bits.

The parity orientation was verified independently on 2026-11-14 (section 9.1). Use systematic polynomial
division with generator `0xC75`: the 12 information bits precede the 11 remainder bits, then one overall even
parity bit. The name Golay(24,12,8) alone does not specify this orientation. Reference matrices are not reproduced.

Each vocoder codeword carries four late-entry bits. In its 72-bit, most-significant-bit-first on-air numbering,
the four bits of the signaling nibble, from most to least significant, occupy positions 71, 67, 63, and 59.
Insert them after generating voice FEC; collect them before any voice decoding changes their representation.

For codeword position k within a burst, k from 0 through 2, take Golay word k. Its information bits 0 through 3
are sent in burst A at that codeword position, bits 4 through 7 in B, and bits 8 through 11 in C. Its parity bits
0 through 3 are sent in D, bits 4 through 7 in E, and bits 8 through 11 in F. Each four-bit group preserves bit
order within the nibble. All six bursts therefore carry 72 signaling bits.

The reviewed receiver corrects up to three errors per extended Golay word and requires the recovered CRC4 to
match before accepting an MI. Missing fragments must be tracked explicitly. A cleared fragment buffer is not
proof that a complete superframe arrived. A CRC4 is an error check, not authentication.

## 6. Late-entry algorithm and key announcement

Burst F also carries a single embedded block identifying the algorithm and key. Its 11 information bits are
the eight-bit key identifier followed by three algorithm bits. The three-bit algorithm value is 5 for AES-256;
the reference also labels value 4 as AES-128.

The reference applies systematic Hamming(16,11,4), repeats the resulting 16-bit word with even column parity,
and applies the non-RC single-burst BPTC interleave to produce 32 embedded bits. Its parity orientation and
interleave match independently derived ETSI vectors (section 9.1). No reference coding tables are reproduced.

The reference requests burst F with LCSS `00`, but does not explicitly set its EMB PI bit in that path.
The spec writer also traced its transmit controls and transport writes: no firmware PI synthesis was found,
and no documented modem rule establishes how the transmitted EMB PI is produced. The emitted bit remains
unverified. ETSI's EMB PI is the pre-emption and power-control indicator, distinct
from a Privacy Indicator header. Under aligned timing, PI `0` associates embedded signaling with the same
logical channel and PI `1` selects reverse-channel signaling. An encrypted voice service option does not
establish the EMB PI value. Confirm the encryption-specific convention through modem behavior and captures.

The in-band MI alone supplies neither an algorithm nor a key identifier. The reviewed receiver sometimes guesses
a key from channel configuration or previous calls instead of decoding this announcement. Such fallback is not
evidence of correct multi-key late-entry interoperability. RadioFoundry should use a validated PI or the decoded
algorithm/key announcement and an explicit matching key; OFB provides no reliable wrong-key detection.

## 7. Stream lifecycle and losses

Maintain separate cipher state for each logical receive stream and for transmit. Bind receive state to the call,
timeslot, algorithm, and key identifier. End or replace that state when the call ends, its identity changes, the
channel changes, or the modem is reset. Do not associate a previous call's cached PI or fragments with a new call.

Use the actual on-air burst and frame position to select keystream bits. Losing a voice frame must not shift the
positions of later frames. At every known superframe boundary, advance the MI once even when some voice was lost.
Duplicates do not advance it. If elapsed superframes cannot be determined, mute and reacquire validated signaling
rather than continue with an uncertain position.

A successful late-entry MI decode establishes the next superframe's MI. Without a valid PI, playback also needs
reliable encryption, algorithm, and key identification. Ordinary unencrypted voice may accidentally satisfy the
late-entry error checks; it must not trigger decryption merely because a possible MI was found.

The reference advances PI-established calls from the LFSR prediction and treats late-entry-established calls
differently when a decoded MI disagrees. This is an implementation policy, not a protocol requirement. Neither
branch establishes recovery after losing an entire superframe. Test recovery independently.

On TX, key selection, initial MI generation, PI emission, encrypted frame generation, and late-entry signaling
must succeed as one call setup. If a selected encryption key is missing, refuse encrypted PTT. The reference
can instead begin a clear transmission; that behavior must not be carried over. Abort and restart must clear all
encryption state before another call, including a deliberately clear one.

## 8. Key representation and MI safety

The cipher key is 32 octets. Hex entry represents those octets as 64 hexadecimal digits in written order; it is
not a passphrase or a decimal integer. Key identifier and local storage-slot number are separate concepts.
The PI key field is eight bits; the reference's smaller key-store capacity and use of zero as an off sentinel are
local choices, not demonstrated protocol restrictions. Vendor flash wrapping and byte reversal are storage
formats and must not be applied to the over-the-air cipher key.

Each call needs a fresh initial MI. The reviewed TX requests a hardware random value but can fall back to an
uptime counter. Do not copy that fallback. A zero MI produces a zero IV and never changes under this LFSR.
Reusing an MI with the same key repeats the corresponding OFB keystream; linear MI expansion does not add entropy
to the 32-bit input. The current random-MI policy and its limits are documented in section 10. Random sampling alone cannot
guarantee no collisions in a 32-bit space.

This voice format supplies confidentiality without a message authentication tag. Neither PI checks, voice FEC,
nor the late-entry CRC authenticates a sender or proves that a selected key is correct.

## 9. Implementation gates and validation

The extended Golay parity orientation and embedded-block coding are resolved by section 9.1. Confirm the full
PI layout, algorithm identifiers, EMB flags, and late-entry timing against the official DMRA specification if
it becomes available. This review does not grant access to that specification or resolve its terms.

The implementation uses a separate build switch, two full-width key slots, a generic entropy interface,
PI TX/RX through the modem contract, and separate stateful receive and transmit contexts. The static basic-privacy
hook remains unchanged.
See [architecture](../dev/architecture.md), [build configuration](../build-config.md), and
[privacy gotchas](gotchas.md#6-privacy). The implementation is in `core/dmr/call/dmr_aes.c` and `dmr_call.c`.

Validation must include independently sourced standard AES/OFB known-answer tests; MI/IV and frame-offset
vectors; first-frame and multi-superframe voice; PI CRC rejection; unsupported algorithm and missing-key refusal;
late entry with multiple key IDs; fragment corruption; missing and repeated bursts; whole-superframe loss;
rapid call changes; clear calls between encrypted calls; abort and reset; and encryption disabled at build time.
Round-tripping one implementation against itself is insufficient evidence of interoperability.

Hardware acceptance requires both TX and RX against each claimed vendor mode, including late entry and key-ID
selection. Record model and firmware versions on the appropriate bring-up pages. Hardware work requires separate
owner authorization and RF opt-in under [the HIL rules](../../tests/hil/README.md). No such tests were performed
for this document, and no test keys or recordings are included.

### 9.1 Independent coding verification

On 2026-11-14, the implementation-side review used
[ETSI TS 102 361-1 V2.6.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/10236101/02.06.01_60/ts_10236101v020601p.pdf),
clauses B.3.1, B.3.4, B.2.2.1, and 9.3.2-9.3.3. The separate spec writer compared the reference's encoders
against the independently derived outputs without supplying foreign source to the implementer.

| Check | Result |
|---|---|
| Extended Golay orientation | All 4,096 information words match |
| Extended Hamming orientation | All 2,048 information words match |
| Complete non-RC single-burst interleave | All 2,048 information words match |
| Burst-F LCSS requested by the reference | `00` |
| Burst-F EMB PI actually transmitted | Unresolved; not explicitly set in the reviewed path |

Hamming uses generator `0x13`, four remainder bits, then overall even parity. Duplicate the resulting row.
Number matrix bits alternately from the two rows at each column, left to right, and place index i at
transmission index `(17 * i) modulo 32`. Transmission index 0 is the most significant output bit.

Reproduce the standard-derived checks with `python3 tools/check_dmr_aes_coding.py`. The script checks the
standard's matrix suffixes against polynomial division, its transmission ordering against the interleave
formula, and the minimum distances. Its vectors are protocol inputs and coded outputs, not encryption keys.

| 11-bit information | Extended Hamming | 32-bit transmitted block |
|---|---|---|
| `001` | `0027` | `0415082A` |
| `005` | `00BE` | `45548AA8` |
| `00D` | `01A8` | `44428881` |
| `123` | `2460` | `1C202C10` |

The native `ambeFecTest` now checks the standard's eight shortened Golay basis vectors. `dmrFecTest` checks
all extended Hamming words against the standard matrix. Both pass. These checks establish coding agreement;
they do not verify encrypted audio, DMRA conformance, or hardware interoperability.

## 10. Firmware integration

`CONFIG_DMR_AES256` follows `CONFIG_DMR` by default. With it off, cipher code and AES call state are absent. A persisted AES transmit selection still refuses DMR PTT;
disabling the build switch must not silently turn that selection into clear voice.
The radio snapshots two key slots into the call stack. The codeplug's AES ring is authoritative when present,
including an explicitly empty ring; only an absent record permits fallback to local settings. A malformed record
provides no usable keys. Key IDs are independent of storage slots. Both valid keys are available for receive.
The Codeplug Editor provisions the ring under Radio > AES-256, using 64 hexadecimal digits and an on-air ID per
slot. Binary codeplugs carry these unwrapped keys and must be kept private. See
[codeplug records](codeplug-settings.md#33-aes-provisioning-and-channel-privacy).

Each channel chooses inherit, clear, or AES-256 with an on-air key ID. Inherit follows the radio-wide selection
(`aes_tx_slot`, or basic privacy); clear disables TX privacy on that channel. AES-256 requires a matching valid
key ID regardless of the global selection. Invalid policies and missing selected keys refuse PTT. Copy to VFO
retains the mode and key ID across restart; it does not duplicate key material. The radio-wide Privacy tile
changes only the inherited selection, not an explicit channel policy. Selecting basic privacy disables the
radio-wide AES selection.

Debug link RADIO operation 39 provisions a key or disables AES transmit. It rejects duplicate key IDs and changes
during a transmission. The radio task applies the key selection; housekeeping saves the settings through the
existing journal. This provisions the local fallback ring; it does not override a codeplug ring. Key bytes are not
returned in diagnostics. The journal stores them without key wrapping.
The menu's decimal basic-privacy editor does not provision AES keys. See [user instructions](../user/09-dmr.md#aes-256).

Transmit requires a modem with PI and single-burst embedded-block support, an installed key, and healthy hardware
entropy. The platform entropy hook rejects startup output, hardware clock/seed errors, repeated output, and timeout.
There is no time-counter or device-ID fallback. A platform without the hook can receive but refuses AES transmit.
The entropy implementation follows the RNG chapter of
[RM0090](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-415-stm32f407-417-stm32f427-437-and-stm32f429-439-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf).

Each call samples a nonzero 32-bit MI. Restart uses fresh hardware entropy. Random selection does not guarantee
uniqueness across calls or radios: the wire format has only a 32-bit MI space, and its linear expansion adds no
entropy. Operational key rotation is required; this implementation does not maintain a persistent history of
used MIs. It is not suitable where guaranteed IV uniqueness or authenticated voice is required.

The call stack sends three voice LC headers, a PI header, then encrypted superframes. Queue retries preserve the
same frame position. The MI advances only after burst F is queued. Both modem engines end an encrypted call on
queue underrun instead of generating clear silence. Voice F carries the raw encoded key/algorithm announcement
with LCSS 00. EMB PI remains a hardware verification item.

Receive validates the complete PI CRC, algorithm, manufacturer, destination, and key ID. A duplicate PI does not
rewind an established stream. A new PI or call identity replaces the old state. Burst indices and available slot
counters determine keystream position through loss. Uncertain timing mutes the stream. Complete in-band signaling
can synchronize the next superframe when a modem exposes its raw embedded block. Current hardware drivers do not
expose that receive block, so joining an encrypted call without its PI is not yet available through those drivers.
Do not guess a key from the active transmit slot. A call acquired with its PI can advance through known missing
bursts without that block.

`dmrAesTest` checks NIST AES/OFB vectors, an independently generated OpenSSL stream, MI/IV mapping, signaling,
key IDs, incomplete late entry, and reset. `dmrCallTest` checks actual TX-to-RX burst flow and fail-closed PTT.
Modem tests check the F announcement and encrypted underrun. Settings tests check key persistence and upgrade
from pre-AES records. The HIL diagnostic test is read-only; hardware encryption acceptance remains manual.
