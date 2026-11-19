<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# HR_C7000 as a DMR modem

This page covers driving the baseband inside the HR_C7000 SoC as a DMR Tier II layer 2 modem and as an FM modem:
registers, buffers, interrupts, what to write when, and what the chip does by itself. The modem-neutral contract is in
[dmr-modem.md](dmr-modem.md); the HR_C6000 equivalent is in [modem-hrc6000.md](modem-hrc6000.md). For which radios
carry this chip, see the [feature matrix](../feature-matrix.md).

The sources are the HR_C7000 chip user guide v2.7 ("the datasheet" below) and measurements on a radio or with an SDR
("measured"). A value without a mark is the one the driver writes.

| Section | Topic |
|---|---|
| 1 | System view: memory map, buffers |
| 2 | Register map: mode, sync words, CRC masks, interrupts, layer 2, BS mode, physical layer transmit |
| 3 | Interrupts and TX timing |
| 4, 5 | Initialization, entering DMR, receive |
| 6, 7 | Transmit sequences, CRC and FEC |
| 8 | FM mode: registers, tones, DTMF, prompt FIFO, receive audio |
| 9 | Driver requirements |
| 10 | Air interface provider and receive tap (`CONFIG_AIR`, derived from `CONFIG_P25`) |

---

## 1. The modem's view of the system

| Item | Value |
|---|---|
| Register block | 0x11000000, 32-bit registers (names below are the datasheet's) |
| TX RAM (layer 2 send RAM) | 0x16000000, 1200 bytes, byte wide |
| RX RAM (layer 2 receive RAM) | 0x160004B0, 1200 bytes |
| AMBE voice RAM | 0x16000960–0x1600097A, 27 bytes: one burst's three 72-bit AMBE+2 frames, both TX and RX |
| Prompt (beep) FIFO | 0x16000980 |
| Codec registers | 0x160009C0 (byte wide) |
| PCM buffers (system SRAM) | mic PCM 0x18000000–0x1800009F (hardware writes), speaker PCM 0x180000A0–0x1800013F (CPU writes) |
| Vocoder | none in hardware: the CPU runs the AMBE+2 codec on the PCM buffers and moves 27-byte bursts through the voice RAM ([codec.md](../dev/codec.md)) |
| Symbol and sample clocks | 4800 baud 4FSK; 30 ms timeslots; internal 38.4 kHz sampling (AK2401 note) |

The modem runs DMR at layer 2. The CPU builds every burst's information bits (LC, CSBK, data blocks, AMBE). The chip
adds the FEC (BPTC, Golay slot type, RS(12,9), QR EMB, CRC with masks), the sync, the CACH handling and the timeslot
timing. The chip also has a layer 3 (its own call control, SMS, BS activation); the driver doesn't use it.

---

## 2. Register map used for DMR

Offsets are from 0x11000000. Values are the ones the driver writes unless noted.

### 2.1 Mode and RF interface

| Offset | Register | Fields | Value used | Why |
|---|---|---|---|---|
| 0x100 | WORK_MODE | b8 digital_analog_en; b7 modulator_mode (0 DMR, 1 FM); b6 Tier II; b5 timeslot mode; b4:3 layer (0 PHY, 1 L2, 2 L3); b2 is_repeater; b1 is_aligned; b0 work_slot_sel | **0x80** FM, **0x6E** DMR (Tier II, timeslot, layer 2, repeater bit, aligned, slot 1) | The driver writes only these two values in normal operation. It picks the timeslot in software with LAYER2_TXRX_CTRL, not with work_slot_sel |
| 0x104 | RF_MODE | b25 phase_avg_en; b24 af_mode; b23:16 AF gain (n/16); b15 RX IQ invert; b14:13 RX mode; b12 dynamic sync threshold; b11 use_dual_phy (second timeslot receive channel, 2.1a); b9 soft ramp; b7 TX IQ invert / deviation mapping; b6:5 TX mode (3 = two-point) | **0x03269860**: AF receive on with averaging, gain 0x26 (2.375), RX IQ inverted, dynamic sync threshold, b11 set (2.1a), two-point modulation | Written at init and left in both modes. b12 is cleared while LOCAL_CC has cc_opt = 1 (no CC filter). Don't switch to 0x03269060 (b11 clear) in DMR |
| 0x108 | SIG_CENTER | b31:24 TX I bias, b23:16 TX Q bias, b15:8 / b7:0 RX I/Q bias | (cal << 24) or (cal << 16) | TX center from factory calibration (board-specific) |
| 0x10C | TX_IF_FREQ | b23:0 | 0x0BB800 (reset value) | |
| 0x110 | RF_CONTROL | b20:16 tx_pre_on (100 µs steps); b13:8 rf_pre_on_tx (RF TX interrupt advance, 100 µs); b5:0 rf_pre_on_rx | **0x00041F1A**: tx_pre_on 0.4 ms, RF TX interrupt **3.1 ms** before the TX burst, RF RX interrupt 2.6 ms before RX | 0x00040A0A (1.0 ms) gives bad timeslots at low power. b13:8 is per board (`hrc7000_t.rf_tx_lead`): a board whose 3.1 ms lead clips the inbound burst before its own uses a shorter lead, 0x14 (2.0 ms), 0x06 (0.6 ms) or 0x04 (0.4 ms), each from a sweep on that board. b20:16 is per board too (`hrc7000_t.tx_pre_on`, default 4): it advances the TX data to make up for the board's RF TX path delay, about 0.107 ms earlier on air per step. A board whose inbound burst runs late against the outbound sets a larger value from a measurement. The RF TX interrupt's lead moves only the carrier, not the burst |
| 0x114 | RF_MOD_BIAS_CTRL | b25:16 two_point_bias; b15:8 sig_reduce (Mod2 amplitude); b7:0 phase_reduce (Mod1 amplitude) | ((cal + 384) << 16) or modcal (0x4D4D before a frequency is set) | Modulation balance from calibration |
| 0x118 | DEV_LIMITER | b15:0 | 0xFF00 (reset value) | |
| 0x11C | RSSI_BIAS_VALUE | read-only per the datasheet: rssi b31:16; b15:8 / b7:0 the measured DC bias of the ADC's I / Q inputs | 0x1381 is written (no effect expected) | |
| 0x128 | PHASE_OUT | read-only: b29:16 / b13:0 the largest and smallest 14-bit AF phase in AF receive mode | not written | A live check that AF reaches the ADC. A board receiving DMR reads about +1216 / -1149 (the datasheet expects about ±1300 at -47 dBm). A value that stays within a few counts means no AF arrives: a board audio-select GPIO left in the wrong state, or an RF chip whose demodulator output is off |
| 0x120 | THRESHOLD_VALUE | b28:24 sync ratio; b23:16 sync low threshold; b15:8 sync high threshold; b7:0 signal detect | **0x0978786F** | |
| 0x164 | RECV_INFO_COUNT | b31:24 sync errors; b8 select FSK error/EVM; b7:0 FSK error | read for BER/EVM only | |
| 0x168 | (undocumented, "REG_X06") | | 0x14 | |
| 0x1B0 | RX_IF_FREQ | b23:0 | 0x0BB800 | |
| 0x1B4 | RF_RECV_DELAY | b18:0, 100 ns steps | **0x36B0** (1.4 ms) | The datasheet gives 2000 (0.2 ms) for AF mode; 0x36B0 is the value that works |

**AF receive input (datasheet, document 2).** In AF mode the RF chip's demodulated audio goes to the AF ADC's P input,
and a DAC output biases its N input. On some boards that is a board DAC output. Document 1 says SIG_CENTER sets the AF
bias instead. A DC step on either input moves the PHASE_OUT center, which the DMR receiver reads as a frequency
offset. AF mode has no frequency offset compensation: RX_IF_FREQ applies to IF mode only.

**Reference oscillator control (MOD1, datasheet).** In receive, RF_MOD_BIAS_CTRL b25:16 sets the MOD1 level over
0-3.3 V, as long as DAC_CONTROL keeps that DAC powered. In TX, SIG_CENTER b31:24 sets it, ±422 mV around 1.65 V in
3.3 mV steps; RF_MODE b9 sets where the ramp starts (0 = 1.65 V). With cal + 384 in b25:16 both states give the same
voltage within one step (measured).

### 2.1a RF_MODE bit 11: the DCDM receive switch

The datasheet names bit 11 `use_dual_phy`. It turns on a second receive channel "for repeater chip receive mode and
DCDM mode" (datasheet document 2, register table). DCDM is dual-timeslot direct mode: two conversations on the two DMR
timeslots of one frequency, with no repeater to supply sync. The datasheet's DCDM set-up (18.3.5.1) has three steps:

1. Turn off MS and BS sync receive.
2. Enable TDMA1 and TDMA2 sync detection.
3. Set RF_MODE[11].

Receive stays enabled all the time.

| Item | Fact |
|---|---|
| Channel mode | The datasheet's DCDM set-up is for simplex channels (RX frequency equal to TX frequency). The datasheet also lists "repeater chip receive mode" as a user of bit 11 |
| RadioFoundry | Writes 0x03269860 (bit 11 set) and enables TDMA1 and TDMA2 sync detection on every channel (`hrc7000.c`). So it runs the DCDM receive set-up on repeater channels too |
| Second channel | A second modem receive path. The RF chip is unchanged. Dual watch with a second RF chip is a separate feature |

### 2.2 Sync words

The registers hold 24-bit halves of the 48-bit ETSI sync patterns, high half first. The driver writes them once at
init.

| Offset | Register | Value | ETSI pattern |
|---|---|---|---|
| 0x12C / 0x130 | SEND_DATA_SYNC_H / L | 0xD5D7F7 / 0x7FD757 | MS sourced data |
| 0x134 / 0x138 | SEND_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |
| 0x13C / 0x140 | RECV_MS_SYNC_H / L | 0xD5D7F7 / 0x7FD757, then set bit 31 of H | MS data (receive) |
| 0x144 / 0x148 | RECV_BS_SYNC_H / L | 0xDFF57D / 0x75DF5D, then set bit 31 of H | BS data (receive) |
| 0x14C / 0x150 | RECV_TDMA1_SYNC_H / L | 0xF7FDD5 / 0xDDFD55 | TDMA direct slot 1 data |
| 0x154 / 0x158 | RECV_TDMA2_SYNC_H / L | 0xD7557F / 0x5FF7F5 | TDMA direct slot 2 data |
| 0x15C / 0x160 | RECV_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |

The voice syncs aren't programmed. The chip derives them; they are the bitwise complements of the data syncs.

Bit 31 of each RECV_* high half is an undocumented enable. RECV_MS and RECV_BS need it, and so do RECV_TDMA1 and
RECV_TDMA2: without it the chip never detects the direct mode syncs, and an SFR's TS1 inbound goes unheard.

SEND_DATA_SYNC is the only send pattern; a voice burst goes out with its complement. Before a burst that asks for
another sync family (`dmr_tx_burst_t.sync_override`: an SFR's TS2 direct mode outbound), RadioFoundry rewrites
SEND_DATA_SYNC, and it writes MS back for the next burst without one. The burst then carries that family on air.

### 2.3 CRC, RS and color code

| Offset | Register | Value | Meaning |
|---|---|---|---|
| 0x350 | PI_CRC_MASK | 0x6969 | PI header CRC-16 mask (init value 0 in b31:16) |
| 0x354 / 0x358 | RS_H_MASK / RS_H_INIT | 0x969696 / 0 | RS(12,9) mask for the voice LC header |
| 0x35C / 0x360 | RS_T_MASK / RS_T_INIT | 0x999999 / 0 | RS(12,9) mask for terminators. The datasheet's reset value 0x969696 is wrong for ETSI; write 0x999999 |
| 0x364 | CSBK_CRC_MASK | 0xA5A5 | CSBK CRC-16 mask |
| 0x368 | MBCH_CRC_MASK | 0xAAAA | MBC header CRC-16 mask. The datasheet's reset 0xA5A5 is wrong for ETSI |
| 0x36C | DATAH_CRC_MASK | 0xCCCC | data header CRC-16 mask. The datasheet's reset 0xA5A5 is wrong for ETSI |
| 0x370–0x37C | (undocumented) | 0xF0, 0x1FF, 0x10F, 0x00 | |
| 0x380 | CRC8_INIT | 0 | short LC CRC-8 init |
| 0x384 | LOCAL_CC | color code in b3:0; b9:8 cc_opt: 0 = deframe only on matching CC, 1 = deframe any CC; b16 encryption type | cc_opt 0 and the filter's code with a CC filter; cc_opt 1 without one (RF_MODE b12 must then be 0). The channel's TX code after the slot lock; the receive code restored after TX |
| 0x388 | VOICE_EMB_CTRL | 0 at init, **2** when entering DMR receive | b1 = burst F EMB from the chip's own RC encoder; b0 = PI bit for bursts B–E |
| 0x38C | SCRAMBLE_REG | 0 | built-in simple voice scrambler keys (RX in b31:16, TX in b15:0); unused |

### 2.4 Interrupt control

| Offset | Register | Value or use |
|---|---|---|
| 0x398 | SYS_INTERP_LIST | system interrupt causes (read) |
| 0x39C | SYS_INTERP_MASK | **1 = enabled** (inverted sense). **0x1187F**: bits 0–6, 11, 12, 16 |
| 0x3A0 | SYS_INTERP_CLEAR | write the bits read from the list; repeat until the list reads 0 |
| 0x3A4 / 0x3A8 / 0x3AC | LAYER3_INTERP_LIST / MASK / CLEAR | layer 3 causes; unused |
| 0x3B0 | INTERP_CLEAR | write 1 to clear: b0 TX timeslot, b1 RX timeslot, b2 RF TX, b3 RF RX, b4 PCM write (mic buffer full), b5 PCM read (speaker buffer consumed) |

SYS_INTERP_LIST has these bits:

| Bit | Name | Meaning in DMR layer 2 |
|---|---|---|
| 0 | dll_rdy_interp_tx | layer 2 TX processing point (ahead of a TX slot) |
| 1 | dll_rdy_interp_rx | layer 2 RX processing point |
| 2 | dll_latelc_rx_interp | **voice late entry**: embedded LC assembled from a superframe |
| 3 | dll_frame_rx_interp | **a burst was deframed**: read RX_TYPE_INFO and the RX RAM |
| 4 | dll_shortlc_rx_interp | short LC (CACH) received |
| 5 / 6 | dll_tx/rx_slot_abnormal | layer 2 TX / RX timeslot boundary abnormal |
| 7 / 8 | gps_lost / gps_slot_abnormal | simulcast only |
| 9 / 10 | ccl_tx / ccl_rx | layer 3 |
| 11 | phy_rx_interp | PHY test reception |
| 12 | phy_rdy_interp | PHY slot early interrupt |
| 13 / 14 / 15 | fm_sig_rx / fm_rx / fm_tx | FM signaling detect (CTCSS/CDCSS/DTMF/selcall) / FM RX data / FM TX data |
| 16 | phy_recv_slot_abnormal | PHY slot boundary abnormal |
| 17 | rdy_next_short_lc | repeater CACH |

### 2.5 Layer 2 control

| Offset | Register | Fields |
|---|---|---|
| 0x400 | LAYER2_CONTROL | b7 txen, b6 rxen, b5 layer2slot, b4 txtmp_master_mode, b3 rx_master_auto_update, b2 tx_slave_auto_mode, b1 tx_master_mode (active TX timing), b0 rx_master_mode |
| 0x404 | LAYER2_SLOTON | write: b0 dll_tx_slot_on, b2 dll_rx_slot_on (establish the TX / RX slot in active mode), plus self-clearing open and close strobes b5–b13 (b5 dll_rx_open_tx: build the TX slot from the receive slot, with tx_slave_auto_mode 0 and b0 set; datasheet 10.2.2.17); read: b31 TX slot on, b29 RX slot on |
| 0x408 | LAYER2_TXRX_CTRL | b31 tx_complete_cpu (continuous mode only), b23 syncfail_layer2_rx (drop and re-acquire RX slot sync), b22 syncfail_layer2_tx, b15 begin_v_layer2 (voice access success), b7 txnextsloten (transmit in the next slot), b6 rxnextsloten (receive in the next slot), b0 autotest |
| 0x40C | LAYER2_SLOT_CNT | active timing presets: b31 / b30 RX / TX slot number, b29 + b28:20 TX temporary slot counter, b19 + b18:10 RX slot counter, b9 + b8:0 TX slot counter (each enable bit write-clear). In the physical layer mode a TX counter preset (in bits or in symbols) leaves the slot timing where it is (2.10) |
| 0x410 | LAYER2_MAX_CNT | slot length trim: b15 sign (1 shorter) and b11:8 RX, b7 sign and b3:0 TX, n × 100 µs. No effect on the physical layer's slot timer (2.10 finding 13) |
| 0x414 | LAYER2_SLOT_UPDATE | slot re-sync strobes (unused) |
| 0x418 | LAYER2_SEND_TYPE | b31:22 codec type for custom data types 11–15; b12 RC in data sync; b11 data EMB RC source; b10 / b9 BS forwarding; b8 crc_calc_sel (0 = chip computes CRCs); **b7:4 data type / voice burst index; b3 1 = voice (or RC) burst, 0 = data/control burst; b2 second data header; b1:0 LCSS** |
| 0x41C | LAYER2_STATUS | b31 TX slot number, b30 RX slot number, b17:9 RX slot bit count, b8:0 TX slot bit count |
| 0x424 | LAYER2_SLOT_PRE | b23:16 0x55 (don't change), b15:8 RX processing-interrupt advance, b7:0 TX advance. Value **0x551010** |
| 0x428 | LAYER2_BS_MODE | b11:8 bs_idle_type (reset 9 = Idle), b5:4 gps_update_rate, b3 gps_pps_en, **b2 bs_tx_mode, b1 bs_rx_mode, b0 bs_single_freq** (0 dual frequency, 1 single frequency). Reset type H: **a chip reset doesn't clear it**. Section 2.9 |
| 0x430 | BS_SECOND_REG | b5:0 GPS second, simulcast only |
| 0x434 | BS_STATUS | read only: b29:28 CACH LCSS counter, b27:19 and b18:8 simulcast slot and frame counters, b5:0 locked second |
| 0x438 | BS_CACH | b1 bs_cach_at, b0 bs_cach_tc. A static value doesn't control the CACH on air (2.9) |
| 0x12C, 0x130 | SEND_DATA_SYNC_H / _L | TX data sync word, reset D5D7F7 7FD757 (MS data). With the BS data sync (DFF57D 75DF5D) written, the chip sends BS-sourced data frames (2.9) |

The driver writes these values to LAYER2_CONTROL:

| Value | Meaning |
|---|---|
| 0xC0 | TX and RX enabled, passive timing: receive and follow the other side's slots |
| 0xC3 | TX and RX enabled, active TX and RX timing: the radio sets the slot clock |
| 0xCB | 0xC3 plus b3 rx_master_auto_update, for `tx_listen` |
| 0xC4 | TX slots follow the receiver: the receiver on a repeater channel |

The datasheet (10.2.2.16) says b0 rx_master_mode is normally 0, and that in active receive mode the receive slot edges
move with the received sync only when b3 is set. With 0xC3 alone an SFR hears almost nothing in its listening slot
while it sends, so `tx_listen` sets b3 for a transmission on the radio's own timing.

**Aligned start (direct).** An aligned start (`DMR_TIMING_ALIGNED` with a burst received, an SFR's repeat) writes no
LAYER2_CONTROL value. LAYER2_CONTROL stays 0xC0, and LAYER2_SLOTON = **0x25** (0x05 plus b5 dll_rx_open_tx) builds the
TX slot from the receive slot. Any LAYER2_CONTROL write at the start leaves the receiver without slot sync for about
1 s, deaf to the call being repeated:

| Write at the start | Result |
|---|---|
| 0xC3 | TX slots 37.7-41.6 ms after the caller's instead of 30 ms |
| 0xC4 (b2) | TX slots follow the receiver: 28.6-30.1 ms, but the receiver loses sync. 0xC4 written over 0xC4 does the same |
| 0xC5, 0xCD (b0 rx_master_mode set) | The receiver keeps its sync, but no TX slot starts |

Without a CACH (an SFR's repeat, a direct mode answer) the first slot after the start is the one the received bursts
came in. The driver sends in the slot whose number (LAYER2_STATUS b31) is the received bursts' chip slot for the same
TS, and in the other one for another TS. The bursts go out 30.1 ms after the caller's.

**Through a repeater.** For BS-sourced bursts the slot comes from the CACH TC alone (5.3): the TC lock, then a TC
estimate that toggles at every timeslot interrupt and gives the TC of the slot starting at that interrupt. The driver
works like this:

1. The receiver runs in LAYER2_CONTROL 0xC4 (TX slots follow the receiver) from every receive restart on a repeater
   channel.
2. That restart releases **both** slot syncs (TXRX b23 and b22). It is also the end of every transmission.
3. A start waits in receive for the lock, then writes only TXRX: no LAYER2_CONTROL or SLOTON write at the key-up.
4. The first TX tick sends in the next slot when the estimate is the other TS's TC.
5. A first TX-vector interrupt less than half a slot after the last receive one is the same slot and doesn't toggle the
   estimate.
6. Without the lock in 1 s the start is refused (`TX_REJECTED`, nothing sent).

The same lock numbers the slot ticks on a BS signal. Once it holds, each `DMR_EV_SLOT_TICK` carries the TS that the
estimate gives for the slot starting there. It also carries the AT bit (RX_TYPE_INFO b23) of the last CACH that named
that slot, when that CACH was read in the last four ticks (`DMR_TICK_AT_KNOWN`, `DMR_TICK_AT_BUSY`). Before the lock,
while transmitting and without a CACH, `ts` is 0 and AT is unknown. On a BS signal on both slots nearly every tick
after the lock carries AT.

These are the approaches that don't put the TX slot on a repeater's grid:

| Approach | Result |
|---|---|
| Slot from the last burst's chip slot (RX_TYPE_INFO b19 against LAYER2_STATUS b31) | The other slot about half the time: b31 reads 0 at every TX start, and the first burst after the receiver syncs carries TC 0 whatever its slot, with b31 set |
| SLOTON 0x25 (TX slot built from the receive slot) at the start | On the repeater's grid only when strobed at one parity of receive interrupt, and then always TS2. A skipped first slot puts the bursts 8-20 ms off the grid |
| 0xC4 written only at the receive restart, only b23 released | TX slots at the phase they had then (the wake-up burst's, sent on the radio's own timing): 0-30 ms off the grid |
| 0xC4 rewritten at the key-up | The same phase problem on some boards: the write doesn't set the phase |
| First TX-vector interrupt counted as a new slot | It can come about 5 ms after the last receive one, which puts the estimate one slot off |

With b22 released together with b23 at every receive restart, a full reset after every TX and nothing written at the
key-up, the first TX tick comes 29.9-30.0 ms after the last receive tick: TS1 and on the grid.

The chip follows these timing rules, which the datasheet doesn't state:

- **The TX slot phase is latched, not derived.** In 0xC4 the TX slots follow the receiver, but only from the moment the
  TX slot sync was last released and re-acquired. A TX sync left from an earlier transmission, including a burst sent
  on the radio's own timing (the wake-up), keeps its phase. The next call then goes out off the repeater's grid by the
  difference. Every receive restart must release b22 as well as b23.
- **Check the phase, not the slot.** "On the grid" means the first TX-vector interrupt comes 30 ms (one slot) after the
  last receive one (`txh u` in the diagnostics: 299-300 on the grid, 90-180 off it). It is cheaper to read than an air
  capture and gives the same answer.
- **LAYER2_STATUS b31 is not a usable slot number** at the start of a transmission through a repeater (always 0), and
  RX_TYPE_INFO b19 is one burst's chip slot, not a reference. Neither decides a TX slot on a repeater.
- **Writing LAYER2_CONTROL drops the receiver's slot sync for about 1 s**, in every value tried. Set it at a receive
  restart and leave it.
- **A TX-vector tick right after an RX-vector one is the same slot.** The two interrupts of one slot can come about
  5 ms apart; counting both as slots puts the estimate one slot off.
- **A deframe near a tick belongs to the previous slot.** The chip deframes a burst late in its slot, around the next
  timeslot interrupt. Count a deframe in the first half of a slot to the slot before.
- **A slot estimate needs a lock first.** The first bursts after the receiver syncs can carry a wrong TC. The driver
  waits for four alternating values.

**SFR.** The repeat's TX slot comes 30.1-30.7 ms after the inbound one, whether the receive restart releases the TX
slot sync only on repeater channels or on every channel. The SLOTON b5 start takes the receiver's phase whatever the TX
sync held, so the stale phase that breaks the repeater path doesn't reach an SFR. Every receive restart releases both
syncs.

- Read at an SFR's start, LAYER2_STATUS b31 picks the right slot (`txh e1`: b31 set, the received chip slot next, the
  radio's own after it).
- The first TX-vector interrupt comes two slots after the last receive one (`txh u` 599-600), not one as through a
  repeater. The slot right after the SLOTON strobe has no TX tick, so a tick count across an SFR's start is one short.
  The phase check is a whole number of slots.
- A direct mode start can take the slot from the TDMA1 / TDMA2 sync type in RX_TYPE_INFO b3:2 plus LAYER2_STATUS b29
  against an offset captured at that moment, never from b31 or b19.

The driver writes these values to LAYER2_TXRX_CTRL:

| Value | Meaning |
|---|---|
| 0x00 | nothing in the next slot |
| 0x40 | receive in the next slot |
| 0x80 | transmit in the next slot |
| 0x8040 | receive in the next slot with the voice-access flag |
| 0x800000 | release the RX slot sync |

### 2.6 LAYER2_SEND_TYPE codes

| Code | Burst | Source of payload in TX RAM |
|---|---|---|
| 0x10 | Voice LC header (data type 1) | LC bytes 0–8 at 0x16000000; bytes 9–11 can be left 0 (the chip adds RS(12,9) with RS_H_MASK) |
| 0x20 | Terminator with LC (data type 2) | LC bytes 0–8 |
| 0x30 | CSBK (data type 3) | 10 bytes; the chip adds the CRC with CSBK_CRC_MASK |
| 0x40 | MBC header (4) | 10 bytes, chip CRC |
| 0x50 | MBC continuation (5) | 12 bytes |
| 0x60 | Data header (6) | 10 bytes, chip CRC with DATAH_CRC_MASK |
| 0x70 | Rate ½ data block (7) | 12 bytes (CRC-9 / CRC-32 by the CPU) |
| 0x80 | Rate ¾ data block (8) | 18 bytes |
| 0xA0 | Rate 1 data block (10) | 24 bytes |
| 0x08 + (n << 4) + LCSS | Voice burst n = 0..5 (A..F) | AMBE 27 bytes to the voice RAM 0x16000960; for burst A also the call's LC (bytes 0–8) at 0x16000000 for the embedded LC |

The datasheet's example for one superframe is A = 0x08, B = 0x19, C = 0x2B, D = 0x3B, E = 0x4A, F = 0x50: LCSS 0 on
A, 1 (first fragment) on B, 3 (continuation) on C and D, 2 (last) on E, 0 on F. **The driver writes LCSS = 0 on every
voice burst** (0x08, 0x18, 0x28, 0x38, 0x48, 0x58).

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

A deframed burst has a good CRC when bits 30:24 are all zero. Data blocks and MBC continuation blocks have no CRC-16,
so don't drop them on these flags; check CRC-9 and CRC-32 (the MBC's CRC-16 in its last block) in software. RX_INFO
(0x394) holds the trunking multiframe number in b24:16 and the received RC in b10:0.

### 2.8 Buffer layouts

**TX RAM (0x16000000)**, datasheet table 52:

| Offset | Content |
|---|---|
| 0x00–0x0B | The current control burst: PI header 10 + 2 CRC, LC header / terminator 9 + 3 RS, CSBK / MBC header / data header 10 + 2 CRC, MBC block 12, rate ½ 12, rate ¾ 18, rate 1 24 |
| 0x00–0x09 | The embedded LC: 72 bits plus a 5-bit checksum in byte 9, "prepared when voice burst A is about to be sent" |
| 0x29–0x2C | Burst F EMB filler (or the key ID / algorithm ID for privacy) |
| 0x30–0x4A | A voice burst, when voice comes from TX RAM (layer 2 BS mode, AUDIO_CONTROL b0 = 1) |
| 0x4B–0x50 | Embedded RC |
| 0x495–0x4AF | Send voice key (27 bytes) |

**RX RAM (0x160004B0)**, datasheet table 53:

| Offset | Content |
|---|---|
| 0x00 | The deframed control burst: LC header / terminator 9 + 3, CSBK / headers 10 + 2, data blocks 12 / 18 / 24, idle 12, C_RC 7 + 2 |
| 0x00–0x08 | The embedded LC after late entry. The chip keeps the previous call's LC there until it decodes a new one (measured) |
| 0x2B–0x2F | The CACH short LC of a BS signal: 28 bits and the CRC-8 received; the low 4 bits of 0x2F are unused. New at system interrupt bit 4 |

The short LC interrupt only counts the short LC, and only while a BS signal is heard; the RAM keeps the last one. The
event task reads it (`hrc7000_dmr_short_lc`, in flash: a new one comes every 120 ms at most) and reports it when it
changes (`DMR_EV_SHORT_LC`). The stack checks the CRC-8. The driver doesn't use the chip's own CRC-8 flag
(RX_TYPE_INFO b30, in the burst report).

**Late-entry LC per slot.** The driver saves each late-entry LC under the chip slot that `hrc7000_dmr_decode` reads
from RX_TYPE_INFO at the late-entry interrupt (`current_rx_slot`, b19). It attaches the LC only to a voice burst on
that chip slot. A single pending LC could cross slots even with a working voice burst filter: an unselected slot's
late-entry interrupt sets it, its deframe is dropped, and the next selected voice burst takes the wrong caller (#495,
reproduced in a host test). Separate pending buffers also keep both LCs when both reports arrive before voice.

These discard a slot's pending LC:

- Any non-voice burst on that slot: LC header, terminator, CSBK, MBC, data header, PI header, idle, and a header with a
  bad CRC.
- A filtered burst.
- The slot's voice aging out (390 ms without a voice burst), so a call that fades before a voice burst takes its LC
  doesn't name the next headerless caller.

Receiver resets and a slot filter change discard both. A failed late-entry checksum invalidates only the reporting
slot's pending LC. The driver doesn't infer a DMR timeslot from the chip slot: BS bursts keep their CACH TC labels.

**Voice RAM (0x16000960).** Write 27 bytes before arming a voice burst; read 27 bytes after a voice deframe interrupt.
The burst numbering for the privacy keystream is RX_TYPE_INFO's 1–6.

### 2.9 BS (repeater) mode

The source is the datasheet (document 2, 10.2.2.17, 10.2.2.18, 10.2.2.22, 10.2.2.25-10.2.2.28) and register
measurements on the chip with the SDR decoding the bursts and CACHs.

| Item | Content |
|---|---|
| BS_MODE b2 / b1 / b0 | BS transmit mode, BS receive mode, dual (0) or single (1) frequency repeater |
| LAYER2_SLOTON b3 / b4 (0x404) | bs_tx_slot_on / bs_rx_slot_on: establish the BS TX / RX slot. Status: b28 o_bs_tx_slot_on, b27 o_bs_rx_slot_on |
| LAYER2_TXRX_CTRL b4 (0x408) | bs_tx_sync_rx_en: the BS TX slot follows the BS RX slot |
| LAYER2_SEND_TYPE b10 bs_tx_data_rdy (0x418) | 0: the chip frames and sends Idle (type from BS_MODE b11:8); 1: forwarded data is ready (self-clears at the slot edge) |
| LAYER2_SEND_TYPE b9 bs_tx_data_source | 0: the chip encodes; 1: send 196 information bits without coding |
| AUDIO tx_voice_source = 1 (4.6.5.3) | voice comes from MCU-written RAM (the layer 2 BS TX path) |
| BS_CACH b1:0 | AT and TC "for BS transmit mode" |
| RDY_NEXT_SHORT_LC interrupt (bit 17) | the chip asks for the next CACH short LC |
| BS_INTER_IN / BS_INTER_OUT pins (P12 / R12) | baseband BS interrupt in and out, apparently to link two chips |
| BS_SECOND_REG, BS_STATUS, GPS bits | same-frequency simulcast from a GPS pulse (2.9a) |

The chip behaves as follows (measured):

| # | Fact |
|---|---|
| 1 | The BS registers exist and read the datasheet's reset values: BS_MODE 0x00000900, the others 0 |
| 2 | The writable bits match the datasheet: BS_MODE b11:8 and b5:0; BS_CACH b1:0; BS_SECOND b5:0. Reserved bits read 0. BS_STATUS ignores writes. 0x42C isn't implemented (reads 0, ignores writes) |
| 3 | BS_MODE survives a chip reset. A radio left in BS mode stays there until the register is written back or the chip gets a hardware reset |
| 4 | With BS_MODE b2 set, the normal call start fails: the TX path never arms (`dmr.tx_error=stalled`, TX denied `rejected`). BS TX needs its own start sequence |
| 5 | With b2 set, then LAYER2_SLOTON b3 set and b0 cleared during a call, the chip reports o_bs_tx_slot_on (SLOTON reads 0x1000000C), and BS_STATUS b29:28 becomes non-zero (0x434 = 0x10000000): the CACH LCSS counter runs |
| 6 | The chip then sends its own Idle frames (data type 9 from bs_idle_type, color code from LOCAL_CC) with no MCU data, one every 60 ms in **one** slot. The RF keying stays the driver's. The first frame comes about 0.8 s after the switch |
| 7 | The sync word on those frames comes from SEND_DATA_SYNC. At its reset value they carry the MS data sync. With the BS data sync written they decode as BS-sourced |
| 8 | **Each BS frame follows a CACH with valid TACT parity**, but BS_CACH doesn't control its AT, TC, LCSS or short LC payload. With BS_CACH held at 0, 2 or 3, AT and TC take all four combinations about equally often and the short LC fragments are noise. TC doesn't alternate |

Fact 6 came from a register switch in the middle of a call, so the one-slot cadence can come from the driver's own TX
loop rather than the chip. The split duplex TX half doesn't use BS mode; it uses the physical layer (2.10).

### 2.9a gps_pps_en: the simulcast slot lock (datasheet only)

LAYER2_BS_MODE b3 `gps_pps_en` enables what the datasheet calls same-frequency simulcast (datasheet
10.2.2.25-10.2.2.27). It is a BS mode feature: the chip disciplines its BS slot timing to a GPS second pulse, so several
repeaters on one frequency send each slot at the same instant. The driver doesn't use it.

| Item | Content |
|---|---|
| BS_MODE b5:4 `gps_update_rate` | how often the pulse corrects the slot timing: every 6 s, 3 s or 1 s |
| BS_SECOND_REG b5:0 | the GPS second (0-59), written by the MCU from the receiver's time messages |
| BS_STATUS | `gps_lcss_cnt` (the CACH LCSS phase), `gps_bit_cnt` (a 30 ms slot counter run from the pulse), `bs_slot_num` (the 60 ms frame number in the 60 s cycle), `bs_second_lock` |
| System interrupt b8 `gps_slot_abnormal`, b7 `gps_lost_interp` | the pulse's slot timing jumped; the pulse was lost |

What this means for the split duplex TX half ([split-duplex.md](split-duplex.md) 3.8, INFO capability bit 5):

1. It needs a pulse on a chip pin. The datasheet names none for it; the only candidate is BS_INTER_IN (P12, "BS sync
   interrupt input"). Where an HR_C7000 radio has a GPS module, the module connects to an MCU UART only, with no pulse
   line to the chip or the MCU. So no radio here can use it.
2. It locks the chip's BS mode slot timing, which the TX half doesn't use: the TX half sends in the physical layer mode
   (2.10).
3. The host-side grid (ALIGN with the host's clock on NTP or a GPS-disciplined source) gives the same slot numbering
   without a pulse, to the clock sync's error bound.

### 2.10 Physical layer transmit: the split duplex TX half

The split duplex TX half ([split-duplex.md](split-duplex.md) 5.4) needs a CACH whose AT, TC and short LC it controls,
which the BS mode of 2.9 doesn't give. It uses the physical layer mode instead (datasheet 9.5), as the HR_C6000 does
([modem-hrc6000.md](modem-hrc6000.md) 9.1). The chip sends the CPU's bits without coding, and the core builds every
slot (`bsOutboundRawSlot`: the CACH, then the burst with BS sync, slot type and coded payload). The driver is
`drivers/modem/hrc7000/hrc7000_bs.c`.

| Item | Content (datasheet 9.2, 9.4.1, 9.5) |
|---|---|
| WORK_MODE | 0x22: physical layer, timeslot mode (b5 set, layermode 0, b1 aligned); 0x02: physical layer, continuous mode |
| RF_MODE | two-point TX (b6:5 = 3), as the 0x03269860 of 2.1 |
| LAYER2_CONTROL | b7 txen, b6 rxen, b1 tx_master_mode, b0 rx_master_mode. 0x82: transmit with the radio's own slot timing |
| LAYER2_TXRX_CTRL | b7 txnextsloten: send in the coming slot; b6 rxnextsloten; b0 autotest. The datasheet's set-up writes 0x81 |
| TX RAM 0x00-0x23 / 0x24-0x47 | SEND_DATA0 / SEND_DATA1: one 36-byte (288-bit) frame each, sent alternately. Two bits per symbol, 00 +1, 01 +3, 10 -1, 11 -3 (the ETSI dibit mapping), first bit in b7 of the first byte |
| System interrupt b12 | phy_rdy_interp: the physical layer's slot interrupt |

The datasheet's example frame for a bit error test carries the MS data sync at bits 120-167. In a burst the sync
starts at bit 108, so that frame is 12 bits of guard, the 264-bit burst and 12 bits of guard: an MS slot, aligned to
the slot edge. A BS outbound has no guard. Its slots follow each other without a gap, each CACH centered on the slot
edge. The driver writes the core's slots (CACH first) back to back, so the bit stream on air is the continuous
outbound. The CACH half that falls before the chip's slot edge is a constant 1.25 ms offset of the slot clock, which
the RX half's phase tracking absorbs ([split-duplex.md](split-duplex.md) 6).

While the BS outbound runs, the driver takes the slot engine's place, as `hrc6000_bs.c` does:

1. Start: TXRX 0, SLOTON 0, WORK_MODE 0x80 then 0x22 (or 0x02), LAYER2_CONTROL 0x82, codec buffers cleared. This starts
   a new slot clock.
2. Slot clock: a boundary at the first of the TX timeslot, RX timeslot or PHY_RDY interrupts in a slot (one more
   within 15 ms is the same slot). Unkeyed, the clock runs and nothing is sent.
3. Keyed: at each boundary a slot's 36 bytes go to TX RAM and TXRX = 0x80. The chip sends them in the slot that starts
   about 5 ms after the boundary's interrupt, not in the one after it (finding 11), so the slot clock names that slot.
   The key-up starts on an outbound TS1 slot. Each slot plays its own queue, an empty one an idle burst.
4. The first RF TX interrupt keys the RF chip and the PA (the RF burst task's continuous mode, 3.1a); the PA then stays
   up. Unkeyed: no more slots, and the PA goes off one slot later. With no boundary for 200 ms the PA goes off and the
   outbound is unkeyed.
5. Stop (and leaving DMR): TXRX 0, PA off, WORK_MODE 0x80 then 0x6E, LAYER2_CONTROL passive, both slot syncs released,
   then the normal receive restart (5.2). The driver never writes LAYER2_BS_MODE; the power-on set-up writes its reset
   value 0x900 (2.9: it survives a chip reset).

The variant (`dmr_bs_config_t.variant`, the debug link's SPLIT op 1, [split-duplex.md](split-duplex.md) 10.1) is a bit
field:

| Bits | Meaning |
|---|---|
| b0 | continuous mode (WORK_MODE 0x02) |
| b1 | the autotest bit with the send bit (TXRX 0x81) |
| b3:2 | the TX RAM halves: 0 the slot to both, 1 alternate starting with SEND_DATA0, 2 alternate starting with SEND_DATA1 |

The default 0 is timeslot mode, TXRX 0x80, both halves. Writing the slot to both TX RAM halves works, so which half the
chip sends in each slot doesn't matter.

The chip and driver behave as follows (measured with a second radio and the SDR receiving the outbound):

| # | Fact |
|---|---|
| 1 | PHY_RDY (system interrupt b12) comes first in every slot and makes the slot clock. The TX and RX timeslot interrupts follow in the same slot (two per slot) |
| 2 | Variants 0, 1, 4 and 8 give one RF TX interrupt per key-up: the chip stays in TX from slot to slot. With the autotest bit (variant 2) it comes every 60 ms: the chip leaves TX between slots |
| 3 | Every slot is set up while keyed, with no underrun and no stall |
| 4 | The outbound decodes: a second radio deframes one idle burst per 60 ms with BS sync and the color code, no CRC errors, labeled TS1 from the CACH TC (variant 8: TS2) |
| 5 | After the role ends (also by lease expiry) a normal call goes out whole |
| 6 | Variant 0 gives a whole outbound: BS data sync on both slots, a burst every 30.0 ms, one key-up edge, every CACH of valid TACT parity, TC alternating on every CACH, AT following SET_AT, the short LC decoding, no underrun |
| 7 | Variant 1 (continuous mode) gives the same outbound: the timeslot mode keeps the carrier and every CACH between slots too |
| 8 | A host-fed voice call goes out whole in its slot |
| 9 | Keyed for 5 minutes the outbound runs with no underrun or stall, and the carrier offset stays within a few hertz |
| 10 | The RX half labels a call with a residual of 0-1 µs after centering, and the label flips with the epoch |
| 11 | The slot set up at a boundary is on air from about 5 ms after the boundary's interrupt. TS1 bursts start 4.6 ms after the epoch. Naming the boundary after the slot before puts the epoch one slot early: a subscriber's TS2 call through a pair then goes up labeled TS1 and is repeated on TS1 |
| 12 | The slot timer restarts when own TX timing is enabled: LAYER2_CONTROL 0xC0 (passive), then 0x82 at a chosen time. The first boundary then comes about 1.2 ms (1167 µs) before a whole slot after the write. With that delay learned, the boundaries land 9-120 µs from the host's grid (ALIGN, [split-duplex.md](split-duplex.md) 3.8). Passive timing keeps counting while the driver waits, so the driver takes no boundary then. Redoing WORK_MODE through FM with LAYER2_CONTROL left at 0x82, or a LAYER2_SLOT_CNT TX counter preset, doesn't move it |
| 13 | LAYER2_MAX_CNT (2.5, datasheet 10.2.2.20: b7 sign and b3:0 n × 100 µs for TX slots, b15 and b11:8 for RX) doesn't move the physical layer's slot timer: not the TX trim for one slot (written at a boundary, cleared at the next), not the TX and RX trims together, not both held set. So the HR_C7000 driver offers no `bs_slew`: a keyed TX half reports its drift and re-aligns at the next unkey, as the HR_C6000 does |

---

## 3. Interrupts

| Vector (PIC source) | Name | Fires | What it means and what to do | Clear |
|---|---|---|---|---|
| 61 (29) | System_inter | after the chip deframes a burst (bit 3), on late entry (bit 2), slot abnormalities, FM signaling | read SYS_INTERP_LIST, act, write it back to SYS_INTERP_CLEAR until 0 | SYS_INTERP_CLEAR |
| 62 (30) | Time_slot_inter_tx | every 30 ms on the TX slot clock | **decide the next slot**: write TX RAM / voice RAM, SEND_TYPE, then TXRX_CTRL (0x80 = send in the coming slot, 0x00 / 0x40 otherwise). Start the PA one-shot whenever 0x80 is written | INTERP_CLEAR b0 and b1 |
| 63 (31) | Time_slot_inter_rx | every 30 ms on the RX slot clock | the same decision while receiving (and for the last repeater-wake wait state) | INTERP_CLEAR b0 and b1 |
| 64 (32) | Rf_tx_inter | rf_pre_on_tx × 100 µs (**3.1 ms** with RF_CONTROL 0x41F1A) before each TX burst | put the RF chip into TX (first burst: full TX set-up; later bursts: idle to TX); note the time | INTERP_CLEAR b2 (first thing) |
| 65 (33) | Rf_rx_inter | rf_pre_on_rx before an RX slot | TX ends at the next timeslot interrupt once the burst has run 20 ms or more | INTERP_CLEAR b3 |
| 59 (27) | PCM_rd_data_interp | every 10 ms while AUDIO_CONTROL b5 is set | speaker buffer consumed: write the next 160 bytes of PCM to 0x180000A0 | INTERP_CLEAR b5 |
| 60 (28) | PCM_wr_data_interp | every 10 ms while AUDIO_CONTROL b4 is set | 160 bytes of mic PCM ready at 0x18000000 | INTERP_CLEAR b4 |

One routine handles both timeslot interrupts. It decides by the current activity: transmitting states on the TX
vector, receiving states on the RX vector. DMR TX doesn't start unless vectors 62 and 64 are hooked.

### 3.1 TX timing within a burst

The times follow from the register values.

| t (ms) relative to the TX timeslot interrupt in which the burst is armed | Event |
|---|---|
| 0 | TX timeslot interrupt: payload and SEND_TYPE written, TXRX_CTRL = 0x80, PA one-shot started |
| ≈ 26.9 | RF TX interrupt (3.1 ms ahead): RF chip idle to TX |
| 28 (default "DMR PA" setting, 1–35 ms) | PA one-shot: band PA select on, PA drive DAC to the calibrated value |
| ≈ 29.6 | tx_pre_on (0.4 ms by default; per board) |
| 30 | burst on air (27.5 ms of symbols plus guard) |
| next timeslot interrupt with 20 ms or more of TX elapsed | PA drive 0, PA selects off, RF chip back to RX |

RadioFoundry keys from the RF TX interrupt instead, on the per-burst RF task (3.1a): the PA a fixed time after it, and
back to receive at the burst's last symbol.

### 3.1a The per-burst RF task

Every HR_C7000 board keys its RF per burst: the RF chip to the TX frequency and its TX state, the paths and supplies,
the PA, and back to receive after the burst. So the radio hears the other slot, and a hotspot listens while keyed.
Where the modem's MOD1 output modulates the reference oscillator instead of the RF chip, this is the analog
transmitter's whole key-up, a few milliseconds with waits in it.

The sequences run in a task of their own at the highest priority (`platform/hrc7000/dmr_rfburst.c`); the interrupts
only wake it. The board gives the two sequences (`hrc_dmr_board_t.burst`, an `hrc_rfburst_board_t`) and arms and
disarms the task from `tx()`, `tx_off()` and `rx()`. The glue starts the task and passes it every switch.

A board's sequences are only its chip steps; the timing is the task's (`dmr_rfburst_timing.h`):

- A sequence's `at` waits count from the RF TX interrupt. A two-point key-up's RF switch and PA times are
  `hrc_rfburst_switch_at_us` and `hrc_rfburst_pa_at_us`.
- The task holds the TX off until the burst's last symbol, computed from the modem's `rf_tx_lead`.
- A board whose switch must come late (to keep receiving the inbound burst before its own) sets a lead about 0.5 ms
  longer than that point and waits: the task can wake up to 0.42 ms after the interrupt.

A split duplex hotspot's base-station outbound (`hrc7000_bs.c`) is continuous. The task runs the TX-on sequence at its
first RF TX interrupt and the TX-off sequence at once when it stops, with no hold and no RF RX interrupt ending it.

| Event | Source | The task does |
|---|---|---|
| TX on | the RF TX interrupt, rf_pre_on_tx before the burst | the board's TX-on sequence |
| TX off | the next timeslot interrupt, or the RF RX interrupt once 20 ms have passed since the RF TX interrupt, whichever comes first | waits until the burst's last symbol, then the board's TX-off sequence |
| Abort | disarm (the end of the transmission, a retune) | the TX-off sequence at once; in a TX-off's hold, the hold's end (the disarm waits for it, up to 40 ms) |
| End of an outbound | the base-station outbound stops (split duplex) | the TX-off sequence at once |

The burst's last symbol is rf_pre_on_tx × 100 µs + 0.85 ms + 27.5 ms after the RF TX interrupt
(`platform/hrc7000/dmr_rfburst_timing.h`). So the lead a board sets is the time its TX-on sequence needs, less
0.85 ms, rounded up to 100 µs. The board's key-up delay in its TX-on sequence counts from the RF TX interrupt, so the
task's wake doesn't delay the PA.

The interrupt itself can come a few hundred µs late while the slot engine holds its lock with the modem's interrupts
masked (3.2). The bursts keep the 60 ms frame grid, so a burst whose interrupt is more than 30 µs past one frame after
the burst before is timed from that point instead.

The first burst of a transmission has no burst before it. With a long lead the modem raises its RF TX interrupt late by
the same amount at every key-up, while the burst itself is on the slot grid: 0.42 ms at lead 26, about 0.2 ms at 24,
none at 9 (measured). Timed from that stamp, the first burst's TX on and TX off come late by as much, and its first
symbols go out on a rising carrier. The slot engine's tick is steady through the key-up, so the task times the first
burst from the tick instead:

| Step | What |
|---|---|
| Learn | Each burst on the frame grid (within 30 µs of one frame after the one before) gives its interrupt's time after the tick before it; the task keeps the largest. A tick held up by the masking makes the time shorter, and a late interrupt is off the grid, so the largest is the true one. The value is kept across transmissions and cleared when the lead changes |
| First burst | Its interrupt's time after its tick, against what was learned: later by more than 30 µs and no more than 1 ms, it is timed at the learned point after the tick. Otherwise, or with nothing learned yet (the first transmission after a start, or after a lead change), it is timed from its interrupt |

The arithmetic is in `dmr_rfburst_timing.h` (`hrc_rfburst_tick_ref`, `hrc_rfburst_first_on`). The diagnostics
(`hrc_rfburst_stats_t`) count these:

- The sequences run, the RF RX interrupts and the TX-offs that started late.
- Since the key-up: the largest wake, the PA-up time after the interrupt (smallest and largest), and the largest grid
  error.
- The bursts timed from the grid, and how much earlier than its interrupt the first burst was timed.

### 3.2 Masking the modem interrupts

PIC_MASK bits 29, 30 and 31 and PIC_MASK_1 bit 0 are vectors 61–64 (datasheet mapping). Mask them around
multi-register updates from task level.

### 3.3 Lost requests

A modem request whose line stays high after the PIC took it leaves that source silent: the PIC interrupts on the line
rising only ([gotchas.md](gotchas.md) 2.9). TS and RF requests have no readable status, so the driver can't see one
pending. The engine handles it like this:

| When | What |
|---|---|
| Every receiver restart (5.2: DMR entry, a color code or framing change, a stale or lost reception, the end of every transmission) | writes INTERP_CLEAR b0-b2 and writes SYS_INTERP_LIST back to SYS_INTERP_CLEAR. Not b3 (RF RX) or the PCM pair: other drivers own them |
| No timeslot interrupt for 200 ms while sending | the transmission ends (`dmr.tx_error=stalled`), and its receiver restart clears as above. Counted (`dmr.modem_stalls`) |
| A SYS_INTERP_LIST cause enabled in SYS_INTERP_MASK, pending at the 10 ms polls for 50 ms with no system interrupt taken | written back to SYS_INTERP_CLEAR. Counted (`dmr.modem_stuck`) |

For the first stall or stuck request, the glue keeps PIC_MASK, PIC_MASK_1, PIC_INT_ST and PIC_INT_ST_1 as read before
that poll (`dmr.modem_lost_ms`, `dmr.modem_lost_regs`, [debuglink.md](debuglink.md) 3.7). It then unmasks the modem
sources again, unless a task holds the engine's lock.

---

## 4. Initialization (power-on, once)

The steps are in order. The driver writes the whole DMR block once here, not at every mode change.

| # | Register | Value | Why |
|---|---|---|---|
| 1 | ADC controller | reset and idle (board-specific) | the baseband shares the analog front end |
| 2 | IO_DIPLEX2 (0x03C) | clear bit 28 | DAC channel B pin to analog (PA drive) |
| 3 | DAC_PD_CTRL / PD_MODE_EN / DATA_A / B / C | 0x01 / 0x01 / 0 / 0 / 0x06E2 | DAC B (PA drive) and C on, A off; PA drive 0 |
| 4 | FM_BANDWIDTH (0x500) | 0x5F | FM wideband defaults |
| 5 | FM_DEV_COEF (0x504) | 0x33500 | FM deviation defaults (overwritten per TX/RX) |
| 6 | SYS_SOFT_RSTN (0x000) | clear bit 4, wait 2 µs | codec reset pulse |
| 7 | codec AICR_DAC, AICR_ADC, CR_MIC1, CR_MIC2, CR_DAC, CR_ADC, CR_VIC, CR_CK, GCR_DACL, GCR_MIC1, GCR_ADCL, CR_DAC_AGC, CR_ADC_AGC, DR_ADC_AGC | 0xC0, 0xC0, 0x00, 0x70, 0x20, 0x20, 0x0C, 0x40, 0x80 (with the line out gain, 0 dB unless set), 0x00, 0x8B, 0, 0, 0x34 | audio codec on, mic input 2, gains (board-specific) |
| 8 | LINEOUT_CTRL (0x088) | 0x01 | speaker line out on |
| 9 | SIG_CENTER | (cal << 24) or (cal << 16) | TX I/Q center |
| 10 | RF_MOD_BIAS_CTRL | ((cal + 384) << 16) or 0x4D4D | modulation balance until a TX frequency is known |
| 11 | RF_MODE | 0x03269860 | two-point TX, AF RX, b11 set (2.1a) |
| 12 | DEV_LIMITER | 0x0000FF00 | |
| 13 | SYS_SOFT_RSTN | 0x190 | the board's reset bits |
| 14 | DAC_CONTROL (0x070) | 0x8000001B | baseband DAC set-up: b31 offset code; b5 / b4 power down DAC A / B; b2 / b1 low power DAC A / B (1 = off or low power per the datasheet). So DAC A is on at full power, and DAC B is powered down and in low power |
| 15 | ADC_CONTROL (0x074) | \|= 0x41C3 | baseband ADC set-up |
| 16 | AUDIO_CONTROL (0x080) | 0x02 | I2S slave (reset default), all audio paths off |
| 17 | AUDIO_BUFFER_CLR (0x084) | 0x03 | clear the vocoder encode and decode buffers |
| 18 | sync words (2.2) | as listed | DMR syncs |
| 19 | THRESHOLD_VALUE | 0x0978786F | sync detection |
| 20 | PI_CRC_MASK, RS_H_MASK, RS_H_INIT, RS_T_MASK, RS_T_INIT, CSBK_CRC_MASK, MBCH_CRC_MASK, DATAH_CRC_MASK | 0x6969, 0x969696, 0, 0x999999, 0, 0xA5A5, 0xAAAA, 0xCCCC | ETSI masks; the chip then computes those CRCs and RS itself |
| 21 | 0x370, 0x374, 0x378, 0x37C | 0xF0, 0x1FF, 0x10F, 0 | undocumented |
| 22 | CRC8_INIT, VOICE_EMB_CTRL, SCRAMBLE_REG | 0, 0, 0 | |
| 23 | SYS_INTERP_MASK | 0x1187F | enable layer 2 TX/RX processing, late entry, deframe, short LC, slot abnormal, PHY, PHY slot abnormal |
| 24 | LAYER2_SLOT_PRE | 0x551010 | processing-interrupt advance |
| 25 | 0x168 | 0x14 | undocumented |
| 26 | RF_RECV_DELAY | 0x36B0 | 1.4 ms receive path delay |
| 27 | RX_IF_FREQ, TX_IF_FREQ | 0x0BB800 each | |
| 28 | RECV_MS_SYNC_H, RECV_BS_SYNC_H | set bit 31 | undocumented enable |
| 29 | RF_CONTROL | 0x00041F1A | RF TX interrupt 3.1 ms early; b20:16 (`tx_pre_on`) and b13:8 (`rf_tx_lead`) are the board's (2.1) |
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
| 3 | DAC_PD_CTRL | clear bit 1 | **PA drive DAC back on.** FM receive powers it down; without this step, DMR after FM has no PA drive |
| 4 | WORK_MODE | 0x80 | pass through FM: resets the DMR state machine |
| 5 | WORK_MODE | 0x6E | DMR, Tier II, timeslot, layer 2 |

AUDIO_CONTROL isn't rewritten here. After FM it ends up 0x00 once FM audio is switched off.

### 5.2 Start or restart receive

The driver uses this sequence wherever it must (re)acquire reception: at DMR entry, after TX through a repeater
(direct: 5.2a), after a stalled transmission, and at a new DMR frequency (the radio core calls `rx_restart` then,
[gotchas.md](gotchas.md) 17.2).

| # | Register | Value | Why |
|---|---|---|---|
| 1 | (enter DMR, 5.1) | | |
| 2 | VOICE_EMB_CTRL | 0x02 | burst F EMB from the chip's encoder |
| 3 | LAYER2_CONTROL | 0xC0 | TX/RX enabled, passive timing |
| 4 | LAYER2_SLOTON | 0x00 | drop slot establishment |
| 5 | LAYER2_TXRX_CTRL | 0x800000 | release RX slot sync |
| 6 | LAYER2_TXRX_CTRL | 0x40 | receive in the next slot |
| 7 | LAYER2_SLOTON | 0x05 | dll_rx_slot_on and dll_tx_slot_on |

A sequence of TXRX 0xC00000, 0, 0xC00000, 0x8040 with no SLOTON writes, or a busy wait in the color code probe, doesn't
acquire sync reliably. Use this sequence everywhere.

A restart also forgets what was heard: the polite-access busy window ([dmr-modem.md](dmr-modem.md)) starts again with
the next burst taken. Polite access ages the last decoded activity by `now_us`, with a 360 ms window, independently of
timeslot interrupts. Every usable burst refreshes the window, including one that the slot or CRC filter drops later;
unusable deframes don't. The own-color-code and all-activity policies keep their color code checks.

Receive recovery uses wall time too, from both the timeslot handler and the task poll. It restarts the receiver after
any of these:

- More than 390 ms without another usable burst.
- 600 ms of stale activity without a usable burst.
- 150 ms after a direct TX hold (5.2a).

A restart clears the pending recovery. Later slot interrupts, deframes or abnormal-slot reports schedule it again, and
the 600 ms count starts with the first of them, not at the restart: after a long silence, a count from the restart
would restart the receiver under the next call's first burst. A silent receiver isn't restarted over and over. This
allows recovery when timeslot interrupts stop but system interrupts continue. Host register-model tests cover stopped
timeslot interrupts.

### 5.2a End of a direct transmission: the hold

The end of a transmission comes on the TX tick a slot after the radio's terminator, about 25 ms into the next slot. A
direct answer in the other timeslot, right after the terminator (30 ms after its start), is then already being
received. A 5.2 restart at that point makes the chip report the answer with BS sync and, on some radios, a header CRC
error ([gotchas.md](gotchas.md) 2.10). So a direct transmission ends without the restart:

| # | Register | Value | Why |
|---|---|---|---|
| 1 | LAYER2_CONTROL | 0xC0 | passive timing from the active one; the slot syncs stay where the transmission left them |
| 2 | LAYER2_TXRX_CTRL | 0x40 | receive the next slot |

The software state is reset as at a restart, the polite busy window too. A burst taken holds the receiver as any
reception does. With none, the 5.2 restart comes once 5 slots (150 ms) pass with nothing taken, so a station on a
timing of its own is taken after that. Through a repeater the restart still comes at once: the next transmission needs
a new TC lock. The diagnostics show `rs restarts/lost/stale h holds`.

| Answer after the start of the radio's terminator | With a restart at the end | With the hold |
|---|---|---|
| 30 ms | The answer is reported with BS sync and often needs a retry | Taken on the first try, MS sync |
| 60 ms | Taken | Taken |
| 90 ms | Taken | Taken |

The RF has the same window. An answer in the radio's own timeslot comes one frame later, 60 ms after the start of its
terminator (for example a commercial portable's link ACK to confirmed data). A receive set-up by the board at that
point (idle, synthesizer, receive) misses it. So the radio core ends a DMR transmission through
`hal_radio_t.dmr_tx_end`, which the glue implements (`hrc_dmr_tx_end`). When the last burst's switch put the RF back to
receive, the board's `tx_end` (`hrc_dmr_board_t`) ends the transmission and leaves the RF as it is if it is on the last
`rx()` settings (frequency, trim, awake). Otherwise, and on a board without `tx_end`, the core calls `tx_off()`
([gotchas.md](gotchas.md) 3.14).

### 5.3 While receiving

| Event | Action |
|---|---|
| System interrupt, bit 3 (deframe) while idle, good CRC, wanted CC | start following: TXRX_CTRL = 0x40 for the next slot (0x8040 here doesn't work) |
| Each TX/RX timeslot interrupt while following one slot | alternate: the followed slot gets TXRX = 0x8040 (receive, voice-access flag) for the next; the other slot gets 0x00 |
| RadioFoundry: each RX timeslot interrupt | TXRX = 0x8040 for the coming chip slot when a voice header or voice burst came on it in the last 390 ms (no terminator since), else 0x40. Both slots can have it: an SFR user hears the talker and the repeat. Without the flag the chip leaves the voice RAM as it was, and the burst reads the last one decoded ([gotchas.md](gotchas.md) 14.15) |
| Voice deframe | read RX_TYPE_INFO and 27 bytes from the voice RAM; the burst number from bits 14:12; LC header / terminator / CSBK / data from the RX RAM |
| BS sync received (recv_sync_type 1) | the other side is a repeater: track the TC bit (RX_TYPE_INFO b22) to find which slot is which. Locked after 4 bursts in a row, one per slot, with TC alternating; held while a burst came in the last 390 ms. RX_TYPE_INFO b19 and LAYER2_STATUS b30/b31 aren't usable for this |
| Any color code | no CC filter: LOCAL_CC cc_opt = 1 deframes every code, RX_TYPE_INFO b7:4 gives the code heard, and RF_MODE b12 is 0 (fixed sync threshold, as cc_opt 1 requires). The receiver restarts at the end of a reception as with a filter. Don't scan by stepping LOCAL_CC at timeslot interrupts: they stop while the chip hears a code it doesn't deframe, so the scan hears nothing |
| Late entry (bit 2) | the embedded LC is in the RX RAM; voice reception starts at the next burst A |
| End | terminator (data type 2) or loss of bursts; TXRX_CTRL = 0 |
| Reception over (nothing taken for 390 ms) | the 5.2 restart: the chip drops the sender's slot timing, and its timeslot interrupts stop |
| Slot timing with nothing usable for 600 ms | the 5.2 restart, again every 600 ms while it lasts. Without it the chip keeps a departed sender's timing, reports only unusable and abnormal slots, and takes no later call (measured) |

### 5.4 The receive tap

In layer 2 mode the RX RAM also holds each burst received as bits. The datasheet doesn't document this (measured).

| Where | What |
|---|---|
| RX 0x30-0x53, RX 0x60-0x83 | one burst each, 288 hard bits in the HR_C6000 layout ([hrc6000-registers.md](hrc6000-registers.md) 2.2): the CACH (24), then the burst deframed, a voice-layout burst's 48-bit sync / EMB / RC field first. Which area takes which slot follows RX_TYPE_INFO b19, not the CACH TC |
| RX 0x48, RX 0x1C8 | the soft values of those two bursts without the CACH, one byte per bit, bits 5-0; the hard bit is 1 at 32 or more |

The first area's soft values overwrite the start of the second area, its CACH and field among them, so the driver
reads that burst's field from its soft values at 0x1C8. The engine reads the tap after a slot it listened to while
sending, for a BS's dedicated RC burst, which the chip doesn't report ([dmr-modem.md](dmr-modem.md), reverse channel).

While the radio sends through a repeater, the chip hears the repeater in only some of the listen slots (the RF is back
on the receive frequency after each burst). In some overs it gives the BS's bursts, typed or not, and the tap fills. In
others it gives about one deframe a second, and the tap areas don't change for seconds. So the modem sets
`tx_listen_gaps`: an RC is caught when heard, and silence isn't a repeater drop. LAYER2_SLOTON stays 0 there (5.3).

- With b2 dll_rx_slot_on set while sending, the chip hears the BS in nearly every listen slot, but the repeater decodes
  none of the radio's voice bursts. Set once, the repeater takes the header only; set for each listen slot and cleared
  before the radio's own, nothing at all.
- SLOTON 0x05, set once or per slot, changes nothing.
- While the radio sends, the second area's soft values are 7-bit signed, with a dibit's first bit in the sign (b6).

---

## 6. Transmit sequences

LOCAL_CC keeps the receive code during an aligned start's TC lock wait. Once locked, or at an unaligned start, the
driver writes the channel's transmit code in bits 3:0 and keeps it through the final burst. After TX it restores the
receive filter or scan code. A receive filter changed during TX takes effect when TX finishes.

All transmit actions happen in the timeslot interrupts. The CPU arms one slot at a time with TXRX_CTRL = 0x80 and
leaves the other slot unused (0x00, or 0x40 / 0x8040 to listen: `tx_listen`, an SFR's inbound slot). That gives one
burst per 60 ms in its timeslot (datasheet figures 99–100). Every arming also starts the PA one-shot of a board without
burst sequences (3.1a). The RF TX interrupt that follows keys the RF chip.

### 6.1 Start of transmission

| # | When | Register writes |
|---|---|---|
| 1 | task level, PTT | load the LC (9 bytes, then 3 zero bytes) into TX RAM 0x16000000; codec reset; LAYER2_CONTROL = **0xC3** (active timing: the radio owns the slot clock); TXRX_CTRL = 0 |
| 2 | next timeslot interrupt | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (LC header) |
| 3 | next | TXRX_CTRL = 0 |
| 4 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (second header) |
| 5 | next | TXRX_CTRL = 0 |
| 6 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (third header) |

TXRX_CTRL bit 31 (tx_complete_cpu) belongs to continuous mode; don't set it at step 2. A direct mode start after
receiving on a repeater channel uses the same LAYER2_CONTROL = 0xC3.

### 6.2 Voice

The slots alternate. In the inactive slot TXRX_CTRL = 0. In the active slot:

1. For burst A (n = 0): rewrite the call LC (or talker alias LC) into TX RAM bytes 0–8 for the embedded LC.
2. Write 27 bytes of AMBE (three 9-byte frames) to the voice RAM 0x16000960, or the AMBE silence pattern when no
   encoded audio is ready.
3. TXRX_CTRL = 0x80, SEND_TYPE = 0x08 + (n << 4) (+ LCSS, see 2.6); n = n + 1 modulo 6.

Encode audio continuously during TX: encode one 20 ms block per tick whenever two PCM buffers are waiting.

### 6.3 Terminator and back to receive

| # | Slot | Writes |
|---|---|---|
| 1 | active | voice RAM = silence; TX RAM = LC; TXRX_CTRL = 0x80; SEND_TYPE = 0x20 |
| 2 | next | direct mode: done, go idle (RadioFoundry: the 5.2a hold). Repeater mode: LAYER2_CONTROL = 0xC0, TXRX_CTRL = 0 |
| 3 | next (repeater mode) | TXRX_CTRL = **0x40** (receive next slot): receiving |

### 6.4 Data and CSBK

For each burst, in an active slot:

1. Write the payload to TX RAM: 10 bytes for a CSBK, MBC header or data header (the chip appends the CRC-16 with the
   programmed mask because SEND_TYPE b8 = 0); 12 / 18 / 24 bytes for rate ½ / ¾ / 1 blocks, with CRC-9 and CRC-32
   computed by the CPU.
2. SEND_TYPE = data type << 4, TXRX_CTRL = 0x80.
3. Start the PA one-shot.

In the inactive slot TXRX_CTRL = 0, or 0x40 to listen for a response. A packet is a data header (0x60), the blocks
(0x70/0x80/0xA0) and an optional terminator. CSBK signaling is optional preamble CSBKs, then the function CSBK, all
0x30 (datasheet appendices B, C). Open RX afterwards to catch the response.

**Rate 3/4 blocks don't decode.** The chip's trellis encoder doesn't start each rate 3/4 block in state 0
(TS 102 361-1 B.2.2). A block re-encodes bit for bit from the CPU's 18 octets with start state 4 or 7, which varies
between blocks and tries, and sometimes one more info bit differs in the second constellation point. A receiver that
starts its decoder in state 0, as the standard has it, takes the wrong path through the first tribits: octets 0-1 (a
confirmed block's DBSN and CRC-9) come out wrong, and the packet fails. Rate 1/2 blocks, headers and CSBKs are sent
exactly. SEND_TYPE b8 makes no difference, and the register map has no encoder reset. The glue sets
`hal_dmr_modem_t.tx_rate34_bad`, so the core sends no rate 3/4 of its own: confirmed IP goes at rate 1/2
([dmr-data.md](dmr-data.md) 12.9). Rate 3/4 blocks the radio relays (hotspot, repeater) keep their rate and have the
same fault.

### 6.5 BS activation (repeater wake)

The driver doesn't use the chip's layer 3 BS activation: LAYER2_BS_MODE, the BS_* registers and WORK_MODE values
0x73/0x76/0x77 stay unused. Section 2.9 has what they do when written.

| # | When | Writes |
|---|---|---|
| 1 | PTT on a repeater channel with no repeater heard | codec reset; LAYER2_CONTROL = 0xC3 |
| 2 | timeslot interrupt | TX RAM bytes 0–11: 0xB8 (last block, CSBKO 0x38 BS_Dwn_Act), FID 0, bytes 2–6 = 0 (BS address 0), bytes 7–9 = own ID big endian, bytes 10–11 = 0; SEND_TYPE = 0x30; TXRX_CTRL = 0x80; **start the PA one-shot**; red LED |
| 3 | next timeslot interrupt | **SEND_TYPE = 0x30, TXRX_CTRL = 0x80 again**: the same CSBK in the immediately following slot; wake response timeout 600 ms |
| 4 | next | go to receive (5.2), count TC toggles |
| 5 | while waiting | when the received TC has toggled correctly 4 times, the repeater is awake: start 6.1. Otherwise retry from step 2 after 600 ms, and give up after a few tries |

One CSBK without keying the PA gives only an unmodulated carrier. Repeater wake needs both the PA one-shot and the
second consecutive CSBK.

---

## 7. CRC and FEC the chip does

| Burst | Chip adds | CPU supplies |
|---|---|---|
| LC header, terminator | RS(12,9) with RS_H_MASK / RS_T_MASK, BPTC(196,96), slot type Golay | 9 LC bytes |
| CSBK, MBC header, data header, PI header | CRC-16 with the programmed mask (when SEND_TYPE b8 = 0), BPTC, slot type | 10 bytes |
| Rate ½ / ¾ / 1 data | BPTC / trellis ¾ / none, slot type | 12 / 18 / 24 bytes including CRC-9 and the final CRC-32 |
| Voice | sync / EMB (QR), embedded LC fragments (5-bit checksum from TX RAM byte 9 or computed), burst F EMB from the RC encoder when VOICE_EMB_CTRL b1 = 1 | 27 AMBE bytes; LC for burst A |
| Short LC (CACH) | CRC-8 (CRC8_INIT) | repeater mode only |

On receive the chip checks the same codes and reports them in RX_TYPE_INFO b30:24. It decodes the FEC and leaves only
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
| 0x518 | CTCSS_ADDR | b15:8 RX tone address, b7:0 TX tone address (table in 8.2) | |
| 0x51C | USER_DEFINE_CTC | b31 enable, b20:0 step = round(f × 65.536) | non-standard tones |
| 0x520 | CDCSS_ADDR | b24:16 RX code, b8:0 TX code (the 9-bit DCS code as binary, for example 114 octal = 001 001 100). The chip takes the TX code when FM_PTT goes on; later writes don't change the code on air (measured) | |
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
| FM receive | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; FM_PTT = 0; AUDIO_CONTROL = 0x40; GCR_DACL = 0x80 with the board's line out gain (`hrc7000_set_dac_gain`, 0 dB unless a soft volume set it); DAC_PD_CTRL bit 1 set (PA drive DAC off); WORK_MODE = 0x80 |
| FM transmit | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; DAC_PD_CTRL bit 1 cleared; WORK_MODE = 0x80; FM_PTT = 1. Board: mic switch to analog, RF chip to TX, PA select, DAC B drive |
| FM audio on / off in DMR context | AUDIO_CONTROL b6 set and WORK_MODE = 0x80 / b6 cleared and WORK_MODE = 0x6E |
| Mute mic / line out | codec CR_ADC b7 / CR_DAC b7 = 1 |

These are the standard CTCSS addresses (datasheet table 59). RX uses the same addresses in CTCSS_ADDR b15:8.

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

The address order isn't ascending, so map from the frequency, not from a list index. Other tones use USER_DEFINE_CTC.

**Sub-audio deviation.** SUBVOICE_COEF b15:8 sets the TX sub-audio deviation (typically 350 Hz narrow, 600 Hz wide).
The FM_BANDWIDTH bits don't scale it. These values are measured on UHF:

| Value | Result |
|---|---|
| CTCSS 0x28 (reset value) | 300 Hz at both bandwidths |
| CTCSS 0x50 | 600 Hz; RadioFoundry writes it at 25 kHz |
| DCS | a third of the CTCSS deviation for the same value (0x28: 100 Hz). The field is signed: 0xF0 sends the code inverted at about 40 Hz |
| DCS 0x78 / 0x7F | 305 Hz / 323 Hz. RadioFoundry writes three times the CTCSS value for DCS, capped at 0x7F: 0x78 at 12.5 kHz, 0x7F at 25 kHz |

The boot stub's restart doesn't reset the modem, so the field keeps the last transmission's value. RadioFoundry's init
writes 0x28 rather than reading it back: a field left at 0xFF after a 25 kHz transmission sends CTCSS at 1.9 kHz
deviation (measured).

**Tail.** When FM_PTT drops, the chip sends the reverse burst or the turn-off code before stopping. FM_MOD b9 (reset 1)
enables the CDCSS turn-off, a 134.4 Hz tone. RadioFoundry sets b9 with DCS. For the core's tail
([radio-features.md](radio-features.md) 3.3a) it drops FM_PTT while the RF stays keyed; FM_PTT on again resumes the
code. On air the turn-off is 134.4 Hz for 0.20 s, then the carrier without modulation until the core unkeys
(measured). Unkeying together with FM_PTT puts none of it on air.

**Detection.** On a detected tone the FM signaling system interrupt fires (SYS_INTERP_LIST b13), and FM_DECODE_FLAG2 b0
(CTCSS) or b1 (CDCSS) goes to 1, back to 0 at the end. CDCSS tail detection uses FM_FUNCTION_COEF1 = 0x7F499D
(134.4 Hz).

### 8.3 DTMF and single tones

| Item | Value |
|---|---|
| Tone step | round(f × 2.048) (= f × 65536 / 32000) |
| Default tone registers | FM_TONE1 = 697 / 770 Hz (0x0593 / 0x0629), FM_TONE2 = 852 / 941 (0x06D1 / 0x0787), FM_TONE3 = 1209 / 1336 (0x09AC / 0x0AB0), FM_TONE4 = 1477 / 1633 (0x0BD1 / 0x0D10); the high half is the first tone |
| DTMF deviation (SUBVOICE_COEF b15:8) | Measured on UHF with a 1750 Hz single tone: about 5.8 Hz per step, 0x60 = 559 Hz, 0xC0 = 1117 Hz; 0x12 gives about 100 Hz. RadioFoundry uses 0xC0 at 12.5 kHz and 0xFF (the most, about 1.5 kHz) at 25 kHz, on both bands. The modulation calibration bytes apply per band, so the UHF values carry over to VHF |
| Send sequence | FM_MOD b1 = 1; FM_PTT = 1; TONE_SEND_CTRL b6 = 0 (mute voice); AUDIO_CONTROL b3 = 1 for sidetone; TONE_SEND_CTRL b3:0 = digit; b7 = 1 to start the digit, b7 = 0 to end it; repeat per digit; finally b6 = 1 to restore voice |
| Sidetone (`hrc7000_tx_tone_side`) | AUDIO_CONTROL b3 set while a tone is sent: the codec plays the tone the generator sends, so the speaker (amplifier on, source = codec) hears the start tone on air without the prompt path, which would reach the modulator. Used for the FM start tone set to speaker + air |
| Arbitrary single tone | write the tone step into the high halves of FM_TONE1 and FM_TONE3 and send DTMF digit "1" (697 + 1209 pair); restore the defaults after. On air at 1750 Hz (measured): TONE_SEND_CTRL = 0x81 (digit 1 on, voice muted) with FM_MOD b1 set, then 0 (the reset value) |
| RadioFoundry's DTMF (`hrc7000_tx_dtmf`, [radio-features.md](radio-features.md) 3.7) | the arbitrary-tone sequence with the low tone's step in FM_TONE1's high half and the high tone's in FM_TONE3's, digit "1" sent: one sequence for every digit and for single tones, without the digit codes of figure 95. The deviation is the single tone's (SUBVOICE_COEF b15:8, `hrc7000_dtmf_dev`). The datasheet says the chip attenuates the low tone 2.5 dB; on air the high tone is 5.0 dB over the low one for every digit (measured), so the datasheet's figure is likely 10 log of the amplitude ratio. That forward twist is the chip's design and isn't corrected: after a receiver's de-emphasis it is +0.4 to +2.1 dB ([radio-features.md](radio-features.md) 3.7) |
| Receive | FM_MOD b1 = 1; SUBVOICE_COEF b27:16 = 1056 (33 ms at 32 kHz), b7:0 threshold; FM_FUNCTION_COEF1..8 = 0x7ECD9D, 0x7E8A34, 0x7E368C, 0x7DD245, 0x7C690D, 0x7B9F03, 0x7AA7A8, 0x7979F5; read FM_DECODE_FLAG2 b11:8 on each signaling interrupt; FM_MOD b23:16 sets the repeat-symbol window |

### 8.4 Prompt (beep) FIFO

To play audio, set AUDIO_CONTROL b7. Then, while AUDIO_CONTROL b31 is 0, write 64 bytes (32 × 16-bit samples, low
byte first, 8 kHz) to 0x16000980..0x160009BF. Clear b7 when done (measured). High byte first (16 samples per push)
sounds distorted. To flush stale audio, fill the FIFO with 0x7F bytes.

RadioFoundry plays all speaker PCM through this FIFO (`platform/hrc7000/audio_pcm.c`). The CPU-fed speaker buffer at
0x180000A0 (AUDIO_CONTROL b5, section 3) gives rough, choppy beeps with every sample layout tried, with no queue
underruns (measured).

**Keep a block ahead.** The FIFO takes two pushes from empty (64 samples, 8 ms) before b31 sets (measured), less than
one 20 ms PCM block. The blocks come at the rate the FIFO plays them, so a queue that pushes each block as it arrives
has the FIFO nearly empty at every block edge. It runs dry there: every tone breaks up at 20 ms intervals, its pitch
jumping and its level dipping by up to 40 dB (measured). So `audio_pcm.c` pushes nothing after the queue is emptied
(route on, gate edges) until two blocks are queued. The queue then stays a block ahead of the FIFO, for one block
(20 ms) of latency, and doesn't run out.

**The write timer.** A dedicated timer checks the FIFO every 2 ms independently of the audio task, because a voice
decoder can occupy that task longer than the FIFO's 8 ms capacity. On seeing room, the timer schedules a write at
least 200 µs later; the interrupt doesn't busy-wait during that interval. Each write interrupt pushes at most one
chunk, after it checks the queue generation, speaker route, TX gate, FIFO ownership and available space again.
Stop/start, queue resets and FIFO ownership changes cancel a pending write. Three queue entries can be published, with
a fourth reserved for the producer: variable decode time can finish a third block before either primed entry has
drained. The occupancy check comes after decoding, so space freed during decoding can be used. The `un` field of the
`pg` diagnostics line (below) counts run-outs and stays 0.

**The two halves.** The FIFO is two halves of one push (32 samples, 4 ms) each. b31 clears exactly every 4.00 ms, as
the modem moves on to play the other half, and the next push sets it again at once (measured). Write the 64 bytes as
byte writes: with halfword or word writes the FIFO counts one entry per bus access and plays nothing usable
(measured).

A push in the first microseconds after b31 clears can land in the half being played. From then on every chunk plays
one chunk early or late, until a later push happens to line the halves up again:

- The audio jumps at every 4 ms boundary, a phase step of 360° × f × 4 ms (none at multiples of 250 Hz, the most near
  125 Hz plus multiples of 250 Hz).
- It sounds like a buzz of broadband clicks at 250 Hz under the tone, limited to the 4 kHz band.
- Nothing is lost, only reordered: the chunks go in at exactly 8 kHz and `un` stays 0.
- On a swept tone, broken stretches have the energy just above the tone's frequency 5-25 dB below the tone; whole
  ones 39-55 dB below it (measured, `test_speaker_sweep`).

Whether a push falls there depends on how a 2 ms poll lines up with the modem's 4 ms, so the fault comes and goes for
seconds at a time. So every push waits 200 µs after it sees b31 clear before it writes, twice the shortest wait that
plays in order (measured). Only a push that saw the bit clear writes, so the bit can only have cleared before that
look. Speaker PCM (`audio_pcm.c`) uses the deferred timer write above. APRS (`hrc_afsk.c`) uses the blocking wrapper
in `platform/hrc7000/prompt_fifo.h`; at 250 pushes a second that wait costs about 5 % of the CPU while AFSK is active.

**Keep b7 clear during any transmission.** The prompt path also feeds the TX modulator: with AUDIO_CONTROL b7 set
during a DMR transmission, the beep's audio rides on the 4FSK. A talk permit tone that overlaps the second and third
voice LC headers leaves them with no decodable sync, and a receiver takes the call by late entry. With b7 held clear
while transmitting, all three headers decode (measured).

`audio_pcm.c` keeps it clear for every board on the platform with one gate:

- The DMR glue closes the gate as the engine starts to transmit (`tx_begin`, and the hotspot's `bs_key`). While it is
  closed, b7 stays clear and nothing is pushed to the FIFO.
- It opens at least 50 ms later, once `hrc_dmr_tx_active()` is false. The audio task checks it (at each PCM block, or
  every 10 ms while gated), as do each speaker route change and each `hrc_audio_pcm_ring()` call. The audio task only
  exists once a stream has run, and APRS sets b7 without one.
- On both edges the speaker queue is emptied, including a block the audio task was filling at that moment. So a beep
  queued during the transmission is dropped, not played late.
- Boards set b7 only through `hrc_audio_pcm_ring()`, which applies the gate.

The last of the DMR glue's diagnostics lines checks the gate: `pg g<closings> l<closings that found speaker audio still
pending> ra<checks that found b7 set while closed> un<speaker queue run-outs>`. `l`, `ra` and `un` stay 0. Its
counters reset with the DMR counters.

So the talk permit can't sound during DMR TX, and the boards set `CAP_BEEP_NOT_ON_AIR`: the radio core plays it in a
short receive-only hold before the first burst ([radio-features.md](radio-features.md) 4). Through a hotspot parrot
the beep is heard before each call and the echoed calls carry no trace of it, with `l0 ra0`. With the permit played
before keying, each over starts with three voice LC headers that decode, and every burst is within 0.2 dB of the
over's level (measured; `test_dmr_tx_permit_before_key`).

The gate doesn't flush the FIFO itself. Filling it with 0x7F bytes would; the permit hold leaves only silence in it at
key-up.

**APRS transmit.** APRS uses the same path on purpose (`platform/hrc7000/hrc_afsk.c`). During an FM transmission, with
b7 set and the microphone muted by the board, the FIFO's samples are the modulation. A timer tops the FIFO up every
2 ms. Speaker PCM is held off the FIFO meanwhile (`hrc_audio_pcm_hold`), since its chunks would land between the
tones. Bell 202 from this path decodes off air with a valid FCS (measured, [aprs.md](aprs.md) 13). The DTMF tone
generator (8.3) needs no PCM, but its deviation field tops out near 1.5 kHz, short of the 3 kHz APRS wants.

**What the datasheet says.** The datasheet doesn't document this route. It names two FM transmit audio sources,
selected by AUDIO_CONTROL b0 (datasheet 7.2.2.1): the codec ADC (0) or the TX RAM (1). As a source, the TX RAM is
0x16000030-0x1600042F, two 512-byte halves of 256 samples (32 ms at 8 kHz), and FM_ADDR_SW (0x56C) names the half in
use (1 low, 2 high); SYS_INTERP b15 is the FM TX interrupt. The datasheet documents the prompt FIFO only as playback to
the codec DAC (datasheet 7.3). Other datasheet facts:

- LINEOUT_CTRL (0x088) b1 is line out 1 and b0 line out 2, the same PWM waveform on both (RadioFoundry sets 0x01).
- Two-point modulation: SIG_CENTER b31:24 biases MOD1 (the VC-TCXO) and b23:16 MOD2 (the VCO), 3.3 mV a step.
  RF_MOD_BIAS b7:0 and b15:8 are the MOD1 and MOD2 amplitudes. RF_MODE b10 sends a 40 Hz test square.

The prompt FIFO path reaches the modulator on some boards but not on others, which take the mixer route below.

**The FM transmit limiter.** FM_DEV_COEF b23:16 caps the deviation of audio that enters through the codec ADC: the
microphone and the mixer route below. It doesn't cap the prompt FIFO's ring play: AFSK through ring play reaches
5.1 kHz peak with the limiter at 4. Through the ADC path the ceiling scales with limiter × TX coefficient (measured,
25 kHz, Bell 202 tones):

| Limiter | TX coefficient | Peak deviation |
|---|---|---|
| 4 | 0x20 | 1.65 kHz (clipped at every level tried) |
| 4 | 0x30 | 2.48 kHz (clipped) |
| 8 | 0x20 | 3.18 kHz (clipped) |
| 0x0F | 0x40 | 4.18 kHz |
| 0x0F | 0x20 | 4.57 kHz, max 4.84 kHz (not clipped) |

So a limiter of 4 holds speech to about 1.65 kHz at 25 kHz. A board sets `fm_tx_limit` for each bandwidth.

**The mixer route.** Where ring play doesn't reach the modulator on its own, the codec's digital mixer (datasheet
6.5.2.10-11) carries the DAC's audio into the ADC's record path, which is the FM transmit input:

- CR_MIX (codec 0x10: b7 mix_en, b6 load, b5:0 the MIX_n address) and DR_MIX (0x11) reach MIX_0..3.
- MIX_2 b0 (mix_rec) makes the ADC's output ADC + DAC, and MIX_3 b7:4 = 0xF zeroes the ADC's own input in that sum.
- `hrc7000_codec_dac_to_tx` sets MIX_3 and then MIX_2, clears them in the reverse order, and ends with CR_MIX = 0.
- The codec mic mute (CR_ADC b7) must stay off on this route: it mutes the mix too.

Bell 202 through this route decodes off air with a valid FCS (measured, on a board where ring play alone sends a bare
carrier).

### 8.5 FM receive audio to the CPU

In FM (WORK_MODE 0x80) the modem copies its demodulated receive audio into the RX RAM as well as to the codec. The
datasheet calls this "analog receive recording", and it is on by default in FM.

| Item | Value |
|---|---|
| Buffer | RX RAM 0x160004E0..0x160008DF: two halves of 512 bytes (256 samples), used in turn |
| Sample format | 16-bit, high byte at the lower address |
| Rate | 8 kHz: the halves alternate every 32.0 ms (measured) |
| Half to read | FM_ADDR_SW (0x56C) b1:0: 1 = the first half, 2 = the second |
| Interrupt | SYS_INTERP bit 14 (`fm_rx_interp`) as each half fills; its mask bit (SYS_INTERP_MASK b14) is off in the power-on mask |
| Input | the modem's AF input. On a board with a separate FM RF chip, that is the RF chip's audio output, routed by a board GPIO |

The audio isn't squelched: it carries noise while no signal is present, so a demodulator can work below the squelch.
Receiver noise reaches full scale (peak 32767) with the reset AF gain (measured).

RadioFoundry reads it in the system interrupt (`platform/hrc7000/fm_rx_audio.c`). The DMR glue owns that interrupt and
calls an FM hook for bit 14, in FM too. The hook copies each half into a 128 ms ring for the APRS demodulator.

---

## 9. Driver requirements

A working DMR driver does these things that a first attempt tends to miss. The sections named have the details.

| # | Area | Requirement |
|---|---|---|
| 1 | Vectors | hook the TX timeslot (62) and RF TX (64) vectors as well as 61 and 63; TX states run on 62, and 64 keys the RF chip (3) |
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
| 18 | Lost requests | clear every pending modem request at each receiver restart; drop a system request pending with no handler run (3.3) |

---

## 10. Air interface provider

`drivers/modem/hrc7000/hrc7000_air.c` makes the modem a `hal_air_t` provider (`hal/hal_air.h`) and a receive tap
(`hal/hal_baseband.h`) under `CONFIG_AIR`. The glue (`platform/hrc7000/dmr_glue.c`) exports them as `hrc_air` and
`hrc_baseband` for a board's `board.air` and `board.baseband`. The provider receives through the soft modem on the FM
receive copy and transmits through the physical layer.

The fit covers continuous profiles at 4800 symbols/s (2400 as `AIR_FIT_RATE_DOUBLED`), 2 or 4 levels. Every fit
carries `AIR_FIT_UNTESTED` unless the board sets `hrc_dmr_board_t.air_verified` (10.4).

| Item | How |
|---|---|
| Ownership | A tap or a session opens only in DMR with no DMR transmission and no FM receive hook (APRS) running. The glue halts the slot engine, and the session lives in the engine's event and TX queues (`_Static_assert`): no RAM of its own. The interrupts go to the session; the modem task polls it instead of the engine. At close the queues are emptied and the engine runs again |
| Interrupts | System (bit 14 copy half, bit 12 PHY_RDY), both timeslot vectors, RF TX. Their handlers run from RAM (9 #15) |
| Mode changes | Leaving DMR under a session ends any transmission (PA off, `AIR_EV_TX_DONE`) and stops receive; the next DMR entry sets the copy up again |
| Events | From the interrupts through an 8-entry queue to the modem task, which calls the session's callback. TX_SPACE events in a row merge into one; the last entry is kept for TX_DONE |

### 10.1 Receive: the FM receive copy

The receive side is the copy of 8.5 with the RF chip left in its DMR set-up and the band-pass off, so the copy carries
the discriminator's output. It carries 4FSK well enough to decode every voice frame of a call.

| Step | Write |
|---|---|
| Save | SYS_INTERP_MASK, FM_BANDWIDTH, FM_DEV_COEF |
| Copy set-up | TXRX 0, SLOTON 0, LAYER2_CONTROL 0xC0, FM_PTT 0, AUDIO_CONTROL FM playback (b6) off, FM_BANDWIDTH 0x00 (band-pass off, 12.5 kHz), FM_DEV_COEF 0x40A25, WORK_MODE 0x80, SYS_INTERP_CLEAR and MASK = bit 14 + bit 12 |
| Each half | Bit 14: the half FM_ADDR_SW names goes into a 512-sample ring (two halves), stamped with the interrupt's time less 255 samples. A half that finds the ring full is dropped, and the next one is flagged as a gap |
| Session | The modem task feeds each half to the soft modem (`core/air/air_softmodem.h`), resetting it at a flagged half; its chunks go out as `AIR_EV_RX` |
| Undo | Mask, band and coefficient back; TXRX 0, WORK_MODE 0x80 then 0x6E, LAYER2_CONTROL 0xC0, TXRX 0xC00000 then 0. A mode change before the undo leaves the band and coefficient to the next FM entry and puts back only the mask (only power-on writes it otherwise) |

FM playback off keeps the copy off the speaker (`hal_air.h` rule 1); in DMR the speaker carries the codec's PCM.
`hrc_baseband` reports 8000 Hz, one channel, 0-3000 Hz, and counts per kHz unknown.

### 10.2 Transmit: the physical layer

The transmit side is the timeslot mode of 2.10 with every slot sent, so the carrier runs from the first slot to the
last without a ramp between slots. One 36-byte TX RAM half is one 144-symbol chunk (30 ms at 4800 symbols/s).

1. `tx_begin` (at least one chunk queued, the core has called the board's TX on): save RF_MOD_BIAS and write it scaled
   (10.3); TXRX 0, SLOTON 0, WORK_MODE 0x80 then 0x22; both halves idle (the profile's idle byte); LAYER2_CONTROL 0x82
   (own TX timing); AUDIO_BUFFER_CLR 3. The copy stops for the transmission.
2. The first RF TX interrupt keys the RF (the board's `rf` and `pa_arm` ops), once, held to the end. Until then, and for
   `HRC7000_AIR_TX_LEAD_SLOTS` (2) boundaries after it, each boundary sends an idle slot and the queue waits (no
   underrun counted). The carrier is up about 45 ms before it carries modulation, and a chunk sent in that time is
   lost.
3. Each slot boundary (the first of the timeslot or PHY_RDY interrupts in a slot; one more within 15 ms is the same
   slot) writes only the half the next slot reads, alternating, then TXRX 0x80. A half takes the oldest queued chunk,
   idle after a short last chunk. With none queued it takes idle, and the driver raises one `AIR_EV_TX_UNDERRUN`.
4. The half's last byte (symbols 140-143) is written at the next boundary, first thing. The chip reads it after the
   boundary that rewrites its half, so written with the rest it goes out with the chunk two slots later.
5. The slot set up at a boundary is on air about 5 ms later: `AIR_EV_TX_STARTED` carries that time plus half a symbol.
6. After the `AIR_TX_LAST` chunk (or `tx_end` with nothing queued) one idle slot, then at the next boundary: TXRX 0, RF
   off, RF_MOD_BIAS back, the copy set up again (or WORK_MODE 0x80 without a receiver), `AIR_EV_TX_DONE`.
7. Watchdog (modem task): no slot boundary for 200 ms, or no RF TX interrupt 200 ms after `tx_begin`, ends the
   transmission as `tx_abort` does.

Writing both halves at each boundary corrupts the end of the slot on air; writing only the next one gives no errors
(measured). WORK_MODE 0x02 (continuous) never keys.

The last-byte rule and the idle lead matter. A P25 call sent with the last byte written with the rest and no idle lead
loses its header, and 50-75 % of the symbols at positions 140-142 of a slot are wrong: they match the chunk written into
the same half two slots later. With the last byte one boundary late and two idle slots, every unit decodes, the symbol
error rate is 0.34 %, and no position in the slot is above 10 % (measured from SDR I/Q against the symbols the radio
queued).

### 10.3 Deviation

The board writes the modulation bytes of RF_MOD_BIAS (b15:8, b7:0) for DMR at its TX on. The provider scales them by
dev_outer_hz / 1820 and clamps them to 1..255; b31:16 stay. 1820 Hz is the outer deviation the calibrated DMR bytes
give, not DMR's nominal 1944 Hz: scaled by 1800 / 1944 they give 1685 Hz (93.6 % of P25's 1800 Hz), which leaves an
MMDVM hotspot little room for its own offset (measured from SDR I/Q). Deviation is linear in the bytes, about 25 Hz per
step through zero. 0x118 and FM_DEV_COEF don't move it (measured).

Outside 100-6000 Hz the fit says `AIR_FIT_DEV_APPROX`. The pulse filter is the chip's own (raised cosine for DMR), so
every shape but RRC 0.2 is `AIR_FIT_SHAPE_APPROX`.

### 10.4 Air verification

A board sets `hrc_dmr_board_t.air_verified` once its receive and transmit through the provider have been checked on
air; until then its fits carry `AIR_FIT_UNTESTED`. The [feature matrix](../feature-matrix.md) lists which radios have
P25 and NXDN.

The provider carries these modes on an HR_C7000 radio: P25 conventional and trunked receive and transmit, and NXDN
receive and transmit at both rates. A trunked radio transmits on its grant after receiving the control channel, so the
copy set-up doesn't keep the RF TX interrupt away.
