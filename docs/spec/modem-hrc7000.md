<!-- SPDX-License-Identifier: MIT -->
# HR_C7000 as a DMR modem

How the baseband inside the HR_C7000 SoC is driven as a DMR Tier II layer-2 modem and as an FM modem: registers,
buffers, interrupts, what to write when, and what the chip does by itself. Board-level facts (pins, RF chip, PA
timer, calibration source) are on the board's hardware page ([feature-matrix.md](../feature-matrix.md) lists the
boards); the stack-side contract is in `dmr-modem.md`; the HR_C6000 equivalent is in `modem-hrc6000.md`.

| Contents | |
|---|---|
| 1 | System view: memory map, buffers |
| 2 | Register map: mode, sync words, CRC masks, interrupts, layer 2, BS mode, physical layer transmit |
| 3 | Interrupts and TX timing |
| 4, 5 | Initialization, entering DMR, receive |
| 6, 7 | Transmit sequences, CRC and FEC |
| 8 | FM mode: registers, tones, DTMF, prompt FIFO, receive audio |
| 9, 10 | Driver requirements, open questions |

Sources: the HR_C7000 chip user guide v2.7 (2016; "the datasheet" below) and measurements on a radio or the SDR bench
(marked "measured"). Values without a mark are the ones the driver writes and that work on air (simplex voice both
ways and repeater wake are checked on the HR_C7000 radios, `docs/feature-matrix.md`). "(unverified)" marks what has
not been confirmed on a radio.

---

## 1. The modem's view of the system

| Item | Value |
|---|---|
| Register block | 0x11000000, 32-bit registers (names below are the datasheet's) |
| TX RAM (layer-2 send RAM) | 0x16000000, 1200 bytes, byte wide |
| RX RAM (layer-2 receive RAM) | 0x160004B0, 1200 bytes |
| AMBE voice RAM | 0x16000960–0x1600097A, 27 bytes: one burst's three 72-bit AMBE+2 frames, both TX and RX |
| Prompt (beep) FIFO | 0x16000980 |
| Codec registers | 0x160009C0 (byte wide) |
| PCM buffers (system SRAM) | mic PCM 0x18000000–0x1800009F (hardware writes), speaker PCM 0x180000A0–0x1800013F (CPU writes) |
| Vocoder | none in hardware: the CPU runs the AMBE+2 codec on the PCM buffers and moves 27-byte bursts through the voice RAM (`codec-placement.md`) |
| Symbol / sample clocks | 4800 baud 4FSK; 30 ms timeslots; internal 38.4 kHz sampling (AK2401 note) |

The modem runs DMR at layer 2: the CPU builds every burst's information bits (LC, CSBK, data blocks, AMBE) and the
chip adds the FEC (BPTC, Golay slot type, RS(12,9), QR EMB, CRC with masks), sync, CACH handling and timeslot
timing. Layer 3 (the chip's own call control, SMS, BS activation) exists but is not used by any working firmware.

---

## 2. Register map used for DMR

Offsets from 0x11000000. Values are the ones the driver writes unless noted.

### 2.1 Mode and RF interface

| Offset | Register | Fields | Value used | Why |
|---|---|---|---|---|
| 0x100 | WORK_MODE | b8 digital_analog_en; b7 modulator_mode (0 DMR, 1 FM); b6 Tier II; b5 timeslot mode; b4:3 layer (0 PHY, 1 L2, 2 L3); b2 is_repeater; b1 is_aligned; b0 work_slot_sel | **0x80** FM, **0x6E** DMR (Tier II, timeslot, layer 2, repeater bit, aligned, slot 1) | Only these two values are ever written. Timeslot choice is done in software with LAYER2_TXRX_CTRL, not with work_slot_sel |
| 0x104 | RF_MODE | b25 phase_avg_en; b24 af_mode; b23:16 AF gain (n/16); b15 RX IQ invert; b14:13 RX mode; b12 dynamic sync threshold; b11 use_dual_phy (second timeslot receive channel, 2.1a); b9 soft ramp; b7 TX IQ invert / deviation mapping; b6:5 TX mode (3 = two-point) | **0x03269860**: AF receive on with averaging, gain 0x26 (2.375), RX IQ inverted, dynamic sync threshold, b11 set (2.1a), two-point modulation | Written once at init and left in both modes. Do not switch to 0x03269060 (b11 clear) in DMR |
| 0x108 | SIG_CENTER | b31:24 TX I bias, b23:16 TX Q bias, b15:8 / b7:0 RX I/Q bias | (cal << 24) or (cal << 16) | TX center from factory calibration (board-specific) |
| 0x10C | TX_IF_FREQ | b23:0 | 0x0BB800 (reset value) | |
| 0x110 | RF_CONTROL | b20:16 tx_pre_on (100 us steps); b13:8 rf_pre_on_tx (RF TX interrupt advance, 100 us); b5:0 rf_pre_on_rx | **0x00041F1A**: tx_pre_on 0.4 ms, RF TX interrupt **3.1 ms** before the TX burst, RF RX interrupt 2.6 ms before RX | 0x00040A0A (1.0 ms) gave bad timeslots at low power. b13:8 is per board (`hrc7000_t.rf_tx_lead`): a board whose 3.1 ms clipped the inbound burst before ours uses 0x14 (2.0 ms) |
| 0x114 | RF_MOD_BIAS_CTRL | b25:16 two_point_bias; b15:8 sig_reduce (Mod2 amplitude); b7:0 phase_reduce (Mod1 amplitude) | ((cal + 384) << 16) or modcal (0x4D4D before a frequency is set) | Modulation balance from calibration |
| 0x118 | DEV_LIMITER | b15:0 | 0xFF00 (reset value) |
| 0x11C | RSSI_BIAS_VALUE | read-only per the datasheet: rssi b31:16; b15:8 / b7:0 the measured DC bias of the ADC's I / Q inputs | 0x1381 is written (no effect expected) |
| 0x128 | PHASE_OUT | read-only: b29:16 / b13:0 the largest and smallest 14-bit AF phase in AF receive mode | not written | A live check that AF reaches the ADC: a board receiving DMR reads about +1216 / -1149 (the datasheet expects about ±1300 at -47 dBm); a value that stays within a few counts means no AF arrives. One board had this when its audio-select GPIO was left high, which takes the RF chip's audio off the ADC |
| 0x120 | THRESHOLD_VALUE | b28:24 sync ratio; b23:16 sync low threshold; b15:8 sync high threshold; b7:0 signal detect | **0x0978786F** | |
| 0x164 | RECV_INFO_COUNT | b31:24 sync errors; b8 select FSK error/EVM; b7:0 FSK error | read for BER/EVM only |
| 0x168 | (undocumented, "REG_X06") | | 0x14 | |
| 0x1B0 | RX_IF_FREQ | b23:0 | 0x0BB800 | |
| 0x1B4 | RF_RECV_DELAY | b18:0, 100 ns steps | **0x36B0** (1.4 ms). the datasheet says 2000 (0.2 ms) for AF mode; the working value is 0x36B0 |

**AF receive input (datasheet, document 2).** In AF mode the RF chip's demodulated audio goes to the AF ADC's
P input, and a DAC output biases its N input; on some boards that is a board DAC output.
Document 1 says the AF bias is set through SIG_CENTER instead. A DC step on either input moves the PHASE_OUT center,
which the DMR receiver reads as a frequency offset. AF mode has no frequency offset compensation: RX_IF_FREQ applies
to IF mode only.

**Reference oscillator control (MOD1, datasheet).** In receive the MOD1 level is RF_MOD_BIAS_CTRL b25:16 over 0-3.3 V, which
holds only while DAC_CONTROL keeps that DAC powered. In TX it is SIG_CENTER b31:24, ±422 mV around 1.65 V in 3.3 mV
steps; RF_MODE b9 sets where the ramp starts (0 = 1.65 V). With cal + 384 in b25:16 both states give about the same
voltage, within one step (measured).

### 2.1a RF_MODE bit 11: the DCDM receive switch

The HR_C7000 datasheet names bit 11 `use_dual_phy`. It turns on a second receive channel, "for repeater chip receive
mode and DCDM mode" (`datasheets/C7000/HR_C7000_Document_2.pdf`, register table at p. 98; our translation from the
Chinese). DCDM is dual-timeslot direct mode: two conversations on the two DMR timeslots of one frequency, with no
repeater to supply sync. The community name is "dual-capacity direct mode"; the datasheet doesn't expand it. Its DCDM
set-up (section 18.3.5.1, p. 410) is three steps: turn off MS and BS sync receive, enable TDMA1 and TDMA2 sync
detection, and set RF_MODE[11]. Receive stays enabled all the time.

- **Channel mode.** The bit can follow the channel mode: simplex channels (RX frequency equal to TX frequency) set it
  and enable TDMA1 and TDMA2 sync; repeater channels clear it and enable only MS and BS sync. That is the
  datasheet's DCDM set-up on simplex. The datasheet also lists "repeater chip receive mode" as a user of bit 11, which
  a repeater channel with the bit clear suggests is a different mode. Not resolved.
- **RadioFoundry:** writes 0x03269860 (bit 11 set) and enables the TDMA1 and TDMA2 sync detection on every channel (`hrc7000.c`,
  the bench note of 2026-10-16), so RadioFoundry runs the DCDM receive set-up on repeater channels too. Whether that matters on a
  repeater is not measured.
- **Not a second RF chip.** The RF chip is unchanged. The second channel is a second modem receive path. Multi-chip
  dual watch is a separate feature (RadioFoundry issue #96).
- **Not known:** whether a simplex receiver with bit 11 on hears a second-timeslot call that it misses with the bit off.
  No on-air test has compared the two (section 10, item 7).

### 2.2 Sync words

24-bit halves of the 48-bit ETSI sync patterns, high half first (all written once at init):

| Offset | Register | Value | ETSI pattern |
|---|---|---|---|
| 0x12C / 0x130 | SEND_DATA_SYNC_H / L | 0xD5D7F7 / 0x7FD757 | MS sourced data |
| 0x134 / 0x138 | SEND_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |
| 0x13C / 0x140 | RECV_MS_SYNC_H / L | 0xD5D7F7 / 0x7FD757, then set bit 31 of H | MS data (receive) |
| 0x144 / 0x148 | RECV_BS_SYNC_H / L | 0xDFF57D / 0x75DF5D, then set bit 31 of H | BS data (receive) |
| 0x14C / 0x150 | RECV_TDMA1_SYNC_H / L | 0xF7FDD5 / 0xDDFD55 | TDMA direct slot 1 data |
| 0x154 / 0x158 | RECV_TDMA2_SYNC_H / L | 0xD7557F / 0x5FF7F5 | TDMA direct slot 2 data |
| 0x15C / 0x160 | RECV_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |

Voice syncs are not programmed; the chip derives them (they are the bitwise complements of the data syncs; unverified).
Bit 31 of the RECV_MS / RECV_BS high halves is an undocumented enable. The RECV_TDMA1 / RECV_TDMA2 high halves
need it too: without it the chip never detects the direct mode syncs (bench, 2026-10-16: an SFR's TS1 inbound was not
heard until it was set).

SEND_DATA_SYNC is the only send pattern; a voice burst goes out with its complement. RadioFoundry rewrites it before a burst
that asks for another sync family (`dmr_tx_burst_t.sync_override`: an SFR's TS2 direct mode outbound) and back to MS
for the next burst without one. On air the burst then carries that family (bench, 2026-10-16: TS2 sync with the
repeated LC).

### 2.3 CRC, RS and color code

| Offset | Register | Value | Meaning |
|---|---|---|---|
| 0x350 | PI_CRC_MASK | 0x6969 | PI header CRC-16 mask (init value 0 in b31:16) |
| 0x354 / 0x358 | RS_H_MASK / RS_H_INIT | 0x969696 / 0 | RS(12,9) mask for the voice LC header |
| 0x35C / 0x360 | RS_T_MASK / RS_T_INIT | 0x999999 / 0 | RS(12,9) mask for terminators (the datasheet's reset value 0x969696 is wrong for ETSI; write 0x999999) |
| 0x364 | CSBK_CRC_MASK | 0xA5A5 | CSBK CRC-16 mask |
| 0x368 | MBCH_CRC_MASK | 0xAAAA | MBC header CRC-16 mask (the datasheet's reset 0xA5A5 is wrong for ETSI) |
| 0x36C | DATAH_CRC_MASK | 0xCCCC | data header CRC-16 mask (the datasheet's reset 0xA5A5 is wrong for ETSI) |
| 0x370–0x37C | (undocumented) | 0xF0, 0x1FF, 0x10F, 0x00 |
| 0x380 | CRC8_INIT | 0 | short LC CRC-8 init |
| 0x384 | LOCAL_CC | color code in b3:0; b9:8 cc_opt = 0 (deframe only on matching CC); b16 encryption type | Set per channel; when cc_opt = 1 RF_MODE b12 must be 0 |
| 0x388 | VOICE_EMB_CTRL | 0 at init, **2** when entering DMR receive | b1 = burst F EMB from the chip's own RC encoder; b0 = PI bit for bursts B–E |
| 0x38C | SCRAMBLE_REG | 0 | built-in simple voice scrambler keys (RX in b31:16, TX in b15:0); unused |

### 2.4 Interrupt control

| Offset | Register | Value / use |
|---|---|---|
| 0x398 | SYS_INTERP_LIST | system interrupt causes (read) |
| 0x39C | SYS_INTERP_MASK | **1 = enabled** (inverted sense). **0x1187F**: bits 0–6, 11, 12, 16 |
| 0x3A0 | SYS_INTERP_CLEAR | write the bits read from the list; repeat until the list reads 0 |
| 0x3A4 / 0x3A8 / 0x3AC | LAYER3_INTERP_LIST / MASK / CLEAR | layer-3 causes; unused |
| 0x3B0 | INTERP_CLEAR | write-1-to-clear: b0 TX timeslot, b1 RX timeslot, b2 RF TX, b3 RF RX, b4 PCM write (mic buffer full), b5 PCM read (speaker buffer consumed) |

SYS_INTERP_LIST bits:

| Bit | Name | Meaning in DMR layer 2 |
|---|---|---|
| 0 | dll_rdy_interp_tx | layer-2 TX processing point (ahead of a TX slot) |
| 1 | dll_rdy_interp_rx | layer-2 RX processing point |
| 2 | dll_latelc_rx_interp | **voice late entry**: embedded LC assembled from a superframe |
| 3 | dll_frame_rx_interp | **a burst was deframed**: read RX_TYPE_INFO and the RX RAM |
| 4 | dll_shortlc_rx_interp | short LC (CACH) received |
| 5 / 6 | dll_tx/rx_slot_abnormal | layer-2 TX / RX timeslot boundary abnormal |
| 7 / 8 | gps_lost / gps_slot_abnormal | simulcast only |
| 9 / 10 | ccl_tx / ccl_rx | layer 3 |
| 11 | phy_rx_interp | PHY test reception |
| 12 | phy_rdy_interp | PHY slot early interrupt |
| 13 / 14 / 15 | fm_sig_rx / fm_rx / fm_tx | FM signaling detect (CTCSS/CDCSS/DTMF/selcall) / FM RX data / FM TX data |
| 16 | phy_recv_slot_abnormal | PHY slot boundary abnormal |
| 17 | rdy_next_short_lc | repeater CACH |

### 2.5 Layer-2 control

| Offset | Register | Fields |
|---|---|---|
| 0x400 | LAYER2_CONTROL | b7 txen, b6 rxen, b5 layer2slot, b4 txtmp_master_mode, b3 rx_master_auto_update, b2 tx_slave_auto_mode, b1 tx_master_mode (active TX timing), b0 rx_master_mode |
| 0x404 | LAYER2_SLOTON | write: b0 dll_tx_slot_on, b2 dll_rx_slot_on (establish TX / RX slot in active mode), plus self-clearing open/close strobes b5–b13 (b5 dll_rx_open_tx: build the TX slot from the receive slot, with tx_slave_auto_mode 0 and b0 set; datasheet 10.2.2.17); read: b31 TX slot on, b29 RX slot on |
| 0x408 | LAYER2_TXRX_CTRL | b31 tx_complete_cpu (continuous mode only), b23 syncfail_layer2_rx (drop and re-acquire RX slot sync), b22 syncfail_layer2_tx, b15 begin_v_layer2 (voice access success), b7 txnextsloten (transmit in the next slot), b6 rxnextsloten (receive in the next slot), b0 autotest |
| 0x40C | LAYER2_SLOT_CNT | active timing presets: b31 / b30 RX / TX slot number, b29 + b28:20 TX temporary slot counter, b19 + b18:10 RX slot counter, b9 + b8:0 TX slot counter (each enable bit write-clear). In the physical layer mode a TX counter preset (in bits or in symbols) left the slot timing where it was (bench 2026-10-21, 2.10) |
| 0x410 | LAYER2_MAX_CNT | slot length trim: b15 sign (1 shorter) and b11:8 RX, b7 sign and b3:0 TX, n × 100 us. No effect on the physical layer's slot timer (2.10 finding 13) |
| 0x414 | LAYER2_SLOT_UPDATE | slot re-sync strobes (unused) |
| 0x418 | LAYER2_SEND_TYPE | b31:22 codec type for custom data types 11–15; b12 RC in data sync; b11 data EMB RC source; b10 / b9 BS forwarding; b8 crc_calc_sel (0 = chip computes CRCs); **b7:4 data type / voice burst index; b3 1 = voice (or RC) burst, 0 = data/control burst; b2 second data header; b1:0 LCSS** |
| 0x41C | LAYER2_STATUS | b31 TX slot number, b30 RX slot number, b17:9 RX slot bit count, b8:0 TX slot bit count |
| 0x424 | LAYER2_SLOT_PRE | b23:16 0x55 (do not change), b15:8 RX processing-interrupt advance, b7:0 TX advance. Value **0x551010** |
| 0x428 | LAYER2_BS_MODE | b11:8 bs_idle_type (reset 9 = Idle), b5:4 gps_update_rate, b3 gps_pps_en, **b2 bs_tx_mode, b1 bs_rx_mode, b0 bs_single_freq** (0 dual frequency, 1 single frequency). Reset type H: **a chip reset does not clear it**. Section 2.9 |
| 0x430 | BS_SECOND_REG | b5:0 GPS second, simulcast only |
| 0x434 | BS_STATUS | read only: b29:28 CACH LCSS counter, b27:19 and b18:8 simulcast slot and frame counters, b5:0 locked second |
| 0x438 | BS_CACH | b1 bs_cach_at, b0 bs_cach_tc. Does not control the CACH on air as a static value (2.9) |
| 0x12C, 0x130 | SEND_DATA_SYNC_H / _L | TX data sync word, reset D5D7F7 7FD757 (MS data). Writing the BS data sync (DFF57D 75DF5D) makes the chip send BS-sourced data frames (2.9) |

Values written to LAYER2_CONTROL: **0xC0** = TX and RX enabled, passive timing (receive, follow the other side's
slots); **0xC3** = TX and RX enabled, active TX and RX timing (we set the slot clock); **0xCB** = 0xC3 plus b3
rx_master_auto_update, for `tx_listen`. The datasheet (10.2.2.16) says b0 rx_master_mode should normally be 0, and that
in active receive mode the receive slot edges move with the received sync only when b3 is set. With 0xC3 alone an SFR
heard almost nothing in its listening slot while it sent (measured, 2026-10-16: `L18/1`), so `tx_listen` sets b3 for
a transmission on our own timing.

An aligned start (`DMR_TIMING_ALIGNED` with a burst received, an SFR's repeat) writes no LAYER2_CONTROL value: it stays
0xC0 and LAYER2_SLOTON = **0x25** (0x05 plus b5 dll_rx_open_tx) builds the TX slot from the receive slot. Any
LAYER2_CONTROL write at the start left the receiver without slot sync for about 1 s, deaf to the call being repeated:
0xC3 (TX slots 37.7-41.6 ms after the caller's instead of 30), 0xC4 (b2, TX slots following the receiver: 28.6-30.1 ms),
even 0xC4 written over 0xC4. With b0 rx_master_mode set (0xC5, 0xCD) the receiver kept its sync but no TX slot started.
Without a CACH (an SFR's repeat, a direct-mode answer) the first slot after the start is the one the received bursts
came in; the driver sends in the slot whose number (LAYER2_STATUS b31) is the received bursts' chip slot for the same
TS, the other one for another TS. Measured on the SDR bench, 2026-10-16: our bursts 30.1 ms after the caller's, the
whole call heard and repeated.

Through a repeater (BS-sourced bursts; two boards on the SDR bench, issue 72, 6 of 6 runs on the right
slot and on the grid) the slot comes from the CACH TC alone (5.3): the TC lock, then a TC estimate that
toggles at every timeslot interrupt and gives the TC of the slot starting at that interrupt. The receiver runs in
LAYER2_CONTROL 0xC4 (TX slots follow the receiver) from every receive restart on a repeater channel, and that restart
releases **both** slot syncs (TXRX b23 and b22) and is also the end of every transmission. A start waits in receive for
the lock, then writes only TXRX (no LAYER2_CONTROL or SLOTON write at the key-up); the first TX tick sends in the next
slot when the estimate is the other TS's TC. A first TX-vector interrupt less than half a slot after the last receive
one is the same slot and does not toggle the estimate. Without the lock in 1 s the start is refused (TX_REJECTED,
nothing sent).

Bench findings behind this (two boards on the SDR bench, 2026-10-18):

| Tried | Result |
|---|---|
| Slot from the last burst's chip slot (RX_TYPE_INFO b19 against LAYER2_STATUS b31) | the other slot about half the time: b31 read 0 at every TX start, and the first burst after the receiver synced carried TC 0 whatever its slot, with b31 set |
| SLOTON 0x25 (TX slot built from the receive slot) at the start | on the repeater's slot grid only when strobed at one parity of receive interrupt, and then always TS2; a skipped first slot put the bursts 8-20 ms off the grid |
| 0xC4 written only at the receive restart, only b23 released | TX slots at the phase they had then (the wake-up burst's, sent on our own timing): 0-30 ms off the grid from run to run |
| 0xC4 rewritten at the key-up | the same phase problem on one radio, right on the other: the write is not what sets the phase |
| First TX-vector interrupt counted as a new slot | about 5 ms after the last receive one in some runs: the estimate one slot off |
| **b22 released with b23 at every receive restart, full reset after every TX, nothing written at the key-up** | **TS1 and on the grid, 6 of 6**: the first TX tick comes 29.9-30.0 ms after the last receive tick |

Timing rules the chip follows (what the bench showed; the data sheet doesn't say):

- **The TX slot phase is latched, not derived.** In 0xC4 the TX slots follow the receiver, but only from the moment the
  TX slot sync was last released and re-acquired. A TX sync left from an earlier transmission, including a burst sent
  on our own timing (the wake-up), keeps its phase and the next call goes out off the repeater's grid by whatever the
  difference was. Every receive restart must release b22 as well as b23.
- **Check the phase, not the slot.** The measure of "on the grid" is the first TX-vector interrupt 30 ms (one slot)
  after the last receive one (`txh u` in the diagnostics: 299-300 in the passing runs, 90-180 in the failing ones). It
  is cheaper to read than the air capture and gives the same verdict.
- **LAYER2_STATUS b31 is not a usable slot number** at the start of a transmission (always 0), and RX_TYPE_INFO b19 is
  one burst's chip slot, not a reference. Neither decides a TX slot on a repeater.
- **Writing LAYER2_CONTROL drops the receiver's slot sync for about 1 s**, in every value tried. Set it at a receive
  restart and leave it.
- **A TX-vector tick right after an RX-vector one is the same slot.** The two interrupts of one slot came about 5 ms
  apart in some runs; counting both as slots put the estimate one slot off.
- **A deframe near a tick belongs to the previous slot.** A burst is deframed late in its slot, around the next
  timeslot interrupt: in the first half of a slot count it to the one before.
- **A slot estimate needs a lock first.** The first bursts after the receiver syncs can carry a wrong TC; the driver waits
  for four alternating values.

SFR (two boards on the SDR bench, 2026-10-18, each run after a key-up on our own timing): the repeat's
TX slot comes 30.1 ms and 30.7 ms after the inbound one, with the TX slot sync released only
on repeater channels (6 runs) and on every channel (10 runs) alike: the SLOTON b5 start takes the receiver's phase
whatever the TX sync held, so the stale phase that broke the repeater path does not reach an SFR. Every receive
restart releases both syncs. Read at an SFR's start, LAYER2_STATUS b31 picked the right slot
in every run (`txh e1`: b31 set, the received chip slot next, ours after it). The first TX-vector interrupt comes two
slots after the last receive one (`txh u` 599-600), not one as through a repeater: the slot right after the SLOTON
strobe has no TX tick, so a tick count across an SFR's start is one short. The phase check is a whole number of
slots. A direct mode start can take the slot from the TDMA1 / TDMA2 sync type in RX_TYPE_INFO b3:2 plus
LAYER2_STATUS b29 against an offset captured at that moment, never from b31 or b19.

Values written to
LAYER2_TXRX_CTRL: 0x00 nothing next slot; 0x40 receive next slot; 0x80 transmit next slot; 0x8040 receive next slot
with the voice-access flag; 0x800000 release RX slot sync.

### 2.6 LAYER2_SEND_TYPE codes

| Code | Burst | Source of payload in TX RAM |
|---|---|---|
| 0x10 | Voice LC header (data type 1) | LC bytes 0–8 at 0x16000000; bytes 9–11 may be left 0 (chip adds RS(12,9) with RS_H_MASK) |
| 0x20 | Terminator with LC (data type 2) | LC bytes 0–8 |
| 0x30 | CSBK (data type 3) | 10 bytes; chip adds CRC with CSBK_CRC_MASK |
| 0x40 | MBC header (4) | 10 bytes, chip CRC |
| 0x50 | MBC continuation (5) | 12 bytes |
| 0x60 | Data header (6) | 10 bytes, chip CRC with DATAH_CRC_MASK |
| 0x70 | Rate ½ data block (7) | 12 bytes (CRC-9 / CRC-32 by the CPU) |
| 0x80 | Rate ¾ data block (8) | 18 bytes |
| 0xA0 | Rate 1 data block (10) | 24 bytes |
| 0x08 + (n << 4) + LCSS | Voice burst n = 0..5 (A..F) | AMBE 27 bytes to the voice RAM 0x16000960; for burst A also the call's LC (bytes 0–8) at 0x16000000 for the embedded LC |

Datasheet example for one superframe: A = 0x08, B = 0x19, C = 0x2B, D = 0x3B, E = 0x4A, F = 0x50, i.e. LCSS 0 on A,
1 (first fragment) on B, 3 (continuation) on C and D, 2 (last) on E, 0 on F. **The driver writes
LCSS = 0 on every voice burst** (0x08, 0x18, 0x28, 0x38, 0x48, 0x58). Whether the chip then still sends the
embedded LC fragments correctly is unverified; follow the datasheet.

### 2.7 RX_TYPE_INFO (0x390, read after a deframe interrupt)

| Bits | Name | Meaning |
|---|---|---|
| 30 | crc8_error_flag | short LC CRC-8 bad |
| 29 | golay_check_flag | slot type Golay bad |
| 28 | qr_check_flag | voice EMB QR bad |
| 27 | cs_check_flag | late-entry checksum bad |
| 26 | crc16_error_flag | CRC-16 bad (CSBK, headers) |
| 25 | rs24_error_flag | RS(12,9) bad (LC header, terminator) |
| 24 | data_block_error | CRC-9 / CRC-32 result for data blocks |
| 23 | cach_at | CACH AT bit |
| 22 | cach_tc | CACH TC bit: the timeslot number of the received burst |
| 21:20 | cach_lcss | CACH LCSS |
| 19 | current_rx_slot | slot number |
| 18 | save_soft_finish | soft information stored |
| 17 | pi | privacy indicator of the received voice burst |
| 16:15 | lcss | voice burst LCSS |
| 14:12 | current_voice_burst | voice burst number, **1–6 = A–F** (0 when not voice) |
| 11:8 | data_type | slot type data type |
| 7:4 | cc | received color code |
| 3:2 | recv_sync_type | 0 MS, 1 BS, 2 TDMA slot 1, 3 TDMA slot 2 |
| 1:0 | sync_type | 0 none (EMB-only burst), 1 voice, 2 data, 3 RC |

"CRC good" for a deframed burst = bits 30:24 all zero. Data blocks have no CRC-16, so do not drop them on these
flags; check CRC-9/CRC-32 in software. RX_INFO (0x394): b24:16 trunking multiframe number, b10:0 received RC.

### 2.8 Buffer layouts

**TX RAM (0x16000000)**, datasheet table 52: 0x00–0x0B the current control burst (PI header 10 + 2 CRC, LC header / terminator
9 + 3 RS, CSBK / MBC header / data header 10 + 2 CRC, MBC block 12, rate ½ 12, rate ¾ 18, rate 1 24); 0x00–0x09 also
embedded LC (72 bits + 5-bit checksum in byte 9, "prepared when voice burst A is about to be sent"); 0x29–0x2C burst F
EMB filler (or key ID / algorithm ID for privacy); 0x30–0x4A voice burst when voice comes from TX RAM (layer-2 BS
mode, AUDIO_CONTROL b0 = 1); 0x4B–0x50 embedded RC; 0x495–0x4AF send voice key (27 bytes).

**RX RAM (0x160004B0)**, datasheet table 53: the deframed control burst at 0x00 (LC header / terminator 9 + 3, CSBK / headers
10 + 2, data blocks 12 / 18 / 24, idle 12, C_RC 7 + 2). Embedded LC after late entry also appears at 0x00–0x08; the
chip keeps the previous call's LC there until a new one is decoded (measured).

**Voice RAM (0x16000960)**: write 27 bytes before arming a voice burst; read 27 bytes after a voice deframe interrupt
Burst numbering for privacy keystream purposes is RX_TYPE_INFO's 1–6.

### 2.9 BS (repeater) mode: bench results 2026-10-20

Datasheet source: the Chinese original (HR_C7000_Document_2, 10.2.2.17, 10.2.2.18, 10.2.2.22, 10.2.2.25-10.2.2.28).
The registers, their bits and what the chip does with them were tried on one board (RadioFoundry build with a scratch memory
poke command, SDR bench capture on 461.4625 MHz, lowest power, `sdrmon.py analyze --cach`). (measured)

Datasheet summary:

| Item | Content |
|---|---|
| BS_MODE b2 / b1 / b0 | BS transmit mode, BS receive mode, dual (0) or single (1) frequency repeater |
| LAYER2_SLOTON b3 / b4 (0x404) | bs_tx_slot_on / bs_rx_slot_on: establish the BS TX / RX slot. Status: b28 o_bs_tx_slot_on, b27 o_bs_rx_slot_on |
| LAYER2_TXRX_CTRL b4 (0x408) | bs_tx_sync_rx_en: BS TX slot follows the BS RX slot |
| LAYER2_SEND_TYPE b10 bs_tx_data_rdy (0x418) | 0: the chip frames and sends Idle (type from BS_MODE b11:8); 1: forwarded data is ready (self-clears at the slot edge) |
| LAYER2_SEND_TYPE b9 bs_tx_data_source | 0: the chip encodes; 1: send 196 information bits without coding |
| AUDIO tx_voice_source = 1 (4.6.5.3) | voice comes from MCU written RAM (the layer 2 BS TX path) |
| BS_CACH b1:0 | AT and TC "for BS transmit mode" |
| RDY_NEXT_SHORT_LC interrupt (bit 17) | the chip asks for the next CACH short LC |
| BS_INTER_IN / BS_INTER_OUT pins (P12 / R12) | baseband BS interrupt in and out, apparently to link two chips |
| BS_SECOND_REG, BS_STATUS, GPS bits | same-frequency simulcast from a GPS pulse (2.9a). Not needed here |

Findings:

| # | Finding | Evidence |
|---|---|---|
| 1 | The BS registers exist and read the datasheet's reset values: BS_MODE 0x00000900, the others 0 | read at idle |
| 2 | Writable bits match the datasheet: BS_MODE b11:8, b5:0; BS_CACH b1:0; BS_SECOND b5:0. Reserved bits read 0. BS_STATUS ignores writes. 0x42C is not implemented (reads 0, ignores writes) | write and read back |
| 3 | BS_MODE survives a chip reset (`stubtool --attach --boot reset`). A radio left in BS mode stays there until written back or hardware reset | read after reset |
| 4 | With BS_MODE b2 set, RadioFoundry's normal call start fails: the TX path never arms (`dmr.tx_error=stalled`, TX denied `rejected`). BS TX needs its own start sequence | `ptt` with b2 set |
| 5 | With b2 set, then LAYER2_SLOTON b3 set and b0 cleared during a call, the chip reports o_bs_tx_slot_on (SLOTON reads 0x1000000C) and BS_STATUS b29:28 becomes non-zero (one read: 0x10000000, so the CACH LCSS counter runs) | read back, 0x434 = 0x10000000 |
| 6 | The chip then sends its own Idle frames (data type 9 from bs_idle_type, color code from LOCAL_CC) with no MCU data, every 60 ms in **one** slot, with the same 30 ms bursts keyed and unkeyed (the RF keying is still RadioFoundry's). The first frame comes about 0.8 s after the switch | SDR: 54 IDLE bursts, 60.0 ms apart |
| 7 | The sync word on those frames comes from SEND_DATA_SYNC. At its reset value they carry MS data sync. Written with the BS data sync they decode as BS-sourced (`bs-d`) | SDR family column |
| 8 | **Each BS frame is preceded by a CACH with valid TACT parity** (the analyzer counts it `tact ok`), but AT, TC, LCSS and the short LC payload are not controlled by BS_CACH: with BS_CACH held at 0, 2 and 3, AT and TC showed all four combinations about equally often and the short LC fragments were noise. TC did not alternate | SDR: 54 CACHs per run, three runs |

Unresolved: where the chip takes the CACH bits from. The likely source is the same as on the HR_C6000, per slot values
the MCU supplies at the RDY_NEXT_SHORT_LC interrupt (BS_CACH written every slot, short LC in a TX RAM window), which a
static poke cannot reproduce. Not tried: the real start sequence (arming BS TX without RadioFoundry's call), `bs_tx_data_rdy` = 1
with MCU data in both slots, `tx_voice_source`, `bs_rx_mode` (receive BS, label slots), `bs_single_freq`, the BS_INTER
pins, and two chips. The test was a register switch in the middle of a RadioFoundry call, so the one-slot cadence in 6 may come
from RadioFoundry's own TX loop and not the chip.

### 2.9a gps_pps_en: the simulcast slot lock (datasheet only)

LAYER2_BS_MODE b3 `gps_pps_en` enables what the datasheet calls same-frequency simulcast (datasheet 10.2.2.25-10.2.2.27).
It is a BS mode feature: the chip disciplines its BS slot timing to a GPS second pulse, so several repeaters on one
frequency send each slot at the same instant. Datasheet only; nothing below has been tried.

| Item | Content |
|---|---|
| BS_MODE b5:4 `gps_update_rate` | how often the slot timing is corrected from the pulse: every 6 s, 3 s or 1 s |
| BS_SECOND_REG b5:0 | the GPS second (0-59), written by the MCU from the receiver's time messages |
| BS_STATUS | `gps_lcss_cnt` (the CACH LCSS phase), `gps_bit_cnt` (a 30 ms slot counter run from the pulse), `bs_slot_num` (the 60 ms frame number in the 60 s cycle), `bs_second_lock` |
| System interrupt b8 `gps_slot_abnormal`, b7 `gps_lost_interp` | the pulse's slot timing jumped; the pulse was lost |

Answers for the split duplex TX half (`split-duplex.md` 3.8, INFO capability bit 5):

1. It needs a pulse on a chip pin. The datasheet names none for it; the only candidate is BS_INTER_IN (P12, "BS sync
   interrupt input"). Where an HR_C7000 radio in `boards/` has a GPS module, it is wired to an MCU UART only, and
   no pulse line to the chip or to the MCU is documented. So no radio here can use it.
2. It locks the chip's BS mode slot timing, which the TX half doesn't use (it sends in the physical layer mode, 2.10).
   Whether it also moves the physical layer's slot timer is unknown.
3. The host-side grid (ALIGN with the host's clock on NTP or a GPS-disciplined source) gives the same slot numbering
   without a pulse, to the clock sync's error bound. A PPS input report stays a follow-up for a board that wires one.

### 2.10 Physical layer transmit: the split duplex TX half

The split duplex TX half (`split-duplex.md` 5.4) needs a CACH whose AT, TC and short LC it controls, which the BS mode
of 2.9 doesn't give. It uses the physical layer mode instead (datasheet 9.5), as the HR_C6000 does (`modem-hrc6000.md` 9.1):
the chip sends the CPU's bits without coding, and the core builds every slot (`bsOutboundRawSlot`: the CACH, then the
burst with BS sync, slot type and coded payload). Driver: `drivers/modem/hrc7000/hrc7000_bs.c`. Datasheet only until the bench
results below say otherwise.

| Item | Content (datasheet 9.2, 9.4.1, 9.5) |
|---|---|
| WORK_MODE | 0x22: physical layer, timeslot mode (b5 set, layermode 0, b1 aligned); 0x02: physical layer, continuous mode |
| RF_MODE | two-point TX (b6:5 = 3), as the 0x03269860 of 2.1 |
| LAYER2_CONTROL | b7 txen, b6 rxen, b1 tx_master_mode, b0 rx_master_mode. 0x82: transmit with our own slot timing |
| LAYER2_TXRX_CTRL | b7 txnextsloten: send in the coming slot; b6 rxnextsloten; b0 autotest. The datasheet's set-up writes 0x81 |
| TX RAM 0x00-0x23 / 0x24-0x47 | SEND_DATA0 / SEND_DATA1: one 36-byte (288-bit) frame each, sent alternately. Two bits per symbol, 00 +1, 01 +3, 10 -1, 11 -3 (the ETSI dibit mapping), first bit in b7 of the first byte |
| System interrupt b12 | phy_rdy_interp: the physical layer's slot interrupt |

The datasheet's example frame for a bit error test carries the MS data sync at bits 120-167. In a burst the sync starts at
bit 108, so that frame is 12 bits of guard, the 264-bit burst and 12 bits of guard: an MS slot, aligned to the slot
edge. A BS outbound has no guard: its slots follow each other without a gap, each CACH centered on the slot edge. The
driver writes the core's slots (CACH first) back to back, so the bit stream on air is the continuous outbound, and the
CACH half that falls before the chip's slot edge is only a constant 1.25 ms offset of the slot clock (the RX half's
phase tracking absorbs it, `split-duplex.md` 6). Whether the chip ramps the PA between two slots of the timeslot mode
(it would cut into the CACH) is what the continuous mode variant is for.

The driver, while the BS outbound runs (it takes the slot engine's place, as `hrc6000_bs.c` does):

1. Start: TXRX 0, SLOTON 0, WORK_MODE 0x80 then 0x22 (or 0x02), LAYER2_CONTROL 0x82, codec buffers cleared. A new
   slot clock.
2. Slot clock: a boundary at the first of the TX timeslot, RX timeslot or PHY_RDY interrupts in a slot (one more
   within 15 ms is the same slot). Unkeyed the clock runs and nothing is sent.
3. Keyed: at each boundary a slot's 36 bytes go to TX RAM and TXRX = 0x80. The chip sends them in the slot that
   starts about 5 ms after the boundary's interrupt, not in the one after it (finding 11), so the slot clock names that
   slot. The key-up starts on an outbound TS1 slot. Each slot plays its own queue, an empty one an idle burst.
4. The first RF TX interrupt keys the RF chip and starts the PA one-shot; the PA then stays up. Unkeyed: no more slots,
   the PA off one slot later. No boundary for 200 ms: PA off, unkeyed.
5. Stop (and leaving DMR): TXRX 0, PA off, WORK_MODE 0x80 then 0x6E, LAYER2_CONTROL passive, both slot syncs released,
   then the normal receive restart (5.2). LAYER2_BS_MODE is never written; the power-on set-up writes its reset value
   0x900 (2.9: it survives a chip reset).

Bench variants (`dmr_bs_config_t.variant`, the debug link's SPLIT op 1, `split-duplex.md` 10.1), a bit field: b0 the
continuous mode, b1 the autotest bit with the send bit (0x81), b3:2 the TX RAM halves (0 the slot to both, 1 alternate
starting with SEND_DATA0, 2 alternate starting with SEND_DATA1). The default 0 is timeslot mode, 0x80, both halves.

Bench results, 2026-10-20 (TX half keyed 8 s per variant at the lowest power on 461.4625 MHz; no SDR
capture: the RTL-SDR was in use elsewhere. A second radio on the same frequency was the receiver). (measured)

| # | Finding | Evidence |
|---|---|---|
| 1 | PHY_RDY (system interrupt b12) comes first in every slot and makes the slot clock; the TX and RX timeslot interrupts follow in the same slot (two per slot) | `bs` lines: 366 PHY_RDY, 734 timeslot interrupts, 1 boundary from a timeslot vector; longest gap 30.0 ms, no late slot |
| 2 | Variants 0, 1, 4 and 8 give one RF TX interrupt per key-up: the chip stays in TX from slot to slot. With the autotest bit (variant 2) it comes every 60 ms: the chip leaves TX between slots | `bs.rftx_ints` 1 against 141 in 8 s |
| 3 | Every slot is set up while keyed (about 280 in 8.4 s), no underrun, no stall | `bs.bursts`, STATUS `idle` |
| 4 | The outbound decodes: the second radio deframed one idle burst per 60 ms with BS sync and color code 2, no CRC errors, labeled TS1 from the CACH TC (variant 8: TS2) | its `I`, `cc2 BS ts1` lines |
| 5 | After the role ends (also by lease expiry) a normal call goes out whole | `dmr.tx_error=ok`; the second radio heard 3 headers, 42 voice bursts, the terminator |

Second board on the SDR bench, 2026-10-21 (lowest power, 461.4625 MHz, `tests/hil/test_split_duplex.py`). (measured)

| # | Finding | Evidence |
|---|---|---|
| 6 | Variant 0: the outbound is whole. BS data sync on both slots (189 / 188 idle bursts in 10 s), a burst every 30.0 ms (same slot 60.000 ms), one key-up edge, every CACH of valid TACT parity, TC alternating on every CACH, AT following SET_AT, the short LC decoding (93 ok, 0 bad), no underrun | `test_split_tx_half_off_air`: 377 BS bursts, tact ok 377, TC alternating 376/376 |
| 7 | Variant 1 (continuous mode) gives the same outbound: the timeslot mode keeps the carrier and every CACH between slots too | 358 BS bursts, tact ok 358, TC 357/357 |
| 8 | A host-fed voice call goes out whole in its slot | `test_split_tx_half_voice` |
| 9 | Keyed 5 minutes: carrier offset +58 Hz at key-up, +56 Hz at the end, no underrun, no stall (10 015 slots) | captures across the key-up and the unkey (`sdrmon.py analyze` needs carrier-free air in a capture to find its noise floor) |
| 10 | The RX half labels a bench call, residual 0-1 us after centering, and the label flips with the epoch | `test_split_rx_half_labels` |

Variant 0 stays the default. Writing the slot to both TX RAM halves works, so which half the chip sends in each slot
isn't needed. Not measured: PA temperature (no sensor on the diagnostics page), receiver desense with the RX half
beside it.

The first board gives the same results on the SDR bench (2026-10-21, variant 0): 352 BS bursts, 176 per slot, tact ok
352, TC alternating 351/351, AT following SET_AT, short LC 87 ok and 0 bad, no underrun; the voice call whole. Its RX half's labels pass too (residual 1 us).

The slot clock against the air, SDR bench, 2026-10-21 (TX half; each GET_EPOCH mapped to wall-clock time
through the host's time sync, compared with the SDR's timestamps of the outbound's bursts, whose slot comes from
their CACH TC). (measured)

| # | Finding | Evidence |
|---|---|---|
| 11 | The slot set up at a boundary is on air from about 5 ms after the boundary's interrupt. The first driver took it to go out one slot later and named the boundary after the slot before: the epoch was one slot early, and a subscriber's TS2 call through a pair went up labeled TS1 and was repeated on TS1 | before the fix TS2 bursts started 5.0 ms after the epoch's "TS1" and TS1 bursts 25.0 ms before it; after it TS1 bursts start 4.6 ms after the epoch (six epochs each). Another board's TX half measures +2.2 ms the same way |
| 12 | The slot timer restarts when own TX timing is enabled: LAYER2_CONTROL 0xC0 (passive), then 0x82 at a chosen time. The first boundary then comes about 1.2 ms before a whole slot after the write; with that learned, the boundaries land 9-120 us from the host's grid (ALIGN, `split-duplex.md` 3.8; the host's clock sync bound was 3 ms). Passive timing keeps counting while the driver waits, so it takes no boundary then. Redoing WORK_MODE through FM with LAYER2_CONTROL left at 0x82, or a LAYER2_SLOT_CNT TX counter preset, doesn't move it | `test_split_align`: 15 ALIGNs in five runs, all aligned; the learned restart delay -1167 us |

Slewing (`split-duplex.md` 3.8, capability bit 2), 2026-10-22, no RF. (measured)

| # | Finding | Evidence |
|---|---|---|
| 13 | LAYER2_MAX_CNT (2.5, datasheet 10.2.2.20: b7 sign and b3:0 n × 100 µs for TX slots, b15 and b11:8 for RX) does not move the physical layer's slot timer. Tried: the TX trim for one slot (written at a boundary, cleared at the next), the TX and RX trims together, and both held set. The boundaries stayed where they were: the HR_C7000 driver offers no `bs_slew`, so a keyed TX half reports its drift and re-aligns at the next unkey, as the HR_C6000 does | `test_split_slew` with a scratch driver: 35 trims of +100 µs asked in 5 s, the phase unchanged (-1535, -1696, -1464 µs in the three runs) |

---

## 3. Interrupts

| Vector (PIC source) | Name | Fires | What it means / what to do | Clear |
|---|---|---|---|---|
| 61 (29) | System_inter | after the chip deframes a burst (bit 3), on late entry (bit 2), slot abnormalities, FM signaling | read SYS_INTERP_LIST, act, write it back to SYS_INTERP_CLEAR until 0 | SYS_INTERP_CLEAR |
| 62 (30) | Time_slot_inter_tx | every 30 ms on the TX slot clock | **decide the next slot**: write TX RAM / voice RAM, SEND_TYPE, then TXRX_CTRL (0x80 = send in the coming slot, 0x00 / 0x40 otherwise). Start the PA one-shot whenever 0x80 is written | INTERP_CLEAR b0 and b1 |
| 63 (31) | Time_slot_inter_rx | every 30 ms on the RX slot clock | same decision while receiving (and for the last repeater-wake wait state) | INTERP_CLEAR b0 and b1 |
| 64 (32) | Rf_tx_inter | rf_pre_on_tx × 100 us (**3.1 ms** with RF_CONTROL 0x41F1A) before each TX burst | put the RF chip into TX (first burst: full TX set-up; later bursts: idle → TX); note the time | INTERP_CLEAR b2 (first thing) |
| 65 (33) | Rf_rx_inter | rf_pre_on_rx before an RX slot | not used; TX is turned off from the next timeslot interrupt once the burst has run ≥ 20 ms | INTERP_CLEAR b3 |
| 59 (27) | PCM_rd_data_interp | every 10 ms while AUDIO_CONTROL b5 is set | speaker buffer consumed: write the next 160 bytes of PCM to 0x180000A0 | INTERP_CLEAR b5 |
| 60 (28) | PCM_wr_data_interp | every 10 ms while AUDIO_CONTROL b4 is set | 160 bytes of mic PCM ready at 0x18000000 | INTERP_CLEAR b4 |

Both timeslot interrupts are handled by one routine; it decides by the current activity
(transmitting states on the TX vector, receiving states on the RX vector). DMR TX does not start unless vectors 62 and 64 are hooked.

### 3.1 TX timing within a burst (inferred from the register values)

| t (ms) relative to the TX timeslot interrupt in which the burst is armed | Event |
|---|---|
| 0 | TX timeslot interrupt: payload and SEND_TYPE written, TXRX_CTRL = 0x80, PA one-shot started |
| ≈ 26.9 | RF TX interrupt (3.1 ms ahead): RF chip idle → TX |
| 28 (default "DMR PA" setting, 1–35 ms) | PA one-shot: band PA select on, PA drive DAC to the calibrated value |
| ≈ 29.6 | tx_pre_on (0.4 ms) |
| 30 | burst on air (27.5 ms of symbols plus guard) |
| next timeslot interrupt with ≥ 20 ms of TX elapsed | PA drive 0, PA selects off, RF chip back to RX |

### 3.2 Masking the modem interrupts

PIC_MASK bits 29, 30, 31 and PIC_MASK_1 bit 0 (vectors 61–64) (datasheet mapping). Mask them around multi-register updates
from task level.

---

## 4. Initialization (power-on, once)

Ordered; (register, value, why). The whole DMR block is written once here and not at every mode change.

| # | Register | Value | Why |
|---|---|---|---|
| 1 | ADC controller | reset and idle (board-specific) | the baseband shares the analog front end |
| 2 | IO_DIPLEX2 (0x03C) | clear bit 28 | DAC channel B pin to analog (PA drive) |
| 3 | DAC_PD_CTRL / PD_MODE_EN / DATA_A / B / C | 0x01 / 0x01 / 0 / 0 / 0x06E2 | DAC B (PA drive) and C on, A off; PA drive 0 |
| 4 | FM_BANDWIDTH (0x500) | 0x5F | FM wideband defaults |
| 5 | FM_DEV_COEF (0x504) | 0x33500 | FM deviation defaults (overwritten per TX/RX) |
| 6 | SYS_SOFT_RSTN (0x000) | clear bit 4, wait 2 us | codec reset pulse |
| 7 | codec AICR_DAC, AICR_ADC, CR_MIC1, CR_MIC2, CR_DAC, CR_ADC, CR_VIC, CR_CK, GCR_DACL, GCR_MIC1, GCR_ADCL, CR_DAC_AGC, CR_ADC_AGC, DR_ADC_AGC | 0xC0, 0xC0, 0x00, 0x70, 0x20, 0x20, 0x0C, 0x40, 0x80, 0x00, 0x8B, 0, 0, 0x34 | audio codec on, mic input 2, gains (board-specific) |
| 8 | LINEOUT_CTRL (0x088) | 0x01 | speaker line out on |
| 9 | SIG_CENTER | (cal << 24) or (cal << 16) | TX I/Q center |
| 10 | RF_MOD_BIAS_CTRL | ((cal + 384) << 16) or 0x4D4D | modulation balance until a TX frequency is known |
| 11 | RF_MODE | 0x03269860 | two-point TX, AF RX, b11 set (2.1a) |
| 12 | DEV_LIMITER | 0x0000FF00 | |
| 13 | SYS_SOFT_RSTN | 0x190 | see the board's reset-bit notes |
| 14 | DAC_CONTROL (0x070) | 0x8000001B | baseband DAC set-up: b31 offset code; b5 / b4 power down DAC A / B; b2 / b1 low power DAC A / B (1 = off or low power per the datasheet). So DAC A on at full power, DAC B powered down and low power |
| 15 | ADC_CONTROL (0x074) | |= 0x41C3 | baseband ADC set-up |
| 16 | AUDIO_CONTROL (0x080) | 0x02 | I2S slave (reset default), all audio paths off |
| 17 | AUDIO_BUFFER_CLR (0x084) | 0x03 | clear vocoder encode and decode buffers |
| 18 | sync words (2.2) | as listed | DMR syncs |
| 19 | THRESHOLD_VALUE | 0x0978786F | sync detection |
| 20 | PI_CRC_MASK, RS_H_MASK, RS_H_INIT, RS_T_MASK, RS_T_INIT, CSBK_CRC_MASK, MBCH_CRC_MASK, DATAH_CRC_MASK | 0x6969, 0x969696, 0, 0x999999, 0, 0xA5A5, 0xAAAA, 0xCCCC | ETSI masks; the chip then computes those CRCs/RS itself |
| 21 | 0x370, 0x374, 0x378, 0x37C | 0xF0, 0x1FF, 0x10F, 0 | undocumented |
| 22 | CRC8_INIT, VOICE_EMB_CTRL, SCRAMBLE_REG | 0, 0, 0 | |
| 23 | SYS_INTERP_MASK | 0x1187F | enable layer-2 TX/RX processing, late entry, deframe, short LC, slot abnormal, PHY, PHY slot abnormal |
| 24 | LAYER2_SLOT_PRE | 0x551010 | processing-interrupt advance |
| 25 | 0x168 | 0x14 | undocumented |
| 26 | RF_RECV_DELAY | 0x36B0 | 1.4 ms receive path delay |
| 27 | RX_IF_FREQ, TX_IF_FREQ | 0x0BB800 each | |
| 28 | RECV_MS_SYNC_H, RECV_BS_SYNC_H | set bit 31 | undocumented enable |
| 29 | RF_CONTROL | 0x00041F1A | RF TX interrupt 3.1 ms early |
| 30 | WORK_MODE | 0x80 | start in FM (idle) |
| 31 | FM_TONE1..4 | 0x05930629, 0x06D10787, 0x09AC0AB0, 0x0BD10D10 | DTMF tone steps (section 8) |
| 32 | GCR_ADCL | mic gain or 0x80 | DMR mic gain setting |

---

## 5. Entering DMR and starting receive

### 5.1 Enter DMR (from FM or idle; also at every DMR TX start)

| # | Register | Value | Why |
|---|---|---|---|
| 1 | RF_MOD_BIAS_CTRL | ((cal + 384) << 16) or modcal(f_TX) | balance for this TX frequency |
| 2 | GCR_ADCL | DMR mic gain or 0x80 | |
| 3 | DAC_PD_CTRL | clear bit 1 | **PA drive DAC back on** (FM receive powers it down; without this, DMR after FM has no PA drive) |
| 4 | WORK_MODE | 0x80 | pass through FM: resets the DMR state machine |
| 5 | WORK_MODE | 0x6E | DMR, Tier II, timeslot, layer 2 |

AUDIO_CONTROL is not rewritten here; after FM it ends up 0x00 once FM audio is switched off.

### 5.2 Start / restart receive

Used at DMR entry, after TX, and wherever reception must be (re)acquired:

| # | Register | Value | Why |
|---|---|---|---|
| 1 | (enter DMR, 5.1) | | |
| 2 | VOICE_EMB_CTRL | 0x02 | burst F EMB from the chip's encoder |
| 3 | LAYER2_CONTROL | 0xC0 | TX/RX enabled, passive timing |
| 4 | LAYER2_SLOTON | 0x00 | drop slot establishment |
| 5 | LAYER2_TXRX_CTRL | 0x800000 | release RX slot sync |
| 6 | LAYER2_TXRX_CTRL | 0x40 | receive in the next slot |
| 7 | LAYER2_SLOTON | 0x05 | dll_rx_slot_on and dll_tx_slot_on |

A sequence of TXRX 0xC00000, 0, 0xC00000, 0x8040 with no SLOTON writes, or a busy wait in the color code probe, does not acquire sync reliably. Use this one everywhere.

### 5.3 While receiving

| Event | Action |
|---|---|
| System interrupt, bit 3 (deframe) while idle, good CRC, wanted CC | start following: TXRX_CTRL = 0x40 for the next slot (0x8040 here does not work) |
| Each TX/RX timeslot interrupt while following one slot | alternate: our slot → TXRX = 0x8040 (receive, voice-access flag) for the next; other slot → 0x00 |
| Voice deframe | read RX_TYPE_INFO, 27 bytes from the voice RAM; burst number from bits 14:12; LC header / terminator / CSBK / data from the RX RAM |
| BS sync received (recv_sync_type 1) | the other side is a repeater: track the TC bit (RX_TYPE_INFO b22) to find which slot is ours. Locked after 4 bursts in a row, one per slot, with TC alternating; held while a burst came in the last 390 ms. RX_TYPE_INFO b19 and LAYER2_STATUS b30/b31 are not usable for this |
| Color-code scan | set LOCAL_CC to the received CC, then LAYER2_CONTROL = 0xC0 and, in repeater mode, the 5.2 restart steps 4–7 (no busy wait) |
| Late entry (bit 2) | embedded LC is in the RX RAM; voice reception should start at the next burst A |
| End | terminator (data type 2) or loss of bursts; TXRX_CTRL = 0 |
| Reception over (nothing taken for 390 ms) | the 5.2 restart: the chip drops the sender's slot timing and its timeslot interrupts stop |
| Slot timing with nothing usable for 600 ms | the 5.2 restart, again every 600 ms while it lasts. Without it the chip kept a departed sender's timing, reported only unusable and abnormal slots and took no later call (measured) |

---

## 6. Transmit sequences

All transmit actions happen in the timeslot interrupts: the CPU arms one slot at a time with TXRX_CTRL = 0x80 and
leaves the other slot unused (0x00, or 0x40 / 0x8040 to listen: `tx_listen`, an SFR's inbound slot), giving one burst per 60 ms in its timeslot (datasheet figures 99–100). Every arming also starts the PA one-shot (a board with a PA one-shot timer). The RF TX interrupt that follows keys the RF
chip.

### 6.1 Start of transmission

| # | When | Register writes |
|---|---|---|
| 1 | task level, PTT | load the LC (9 bytes, then 3 zero bytes) into TX RAM 0x16000000; codec reset; LAYER2_CONTROL = **0xC3** (active timing, we own the slot clock); TXRX_CTRL = 0 |
| 2 | next timeslot interrupt | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (LC header) |
| 3 | next | TXRX_CTRL = 0 |
| 4 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (second header) |
| 5 | next | TXRX_CTRL = 0 |
| 6 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (third header) |

TXRX_CTRL bit 31 (tx_complete_cpu) belongs to continuous mode and must not be set at step 2. For a direct-mode start after receiving on a repeater channel the same LAYER2_CONTROL = 0xC3 applies.

### 6.2 Voice

Alternating slots: in the inactive slot TXRX_CTRL = 0; in the active slot:

1. For burst A (n = 0): rewrite the call LC (or talker alias LC) into TX RAM bytes 0–8 for the embedded LC.
2. Write 27 bytes of AMBE (three 9-byte frames) to the voice RAM 0x16000960, or the AMBE silence pattern when no
   encoded audio is ready.
3. TXRX_CTRL = 0x80, SEND_TYPE = 0x08 + (n << 4) (+ LCSS, see 2.6); n = n + 1 modulo 6.

Encoded audio must be produced continuously during TX: encode one 20 ms block per tick whenever two PCM buffers are waiting.

### 6.3 Terminator and back to receive

| # | Slot | Writes |
|---|---|---|
| 1 | active | voice RAM = silence; TX RAM = LC; TXRX_CTRL = 0x80; SEND_TYPE = 0x20 |
| 2 | next | direct mode: done → idle. Repeater mode: LAYER2_CONTROL = 0xC0, TXRX_CTRL = 0 |
| 3 | next (repeater mode) | TXRX_CTRL = **0x40** (receive next slot) → receiving |

### 6.4 Data and CSBK

Per burst, in an active slot: write the payload to TX RAM (10 bytes for CSBK / MBC header / data header: the chip
appends the CRC-16 with the programmed mask because SEND_TYPE b8 = 0; 12 / 18 / 24 bytes for rate ½ / ¾ / 1 blocks,
CRC-9 and CRC-32 computed by the CPU), SEND_TYPE = data type << 4, TXRX_CTRL = 0x80, start the PA one-shot. Inactive
slot: TXRX_CTRL = 0 (or 0x40 to listen for a response). Sequence for a packet: data header (0x60), blocks
(0x70/0x80/0xA0), optional terminator. CSBK signaling: optional preamble CSBKs then the function CSBK, all 0x30
(datasheet appendices B, C). Open RX afterwards to catch the response.

### 6.5 BS activation (repeater wake)

No layer-3 BS activation: LAYER2_BS_MODE, BS_* registers and WORK_MODE values 0x73/0x76/0x77 are not used. What they do when written is in 2.9 (measured).

| # | When | Writes |
|---|---|---|
| 1 | PTT on a repeater channel with no repeater heard | codec reset; LAYER2_CONTROL = 0xC3 |
| 2 | timeslot interrupt | TX RAM bytes 0–11: 0xB8 (last block, CSBKO 0x38 BS_Dwn_Act), FID 0, bytes 2–6 = 0 (BS address 0), bytes 7–9 = own ID big endian, bytes 10–11 = 0; SEND_TYPE = 0x30; TXRX_CTRL = 0x80; **start the PA one-shot**; red LED |
| 3 | next timeslot interrupt | **SEND_TYPE = 0x30, TXRX_CTRL = 0x80 again**: the same CSBK in the immediately following slot; wake response timeout 600 ms |
| 4 | next | go to receive (5.2), count TC toggles |
| 5 | while waiting | when the received TC has toggled correctly 4 times → repeater awake, start 6.1; otherwise retry from step 2 after 600 ms, give up after a few tries |

One CSBK without keying the PA gives only an unmodulated carrier. The PA one-shot and the second consecutive CSBK
make repeater wake work (which of the two matters is not known).

---

## 7. CRC and FEC the chip does

| Burst | Chip adds | CPU supplies |
|---|---|---|
| LC header, terminator | RS(12,9) with RS_H_MASK / RS_T_MASK, BPTC(196,96), slot type Golay | 9 LC bytes |
| CSBK, MBC header, data header, PI header | CRC-16 with the programmed mask (when SEND_TYPE b8 = 0), BPTC, slot type | 10 bytes |
| Rate ½ / ¾ / 1 data | BPTC / trellis ¾ / none, slot type | 12 / 18 / 24 bytes including CRC-9 and the final CRC-32 |
| Voice | sync / EMB (QR), embedded LC fragments (5-bit checksum from TX RAM byte 9 or computed), burst F EMB from the RC encoder when VOICE_EMB_CTRL b1 = 1 | 27 AMBE bytes; LC for burst A |
| Short LC (CACH) | CRC-8 (CRC8_INIT) | repeater mode only |

On receive the chip checks the same codes and reports them in RX_TYPE_INFO b30:24; it decodes FEC and leaves only
information bits in the RX RAM.

---

## 8. FM mode

### 8.1 Registers

| Offset | Register | Fields | Values used |
|---|---|---|---|
| 0x500 | FM_BANDWIDTH | b6 band-pass on; b4 pre-emphasis; b3 TX 25 kHz; b2 adjacent-channel filter 25 kHz; b1 RX 25 kHz; b0 CTCSS RX 25 kHz | TX: 0x5F (25 kHz), 0x50 (12.5 kHz). RX: 0x4F (25 kHz), 0x40 (12.5 kHz) |
| 0x504 | FM_DEV_COEF | b23:16 TX limiter (`tr_sig_lim`, reset 0x0F), b15:8 RX deviation coefficient, b7:0 TX deviation coefficient | 0x40A20 (25 kHz), 0x40A25 (12.5 kHz); the limiter is the board's (`hrc7000_t.fm_tx_limit`, 0 = 4) |
| 0x508 | FM_SQL | b15:0 noise measure: higher = less signal | squelch threshold in software |
| 0x50C | SUBVOICE_COEF | b27:16 detector sample depth (default 0x7D0), b15:8 sub-audio / DTMF deviation, b7:0 detection threshold | DTMF deviation (8.3) |
| 0x510 | FM_MOD | b23:16 repeat-symbol threshold; b15:14 XTCSS length; b13:12 CTCSS reverse-burst mode (01 = 240°, 10 = 180°); b11 CDCSS TX invert; b10 CDCSS RX invert; b9 CDCSS turn-off code; b8 XTCSS EOM; b4 selcall; **b3 CTCSS; b2 CDCSS; b1 DTMF**; b0 XTCSS | |
| 0x518 | CTCSS_ADDR | b15:8 RX tone address, b7:0 TX tone address (table 8.2) | |
| 0x51C | USER_DEFINE_CTC | b31 enable, b20:0 step = round(f × 65.536) | non-standard tones |
| 0x520 | CDCSS_ADDR | b24:16 RX code, b8:0 TX code (the 9-bit DCS code as binary, e.g. 114 octal = 001 001 100). The TX code is taken when FM_PTT goes on; later writes don't change the code on air (measured: written after FM_PTT, the radio sent the previous code) | |
| 0x524 | SELCALL_TONE | 2/5-tone frequency | |
| 0x528 | TONE_SEND_CTRL | b9 selcall send; b8 selcall finish; **b7 dtmf_send; b6 dtmf_finish (0 mutes voice during signaling); b3:0 DTMF digit** | |
| 0x550–0x55C | FM_TONE1..4 | two 16-bit DTMF tone steps each | 8.3 |
| 0x560 | FM_PTT | b0 FM transmit | 1 in FM TX, 0 in FM RX |
| 0x564 / 0x568 | FM_DECODE_FLAG1 / 2 | FLAG2: b11:8 DTMF digit received; **b1 CDCSS detected; b0 CTCSS detected**; XTCSS flags | sub-audio squelch |
| 0x530–0x54C | FM_FUNCTION_COEF1..8 | detector coefficients, 2·cos(2πf / fs)·2^22 (fs 32 kHz for DTMF, 8 kHz for custom CTCSS) | |
| 0x080 | AUDIO_CONTROL | b31 prompt FIFO full; b7 prompt path; **b6 FM audio to speaker**; b5 CPU-fed speaker PCM; b4 mic PCM to CPU; b3 sidetone; b2 I2S select; b1 I2S slave (reset 1); b0 voice source (0 codec, 1 TX RAM), FM transmit included (8.4) | |
| 0x56C | FM_ADDR_SW | 1 = low half, 2 = high half of the TX / RX RAM audio buffer in use (8.4) | |

### 8.2 FM entry sequences

| Mode | Writes |
|---|---|
| FM receive | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; FM_PTT = 0; AUDIO_CONTROL = 0x40; GCR_DACL = 0x80 (0 dB); DAC_PD_CTRL bit 1 set (PA drive DAC off); WORK_MODE = 0x80 |
| FM transmit | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; DAC_PD_CTRL bit 1 cleared; WORK_MODE = 0x80; FM_PTT = 1. Board: mic switch to analog, RF chip to TX, PA select, DAC B drive |
| FM audio on / off in DMR context | AUDIO_CONTROL b6 set and WORK_MODE = 0x80 / b6 cleared and WORK_MODE = 0x6E |
| Mute mic / line out | codec CR_ADC b7 / CR_DAC b7 = 1 |

Standard CTCSS addresses (datasheet table 59; RX uses the same addresses in CTCSS_ADDR b15:8):

| Addr | Hz | Addr | Hz | Addr | Hz | Addr | Hz |
|---|---|---|---|---|---|---|---|
| 0x01 | 67.0 | 0x0E | 107.2 | 0x1B | 167.9 | 0x28 | 62.5 |
| 0x02 | 71.9 | 0x0F | 110.9 | 0x1C | 173.8 | 0x29 | 159.8 |
| 0x03 | 74.4 | 0x10 | 114.8 | 0x1D | 179.9 | 0x2A | 165.5 |
| 0x04 | 77.0 | 0x11 | 118.8 | 0x1E | 186.2 | 0x2B | 171.3 |
| 0x05 | 79.7 | 0x12 | 123.0 | 0x1F | 192.8 | 0x2C | 177.3 |
| 0x06 | 82.5 | 0x13 | 127.3 | 0x20 | 203.5 | 0x2D | 183.5 |
| 0x07 | 85.4 | 0x14 | 131.8 | 0x21 | 210.7 | 0x2E | 189.9 |
| 0x08 | 88.5 | 0x15 | 136.5 | 0x22 | 218.1 | 0x2F | 196.6 |
| 0x09 | 91.5 | 0x16 | 141.3 | 0x23 | 225.7 | 0x30 | 199.5 |
| 0x0A | 94.8 | 0x17 | 146.2 | 0x24 | 233.6 | 0x31 | 206.5 |
| 0x0B | 97.4 | 0x18 | 151.4 | 0x25 | 241.8 | 0x32 | 229.1 |
| 0x0C | 100.0 | 0x19 | 156.7 | 0x26 | 250.3 | 0x33 | 254.1 |
| 0x0D | 103.5 | 0x1A | 162.2 | 0x27 | 69.3 | | |

The address order is not ascending; map from frequency, not from a list index. Other tones: USER_DEFINE_CTC. TX
sub-audio deviation via SUBVOICE_COEF b15:8 (typical 350 Hz narrow, 600 Hz wide). The FM_BANDWIDTH bits don't scale
it: the reset value 0x28 gives 300 Hz CTCSS at both bandwidths, and RadioFoundry writes 0x50 at 25 kHz (600 Hz) (measured, UHF). DCS gets a third of the deviation from the same value (0x28: 100 Hz), and reads the field as signed:
0xF0 sent the code inverted at about 40 Hz. RadioFoundry writes three times the CTCSS value for DCS, capped at 0x7F: 0x78 at
12.5 kHz (305 Hz) and 0x7F at 25 kHz (323 Hz) (measured, UHF). The boot stub's restart doesn't reset the modem, so the field
keeps the last transmission's value; RadioFoundry's init writes 0x28 rather than reading it back (one board read 0xFF there
after a 25 kHz transmission and sent CTCSS at 1.9 kHz deviation, measured). When FM_PTT drops, the chip sends
the reverse burst / turn-off code before stopping. FM_MOD b9 (reset 1) enables the CDCSS turn-off: a 134.4 Hz
tone. RadioFoundry sets b9 with DCS and, for the core's tail (`radio-features.md` 3.3a), drops FM_PTT while the RF stays
keyed; FM_PTT on again resumes the code. On the SDR bench: 134.4 Hz for 0.20 s, then the carrier without
modulation until the core unkeys (measured). Unkeying together with FM_PTT, as before, put none of it on air. On a detected tone the FM signaling system interrupt fires
(SYS_INTERP_LIST b13) and FM_DECODE_FLAG2 b0 (CTCSS) or b1 (CDCSS) goes 1, back to 0 at the end. CDCSS tail
detection uses FM_FUNCTION_COEF1 = 0x7F499D (134.4 Hz).

### 8.3 DTMF and single tones

| Item | Value |
|---|---|
| Tone step | round(f × 2.048) (= f × 65536 / 32000) |
| Default tone registers | FM_TONE1 = 697 / 770 Hz (0x0593 / 0x0629), FM_TONE2 = 852 / 941 (0x06D1 / 0x0787), FM_TONE3 = 1209 / 1336 (0x09AC / 0x0AB0), FM_TONE4 = 1477 / 1633 (0x0BD1 / 0x0D10); high half = first tone |
| DTMF deviation (SUBVOICE_COEF b15:8) | Measured (UHF, 1750 Hz single tone): about 5.8 Hz per step, 0x60 = 559 Hz, 0xC0 = 1117 Hz; a value of 0x12 gives about 100 Hz. RadioFoundry uses 0xC0 at 12.5 kHz and 0xFF (the most, about 1.5 kHz) at 25 kHz, on both bands. VHF is unmeasured (a VHF value about 3.25 times the UHF one would match the ratio of the VHF to the UHF modulation calibration bytes, unverified; RadioFoundry applies those bytes per band and keeps the UHF values) |
| Send sequence | FM_MOD b1 = 1; FM_PTT = 1; TONE_SEND_CTRL b6 = 0 (mute voice); AUDIO_CONTROL b3 = 1 for sidetone; TONE_SEND_CTRL b3:0 = digit; b7 = 1 to start the digit, b7 = 0 to end it; repeat per digit; finally b6 = 1 to restore voice |
| Arbitrary single tone | write the tone step into the high halves of FM_TONE1 and FM_TONE3 and send DTMF digit "1" (697 + 1209 pair), restore the defaults after. On air at 1750 Hz (measured): TONE_SEND_CTRL = 0x81 (digit 1 on, voice muted) with FM_MOD b1 set, 0 afterwards (the reset value) |
| RadioFoundry's DTMF (`hrc7000_tx_dtmf`, `radio-features.md` 3.7) | the arbitrary-tone sequence with the low tone's step in FM_TONE1's high half and the high tone's in FM_TONE3's, digit "1" sent: one sequence for every digit and for single tones, without the digit codes of figure 95. The deviation is the single tone's (SUBVOICE_COEF b15:8, `hrc7000_dtmf_dev`); the chip attenuates the low tone 2.5 dB; on air the high tone is 5.0 dB over the low one for every digit on two boards (measured), so the datasheet's figure is likely 10 log of the amplitude ratio. That forward twist is the chip's design and isn't corrected: after a receiver's de-emphasis it is +0.4 to +2.1 dB (`radio-features.md` 3.7) |
| Receive | FM_MOD b1 = 1; SUBVOICE_COEF b27:16 = 1056 (33 ms at 32 kHz), b7:0 threshold; FM_FUNCTION_COEF1..8 = 0x7ECD9D, 0x7E8A34, 0x7E368C, 0x7DD245, 0x7C690D, 0x7B9F03, 0x7AA7A8, 0x7979F5; read FM_DECODE_FLAG2 b11:8 on each signaling interrupt; FM_MOD b23:16 sets the repeat-symbol window |

### 8.4 Prompt / beep FIFO

Set AUDIO_CONTROL b7, then while AUDIO_CONTROL b31 is 0 write 64 bytes (32 × 16-bit samples, low byte first, 8 kHz)
to 0x16000980..0x160009BF; clear b7 when done (measured). High byte first (16 samples per push) sounds distorted. Flushing stale audio: fill with 0x7F bytes.

RadioFoundry plays all speaker PCM through this FIFO (`platform/hrc7000/audio_pcm.c`). The CPU-fed speaker buffer at
0x180000A0 (AUDIO_CONTROL b5, section 3) gave rough, choppy beeps on one board with every sample layout tried,
with no queue underruns (measured).

The prompt path also feeds the TX modulator: with AUDIO_CONTROL b7 set during a DMR transmission, the beep's audio
rides on the 4FSK. On one board the talk permit tone overlapped the second and third voice LC headers, which then
carried no decodable sync on the SDR bench and the bench modem took the call by late entry; with b7 held clear while
transmitting, all three headers decoded in five runs (measured). Keep b7 clear during any transmission; a beep that
starts then plays silently, and b7 comes back on at TX end if the beep is still running.

APRS transmit uses the same path on purpose (`platform/hrc7000/hrc_afsk.c`): during an FM transmission, with b7 set
and the microphone muted by the board, the FIFO's samples are the modulation. A timer tops the FIFO up every 2 ms;
speaker PCM is held off the FIFO meanwhile (`hrc_audio_pcm_hold`), since its chunks would land between the tones.
Bell 202 decoded off air from this path with a valid FCS (measured, `aprs.md` 13). The DTMF tone
generator (8.3) needs no PCM, but its deviation field tops out near 1.5 kHz, short of the 3 kHz APRS wants.

The datasheet doesn't document this route. It names two FM transmit audio sources, selected by AUDIO_CONTROL b0 (datasheet 7.2.2.1):
the codec ADC (0) or the TX RAM (1). As a source, the TX RAM is 0x16000030-0x1600042F, two 512-byte halves of 256
samples (32 ms at 8 kHz), and FM_ADDR_SW (0x56C) names the half in use (1 low, 2 high); SYS_INTERP b15 is the FM TX
interrupt. The prompt FIFO is documented only as playback to the codec DAC (datasheet 7.3). LINEOUT_CTRL (0x088) b1 is line
out 1 and b0 line out 2, the same PWM waveform on both (RadioFoundry sets 0x01). Two-point modulation: SIG_CENTER b31:24
biases MOD1 (the VC-TCXO) and b23:16 MOD2 (the VCO), 3.3 mV a step; RF_MOD_BIAS b7:0 and b15:8 are the MOD1 and MOD2
amplitudes; RF_MODE b10 sends a 40 Hz test square. The prompt FIFO path decodes on some boards but not on others,
which take the mixer route below.

The FM transmit limiter (FM_DEV_COEF b23:16) caps the deviation of audio that enters through the codec ADC, the
microphone and the mixer route below. The prompt FIFO's ring play is not capped by it: AFSK through
ring play reached 5.1 kHz peak with the limiter at 4. Through the ADC path the ceiling scales with limiter x TX
coefficient (measured, 25 kHz, Bell 202 tones):

| Limiter | TX coefficient | Peak deviation |
|---|---|---|
| 4 | 0x20 | 1.65 kHz (clipped at every level tried) |
| 4 | 0x30 | 2.48 kHz (clipped) |
| 8 | 0x20 | 3.18 kHz (clipped) |
| 0x0F | 0x40 | 4.18 kHz |
| 0x0F | 0x20 | 4.57 kHz, max 4.84 kHz (not clipped) |

So a limiter of 4 holds speech to about 1.65 kHz at 25 kHz. A board sets `fm_tx_limit` for each bandwidth.

Where ring play doesn't reach the modulator on its own, the codec's digital mixer (datasheet 6.5.2.10-11) carries the DAC's
audio into the ADC's record path, which is the FM transmit input. CR_MIX (codec 0x10: b7 mix_en, b6 load, b5:0 the
MIX_n address) and DR_MIX (0x11) reach MIX_0..3. MIX_2 b0 (mix_rec) makes the ADC's output ADC + DAC, and MIX_3 b7:4
= 0xF zeroes the ADC's own input in that sum. `hrc7000_codec_dac_to_tx` sets MIX_3 and then MIX_2, clears them in the
reverse order and ends with CR_MIX = 0. The codec mic mute (CR_ADC b7) must stay off on this route: it mutes the mix
too. Through this route Bell 202 decoded off air with a valid FCS (measured, a board where ring play alone sent a bare
carrier).

### 8.5 FM receive audio to the CPU

In FM (WORK_MODE 0x80) the modem copies its demodulated receive audio into the RX RAM as well as to the codec (the
datasheet's "analog receive recording", on by default in FM):

| Item | Value |
|---|---|
| Buffer | RX RAM 0x160004E0..0x160008DF: two halves of 512 bytes (256 samples), used in turn |
| Sample format | 16-bit, high byte at the lower address |
| Rate | 8 kHz: the halves alternate every 32.0 ms (measured over 95 changes) |
| Half to read | FM_ADDR_SW (0x56C) b1:0: 1 = the first half, 2 = the second |
| Interrupt | SYS_INTERP bit 14 (`fm_rx_interp`) as each half fills; its mask bit (SYS_INTERP_MASK b14) is off in the power-on mask |
| Input | the modem's AF input. On a board with a separate FM RF chip that is its audio output, routed by a board GPIO |

The audio is not squelched: it carries noise while no signal is present, so a demodulator can work below the squelch.
Receiver noise reaches full scale (peak 32767) with the reset AF gain (measured).

RadioFoundry reads it in the system interrupt (`platform/hrc7000/fm_rx_audio.c`): the DMR glue owns that interrupt and calls
an FM hook for bit 14, in FM too, and the hook copies each half into a 128 ms ring for the APRS demodulator.

---

## 9. Driver requirements

What a working DMR driver does that a first attempt tends to miss. Details are in the sections named.

| # | Area | Requirement |
|---|---|---|
| 1 | Vectors | hook the TX timeslot (62) and RF TX (64) vectors as well as 61 and 63; TX states run on 62, 64 keys the RF chip (3) |
| 2 | Register set-up | write the DMR block once at init, not at every DMR entry (4) |
| 3 | RF_MODE | 0x03269860 always, b11 set (2.1a) |
| 4 | RF_CONTROL | 0x00041F1A, RF TX interrupt 3.1 ms early; 0x00040A0A gives bad timeslots at low power |
| 5 | DMR entry | calibration, mic gain, DAC_PD_CTRL bit 1 cleared, WORK_MODE 0x80 then 0x6E (5.1) |
| 6 | RX restart | SLOTON 0, TXRX 0x800000, 0x40, SLOTON 5, VOICE_EMB_CTRL 2 (5.2) |
| 7 | RX start and after TX | TXRX 0x40; 0x8040 only in the steady receive alternation |
| 8 | CC probe | no busy wait |
| 9 | TX start | LAYER2_CONTROL 0xC3, TXRX bit 31 not set (6.1) |
| 10 | PA keying | one-shot timer from each arming, "DMR PA" delay default 28 ms |
| 11 | RF chip | idle to TX at TX start and at every RF TX interrupt |
| 12 | Mic switch | set the board's mic switch to analog at each TX start so DMR audio reaches the codec |
| 13 | Repeater wake | CSBK in two consecutive slots, PA keyed, extra wait state (6.5) |
| 14 | TX audio | encode in the tick when 2 or more PCM buffers wait (6.2) |
| 15 | Timeslot handler | run it from RAM so flash busy time adds no jitter |
| 16 | Calibration | SIG_CENTER and RF_MOD_BIAS_CTRL from factory data |
| 17 | Interrupt masking | PIC_MASK bits 29-31 and PIC_MASK_1 bit 0 (3.2) |

---

## 10. Open questions

1. LCSS bits on voice bursts B–E (2.6): the datasheet sets them, the driver does not. Does late entry work on other
   radios receiving this modem's transmissions?
2. Whether the 0x370–0x37C, 0x168 and bit-31 sync-enable writes matter.
3. RF_RECV_DELAY 0x36B0 vs the datasheet's 2000 for AF mode.
4. The real timing of the TX timeslot interrupt relative to the slot boundary (section 3.1 is inferred).
5. Data and CSBK transmission on the C7000 have only been checked against the datasheet and shared host tests, not on air.
6. FM sub-audio: the C7000's own CTCSS/CDCSS engine (section 8) is not used on a board with a separate FM RF chip; the driver routes
   TX CTCSS to that chip instead.
7. RF_MODE b11 (2.1a) on simplex receive: does a radio with the bit set hear a call on the second timeslot that it misses
   with the bit clear? No on-air test has compared the two. A test: a call on one timeslot while the radio receives the
   other, with b11 on and then off. Also: RadioFoundry sets b11 on repeater channels. Does that change repeater
   reception?
8. BS mode (2.9): where the chip's CACH AT, TC and short LC come from (likely BS_CACH and a short LC written at every
   RDY_NEXT_SHORT_LC). The split duplex TX half doesn't need it: it uses the physical layer (2.10).
9. A slot length trim for the physical layer mode (2.10 finding 13): LAYER2_MAX_CNT has none. Whether the trim works
   in the layer 2 timing (LAYER2_CONTROL b5 layer2slot), and any other register that does, is untested.
