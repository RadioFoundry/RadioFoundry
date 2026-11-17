<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Air interface

The air interface carries channel symbols with time stamps between a radio's modem hardware and the core. It knows
no mode. A digital mode the core frames itself (P25, [p25.md](p25.md)) describes what it needs as a profile, and
every layer below works from that profile. Built with the modes on it: `CONFIG_P25`, `CONFIG_NXDN` ([nxdn.md](nxdn.md),
[build-config.md](../build-config.md); `CONFIG_AIR` is derived from them). Which
radios have a provider: [feature-matrix.md](../feature-matrix.md).

State: host tested. The HR_C6000 provider is also checked on air both ways with C4FM
([modem-hrc6000.md](modem-hrc6000.md) 10.5). A provider answers a fit with `AIR_FIT_UNTESTED` until its board says
it has been checked.

## 1. Layers

| Layer | File | What it does |
|---|---|---|
| Mode | `core/p25/`, `core/nxdn/` | units, FEC, calls; an `air_mode_t` (profile, frame geometry, hooks) |
| Session | `core/air/air_session.c` | one mode on one provider: open, close, receive to the framer, a transmission from the first symbol to its end (section 7) |
| Framer | `core/air/air_framer.c` | sync search, levels, frames cut from the symbol stream, coasting; symbols packed into transmit chunks (section 5) |
| Provider | `board_t.air` (`hal/hal_air.h`) | symbols in air order with time stamps (section 3). A modem chip in its physical layer mode, or the soft modem on sample taps |
| Soft modem | `core/air/air_softmodem.c` | discriminator samples to symbols, for a provider that receives through a tap (section 6) |
| Sample taps | `board_t.baseband` (`hal/hal_baseband.h`) | the discriminator read by the MCU, samples from the MCU to the modulator (section 6) |

Both `board_t.air` and `board_t.baseband` are optional and independent: a board gives the symbol interface, the
taps, both or neither. The core uses `board_t.air`; a board whose chip has no symbol pipe for a direction builds its
provider from the taps and the soft modem.

FEC shared by the modes is in `core/fec/` (Reed-Solomon over GF(64), Golay, Hamming, BCH(63,16), the trellis codes, the
rate 1/2 convolutional codes of constraint length 5 with a Viterbi decoder).

## 2. Symbols and profiles

A symbol is a dibit everywhere: `01` = +3, `00` = +1, `10` = -1, `11` = -3, packed four to a byte, the first on air
in bits 7-6 of byte 0. A 2-level mode uses `01` and `11`. Received symbols can carry a soft value each: a signed
byte, `AIR_SOFT_OUTER` (96) for a +3 symbol at the profile's deviation.

`air_profile_t` is everything a provider needs to know about a mode:

| Field | Meaning |
|---|---|
| `sym_rate`, `levels` | symbols per second (4800, 2400, 1200), 2 or 4 levels |
| `shape`, `shape_param` | transmit pulse: root raised cosine, C4FM, Gaussian, square; roll-off or BT x 100 |
| `dev_outer_hz` | deviation of the outer symbol; the inner one is a third of it |
| `timing`, `slot_syms` | continuous, or TDMA with this many symbols per slot |
| `bw` | channel bandwidth the mode wants |
| `sync[]` | the fixed patterns the mode puts on air, most useful first: symbols, length (8 to 24), errors tolerated, where the frame starts relative to the pattern, whether it is only at the start of a transmission |
| `sync_every_syms` | the longest run of symbols between two 24-symbol patterns inside a transmission; 0 when none recurs |
| `sync_any_syms` | the longest run between two patterns of any length (a mode whose recurring sync is shorter than 24 symbols); 0 = `sync_every_syms`. The soft modem holds its timing this long |
| `idle` | four dibits a provider sends when a continuous transmission runs dry |

## 3. The provider (`hal_air_t`)

One session at a time, `open` to `close`. The core stops the board's DMR stack before `open` and starts it again
after `close`; the board's RF side is already tuned and in its digital mode.

| Call | Meaning |
|---|---|
| `fit(profile)` | what the provider gives this profile: `AIR_FIT_xxx` bits (section 4), 0 = can't |
| `open(profile, cb, ctx)` | start receiving; events go to `cb` at task level, never from an interrupt |
| `close()` | back to how the board was: FM and DMR work again without a restart |
| `rx_restart()` | drop what the receiver holds and search again (after a retune) |
| `tx_queue(chunk)`, `tx_space()` | queue `tx_chunk_syms` symbols; how many chunks the queue takes now |
| `tx_begin()` | start modulating the oldest queued chunk. At least one chunk is queued and `hal_radio_t.tx()` was called first |
| `tx_end()` | no more chunks will come: the newest queued one is the last |
| `tx_abort()` | stop at once and drop the queue |
| `now_us()` | the clock of every time stamp here: free running microseconds |
| `slot_clock()`, `slot_slew()` | TDMA providers: the latest slot boundary, and stretching the next slot |
| `rx_load(work_us, span_us)` | optional: what receive costs the MCU since `open`: the time the provider spent turning samples into symbols, and the time of signal they covered. NULL when a chip does that work; false when a chip does it for the profile that is open |

Constants a provider states: `tx_chunk_syms`, `tx_queue_depth`, `tx_lead_ms` (how far ahead of the air the queue
must be kept) and `time_accuracy_us`.

Events:

| Event | Meaning |
|---|---|
| `AIR_EV_RX` | a chunk of received symbols: time of symbol 0, 1 to 144 symbols, hard dibits, soft values if the fit says so, where the provider's own correlator saw a sync (a hint), flags: a gap before it, timing coasting, no signal, a TDMA slot start |
| `AIR_EV_TX_STARTED` | the first queued symbol is on air, with its time |
| `AIR_EV_TX_SPACE` | a chunk left the queue |
| `AIR_EV_TX_UNDERRUN` | the queue ran dry before the last chunk: idle symbols went out |
| `AIR_EV_TX_DONE` | the last chunk has left the air, or the abort is through; receiving again |
| `AIR_EV_SLOT` | TDMA: a slot boundary, with its counter and time |

A chunk starts wherever the provider's record does, never at a frame boundary. The framer finds the frames.

What every provider keeps while a session is open:

1. Raw receive never reaches the speaker. `open` fails when the board can't guarantee that.
2. The microphone, tones and beeps never reach the modulator.
3. It keys the transmitter only inside the core's permission: between `hal_radio_t.tx()` (the core's inhibit, band
   and power checks) and `tx_off()`, which the core calls at `AIR_EV_TX_DONE`. Never by itself.
4. `close` restores the board.

## 4. Fit

| Bit | Meaning |
|---|---|
| `AIR_FIT_RX`, `AIR_FIT_TX` | the direction works for this profile |
| `AIR_FIT_SOFT` | a soft value per received symbol |
| `AIR_FIT_SHAPE_APPROX` | the transmit pulse is the provider's own, not the profile's |
| `AIR_FIT_DEV_APPROX` | transmit deviation off by more than a tenth |
| `AIR_FIT_RATE_DOUBLED` | the profile runs at half the provider's rate: the framer sends each symbol twice |
| `AIR_FIT_NEEDS_SYNC` | receive starts only at a 24-symbol pattern: no late entry on shorter syncs |
| `AIR_FIT_TDMA` | slot clock and per-slot keying |
| `AIR_FIT_FULL_DUPLEX` | receives while it transmits |
| `AIR_FIT_UNTESTED` | from register facts, never checked on air on this board |

A provider with a correlator of its own takes the first patterns of the profile it can use (24 symbols, all outer,
not start-only). A provider that receives only while it sees such patterns compares `sync_every_syms` with what it
can coast.

## 5. The framer

Receive:

| Step | Rule |
|---|---|
| Search | the last 24 symbols against every pattern of the profile, each within its error limit |
| Levels | with soft values, a least squares fit of the pattern's levels gives the signal's level and offset; a match that does not lie on a line is noise. The frame's symbols are sliced again with those levels |
| Head | a mode with units of different lengths gives a head hook: it sees the first symbols behind a sync, confirms the unit (or says it is none: the search goes on, replaying what it held) and gives its length |
| Frames | a frame is handed over whole, or in pieces when it is longer than the mode's buffer. A pattern may sit inside its frame (a negative start offset): the framer keeps history for it |
| Coasting | a frame whose sync is missed is placed by counting, up to the mode's limit; then the mode is told the signal is lost |
| Gaps | a chunk that says symbols were lost, or no signal, ends the lock |

Transmit: the mode sends everything on air as symbols, its preamble and end marker included. The framer packs them
into the provider's chunks, doubles them for `AIR_FIT_RATE_DOUBLED`, and fills the last chunk with idle symbols
where the provider needs whole chunks.

## 6. Sample taps and the soft modem

`hal_baseband_t` describes a board's taps: rate (8 kHz or more), band, scale in counts per kHz when known, start,
read or write, stop, and the sample clock. The isolation rules of section 3 hold while a tap runs.

`air_softmodem` turns a receive tap into the receive half of a provider, for any 2- or 4-level profile the tap's
band carries (`air_softmodem_rx_fit`: the band must reach a little past half the symbol rate and, for four levels,
down to a hundredth of it).

| Step | What |
|---|---|
| Offset | a slow average of the input is taken off; it holds while a signal is locked |
| Resampling | a polyphase interpolator (12 phases, 8 taps) to five samples per symbol |
| Receive filter | an integrate and dump over one symbol for C4FM and square pulses, a shorter one otherwise |
| Sync fit | the newest symbol-spaced samples are fitted to each pattern (level and offset, least squares). Where the error passes through a minimum under the limit, that sample is a symbol center; a parabola through the neighbors places it between samples |
| Gate | the fit runs only where a pattern can be: per sample phase the modem keeps one bit per symbol (this sample was above the one a symbol before), and a pattern is fitted when the steps of four levels or more among its last 17 symbols are all there but one. Two missing steps put a fit over the limit whatever the rest does, so the gate changes no output; it removes the fit from all but a few percent of the samples of noise or data |
| Output | symbols leave from the far end of a delay line 26 symbols long, sampled on the latest sync's timing and scaled by its level and offset, so a sync is sampled on the timing it gave |
| Level | a tap of known scale starts at the profile's deviation. A tap of unknown scale follows the input's average magnitude until the first sync gives the level. The averages start fast and slow down; when a 16-sample average of the magnitude is off the slow one by half or double (a signal after an unmodulated carrier, which leaves the discriminator quiet), both start fast again |

Between syncs the timing, level and offset hold. Two crystal clocks do not move a fifth of a symbol in a second, so
a mode whose syncs recur gets every frame on fresh timing. Chunks say `AIR_CHUNK_FREE_RUN` when no sync was seen
within the profile's spacing, and `AIR_CHUNK_GAP` after a hole in the samples (the modem starts over).

Cost: 3.3 KB of code and about 370 bytes of state, which the provider places. No transmit half is written: a
provider with a tap for receive only sends through its chip.

The work is per filtered sample, 24,000 a second at 4800 symbols, so that path decides the load. Its rules are in
`core/dsp/fxp.h`, with the kernels the signal code shares (the interpolator's dot product, exact scaling in 32-bit
steps, 64-bit products for the voice codecs), each in plain C and with a core's own instructions where they help:

| Rule | Why |
|---|---|
| No 64-bit product, no divide, no `%` on a ring index | a library call or a divide instruction per sample on a core without a long multiply |
| `int` or `unsigned` counters and indexes | a narrower one costs an extend on every step on some cores |
| The input history written twice, a filter's length apart | the interpolator reads its samples as one run, so a core with paired 16 x 16 multiply-accumulates takes them two at a time |
| The fit behind the gate | it was nine tenths of the work |

Measured on the `hrc7000` platform (no paired multiplies, no long multiply without its HI/LO unit) with an 8 kHz
tap and the P25 profile, receiving noise: 8.5 % of the CPU (`rx_load`), against 33 % with the fit at every sample,
a 64-bit gain product and ring indexes by `%`. The HI/LO unit's multiply-accumulate would save one instruction in
eight of the dot product and is not used: the context switch does not save HI and LO. Open: a provider that knows
the channel is empty (`AIR_CHUNK_NO_SIGNAL`) could stop feeding the modem.

## 7. The session

`air_session` runs one mode on one provider. Two tasks use it:

| Task | Does |
|---|---|
| The radio core's | opens and closes; primes a transmission (the mode queues its first symbols), calls `hal_radio_t.tx()`, then `air_session_tx_go`; at `AIR_TX_DONE` calls `tx_off()` and clears |
| The provider's | delivers events: received chunks go to the framer and on to the mode's hooks; once a transmission runs, `tx_fill` asks the mode for more symbols whenever a chunk has left the queue |

Before `air_session_tx_go` only the radio core's task touches the transmit side, after it only the provider's.
A mode that has nothing ready in `tx_fill` adds nothing and is asked again one chunk later; it must add something
before the queue falls under the provider's lead, or the provider sends idle symbols and reports an underrun.

Transmit states: idle, priming, on, ending (the last symbol is queued), done.

In the radio core each mode on the air interface fills one table of these steps (`radio_air_ops_t`,
`core/radio/radio_air.h`: running, no TX, prepare, go, stop, abort, state, clear), so PTT, the time-out and the
release are one path for every mode. A mode whose session opened with `AIR_FIT_RX` and no `AIR_FIT_TX` refuses PTT
before anything is queued: `tx_deny=air_no_tx` ("This mode: receive only").

## 8. Users

| User | State |
|---|---|
| P25 Phase 1 conventional voice | written, host tested ([p25.md](p25.md)) |
| NXDN conventional voice, 4800 and 9600 bit/s | written, host tested, in the radio core ([nxdn.md](nxdn.md) 6) |
| A DMR engine on symbols | not written. The interface carries TDMA (slot length, slot events, the slot clock, per-slot transmit chunks, syncs inside the burst); today's DMR runs on the modem chip's own layer 2 |
| 2-level modes | carried (the dibit map, the soft modem's fit) |
| AFSK (APRS) | not a symbol mode: its tones ride the audio band. The sample taps can feed a demodulator for it; nothing uses them for that yet |

## 9. Bench access

`CONFIG_AIR_RAW` adds debug link commands that open a session and move raw symbols and tap samples, for bench tests
([debuglink.md](debuglink.md)). Off by default. While the bench holds the modem, PTT is refused with
`tx_deny=bench` ("Air bench: PTT off").

## 10. Tests

| Test | Covers |
|---|---|
| `tests/host/fecTest.c` | the shared codes against the standard's matrices |
| `tests/host/airFramerTest.c` | sync search, levels, head hook, pieces, history, coasting, transmit packing |
| `tests/host/airSoftmodemTest.c` | the soft modem on a modeled tap: every tap phase, 8 to 48 kHz, scale and offset errors, noise, other pulse shapes, a P25 call end to end; the same output with the gate and with every sample fitted |
| `tests/host/fxpTest.c` | the shared fixed-point kernels against wide arithmetic |
| `tests/host/p25EngineTest.c` | the session under two P25 engines on made-up providers |
| `tests/host/nxdnEngineTest.c` | the session under two NXDN engines; an NXDN call through the soft modem at 2400 symbols/s |
