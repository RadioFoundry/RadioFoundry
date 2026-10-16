<!-- SPDX-License-Identifier: MIT -->
# HR_C7000 as a DMR modem

Status: research, 2026-10-09. How the baseband inside the HR_C7000 SoC (DM-32UV) is driven as a DMR Tier II
layer-2 modem and as an FM modem: registers, buffers, interrupts, what to write when, and what the chip does by
itself. Board-level facts (pins, RF chip, PA timer, calibration source) are in `hardware-dm32.md`; the stack-side
contract is in `dmr-modem.md`; the HR_C6000 equivalent is in `modem-hrc6000.md`.

**Nothing here has been verified on a radio by us.** The sequences marked BIN are what other firmware builds
(July–September 2026) do, and those builds work on air (simplex voice both ways, repeater wake from the August build).

| Tag | Meaning |
|---|---|
| **DS** | HR_C7000 chip user guide v2.7 (2016, Chinese with English translation), register chapters and application appendices |
| **BIN** | Observed on the OpenDM32 Jul/Aug/Sep 2026 builds (Aug and Sep have identical modem access sequences), or the DM-1801 Pro stock image where the text says so |
| **KP** | another firmware's source (DMR did not work in it); unverified |
| **BENCH** | Measured on a radio on our bench (the text names it) |
| **INF** | Our inference |

---

## 1. The modem's view of the system

| Item | Value | Tag |
|---|---|---|
| Register block | 0x11000000, 32-bit registers (names below are the user guide's) | DS |
| TX RAM (layer-2 send RAM) | 0x16000000, 1200 bytes, byte wide | DS |
| RX RAM (layer-2 receive RAM) | 0x160004B0, 1200 bytes | DS |
| AMBE voice RAM | 0x16000960–0x1600097A, 27 bytes: one burst's three 72-bit AMBE+2 frames, both TX and RX | DS, BIN |
| Prompt (beep) FIFO | 0x16000980 | DS, KP |
| Codec registers | 0x160009C0 (byte wide) | DS |
| PCM buffers (system SRAM) | mic PCM 0x18000000–0x1800009F (hardware writes), speaker PCM 0x180000A0–0x1800013F (CPU writes) | DS |
| Vocoder | none in hardware: the CPU runs the AMBE+2 codec on the PCM buffers and moves 27-byte bursts through the voice RAM (`codec-dm32.md`) | DS, BIN |
| Symbol / sample clocks | 4800 baud 4FSK; 30 ms timeslots; internal 38.4 kHz sampling (AK2401 note) | DS |

The modem runs DMR at layer 2: the CPU builds every burst's information bits (LC, CSBK, data blocks, AMBE) and the
chip adds the FEC (BPTC, Golay slot type, RS(12,9), QR EMB, CRC with masks), sync, CACH handling and timeslot
timing. Layer 3 (the chip's own call control, SMS, BS activation) exists but is not used by any working firmware.

---

## 2. Register map used for DMR

Offsets from 0x11000000. Values are those used by OpenDM32 (BIN) unless noted.

### 2.1 Mode and RF interface

| Offset | Register | Fields (DS) | Value used | Why |
|---|---|---|---|---|
| 0x100 | WORK_MODE | b8 digital_analog_en; b7 modulator_mode (0 DMR, 1 FM); b6 Tier II; b5 timeslot mode; b4:3 layer (0 PHY, 1 L2, 2 L3); b2 is_repeater; b1 is_aligned; b0 work_slot_sel | **0x80** FM, **0x6E** DMR (Tier II, timeslot, layer 2, repeater bit, aligned, slot 1) | Only these two values are ever written (BIN). Timeslot choice is done in software with LAYER2_TXRX_CTRL, not with work_slot_sel |
| 0x104 | RF_MODE | b25 phase_avg_en; b24 af_mode; b23:16 AF gain (n/16); b15 RX IQ invert; b14:13 RX mode; b12 dynamic sync threshold; b11 dual receive channels; b9 soft ramp; b7 TX IQ invert / deviation mapping; b6:5 TX mode (3 = two-point) | **0x03269860**: AF receive on with averaging, gain 0x26 (2.375), RX IQ inverted, dynamic sync threshold, dual RX channels, two-point modulation | Written once at init and left in both modes. The Dec 2025 code switched to 0x03269060 (no dual RX) in DMR; OpenDM32 does not |
| 0x108 | SIG_CENTER | b31:24 TX I bias, b23:16 TX Q bias, b15:8 / b7:0 RX I/Q bias | (cal << 24) or (cal << 16) | TX center from factory calibration (`hardware-dm32.md` 11) |
| 0x10C | TX_IF_FREQ | b23:0 | 0x0BB800 (reset value) | |
| 0x110 | RF_CONTROL | b20:16 tx_pre_on (100 us steps); b13:8 rf_pre_on_tx (RF TX interrupt advance, 100 us); b5:0 rf_pre_on_rx | **0x00041F1A**: tx_pre_on 0.4 ms, RF TX interrupt **3.1 ms** before the TX burst, RF RX interrupt 2.6 ms before RX | The July build used 0x00040A0A (1.0 ms) and had bad timeslots at low power |
| 0x114 | RF_MOD_BIAS_CTRL | b25:16 two_point_bias; b15:8 sig_reduce (Mod2 amplitude); b7:0 phase_reduce (Mod1 amplitude) | ((cal + 384) << 16) or modcal (0x4D4D before a frequency is set) | Modulation balance from calibration |
| 0x118 | DEV_LIMITER | b15:0 | 0xFF00 (reset value) | KP |
| 0x11C | RSSI_BIAS_VALUE | read-only per DS (rssi b31:16, ADC I/Q DC bias) | port writes 0x1381 (no effect expected) | KP |
| 0x128 | PHASE_OUT | read-only: b29:16 / b13:0 the largest and smallest 14-bit AF phase in AF receive mode (DS) | not written | A live check that AF reaches the ADC: a DM-1801 Pro receiving DMR reads about +1216 / -1149 (the guide expects about ±1300 at -47 dBm); a value that stays within a few counts means no AF arrives. On the DM-32UV that was GPIO 0x2F left high (`hardware-dm32.md` 5), which takes the FD6818's audio off the ADC (HW) |
| 0x120 | THRESHOLD_VALUE | b28:24 sync ratio; b23:16 sync low threshold; b15:8 sync high threshold; b7:0 signal detect | **0x0978786F** | |
| 0x164 | RECV_INFO_COUNT | b31:24 sync errors; b8 select FSK error/EVM; b7:0 FSK error | read for BER/EVM only | DS |
| 0x168 | (undocumented, "REG_X06") | | 0x14 | BIN |
| 0x1B0 | RX_IF_FREQ | b23:0 | 0x0BB800 | |
| 0x1B4 | RF_RECV_DELAY | b18:0, 100 ns steps | **0x36B0** (1.4 ms). DS says 2000 (0.2 ms) for AF mode; the working value is 0x36B0 | BIN |

### 2.2 Sync words

24-bit halves of the 48-bit ETSI sync patterns, high half first (BIN, all written once at init):

| Offset | Register | Value | ETSI pattern |
|---|---|---|---|
| 0x12C / 0x130 | SEND_DATA_SYNC_H / L | 0xD5D7F7 / 0x7FD757 | MS sourced data |
| 0x134 / 0x138 | SEND_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |
| 0x13C / 0x140 | RECV_MS_SYNC_H / L | 0xD5D7F7 / 0x7FD757, then set bit 31 of H | MS data (receive) |
| 0x144 / 0x148 | RECV_BS_SYNC_H / L | 0xDFF57D / 0x75DF5D, then set bit 31 of H | BS data (receive) |
| 0x14C / 0x150 | RECV_TDMA1_SYNC_H / L | 0xF7FDD5 / 0xDDFD55 | TDMA direct slot 1 data |
| 0x154 / 0x158 | RECV_TDMA2_SYNC_H / L | 0xD7557F / 0x5FF7F5 | TDMA direct slot 2 data |
| 0x15C / 0x160 | RECV_RC_SYNC_H / L | 0x77D55F / 0x7DFD77 | RC |

Voice syncs are not programmed; the chip derives them (INF: they are the bitwise complements of the data syncs).
Bit 31 of the RECV_MS / RECV_BS high halves is an undocumented enable (BIN).

### 2.3 CRC, RS and color code

| Offset | Register | Value | Meaning (DS) |
|---|---|---|---|
| 0x350 | PI_CRC_MASK | 0x6969 | PI header CRC-16 mask (init value 0 in b31:16) |
| 0x354 / 0x358 | RS_H_MASK / RS_H_INIT | 0x969696 / 0 | RS(12,9) mask for the voice LC header |
| 0x35C / 0x360 | RS_T_MASK / RS_T_INIT | 0x999999 / 0 | RS(12,9) mask for terminators (the DS reset value 0x969696 is wrong for ETSI; write 0x999999) |
| 0x364 | CSBK_CRC_MASK | 0xA5A5 | CSBK CRC-16 mask |
| 0x368 | MBCH_CRC_MASK | 0xAAAA | MBC header CRC-16 mask (DS reset 0xA5A5 is wrong for ETSI) |
| 0x36C | DATAH_CRC_MASK | 0xCCCC | data header CRC-16 mask (DS reset 0xA5A5 is wrong for ETSI) |
| 0x370–0x37C | (undocumented) | 0xF0, 0x1FF, 0x10F, 0x00 | BIN |
| 0x380 | CRC8_INIT | 0 | short LC CRC-8 init |
| 0x384 | LOCAL_CC | color code in b3:0; b9:8 cc_opt = 0 (deframe only on matching CC); b16 encryption type | Set per channel; when cc_opt = 1 RF_MODE b12 must be 0 (DS) |
| 0x388 | VOICE_EMB_CTRL | 0 at init, **2** when entering DMR receive | b1 = burst F EMB from the chip's own RC encoder; b0 = PI bit for bursts B–E |
| 0x38C | SCRAMBLE_REG | 0 | built-in simple voice scrambler keys (RX in b31:16, TX in b15:0); unused |

### 2.4 Interrupt control

| Offset | Register | Value / use |
|---|---|---|
| 0x398 | SYS_INTERP_LIST | system interrupt causes (read) |
| 0x39C | SYS_INTERP_MASK | **1 = enabled** (inverted sense). **0x1187F**: bits 0–6, 11, 12, 16 |
| 0x3A0 | SYS_INTERP_CLEAR | write the bits read from the list; repeat until the list reads 0 |
| 0x3A4 / 0x3A8 / 0x3AC | LAYER3_INTERP_LIST / MASK / CLEAR | layer-3 causes; unused (OpenDM32 only peeks at LIST bit 28 in one place) |
| 0x3B0 | INTERP_CLEAR | write-1-to-clear: b0 TX timeslot, b1 RX timeslot, b2 RF TX, b3 RF RX, b4 PCM write (mic buffer full), b5 PCM read (speaker buffer consumed) |

SYS_INTERP_LIST bits (DS):

| Bit | Name | Meaning in DMR layer 2 |
|---|---|---|
| 0 | dll_rdy_interp_tx | layer-2 TX processing point (ahead of a TX slot) |
| 1 | dll_rdy_interp_rx | layer-2 RX processing point |
| 2 | dll_latelc_rx_interp | **voice late entry**: embedded LC assembled from a superframe (the Dec 2025 code treated it as "abnormal exit") |
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

| Offset | Register | Fields (DS) |
|---|---|---|
| 0x400 | LAYER2_CONTROL | b7 txen, b6 rxen, b5 layer2slot, b4 txtmp_master_mode, b3 rx_master_auto_update, b2 tx_slave_auto_mode, b1 tx_master_mode (active TX timing), b0 rx_master_mode |
| 0x404 | LAYER2_SLOTON | write: b0 dll_tx_slot_on, b2 dll_rx_slot_on (establish TX / RX slot in active mode), plus self-clearing open/close strobes b5–b13; read: b31 TX slot on, b29 RX slot on |
| 0x408 | LAYER2_TXRX_CTRL | b31 tx_complete_cpu (continuous mode only), b23 syncfail_layer2_rx (drop and re-acquire RX slot sync), b22 syncfail_layer2_tx, b15 begin_v_layer2 (voice access success), b7 txnextsloten (transmit in the next slot), b6 rxnextsloten (receive in the next slot), b0 autotest |
| 0x40C | LAYER2_SLOT_CNT | slot counter presets (unused) |
| 0x410 | LAYER2_MAX_CNT | slot fine tune ±n × 100 us (unused) |
| 0x414 | LAYER2_SLOT_UPDATE | slot re-sync strobes (unused) |
| 0x418 | LAYER2_SEND_TYPE | b31:22 codec type for custom data types 11–15; b12 RC in data sync; b11 data EMB RC source; b10 / b9 BS forwarding; b8 crc_calc_sel (0 = chip computes CRCs); **b7:4 data type / voice burst index; b3 1 = voice (or RC) burst, 0 = data/control burst; b2 second data header; b1:0 LCSS** |
| 0x41C | LAYER2_STATUS | b31 TX slot number, b30 RX slot number, b17:9 RX slot bit count, b8:0 TX slot bit count |
| 0x424 | LAYER2_SLOT_PRE | b23:16 0x55 (do not change), b15:8 RX processing-interrupt advance, b7:0 TX advance. Value **0x551010** |
| 0x428–0x438 | LAYER2_BS_MODE, BS_SECOND_REG, BS_STATUS, BS_CACH | repeater / simulcast modes; never written by working firmware |

Values written to LAYER2_CONTROL: **0xC0** = TX and RX enabled, passive timing (receive, follow the other side's
slots); **0xC3** = TX and RX enabled, active TX and RX timing (we set the slot clock). Values written to
LAYER2_TXRX_CTRL: 0x00 nothing next slot; 0x40 receive next slot; 0x80 transmit next slot; 0x8040 receive next slot
with the voice-access flag; 0x800000 release RX slot sync.

### 2.6 LAYER2_SEND_TYPE codes

| Code | Burst | Source of payload in TX RAM | Tag |
|---|---|---|---|
| 0x10 | Voice LC header (data type 1) | LC bytes 0–8 at 0x16000000; bytes 9–11 may be left 0 (chip adds RS(12,9) with RS_H_MASK) | DS, BIN |
| 0x20 | Terminator with LC (data type 2) | LC bytes 0–8 | DS, BIN |
| 0x30 | CSBK (data type 3) | 10 bytes; chip adds CRC with CSBK_CRC_MASK | DS, BIN |
| 0x40 | MBC header (4) | 10 bytes, chip CRC | DS |
| 0x50 | MBC continuation (5) | 12 bytes | DS |
| 0x60 | Data header (6) | 10 bytes, chip CRC with DATAH_CRC_MASK | DS, BIN |
| 0x70 | Rate ½ data block (7) | 12 bytes (CRC-9 / CRC-32 by the CPU) | DS, BIN |
| 0x80 | Rate ¾ data block (8) | 18 bytes | DS, BIN |
| 0xA0 | Rate 1 data block (10) | 24 bytes | DS, BIN |
| 0x08 + (n << 4) + LCSS | Voice burst n = 0..5 (A..F) | AMBE 27 bytes to the voice RAM 0x16000960; for burst A also the call's LC (bytes 0–8) at 0x16000000 for the embedded LC | DS, BIN |

User guide example for one superframe: A = 0x08, B = 0x19, C = 0x2B, D = 0x3B, E = 0x4A, F = 0x50, i.e. LCSS 0 on A,
1 (first fragment) on B, 3 (continuation) on C and D, 2 (last) on E, 0 on F (DS). **OpenDM32 and the port write
LCSS = 0 on every voice burst** (0x08, 0x18, 0x28, 0x38, 0x48, 0x58; BIN). Whether the chip then still sends the
embedded LC fragments correctly is unverified; follow the user guide.

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

"CRC good" for a deframed burst = bits 30:24 all zero (BIN). Data blocks have no CRC-16, so do not drop them on these
flags; check CRC-9/CRC-32 in software. RX_INFO (0x394): b24:16 trunking multiframe number, b10:0 received RC.

### 2.8 Buffer layouts

**TX RAM (0x16000000)**, DS table 52: 0x00–0x0B the current control burst (PI header 10 + 2 CRC, LC header / terminator
9 + 3 RS, CSBK / MBC header / data header 10 + 2 CRC, MBC block 12, rate ½ 12, rate ¾ 18, rate 1 24); 0x00–0x09 also
embedded LC (72 bits + 5-bit checksum in byte 9, "prepared when voice burst A is about to be sent"); 0x29–0x2C burst F
EMB filler (or key ID / algorithm ID for privacy); 0x30–0x4A voice burst when voice comes from TX RAM (layer-2 BS
mode, AUDIO_CONTROL b0 = 1); 0x4B–0x50 embedded RC; 0x495–0x4AF send voice key (27 bytes).

**RX RAM (0x160004B0)**, DS table 53: the deframed control burst at 0x00 (LC header / terminator 9 + 3, CSBK / headers
10 + 2, data blocks 12 / 18 / 24, idle 12, C_RC 7 + 2). Embedded LC after late entry also appears at 0x00–0x08; the
chip keeps the previous call's LC there until a new one is decoded (BIN observation from hotspot work).

**Voice RAM (0x16000960)**: write 27 bytes before arming a voice burst; read 27 bytes after a voice deframe interrupt
(DS, BIN). Burst numbering for privacy keystream purposes is RX_TYPE_INFO's 1–6 (DS).

---

## 3. Interrupts

| Vector (PIC source) | Name | Fires | What it means / what to do | Clear | Tag |
|---|---|---|---|---|---|
| 61 (29) | System_inter | after the chip deframes a burst (bit 3), on late entry (bit 2), slot abnormalities, FM signaling | read SYS_INTERP_LIST, act, write it back to SYS_INTERP_CLEAR until 0 | SYS_INTERP_CLEAR | DS, BIN |
| 62 (30) | Time_slot_inter_tx | every 30 ms on the TX slot clock | **decide the next slot**: write TX RAM / voice RAM, SEND_TYPE, then TXRX_CTRL (0x80 = send in the coming slot, 0x00 / 0x40 otherwise). Start the PA one-shot whenever 0x80 is written | INTERP_CLEAR b0 and b1 | DS, BIN |
| 63 (31) | Time_slot_inter_rx | every 30 ms on the RX slot clock | same decision while receiving (and for the last repeater-wake wait state) | INTERP_CLEAR b0 and b1 | DS, BIN |
| 64 (32) | Rf_tx_inter | rf_pre_on_tx × 100 us (**3.1 ms** with RF_CONTROL 0x41F1A) before each TX burst | put the RF chip into TX (first burst: full TX set-up; later bursts: idle → TX); note the time | INTERP_CLEAR b2 (first thing) | DS, BIN |
| 65 (33) | Rf_rx_inter | rf_pre_on_rx before an RX slot | not used; TX is turned off from the next timeslot interrupt once the burst has run ≥ 20 ms | INTERP_CLEAR b3 | DS, BIN |
| 59 (27) | PCM_rd_data_interp | every 10 ms while AUDIO_CONTROL b5 is set | speaker buffer consumed: write the next 160 bytes of PCM to 0x180000A0 | INTERP_CLEAR b5 | DS, KP |
| 60 (28) | PCM_wr_data_interp | every 10 ms while AUDIO_CONTROL b4 is set | 160 bytes of mic PCM ready at 0x18000000 | INTERP_CLEAR b4 | DS |

Both timeslot interrupts were handled by one routine in the working builds; it decides by the current activity
(transmitting states on the TX vector, receiving states on the RX vector) (BIN). The Dec 2025 code never hooked
vectors 62 and 64, so DMR TX never started (BIN).

### 3.1 TX timing within a burst (INF from the values, consistent with BIN behavior)

| t (ms) relative to the TX timeslot interrupt in which the burst is armed | Event |
|---|---|
| 0 | TX timeslot interrupt: payload and SEND_TYPE written, TXRX_CTRL = 0x80, PA one-shot started |
| ≈ 26.9 | RF TX interrupt (3.1 ms ahead): RF chip idle → TX |
| 28 (default "DMR PA" setting, 1–35 ms) | PA one-shot: band PA select on, PA drive DAC to the calibrated value |
| ≈ 29.6 | tx_pre_on (0.4 ms) |
| 30 | burst on air (27.5 ms of symbols plus guard) |
| next timeslot interrupt with ≥ 20 ms of TX elapsed | PA drive 0, PA selects off, RF chip back to RX |

### 3.2 Masking the modem interrupts

PIC_MASK bits 29, 30, 31 and PIC_MASK_1 bit 0 (vectors 61–64) (DS mapping). Mask them around multi-register updates
from task level.

---

## 4. Initialization (power-on, once)

Ordered; (register, value, why). OpenDM32 writes the whole DMR block once here and not at every mode change (BIN).

| # | Register | Value | Why | Tag |
|---|---|---|---|---|
| 1 | ADC controller | reset and idle (`hardware-dm32.md` 3.7) | the baseband shares the analog front end | KP |
| 2 | IO_DIPLEX2 (0x03C) | clear bit 28 | DAC channel B pin to analog (PA drive) | KP |
| 3 | DAC_PD_CTRL / PD_MODE_EN / DATA_A / B / C | 0x01 / 0x01 / 0 / 0 / 0x06E2 | DAC B (PA drive) and C on, A off; PA drive 0 | KP |
| 4 | FM_BANDWIDTH (0x500) | 0x5F | FM wideband defaults | KP |
| 5 | FM_DEV_COEF (0x504) | 0x33500 | FM deviation defaults (overwritten per TX/RX) | KP |
| 6 | SYS_SOFT_RSTN (0x000) | clear bit 4, wait 2 us | codec reset pulse | KP |
| 7 | codec AICR_DAC, AICR_ADC, CR_MIC1, CR_MIC2, CR_DAC, CR_ADC, CR_VIC, CR_CK, GCR_DACL, GCR_MIC1, GCR_ADCL, CR_DAC_AGC, CR_ADC_AGC, DR_ADC_AGC | 0xC0, 0xC0, 0x00, 0x70, 0x20, 0x20, 0x0C, 0x40, 0x80, 0x00, 0x8B, 0, 0, 0x34 | audio codec on, mic input 2, gains (`hardware-dm32.md` 9.1) | KP |
| 8 | LINEOUT_CTRL (0x088) | 0x01 | speaker line out on | KP |
| 9 | SIG_CENTER | (cal << 24) or (cal << 16) | TX I/Q center | BIN |
| 10 | RF_MOD_BIAS_CTRL | ((cal + 384) << 16) or 0x4D4D | modulation balance until a TX frequency is known | BIN |
| 11 | RF_MODE | 0x03269860 | two-point TX, AF RX, dual RX channels | BIN |
| 12 | DEV_LIMITER | 0x0000FF00 | | KP |
| 13 | SYS_SOFT_RSTN | 0x190 | see the reset-bit caveat in `hardware-dm32.md` 16 | BIN, KP |
| 14 | DAC_CONTROL (0x070) | 0x8000001B | baseband DAC set-up (undocumented bits) | KP |
| 15 | ADC_CONTROL (0x074) | |= 0x41C3 | baseband ADC set-up | KP |
| 16 | AUDIO_CONTROL (0x080) | 0x02 | I2S slave (reset default), all audio paths off | KP, BIN |
| 17 | AUDIO_BUFFER_CLR (0x084) | 0x03 | clear vocoder encode and decode buffers | KP, BIN |
| 18 | sync words (2.2) | as listed | DMR syncs | BIN |
| 19 | THRESHOLD_VALUE | 0x0978786F | sync detection | BIN |
| 20 | PI_CRC_MASK, RS_H_MASK, RS_H_INIT, RS_T_MASK, RS_T_INIT, CSBK_CRC_MASK, MBCH_CRC_MASK, DATAH_CRC_MASK | 0x6969, 0x969696, 0, 0x999999, 0, 0xA5A5, 0xAAAA, 0xCCCC | ETSI masks; the chip then computes those CRCs/RS itself | BIN |
| 21 | 0x370, 0x374, 0x378, 0x37C | 0xF0, 0x1FF, 0x10F, 0 | undocumented | BIN |
| 22 | CRC8_INIT, VOICE_EMB_CTRL, SCRAMBLE_REG | 0, 0, 0 | | BIN |
| 23 | SYS_INTERP_MASK | 0x1187F | enable layer-2 TX/RX processing, late entry, deframe, short LC, slot abnormal, PHY, PHY slot abnormal | BIN |
| 24 | LAYER2_SLOT_PRE | 0x551010 | processing-interrupt advance | BIN |
| 25 | 0x168 | 0x14 | undocumented | BIN |
| 26 | RF_RECV_DELAY | 0x36B0 | 1.4 ms receive path delay | BIN |
| 27 | RX_IF_FREQ, TX_IF_FREQ | 0x0BB800 each | | BIN |
| 28 | RECV_MS_SYNC_H, RECV_BS_SYNC_H | set bit 31 | undocumented enable | BIN |
| 29 | RF_CONTROL | 0x00041F1A | RF TX interrupt 3.1 ms early | BIN |
| 30 | WORK_MODE | 0x80 | start in FM (idle) | BIN |
| 31 | FM_TONE1..4 | 0x05930629, 0x06D10787, 0x09AC0AB0, 0x0BD10D10 | DTMF tone steps (section 8) | KP, DS |
| 32 | GCR_ADCL | mic gain or 0x80 | DMR mic gain setting | BIN |

---

## 5. Entering DMR and starting receive

### 5.1 Enter DMR (from FM or idle; also at every DMR TX start)

| # | Register | Value | Why | Tag |
|---|---|---|---|---|
| 1 | RF_MOD_BIAS_CTRL | ((cal + 384) << 16) or modcal(f_TX) | balance for this TX frequency | BIN |
| 2 | GCR_ADCL | DMR mic gain or 0x80 | | BIN |
| 3 | DAC_PD_CTRL | clear bit 1 | **PA drive DAC back on** (FM receive powers it down; the Dec 2025 code never re-enabled it, so DMR after FM had no PA drive) | BIN |
| 4 | WORK_MODE | 0x80 | pass through FM: resets the DMR state machine | BIN |
| 5 | WORK_MODE | 0x6E | DMR, Tier II, timeslot, layer 2 | BIN |

AUDIO_CONTROL is not rewritten here; after FM it ends up 0x00 once FM audio is switched off (BIN).

### 5.2 Start / restart receive

Used at DMR entry, after TX, and wherever reception must be (re)acquired (BIN; identical in July and August):

| # | Register | Value | Why |
|---|---|---|---|
| 1 | (enter DMR, 5.1) | | |
| 2 | VOICE_EMB_CTRL | 0x02 | burst F EMB from the chip's encoder |
| 3 | LAYER2_CONTROL | 0xC0 | TX/RX enabled, passive timing |
| 4 | LAYER2_SLOTON | 0x00 | drop slot establishment |
| 5 | LAYER2_TXRX_CTRL | 0x800000 | release RX slot sync |
| 6 | LAYER2_TXRX_CTRL | 0x40 | receive in the next slot |
| 7 | LAYER2_SLOTON | 0x05 | dll_rx_slot_on and dll_tx_slot_on |

The Dec 2025 sequence (TXRX 0xC00000, 0, 0xC00000, 0x8040 and no SLOTON writes, plus a 3 ms busy wait in the color
code probe) does not work; replace it everywhere by this one (BIN).

### 5.3 While receiving

| Event | Action | Tag |
|---|---|---|
| System interrupt, bit 3 (deframe) while idle, good CRC, wanted CC | start following: TXRX_CTRL = 0x40 for the next slot (OpenDM32 uses 0x40 here; the Dec 2025 code used 0x8040) | BIN |
| Each TX/RX timeslot interrupt while following one slot | alternate: our slot → TXRX = 0x8040 (receive, voice-access flag) for the next; other slot → 0x00 | BIN |
| Voice deframe | read RX_TYPE_INFO, 27 bytes from the voice RAM; burst number from bits 14:12; LC header / terminator / CSBK / data from the RX RAM | DS, BIN |
| BS sync received (recv_sync_type 1) | the other side is a repeater: track the TC bit (RX_TYPE_INFO b22) to find which slot is ours | BIN |
| Color-code scan | set LOCAL_CC to the received CC, then LAYER2_CONTROL = 0xC0 and, in repeater mode, the 5.2 restart steps 4–7 (no busy wait) | BIN |
| Late entry (bit 2) | embedded LC is in the RX RAM; voice reception should start at the next burst A | DS |
| End | terminator (data type 2) or loss of bursts; TXRX_CTRL = 0 | BIN |
| Reception over (nothing taken for 390 ms) | the 5.2 restart: the chip drops the sender's slot timing and its timeslot interrupts stop | BENCH |
| Slot timing with nothing usable for 600 ms | the 5.2 restart, again every 600 ms while it lasts. Without it the chip kept a departed sender's timing, reported only unusable and abnormal slots and took no later call (DM-1801 Pro bench) | BENCH |

---

## 6. Transmit sequences

All transmit actions happen in the timeslot interrupts: the CPU arms one slot at a time with TXRX_CTRL = 0x80 and
leaves the other slot unused (0x00, or 0x40 to listen), giving one burst per 60 ms in its timeslot (DS figures 99–100,
BIN). Every arming also starts the PA one-shot (`hardware-dm32.md` 8.6). The RF TX interrupt that follows keys the RF
chip.

### 6.1 Start of transmission

| # | When | Register writes | Tag |
|---|---|---|---|
| 1 | task level, PTT | load the LC (9 bytes, then 3 zero bytes) into TX RAM 0x16000000; codec reset; LAYER2_CONTROL = **0xC3** (active timing, we own the slot clock); TXRX_CTRL = 0 | BIN |
| 2 | next timeslot interrupt | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (LC header) | BIN |
| 3 | next | TXRX_CTRL = 0 | BIN |
| 4 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (second header) | BIN |
| 5 | next | TXRX_CTRL = 0 | BIN |
| 6 | next | TXRX_CTRL = 0x80, SEND_TYPE = 0x10 (third header) | BIN |

The Dec 2025 code also set TXRX_CTRL bit 31 (tx_complete_cpu) at step 2; it belongs to continuous mode and must not
be set (BIN). For a direct-mode start after receiving on a repeater channel the same LAYER2_CONTROL = 0xC3 applies.

### 6.2 Voice

Alternating slots: in the inactive slot TXRX_CTRL = 0; in the active slot:

1. For burst A (n = 0): rewrite the call LC (or talker alias LC) into TX RAM bytes 0–8 for the embedded LC.
2. Write 27 bytes of AMBE (three 9-byte frames) to the voice RAM 0x16000960, or the AMBE silence pattern when no
   encoded audio is ready.
3. TXRX_CTRL = 0x80, SEND_TYPE = 0x08 + (n << 4) (+ LCSS, see 2.6); n = n + 1 modulo 6.

Encoded audio must be produced continuously during TX: the working builds encode one 20 ms block per tick whenever
two PCM buffers are waiting (the Dec 2025 code had the encode call commented out) (BIN).

### 6.3 Terminator and back to receive

| # | Slot | Writes | Tag |
|---|---|---|---|
| 1 | active | voice RAM = silence; TX RAM = LC; TXRX_CTRL = 0x80; SEND_TYPE = 0x20 | BIN |
| 2 | next | direct mode: done → idle. Repeater mode: LAYER2_CONTROL = 0xC0, TXRX_CTRL = 0 | BIN |
| 3 | next (repeater mode) | TXRX_CTRL = **0x40** (receive next slot; Dec 2025 used 0x8040) → receiving | BIN |

### 6.4 Data and CSBK

Per burst, in an active slot: write the payload to TX RAM (10 bytes for CSBK / MBC header / data header: the chip
appends the CRC-16 with the programmed mask because SEND_TYPE b8 = 0; 12 / 18 / 24 bytes for rate ½ / ¾ / 1 blocks,
CRC-9 and CRC-32 computed by the CPU), SEND_TYPE = data type << 4, TXRX_CTRL = 0x80, start the PA one-shot. Inactive
slot: TXRX_CTRL = 0 (or 0x40 to listen for a response). Sequence for a packet: data header (0x60), blocks
(0x70/0x80/0xA0), optional terminator. CSBK signaling: optional preamble CSBKs then the function CSBK, all 0x30
(DS appendices B, C; BIN for the CRC behavior). Open RX afterwards to catch the response.

### 6.5 BS activation (repeater wake)

No layer-3 BS activation: LAYER2_BS_MODE, BS_* registers and WORK_MODE values 0x73/0x76/0x77 are never used (BIN).

| # | When | Writes | Tag |
|---|---|---|---|
| 1 | PTT on a repeater channel with no repeater heard | codec reset; LAYER2_CONTROL = 0xC3 | BIN |
| 2 | timeslot interrupt | TX RAM bytes 0–11: 0xB8 (last block, CSBKO 0x38 BS_Dwn_Act), FID 0, bytes 2–6 = 0 (BS address 0), bytes 7–9 = own ID big endian, bytes 10–11 = 0; SEND_TYPE = 0x30; TXRX_CTRL = 0x80; **start the PA one-shot**; red LED | BIN |
| 3 | next timeslot interrupt | **SEND_TYPE = 0x30, TXRX_CTRL = 0x80 again**: the same CSBK in the immediately following slot; wake response timeout 600 ms | BIN |
| 4 | next | go to receive (5.2), count TC toggles | BIN |
| 5 | while waiting | when the received TC has toggled correctly 4 times → repeater awake, start 6.1; otherwise retry from step 2 after 600 ms, give up after a few tries | BIN |

The July build sent one CSBK without keying the PA (forum: "only an unmodulated carrier"); the August build added
the PA one-shot and the second consecutive CSBK, and repeater wake then worked (BIN; which of the two fixed it is not
known).

---

## 7. CRC and FEC the chip does

| Burst | Chip adds | CPU supplies | Tag |
|---|---|---|---|
| LC header, terminator | RS(12,9) with RS_H_MASK / RS_T_MASK, BPTC(196,96), slot type Golay | 9 LC bytes | DS, BIN |
| CSBK, MBC header, data header, PI header | CRC-16 with the programmed mask (when SEND_TYPE b8 = 0), BPTC, slot type | 10 bytes | DS, BIN |
| Rate ½ / ¾ / 1 data | BPTC / trellis ¾ / none, slot type | 12 / 18 / 24 bytes including CRC-9 and the final CRC-32 | DS |
| Voice | sync / EMB (QR), embedded LC fragments (5-bit checksum from TX RAM byte 9 or computed), burst F EMB from the RC encoder when VOICE_EMB_CTRL b1 = 1 | 27 AMBE bytes; LC for burst A | DS |
| Short LC (CACH) | CRC-8 (CRC8_INIT) | repeater mode only | DS |

On receive the chip checks the same codes and reports them in RX_TYPE_INFO b30:24; it decodes FEC and leaves only
information bits in the RX RAM (DS, BIN).

---

## 8. FM mode

### 8.1 Registers

| Offset | Register | Fields (DS) | Values used |
|---|---|---|---|
| 0x500 | FM_BANDWIDTH | b6 band-pass on; b4 pre-emphasis; b3 TX 25 kHz; b2 adjacent-channel filter 25 kHz; b1 RX 25 kHz; b0 CTCSS RX 25 kHz | TX: 0x5F (25 kHz), 0x50 (12.5 kHz). RX: 0x4F (25 kHz), 0x40 (12.5 kHz) (KP) |
| 0x504 | FM_DEV_COEF | b23:16 TX limiter, b15:8 RX deviation coefficient, b7:0 TX deviation coefficient | 0x40A20 (25 kHz), 0x40A25 (12.5 kHz) (KP) |
| 0x508 | FM_SQL | b15:0 noise measure: higher = less signal | squelch threshold in software |
| 0x50C | SUBVOICE_COEF | b27:16 detector sample depth (default 0x7D0), b15:8 sub-audio / DTMF deviation, b7:0 detection threshold | DTMF deviation (8.3) |
| 0x510 | FM_MOD | b23:16 repeat-symbol threshold; b15:14 XTCSS length; b13:12 CTCSS reverse-burst mode (01 = 240°, 10 = 180°); b11 CDCSS TX invert; b10 CDCSS RX invert; b9 CDCSS turn-off code; b8 XTCSS EOM; b4 selcall; **b3 CTCSS; b2 CDCSS; b1 DTMF**; b0 XTCSS | |
| 0x518 | CTCSS_ADDR | b15:8 RX tone address, b7:0 TX tone address (table 8.2) | |
| 0x51C | USER_DEFINE_CTC | b31 enable, b20:0 step = round(f × 65.536) | non-standard tones |
| 0x520 | CDCSS_ADDR | b24:16 RX code, b8:0 TX code (the 9-bit DCS code as binary, e.g. 114 octal = 001 001 100). The TX code is taken when FM_PTT goes on; later writes don't change the code on air (BENCH, DM-1801 Pro: written after FM_PTT, the radio sent the previous code) | |
| 0x524 | SELCALL_TONE | 2/5-tone frequency | |
| 0x528 | TONE_SEND_CTRL | b9 selcall send; b8 selcall finish; **b7 dtmf_send; b6 dtmf_finish (0 mutes voice during signaling); b3:0 DTMF digit** | |
| 0x550–0x55C | FM_TONE1..4 | two 16-bit DTMF tone steps each | 8.3 |
| 0x560 | FM_PTT | b0 FM transmit | 1 in FM TX, 0 in FM RX |
| 0x564 / 0x568 | FM_DECODE_FLAG1 / 2 | FLAG2: b11:8 DTMF digit received; **b1 CDCSS detected; b0 CTCSS detected**; XTCSS flags | sub-audio squelch |
| 0x530–0x54C | FM_FUNCTION_COEF1..8 | detector coefficients, 2·cos(2πf / fs)·2^22 (fs 32 kHz for DTMF, 8 kHz for custom CTCSS) | |
| 0x080 | AUDIO_CONTROL | b31 prompt FIFO full; b7 prompt path; **b6 FM audio to speaker**; b5 CPU-fed speaker PCM; b4 mic PCM to CPU; b3 sidetone; b2 I2S select; b1 I2S slave (reset 1); b0 voice source (0 codec, 1 TX RAM) | |

### 8.2 FM entry sequences (KP; FM worked in the Dec 2025 build)

| Mode | Writes |
|---|---|
| FM receive | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; FM_PTT = 0; AUDIO_CONTROL = 0x40; GCR_DACL = 0x80 (0 dB); DAC_PD_CTRL bit 1 set (PA drive DAC off); WORK_MODE = 0x80 |
| FM transmit | FM_BANDWIDTH and FM_DEV_COEF for the bandwidth; DAC_PD_CTRL bit 1 cleared; WORK_MODE = 0x80; FM_PTT = 1. Board: mic switch to analog, RF chip to TX, PA select, DAC B drive |
| FM audio on / off in DMR context | AUDIO_CONTROL b6 set and WORK_MODE = 0x80 / b6 cleared and WORK_MODE = 0x6E |
| Mute mic / line out | codec CR_ADC b7 / CR_DAC b7 = 1 |

Standard CTCSS addresses (DS table 59; RX uses the same addresses in CTCSS_ADDR b15:8):

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
it: the reset value 0x28 gives 300 Hz CTCSS at both bandwidths, and ODR writes 0x50 at 25 kHz (600 Hz) (BENCH,
DM-1801 Pro UHF). DCS at 0x28 measured about 100 Hz. When FM_PTT drops, the chip sends
the reverse burst / turn-off code before stopping (DS). On a detected tone the FM signaling system interrupt fires
(SYS_INTERP_LIST b13) and FM_DECODE_FLAG2 b0 (CTCSS) or b1 (CDCSS) goes 1, back to 0 at the end (DS). CDCSS tail
detection uses FM_FUNCTION_COEF1 = 0x7F499D (134.4 Hz) (DS).

### 8.3 DTMF and single tones

| Item | Value | Tag |
|---|---|---|
| Tone step | round(f × 2.048) (= f × 65536 / 32000) | DS |
| Default tone registers | FM_TONE1 = 697 / 770 Hz (0x0593 / 0x0629), FM_TONE2 = 852 / 941 (0x06D1 / 0x0787), FM_TONE3 = 1209 / 1336 (0x09AC / 0x0AB0), FM_TONE4 = 1477 / 1633 (0x0BD1 / 0x0D10); high half = first tone | DS |
| DTMF deviation (SUBVOICE_COEF b15:8) | KP: UHF 0x24 (25 kHz), 0x12 (12.5 kHz); VHF 0x75 / 0x3A. Measured (BENCH, DM-1801 Pro UHF, 1750 Hz single tone): about 5.8 Hz per step, 0x60 = 559 Hz, 0xC0 = 1117 Hz; KP's 0x12 is about 100 Hz. ODR uses 0xC0 at 12.5 kHz and 0xFF (the most, about 1.5 kHz) at 25 kHz; VHF unmeasured | KP, BENCH |
| Send sequence | FM_MOD b1 = 1; FM_PTT = 1; TONE_SEND_CTRL b6 = 0 (mute voice); AUDIO_CONTROL b3 = 1 for sidetone; TONE_SEND_CTRL b3:0 = digit; b7 = 1 to start the digit, b7 = 0 to end it; repeat per digit; finally b6 = 1 to restore voice | DS |
| Arbitrary single tone | write the tone step into the high halves of FM_TONE1 and FM_TONE3 and send DTMF digit "1" (697 + 1209 pair), restore the defaults after. On air at 1750 Hz on the DM-1801 Pro (BENCH): TONE_SEND_CTRL = 0x81 (digit 1 on, voice muted) with FM_MOD b1 set, 0 afterwards (the reset value) | KP, BENCH |
| Receive | FM_MOD b1 = 1; SUBVOICE_COEF b27:16 = 1056 (33 ms at 32 kHz), b7:0 threshold; FM_FUNCTION_COEF1..8 = 0x7ECD9D, 0x7E8A34, 0x7E368C, 0x7DD245, 0x7C690D, 0x7B9F03, 0x7AA7A8, 0x7979F5; read FM_DECODE_FLAG2 b11:8 on each signaling interrupt; FM_MOD b23:16 sets the repeat-symbol window | DS |

### 8.4 Prompt / beep FIFO

Set AUDIO_CONTROL b7, then while AUDIO_CONTROL b31 is 0 write 64 bytes (32 × 16-bit samples, low byte first, 8 kHz)
to 0x16000980..0x160009BF; clear b7 when done (BIN from the DM-1801 Pro stock image; BENCH on the DM-1801 Pro). High byte first, as KP has it (16
samples per push), sounds distorted on the DM-1801 Pro. Flushing stale audio: fill with 0x7F bytes (KP).

ODR plays all speaker PCM through this FIFO (`platform/hrc7000/audio_pcm.c`). The CPU-fed speaker buffer at
0x180000A0 (AUDIO_CONTROL b5, section 3) gave rough, choppy beeps on the DM-1801 Pro with every sample layout tried,
with no queue underruns (BENCH).

The prompt path also feeds the TX modulator: with AUDIO_CONTROL b7 set during a DMR transmission, the beep's audio
rides on the 4FSK. On the DM-32UV the talk permit tone overlapped the second and third voice LC headers, which then
carried no decodable sync on the SDR bench and the bench modem took the call by late entry; with b7 held clear while
transmitting, all three headers decoded in five runs (BENCH). Keep b7 clear during any transmission; a beep that
starts then plays silently, and b7 comes back on at TX end if the beep is still running.

### 8.5 FM receive audio to the CPU

In FM (WORK_MODE 0x80) the modem copies its demodulated receive audio into the RX RAM as well as to the codec (the
guide's "analog receive recording", on by default in FM; DS):

| Item | Value | Tag |
|---|---|---|
| Buffer | RX RAM 0x160004E0..0x160008DF: two halves of 512 bytes (256 samples), used in turn | DS |
| Sample format | 16-bit, high byte at the lower address | DS, BENCH |
| Rate | 8 kHz: the halves alternate every 32.0 ms (DM-32UV, measured over 95 changes) | BENCH |
| Half to read | FM_ADDR_SW (0x56C) b1:0: 1 = the first half, 2 = the second | DS |
| Interrupt | SYS_INTERP bit 14 (`fm_rx_interp`) as each half fills; its mask bit (SYS_INTERP_MASK b14) is off in the power-on mask | DS |
| Input | the modem's AF input. On the DM-32UV that is the FD6818's audio through PTB15 = 0 (`hardware-dm32.md` 9) | HW |

The audio is not squelched: it carries noise while no signal is present, so a demodulator can work below the squelch.
On the DM-32UV receiver noise reaches full scale (peak 32767) with the reset AF gain (BENCH).

ODR reads it in the system interrupt (`platform/hrc7000/fm_rx_audio.c`): the DMR glue owns that interrupt and calls
an FM hook for bit 14, in FM too, and the hook copies each half into a 128 ms ring for the APRS demodulator.

---

## 9. The 2026 fixes (what made DMR work)

From the research notes.
All BIN; none tested by us on air.

| # | Area | Dec 2025 (broken) | Working builds | Effect |
|---|---|---|---|---|
| 1 | Vectors | only 61 and 63 hooked; RF TX handler never called | 62 (TX timeslot) and 64 (RF TX) hooked; TX states run on 62; 64 keys the RF chip | DMR TX starts at all |
| 2 | Register set-up | whole DMR block rewritten at every DMR entry | written once at init | |
| 3 | RF_MODE | 0x03269060 in DMR | 0x03269860 always (dual RX channels) | minor |
| 4 | RF_CONTROL | 0x00040A0A in the July build | 0x00041F1A (RF TX interrupt 3.1 ms early) | part of the low-power timeslot fix |
| 5 | DMR entry | long sequence, PA drive DAC left off after FM RX | calibration, mic gain, DAC_PD_CTRL bit 1 cleared, WORK_MODE 0x80 → 0x6E | PA drive present in DMR |
| 6 | RX restart | TXRX 0xC00000, 0, 0xC00000, 0x8040; SLOTON never written | SLOTON 0, TXRX 0x800000, 0x40, SLOTON 5; VOICE_EMB_CTRL 2 | reliable RX sync |
| 7 | RX start / after TX | 0x8040 | 0x40 (0x8040 only in the steady receive alternation) | |
| 8 | CC probe | 3 ms busy wait | none | |
| 9 | TX start | TXRX bit 31 set | LAYER2_CONTROL 0xC3, no bit 31 | TX slot timing |
| 10 | PA keying | PA select + drive at the RF TX interrupt (July) or never (Dec) | one-shot timer from each arming, "DMR PA" delay default 28 ms | timeslot correct at 1 W |
| 11 | RF chip | stays in RX in DMR | 0x43 = 0x4048, idle → TX at TX start and every RF TX interrupt | actually transmits |
| 12 | Mic switch | never changed | GPIO 0x2E: 1 analog, 0 DMR, at each TX start | DMR audio reaches the codec |
| 13 | Repeater wake | one CSBK, PA not keyed | CSBK in two consecutive slots, PA keyed, extra wait state | repeater wakes |
| 14 | TX audio | encode call commented out | encode in the tick when ≥ 2 PCM buffers wait | real voice instead of silence |
| 15 | Timeslot handler | in XIP flash (July) | in RAM with the TX helpers (Aug) | less jitter when flash is busy |
| 16 | Calibration | constants, HR-C6000-style calls | SIG_CENTER / RF_MOD_BIAS_CTRL from factory data | modulation accuracy |
| 17 | Interrupt masking | PIC_MASK_1 bits 29–31 (vectors 93–95) | PIC_MASK bits 29–31 + PIC_MASK_1 bit 0 (our fix; OpenDM32 has no masking calls) | |

---

## 10. Open questions

1. LCSS bits on voice bursts B–E (2.6): the guide sets them, the working builds do not. Does late entry work on other
   radios receiving a DM-32?
2. Whether the 0x370–0x37C, 0x168 and bit-31 sync-enable writes matter.
3. RF_RECV_DELAY 0x36B0 vs the guide's 2000 for AF mode.
4. The real timing of the TX timeslot interrupt relative to the slot boundary (section 3.1 is inferred).
5. Data and CSBK transmission on the C7000 have only been checked against the guide and shared host tests, not on air.
6. FM sub-audio: the C7000's own CTCSS/CDCSS engine (section 8) has not been used by any firmware seen; the port routes
   TX CTCSS to the FD6818 instead.
