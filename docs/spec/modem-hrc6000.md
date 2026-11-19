<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# HR_C6000 as a DMR modem

This page covers driving the Hongrui HR_C6000 as a DMR Tier II modem from an STM32 host, at register level: what to
write, when, and what the chip reports. The modem-neutral behavior a DMR stack needs is in
[dmr-modem.md](dmr-modem.md); every register is in [hrc6000-registers.md](hrc6000-registers.md). The HR_C5000, which
the same driver runs, is covered where it differs. For which radios carry these chips, see the
[feature matrix](../feature-matrix.md).

The sources are the HR_C6000 user manual (the register chapter and the "Layer 2" application chapter) and on-air
measurements with an SDR decoding every burst bit exact. Init values that the manual doesn't explain are marked
"(unexplained)"; keep them.

| Section | Topic |
|---|---|
| 1, 2, 3 | Interfaces, registers, power-on initialization |
| 4, 5, 6 | Interrupts, receive, transmit |
| 7 | Timing quirks found on air |
| 8 | FM mode |
| 9 | Single-frequency repeater and BS outbound |
| 10 | Physical layer pipe |

## 1. Interfaces

| Interface | Manual name | Use | Host side |
|---|---|---|---|
| Config SPI | U_SPI | every register page, LC / data RAM, sync pattern | bit-banged by the MCU |
| Voice SPI | V_SPI | the 27-byte AMBE voice frame for each voice burst (page 3) | STM32 SPI, mode CPOL=1 CPHA=0 (first edge), MSB first, prescaler 8 |
| I2S | I2S | 8 kHz PCM between the chip's built-in codec (mic ADC, line-out DAC) and the MCU, which runs the AMBE codec in software | the board's I2S port |
| PWD pin | PWD | power down; drive low to run, wait 10 ms before the first SPI access | a GPIO |
| TIME_SLOT_INTER | timeslot interrupt | every 30 ms slot boundary once timing exists | an EXTI line, falling edge |
| SYS_INTER | system interrupt | events (burst decoded, TX start/end, late entry, ...) | an EXTI line, falling edge |
| RF_TX_INTER | RF TX interrupt | "key the RF now": comes shortly before each transmitted burst | an EXTI line, falling edge |
| RF_RX_INTER | RF RX interrupt | "RX window now" / end of TX burst | routed on some boards only |
| RF_TX_EN / RF_RX_EN | chip-timed RF enables | could switch the RF directly | routed on some boards only |

All three interrupt lines share one NVIC priority. On the `stm32f4` platform it's 5, more urgent than USB and the other
EXTI lines at 6 (numerically lower is more urgent). Thread-level SPI traffic must not re-enter the handlers. A thread
that touches the modem masks the three EXTI lines, and waits until any running handler has finished before it starts
its own transfers.

### 1.1 U_SPI framing

Each transaction is CS low, a command byte, an address, data bytes, and CS high. Bytes are MSB first. The bit-banged
master sets MOSI, drives CLK low, then high: data is sampled on the rising edge, and CLK idles high. Reads sample MISO
after the falling edge, before the rising edge.

| Byte | Meaning |
|---|---|
| Command | bit 7: 1 = read, 0 = write. bit 6: 1 = 11-bit (extended) address. bits 2-0: page |
| Address | 8-bit address; with bit 6 set, two bytes: low 8 bits first, then bits 10-8 in the low 3 bits of the second byte |
| Data | one or more bytes; the address auto-increments while CS stays low |

A read sends the command and address, then clocks dummy bytes (0xFF) while reading. The driver uses burst writes of up
to 128 data bytes and burst reads of up to 96.

| Page | Contents | Used for |
|---|---|---|
| 1 | auxiliary parameters (sync patterns, DTMF tone table at extended addresses 0x11A-0x129, unexplained values) | init, sync pattern, tones |
| 2 | data RAM: a write fills the **TX** RAM, a read returns the **RX** RAM ("1.2K RAM" in the manual) | LC, CSBK, data header / block payloads |
| 3 | vocoder frame buffer | 128 bytes of 0x7F to flush it; on V_SPI, the 27-byte AMBE frame |
| 4 | system parameter table (the main registers) | everything else |

The manual's command table lists page 2 as the RX ("receive end") RAM and page 6 as the TX ("write end") RAM. Writes to
page 2 land in the TX RAM and go on air bit exact; the driver uses page 2 for TX.

The TX RAM layout, from the manual:

| Address | Contents |
|---|---|
| 0x00-0x17 | LC, CSBK, data header, data blocks |
| 0x18-0x23 | the idle message |
| 0x24-0x28 | the CACH short LC: 28 bits in 0x24-0x26 and 0x27 bits 7-4, its CRC-8 in 0x28 |
| 0x29-0x2C | voice F's EMB fill |

V_SPI uses the same command and address framing: page 3, address 0x00, then 27 bytes; the read command is 0x83.

### 1.2 Clock

The PLL settings match the manual's 29.4912 MHz crystal row: PLL output 49.152 MHz, system clock 9.8304 MHz, codec
clock 12.288 MHz, and CLKOUT 24.576 MHz.

## 2. Registers used (page 4 unless noted)

Names are the manual's. The table lists only the bits the driver relies on.

| Reg | Name | Bits / values used |
|---|---|---|
| 0x00 | reset (active low per module: DMR, PHY, codec, FM, vocoder IF, MSK, I2S, built-in codec) | 0xFF at init (all running) then 0x2A (partial reset, unexplained) |
| 0x01 | H/W config | 0xF0 in DMR (two-point modulation TX, IF RX); 0xB0 then 0xF8 during init (unexplained). The same value is used on UHF and VHF. Bit 6 swaps the receive I and Q (the manual: the receive port's mapping to the RF port). With an IF input that inverts the received spectrum, so it must match the radio's mixers: inverted 4FSK flips every symbol, and since the voice and data sync patterns are complements, the chip then sees the odd voice burst as a data sync and tracks no call. A board whose first LO changes side between bands sets the bit per band (`hrc6000_rx_iq_swap()`); the default is set |
| 0x02-0x05 | TX / RX I and Q offsets | 0 at init; 0x04 is rewritten in FM with the calibration "Mod2 offset". In two-point mode 0x04 is the MOD1 (reference crystal) offset and 0x02 the MOD2 (VCO) one (manual 4.7.2; the register table says MOD2 for both): 0x04 moves the FM carrier about 75 Hz a step, signed |
| 0x06 | vocoder select | 0x21 = vocoder over SPI under MCU control; 0x22 = "open music" (used only while flushing page 3); 0x20 = normal |
| 0x07-0x09 | IF frequency H/M/L | 0x0B, 0xB8, 0x00 (450 kHz) |
| 0x0A-0x0C, 0xB9-0xBB | clock source, PLL M/N, system clock dividers | section 3 |
| 0x10 | ModulatorMode / TierMode / ContinueMode / LayerMode / ISRepeater / ISAligned / RepeaterSlot | bit 7 FM(1)/DMR(0); bit 6 Tier II; bit 5 timeslot mode; bits 4-3 layer (01 = layer 2); bit 2 relay (BS); bit 1 aligned; bit 0 slot. **0x6E in DMR operation**, 0x6B at init (unexplained), 0x80 for FM |
| 0x11 | LocalChanMode | 0x80 (bit 7: direct simplex in direct mode, repeated simplex in relay mode). bit 6 direct duplex / repeated duplex in relay mode, bit 3 single-frequency simplex (not used) |
| 0x14-0x16 | local DMR ID (H/M/L) | set at init to a placeholder, then the radio ID |
| 0x1D | unaddressed mask | 0xFF |
| 0x1E | broadcast address | 0xF1 |
| 0x1F | LocalEMB | bits 7-4 the color code; bit 3 PI; others 0. Written as CC << 4 |
| 0x20 | LocalAccessPolicy | 2-bit fields: 00 impolite, 01 polite to all, 10 polite to own CC |
| 0x21 | LocalAccessPolicy1 | bits 7-6 / 5-4 as 0x20; bit 1 clears the vocoder encode buffer (self-clearing); bit 0 clears the decode buffer. **0xA2 before a transmission** (polite to CC, clear encode buffer); 0x02 = impolite + clear |
| 0x22 | EncodeStart / Stop / DecodeStart / Stop / I2S mode | **0x86 before a transmission** (start encode, I2S) |
| 0x25 / 0x26 | DAC power / ADC control (manual: undocumented) | 0x0E / 0xFD in DMR and FM RX; 0x26 = 0xFE turns the FM receive path off during FM TX |
| 0x34 | FM audio filters | 0x3C: compressor off, pre/de-emphasis on, 3 kHz filter |
| 0x35 | FM TX deviation coefficient | section 8 |
| 0x36 | FM audio feed-through | 0x02 on, 0x00 off (FM RX mute) |
| 0x37 | DMR RX audio gain / MCU control | 0x80 + (0x40 if gain > 0) + (gain & 0x1F), gain clamped to -31..+31 dB; 0x9E at init |
| 0x3E / 0x3F | FM RX deviation coefficient / TX limiter | 0x08 / 0x04 |
| 0x40 | TxEn (7), RxEn (6), MasterMode (5, 1 = active timing), Layer2Slot (4), CRC_MCU_Control (3), Decode_Mode (1-0, 11 = normal) | **0xC3** = TX+RX enabled, passive timing (follows received bursts). **0xE3** = TX+RX enabled, active timing (own timing). 0x43 = RX only, passive (manual default for RX). Bit 3 set = the MCU supplies the CRC; the chip sends the 96 info bits verbatim |
| 0x41 | TxNxtSlotEn (7), RxNxtSlotEn (6), SyncFail (5), begin_v_layer2 (4) | written at every timeslot interrupt: **0x80** send in the next slot, **0x50** receive in the next slot (+ access-success flag), **0x40** receive (manual), **0x00** neither, **0x20** drop sync and search again. Bit 3 is CC_Match_Ctrl (1 = deframe without a CC match) |
| 0x42 | TransSlot (7), RecvSlot (6), UsedSlot (5) | read at the timeslot interrupt: what the slot now starting is |
| 0x43 / 0x44 | RSSI H / L | not used (the AT1846S RSSI is used instead) |
| 0x45 / 0x46 | Mod2 / Mod1 amplitude | from calibration: the DMR pair in DMR, the FM pair in FM |
| 0x47 / 0x48 | reference oscillator offset / two-point bias | 0x21 / 0x00 at init (unexplained); 0x48 = 0 in FM |
| 0x49 / 0x4A | modulation limits | 0xFF / 0xFF in FM |
| 0x50 | LocalDataType (7-4), LocalVoD (3, 1 = voice), data-header-2 (2), LCSS (1-0) | the burst to send in the next slot: data burst = DT << 4; voice burst = 0x08 \| (n << 4) with n = 0..5 for A..F |
| 0x51 (R) | DLLRecvDataType (7-4), DLLRecvPI (3), DLLRecvCRC (2, 1 = error), SyncClass (1-0: 0 none / EMB only, 1 voice, 2 data, 3 RC) | read on every "received data" interrupt. For voice, bit 7 is set and bits 6-4 are 1..6 for A..F, so the 4-bit field reads 0x9..0xE: **use the low 3 bits** |
| 0x52 (R) | DLLCC (7-4), AT (3), TC (2), CACH LCSS (1-0) | received color code; the CACH TC bit gives the slot number (repeater signals only) |
| 0x52 (W) | CACH AT (3), TC (2) | the BS downstream CACH's AT and TC for the burst being set up (relay mode) |
| 0x53 / 0x54 (R) | RC info / EMB LCSS | the reverse channel ([dmr-modem.md](dmr-modem.md) 2.8) |
| 0x55 (R) | per-check error flags (CRC8, golay, QR, CS, CRC16, CRC24, CRC9) | not used |
| 0x56 / 0x57 | unknown; 0x56 = 0x00 at init (unexplained) | not used |
| 0x5C | unknown; 0x09 at init (unexplained) | |
| 0x5F | Sync_Detect_Ctrl (7-4: MS, BS, TDMA1, TDMA2 enables); read: Recv_Sync_Type (1-0: 0 MS, 1 BS, 2 TS1 direct, 3 TS2 direct) | **0xF0**: detect all four |
| 0x60 | TransControl | 0x80 = analog voice TX on (FM); 0x00 off. (Manual: 10 voice request, 01 data request, for layer 3) |
| 0x64, 0x65, 0x39, 0x3D, 0xD1 | unknown (unexplained) | 0x00, 0x0A, 0x02, 0x0A, 0xC4 at init |
| 0x81 | InterClass1Mask (mask for 0x82, 0 = masked) | **0x19** |
| 0x82 (R) | interrupt flags | section 4 |
| 0x83 (W) | interrupt clear | write back the value read from 0x82; 0xFF clears all |
| 0x84 (R) / 0x85 | TX-start sub-status / mask | mask 0x00 |
| 0x86 (R) / 0x87 | TX-end sub-status / mask | mask 0x00 |
| 0x90 (R) | received-information sub-status | 0x80 good, 0x00 CRC error, 0x40 unconfirmed SMS error (layer 3 only) |
| 0x93 (R) | tone detect | bit 0: CTCSS / DCS detected (FM) |
| 0x98 | voice_lostmask (7-5) / voice_abnormal_state (2-0) | bit 2: slot 1 voice sync lost; bit 1: slot 0 voice sync lost; bit 0: other voice abnormality |
| 0xA0-0xAF, 0xD2-0xD4 | tone generator / decoder | section 8 |
| 0xC2 | mic AGC | 0x00 (off) in FM TX |
| 0xE0 | codec control (MCU control, line in, mic, line out, I2S slave) | **0xC9** in DMR and FM TX (mic on); 0x89 FM RX (mic off). bit 6 = mic input enable |
| 0xE1, 0xE3, 0xE5 | codec defaults (unexplained) | 0x0F, 0x52, 0x1A |
| 0xE2 | codec config; bit 1 = DAC to line-out | 0x06 at init; bit 1 cleared mutes the line out |
| 0xE4 | line-out gain (7-6) / mic gain | 0x20 + mic gain (0..31) |

Page 1 0x04-0x09 holds the 6-byte **data** sync pattern the chip transmits. The chip sends the voice sync as its
bitwise inverse. For normal operation, write the MS-sourced data sync. The values are the ETSI ones, and the voice
syncs are their inverses.

| Data sync | Bytes |
|---|---|
| MS sourced (normal operation) | D5 D7 F7 7F D7 57 |
| BS sourced | DF F5 7D 75 DF 5D |
| direct mode TS1 | F7 FD D5 DD FD 55 |
| direct mode TS2 | D7 55 7F 5F F7 F5 |

## 3. Power-on initialization

Drive PWD low, wait 10 ms, then write the following in order. Everything is page 4 unless a page is given. Keep the
"(unexplained)" values.

| Step | Writes | Why |
|---|---|---|
| 1 | 0x0A=0x80, 0x0B=0x28, 0x0C=0x33, 0x0A=0x00, 0xB9=0x05, 0xBA=0x04, 0xBB=0x02 | run from the crystal, program the PLL (M, N/NO), switch to the PLL, set system / codec / CLKOUT dividers |
| 2 | 0xA1=0x80, 0x10=0xF3, 0x5F=0xF0, 0x40=0x43, 0x07=0x0B, 0x08=0xB8, 0x09=0x00, 0x06=0x21, 0x00=0xFF, 0x01=0xB0, 0x02..0x05=0x00, 0x01=0xF8 | tones off, temporary mode (unexplained), detect all syncs, RX passive, 450 kHz IF, vocoder over SPI under MCU control, release all resets, two-point modulation, zero I/Q offsets |
| 3 | page 4 burst from 0x11, 44 bytes: 80 0C 22 01 00 00 33 EF 00 FF FF FF F0 F0 10 00 00 06 3B F8 0E FD 40 FF 00 0B 00 00 00 06 0B 00 17 02 FF E0 14 00 00 00 00 00 00 00 | bulk system parameters 0x11-0x3C (unexplained) |
| 4 | 0x00=0x2A, 0x06=0x22 | partial reset, "open music" (vocoder buffer access) |
| 5 | page 3 burst from 0x00: 128 x 0x7F | flush the vocoder frame buffer |
| 6 | 0x06=0x20, 0x14/0x15/0x16 = a placeholder ID | music off; the real radio ID is written later |
| 7 | page 1 bursts: from 0x10 (32 bytes) 69 69 96 96 96 99 99 99 A5 A5 AA AA CC CC 00 F0 01 FF 01 0F 00 00 00 00 0D 70 00 00 00 00 00 00; from 0x30 (16) 00 00 20 3C FF FF 3F 50 07 60 00 00 00 00 00 00; from 0x40 (7) 00 01 01 02 01 1E F0; from 0x50 (5) 00 08 EB 78 67 | auxiliary parameters (unexplained). The 0x10 block holds the CRC masks: PI 0x6969, RS header 0x969696, RS terminator 0x999999, CSBK 0xA5A5, MBC 0xAAAA, data header 0xCCCC (as in ETSI TS 102 361-1 B.3.12) |
| 8 | page 1: 0x52=0x08, 0x53=0xEB, 0x54=0x78, 0x45=0x1E, 0x37=0x50, 0x35=0xFF | more auxiliary values (unexplained) |
| 9 | 0x39=0x02, 0x3D=0x0A, 0x83=0xFF, 0x87=0x00, 0x65=0x0A, 0x1D=0xFF, 0x1E=0xF1, 0xE2=0x06, 0xE4=0x27, 0xE3=0x52, 0xE5=0x1A, 0xE1=0x0F, 0xD1=0xC4, 0x25=0x0E, 0x26=0xFD, 0x64=0x00, 0x10=0x6B, 0x81=0x19, 0x01=0xF0, 0xE4=0x27, 0xE5=0x1A, 0x37=0x9E, 0xE0=0xC9, 0x25=0x0E, 0x26=0xFD, 0x48=0x00, 0x47=0x21, 0x1F=0x10 | clear interrupts, unmask TX sub-status, addressing, codec defaults and mic gain, DAC / ADC power, DMR layer 2 mode, interrupt mask, two-point modulation, RX gain, codec under MCU control, reference oscillator offset, color code 1 |
| 10 | page 1: 0x54=0x78, 0x24..0x27=0x00 | (unexplained) |
| 11 | 0x41=0x40, 0x56=0x00, 0x5C=0x09, 0x5F=0xF0 | receive in the next slot; detect all syncs |
| 12 | page 1 burst from 0x04: D5 D7 F7 7F D7 57 | MS data sync |
| 13 | 0x11=0x80, 0x81=0x19, 0x85=0x00 | direct simplex channel mode, interrupt mask, TX-start sub-status mask |
| 14 | 0xE4 = 0x20 + DMR mic gain; DTMF tone table (section 8) | |

### 3.1 Entering DMR (after FM, or at every DMR channel change)

Write 0x01=0xF0, 0x45 / 0x46 = the DMR modulation pair from calibration, **0x10=0x6E**, 0xE0=0xC9, 0xE4=0x20+DMR mic
gain, and 0x26=0xFD. Then restart the receiver (5.1). In operation 0x10 is 0x6E, not the 0x6B written at init.

### 3.2 Flushing stale vocoder audio

To flush the vocoder, set the RX gain to -15 dB (any lower and the flush doesn't take), write 0x06=0x22, write 128 x
0x7F to page 3, write 0x06=0x20, and restore the RX gain.

## 4. Interrupts

### 4.1 Timeslot interrupt (every 30 ms)

The interrupt comes at each slot boundary once the chip has timing:

- Passive timing (0x40=0xC3) derives the timing from received bursts. When the signal goes away, the chip keeps
  counting from the last sync. It applies corrections within 1.25 ms in real time.
- Active timing (0x40=0xE3) generates the timing locally.
- The interrupt stops only when TxEn and RxEn are both cleared.

At every timeslot interrupt, the MCU decides the **next** slot and writes 0x41. For TX it also writes the payload and
0x50. These writes must land **before the slot after the next starts**. In the manual's timing, the chip starts
framing the next slot's burst at t1, and the MCU writes its data in the window t2 before t1. The driver writes
everything at the start of the timeslot handler, and the burst goes out in the following slot. A late set-up slips the
burst by one slot (30 ms) or by a frame (60 ms).

While transmitting, the slot between our bursts gets 0x00. An SFR writes 0x50 there instead, because it hears its
inbound slot in that slot (`hrc6000_dmr_tx_listen`, [sfr.md](sfr.md) 3).

The CACH TC bit (0x52 bit 2) can be read here. The chip refreshes it only in slots being received. It's the TC of the
slot that just ended, not the one starting (5.3).

### 4.2 System interrupt

Read 0x82 (flags) and 0x52 (CC / CACH) first, handle each set flag, then write the value read back to 0x83.

| 0x82 bit | Meaning (DMR) | Sub-status | Use |
|---|---|---|---|
| 7 | send request rejected (channel busy and the access policy is polite) | none | the transmission didn't go out |
| 6 | TX start | 0x84: 7 voice, 6 OACSU, 5 encryption key update, 4 vocoder config done, 3 data, 2 partial retransmission, 1 retransmission, 0 vocoder init | read and ignored |
| 5 | TX end | 0x86: 7 service complete, 6 fragment-length ack sent, 5 OACSU timeout, 4 layer 2 "last moment to set up the next slot", 3 confirmed fragment sent, 2 short LC received, 1 BS activation timeout | read and ignored |
| 4 | post access (late entry) | none | a voice call already in progress has been acquired from its embedded signaling: start receiving as if a header had been heard |
| 3 | received data ("control frame parsed") | 0x51 | a burst was decoded: section 5.2 |
| 2 | received information (layer 3 service data) | 0x90 | read and ignored |
| 1 | voice abnormal exit | 0x98 | voice sync lost on slot 0 (bit 1) or slot 1 (bit 2): replace the next voice frame of that slot with silence |
| 0 | physical layer receive | none | not used in DMR (section 10) |

The mask 0x81=0x19 enables bits 4, 3, and 0. Bit 7 (send rejected) still shows set when 0x82 is read on another
interrupt, and the SFR debug build counts these. So 0x82 shows more flags than the mask lets interrupt.

**Two interrupts per voice burst.** A voice burst produces one received-data interrupt for its EMB (SyncClass 0) and
one for the burst. A voice handler takes a burst number 1..6 at most once per 45 ms: it drops a second report that
comes within 45 ms of the last voice burst taken.

**The same status reported again one slot later.** In direct mode, the chip can report a burst a second time at the
next timeslot interrupt, with the same status and payload. The driver drops a report of the same kind (and, for data,
the same payload) within 45 ms as a repeat. Idle bursts with BS sync are exempt. A base station sends a burst in every
slot, and every idle burst has the same bits, so on a carrier idle on both slots each slot's idle looks like a repeat
of the one before. Dropping them makes the carrier look silent, and a trunked call's hang of idles times out.

**Timing of a burst report.** For a burst received in step with the chip's slot timing, the received-data interrupt
comes 3-5 ms after the timeslot interrupt (2-10 ms in all). A delay of 25-30 ms means the chip's timing is out of step
with the sender, because it locked to noise or to another station. Resynchronize the receiver then.

### 4.3 RF TX interrupt

The interrupt comes before each burst the chip is about to send. The handler keys the PA and switches the RF chip to
TX. On a board without the RF RX interrupt, the MCU ends each TX burst itself:

- At the next timeslot interrupt, if the PA has been on for more than 20 ms, the MCU turns it off and puts the RF chip
  back in RX.
- After the last burst of a transmission, a one-shot 29 ms timer does the same.

Keying takes the PA enables high and writes AT1846S register 0x30 = 0x60C6 (digital TX). Unkeying takes them low and
writes 0x30 = 0x7026 (25 kHz) or 0x6026 (12.5 kHz). A board can key a fixed delay after the interrupt (a 1 ms timer)
and switch from RX straight to TX or through idle; the board's glue gives its sequences.

On such a board, the RF chip's "transmitting" state goes on and off **burst by burst**. Code that needs to know
whether a transmission is in progress must use the modem slot state (section 7.1), not the PA.

### 4.4 Lost interrupts

The three lines are edge triggered. A system line still low after its handler (a flag set again, or one whose edge was
missed) gives no further edge, so no later request interrupts. The STM32F4 glue (`platform/stm32f4/hrc/dmr_glue.c`)
recovers and counts these (`hal_dmr_modem_t.health`, the debug link's `dmr.modem_*` keys):

| Count | Found | Recovery |
|---|---|---|
| stalls | no timeslot interrupt for 200 ms while transmitting (the engine's `tx_stalls`; a stall while receiving isn't counted) | the transmission ends, the receiver restarts |
| stuck | the system line low with no edge pending: at a timeslot interrupt (EXTI pending bit clear), or at the task's 10 ms poll (still low 5 us later with no handler run) | the timeslot handler services it, or the task pends the handler; counted once per low stretch |

Entering DMR with the line already low (a flag from FM) pends the handler at once and isn't counted. The first loss
keeps four words: 0x82, EXTI_PR, the interrupt port's IDR, and the three lines' NVIC enable, pending, and active bits
(bits 0-2, 8-10, 16-18).

## 5. Receive

### 5.1 Restarting the receiver

Write 0x40=0xC3, then 0x41=0x20, 0x41=0x00, 0x41=0x20, 0x41=0x50. (The manual's recipe: if the received CC doesn't
match, write 0x20 to drop the sync and search again.) The driver restarts the receiver after leaving TX in direct mode,
after a call ends, after color code changes, and on any loss of state.

### 5.2 Per burst (received-data interrupt)

Read 0x51 and 0x5F; 0x52 is already read.

| Field | From | Notes |
|---|---|---|
| sync class | 0x51 bits 1-0 | 0 none (EMB report), 1 voice, 2 data, 3 RC |
| data type / voice burst | 0x51 bits 7-4 | data: the ETSI data type 0..15. voice: low 3 bits 1..6 = A..F |
| CRC error | 0x51 bit 2 | for voice it reflects the EMB / embedded checks; for data the burst CRC |
| PI | 0x51 bit 3 | the privacy indicator; the voice path drops bursts with it set |
| color code | 0x52 bits 7-4 | |
| sync type | 0x5F bits 1-0 | MS / BS / TS1 direct / TS2 direct |

Payloads are on page 2 from 0x00:

- 12 bytes for an LC (voice header, terminator, or the embedded LC once assembled), CSBK, MBC, data header, and rate
  1/2 blocks.
- 18 bytes for rate 3/4 (the chip decodes the trellis code).
- 24 bytes for rate 1.
- Voice: 27 bytes from page 3 on V_SPI (three 72-bit AMBE+2 frames with their FEC, as the codec expects).

The driver applies these rules:

- **Mode from sync type.** BS sync means a repeater; anything else is direct mode. The receiver follows it, unless the
  channel forces direct mode (and it always stays direct for SFR).
- **Data blocks have no CRC of their own.** The packet CRC-32 covers them, so take rate 1/2, 3/4, and 1 blocks even
  when the CRC flag is set, and flag them. Drop CSBK, MBC, and data headers with a CRC error.
- **Data on our slot only.** Through a repeater, accept a data burst only when the slot number (from the TC bit) is
  ours and differs from the previous burst's slot; a second report in the same slot is a duplicate. In direct mode,
  accept the bursts of the slot the receiver is locked to.
- **Voice numbers by the frame count.** A class 0 report's number (0x51 bits 6-4) isn't always its burst's: in about
  one call in two, a burst is reported with the number of the burst before it or one further back, with no frame
  missed. Within a superframe in progress on the slot, with the timeslot interrupts running, the driver numbers each
  voice burst from the last one taken plus the frames counted since (two ticks a frame), and counts a disagreeing
  report (`sx` in the `v0` diagnostics line). A report with the last burst's number and the same voice is stale and
  dropped; with new voice it's the next burst. Burst A keeps its voice sync. Without the timing, or where the count
  would land on A, the driver uses the chip's number.
- **The LC registers are sticky.** Page 2 keeps the previous call's LC until the new one is decoded, so on a new
  reception the first LC read can be the last call's. Either wait for two superframes (burst A seen twice after the
  slot lock) before trusting it, or capture page 2 when the reception starts and accept an LC only when it differs
  from that capture, or after more than one superframe of voice.
  - The driver takes an unchanged LC at the burst A after the reception's first whole superframe (A-F of this
    reception, F with no CRC error), by which time it's in page 2 (next item): the same talker again. Waiting for the
    second F instead costs a superframe of audio on a late entry.
  - The driver captures page 2 at the first report of a quiet period (a status report before any burst is taken, for a
    late entry), or else at the first burst.
- **When the LC reaches page 2.** The voice header's LC and the terminator's LC are read at their bursts. During voice,
  the embedded LC appears in page 2 once the chip has assembled it from bursts B-E, but not yet at F's first report
  (the EMB one) ([gotchas](gotchas.md) 1.4a). So until a reception has its LC, the driver also reads page 2 at the
  next burst A, at F's repeated report, and at the post-access interrupt. An LC that differs there is handed over with
  the next voice burst.
- **Page 2 isn't per slot.** With a call on each slot, page 2 holds what the chip assembled last, of either call, so at
  one call's F it can hold the other's. The driver keeps each slot's call LC (a voice LC, FLCO 0 or 3, from its header
  or terminator, or the embedded LC it handed over), and checks each embedded LC it would hand over for a burst on a
  slot:

  | Page 2 holds | Handed over |
  |---|---|
  | This slot's open call's LC | Yes, even when the other slot's call has the same LC (a group on both slots) |
  | The other slot's call's LC, while that call may go on | No, unless this slot has no call of its own and 8 of its F reads in a row showed only that LC (then it's this call's too: a group on both slots, or the other call over unseen) |
  | Another voice LC, the other slot idle | Yes: this slot's next talker (no header, a terminator missed) |
  | Another voice LC, the other slot's call maybe going on | No while this slot's call goes on (voice within the last superframe): its talker can't change without a gap, and a mix of both calls stays in page 2 for as long as both talk. Otherwise only when read again a superframe later, so such a call gets its LC about 360 ms later. Page 2 can hold a mix of both calls' LCs (a source and talkgroup neither call has, from about 2.2 s into the call); the embedded LC has only a 5-bit checksum, so about 1 mix in 32 passes it |
  | A talker alias or GPS block (other FLCOs) | Only when the other slot isn't busy. Never kept as a slot's call LC |

  A slot is busy for 2.2 s (6 superframes) after its header, voice, or LC was last taken, or its LC last seen in page
  2. Page 2 doesn't keep a call busy once its terminator was heard, nor when the driver takes that slot's voice (both
  slots heard on a chip without the tick voice read). Otherwise the other slot's voice is never taken (the voice read
  locked to one slot, or the slot closed on a repeater channel), so its call can go on unseen: it counts as going on
  up to 60 s after its header or LC was last taken. A start, an `rx_restart` (a new frequency), or another color code
  or repeater setting forgets both slots.

  While the other slot has a call, a slot with no LC of its own keeps looking at the next burst A. There an unchanged
  page 2 counts as at F once the reception has had more than one superframe of voice or a header (on either slot),
  because the capture can come just after this call's F. Bursts with no slot number are left alone. The counter
  `lc_other_slot` counts what would have been handed over but was held back.

  The rule has limits. The two slots' superframes share a period, so page 2 at this slot's F holds the same call's LC
  nearly every time. A call joined by late entry beside another call, whose own LC isn't found at A, F's repeat, or
  the post-access interrupt within 8 superframes (a weak signal, CRC errors), takes the other call's LC: its caller
  and talkgroup show (and the talkgroup filter uses them) until its own LC comes. A call with the other slot's LC gets
  its embedded LC only after those 8 superframes (2.9 s). A talker change with no header and no gap, while the other
  slot's call may go on, keeps the first talker's LC until the voice pauses.
- **LC trace.** Every look at page 2 until the reception's LC is handed over is logged, with the ticks since the
  capture, the voice bursts taken, and page 2's source ID:
  - Debug link log line `dle cap<how> <src>`, where how is `0` (status report), `A` (lone burst A), or `T` (burst
    taken).
  - Debug link log line `dle <ev><res> @<ticks> v<voice> <src>`. ev is `F` (burst F), `f` (F's repeated report), `A`
    (the next burst A), `P` (post-access), `p` (post-access just before the reception), or `H` (header). res is `n`
    (differs from the capture, taken), `s` (same, held back), `v` (same, taken after more than one superframe), `k`
    (differs, kept for the next voice burst), `c` (CRC error flagged), `o` (the other slot's call's, held back), or
    `r` (an interrupted call's, read twice, kept, 5.6).
  - Diagnostics lines `lc n= s= v= c=` (at F); `lc2 a= f= p= k= o= r=` (found at A, at F's repeat, at post-access;
    kept LCs handed over; held back as the other slot's call's; interrupted calls taken back); and
    `lt <how><capture src> <ev><res><ticks>...` (the last reception).
- Talker alias and GPS embedded data (FLCO 4-8) appear in the same page 2 location as the embedded LC.

### 5.3 Timeslot tracking (repeater signals)

The TC bit of the CACH alternates 0/1 slot by slot. At each timeslot interrupt, the driver toggles its own slot number
and compares it with the received TC. The TC read there belongs to the slot that just ended, so the slot starting is
the other one (`HRC6000_DMR_TC_IS_SLOT_NOW` 0). Read as the slot starting, a subscriber through a full duplex repeater
is one slot off both ways: it hears outbound TS2 as TS1 and transmits its TS1 under outbound TS2. Read correctly, a
subscriber on TSx receives outbound TSx and transmits under it: the aligned timing of ETSI TS 102 361-1 5.1.1, table
5.1. The AT for TSx comes in the CACH before the other outbound slot ([dmr-modem.md](dmr-modem.md) 1.6,
[split-duplex.md](split-duplex.md) 5.2).

The driver toggles its slot number at every timeslot interrupt, received or not, and compares only after a received
slot. Locked to one slot, the chip receives one slot in two, and a transmission skips the compare altogether. A number
toggled only at a compare falls out of step and resyncs onto the wrong slot mid-call.

| Parameter | Value |
|---|---|
| agreements needed for "stable" | more than 4 consecutive |
| "locked" | stable, then the first slot whose number isn't the wanted one; from then on receive one slot in two |
| disagreements before resync | more than 3 (each disagreement also takes one agreement away) |

Once locked, write 0x41=0x50 before our slot and 0x41=0x00 before the other. The CACH then isn't refreshed in the other
slot, so the TC compare is meaningful only before the lock. The driver compares only when the slot that ended and the
one before it were both received ([gotchas](gotchas.md) 1.10): after a slot not received, the TC read is junk (about
43% agreement), and such reads would resync a good lock. Once locked, the toggle alone keeps the number until the
reception ends (5.5).

In direct mode there's no CACH: lock to the slot of the first good burst (its number is meaningless), and receive one
slot in two from then on.

### 5.4 Color code acquisition (when the user's CC filter is off)

Reconfiguration keeps any-CC listening. The receive code stays in 0x1F while an aligned start waits for its slot.
Before the first TX slot, the driver writes the channel's transmit code; after TX, it restores the receive code. A
receive filter change or a reconfiguration during TX takes effect when TX finishes: the receiver restarts then (5.1),
in place of a direct transmission's end hold.

The chip deframes only bursts whose CC matches 0x1F, unless 0x41 bit 3 (CC_Match_Ctrl) is set. The driver chooses the
method by the chip description's `cc_any_rx`.

**HR_C6000.** With the filter off, every receive write to 0x41 sets bit 3 (0x58 in place of 0x50), so the chip
deframes every call whatever its CC. The driver only notes the CC seen in 0x52 (two matches within 200 ms) for the
next transmission. It doesn't rewrite 0x1F or restart the receiver: restarting on a CC change loses the first call
after the change (its headers always, its voice in most cases). With bit 3 set, the first call after a change decodes
in full, through a repeater and in direct mode with each call on another CC.

**HR_C5000.** On this chip 0x41 bit 3 makes no difference, so the driver follows an unknown CC:

1. At each system interrupt with an SNR margin of at least 5 dB, compare 0x52's CC with the last one seen (within
   200 ms).
2. On every second match, write the new CC to 0x1F and rewrite 0x40=0xC3 (through a repeater, also the 0x41 resync
   sequence: 0x20, 0x00, 0x20, 0x50), then wait 3 ms. This is a busy wait in the interrupt: faster rewrites crash the
   radio.
3. After 8 matches, the CC is held. Release the hold after 5 s of no received audio while idle.

A call in a CC other than 0x1F's raises no system interrupt on the HR_C5000, so the follow above never starts for it.
The timeslot ticks still come while a signal is present. At each tick while idle and receiving, 0x52 bits 7-4 read
the call's CC on the call's slot phase and other values on the other phase. So the driver counts 0x52's CC per slot
phase (`cc_tick_follow`). After 3 equal reads on one phase that differ from 0x1F's CC, it writes that CC to 0x1F and
restarts the receiver, as for any CC change. It skips this when the CC is held, during TX, while a restart is pending,
and for 4 ticks after one.

### 5.4a CACH short LC

The chip assembles the CACH short LC from four CACHs and leaves it in page 2 0x2B-0x2F: 36 bits, MSB first, the 28
short LC bits, then the CRC-8; the low 4 bits of 0x2F are unused ([hrc6000-registers.md](hrc6000-registers.md) 2.2).

While receiving both slots of a BS signal (`rx_lock_slot(0)`, a trunked control channel) and idle, the driver reads it
at each timeslot interrupt after a received slot, and reports it (`DMR_EV_SHORT_LC`) when the bytes change. It doesn't
wait for 0x86 bit 2 (short LC received): that flag comes with the TX-end interrupt (0x82 bit 5), which the mask leaves
off. The chip descriptor's `slc_rx_addr` gives the location; a chip without one reports none. A TSCC gives about 8
short LCs a second on air, about 0.15% of them with a bad CRC.

### 5.5 End of a reception

| Signal | Rule |
|---|---|
| Terminator with LC, good CRC | ends the call in **direct mode**. Through a repeater, ignore it: the repeater echoes our own terminators, which would end our next reception |
| no valid burst for 13 timeslot interrupts while receiving (about 390 ms) | call over; restart the receiver. The restart drops the chip's sync, so it costs more than the wait ([gotchas](gotchas.md) 1.12) |
| no valid burst for 20 timeslot interrupts while idle | restart the receiver |
| an aligned start (through a repeater) waiting for its slot, no valid burst for 8 timeslot interrupts and no slot timing | restart the receiver, every 8 interrupts until the slot is found or the start times out at 1.5 s ([gotchas](gotchas.md) 1.13). Diagnostics: `rs ... a` |
| no timeslot interrupt for 200 ms while not idle | the modem has stopped: re-initialize the DMR state and the receiver |
| slot captured but silent for the "capture timeout" (user setting, seconds) through a repeater, no slot filter | switch to the other slot |
| slot sync search timeouts | 2.5 s at start-up, 390 ms (one superframe + one slot) while scanning |

### 5.6 Late entry

The post-access interrupt (0x82 bit 4) comes when the chip acquires a voice call in progress from its embedded
signaling. If the driver is idle, with the CC held and matching, it writes 0x41=0x50 and starts receiving as for a
header. It skips voice until two bursts A have passed, unless the source ID is already known and isn't our own. The LC
comes from page 2 once the embedded LC is complete, subject to the sticky-LC rule (5.2). The driver reads page 2 at
the interrupt and keeps an LC that differs from the capture for the next voice burst (counted as `lc2 p=`, traced as
`P`).

- **HR_C5000.** The interrupt comes once per superframe, just after burst E, and starts the decoder, so it comes
  before the reception. An LC read then that differs from the last capture is kept for the reception's first burst
  (traced `pk`): the capture that follows holds that same LC, and the sticky rule would make it wait for the next A.
- **HR_C6000.** The interrupt usually comes before the next F, and the LC is taken there. On a weak signal it can come
  one superframe later; page 2 then still holds the old LC at F, and the call plays from the superframe after that.

**Joining at burst A.** With no header and no superframe in progress on its slot, a voice sync report (burst A) is
held, not played: it can be noise or the tail of another slot's call. It's taken when the next frame (2 ticks later)
brings a class 0 report for burst B. Otherwise it's dropped (counter `rx_lone`), and the class 0 reports that follow
count as `rx_no_call` until the next A. The B check differs by mode:

| Receiving | B is |
|---|---|
| One color code (0x41 bit 3 clear) | a class 0 report numbered 1 |
| Any color code (bit 3 set, 5.4) | a class 0 report numbered 1, or any class 0 report one frame after the A with voice other than A's (a stale repeat of A's report has A's voice). For a call in a code other than 0x1F's, the chip keeps the voice number of the last call it decoded in full (B-F all reported as F), and the driver can't tell which code the call is in, so the number isn't checked; the frame count numbers the rest (5.2) |

Without the second rule, a scan with digital monitor on passes any call in another color code that it reaches after
the header: the A is dropped as lone and B-F as no-call, superframe after superframe, and the stop times out.

**Resuming a call.** A call that stopped with no terminator and comes back has the sticky LC problem in its worst form:
the LC in page 2 is the call's own, so the sticky rule waits for the A after the first whole superframe. That page 2
still shows the call's LC proves nothing; it would show the same if another caller had started. The post-access
interrupt, read twice, does prove it:

| Rule | Why |
|---|---|
| Two post-access reads, 8-13 ticks apart, both show the LC of a slot's call that is open (no terminator) with voice in the last 50 ticks (1.5 s) | Each read is an LC the chip assembled from what is on air. One read isn't enough: nothing on air shows page 2 is written by the time the interrupt comes, and [gotchas](gotchas.md) 1.4a shows it can lag |
| Any read showing another LC clears the count | That LC follows the normal rules, so another caller is never played or named as the old call |
| A terminator closes the call | The same LC after it is a new call, with the sticky rule |
| An identical LC from a new transmission | Treated as the same call. Same source and destination, so the name shown is right |

This takes back about one superframe of the wait (traced `Pr`, counted as `lc2 r=`). The call stack counts the call
once (`rx_calls_resumed`, [dmr-modem.md](dmr-modem.md) 2.4).

### 5.7 Audio buffering

Decode the 27 bytes as three 20 ms frames.

- If fewer than 5 other voice bursts arrived between two bursts A, insert one frame of silence, so the playback buffer
  recovers instead of stuttering.
- If the PCM buffer is nearly full (fewer than 3 free 20 ms buffers), skip decoding the next 6 frames.
- After a voice-abnormal interrupt on our slot, play silence for the next frame.

The AMBE silence frame (9 bytes, repeated three times per burst) is B9 E8 81 52 61 73 00 2A 6B.

## 6. Transmit

### 6.1 Burst set-up

Set up each burst in the timeslot interrupt **before** its slot:

| Burst | Writes |
|---|---|
| Voice LC header | LC (12 bytes, section 6.4) to page 2 0x00; 0x41=0x80; 0x50=0x10 |
| Voice A..F | AMBE (27 bytes) to page 3 on V_SPI; 0x41=0x80; 0x50 = 0x08 \| (n << 4), n = 0..5. The chip adds the voice sync on A and the EMB + embedded LC fragments on B-F from the LC in page 2 |
| Terminator with LC | LC to page 2 if it changed; silence AMBE to page 3; 0x41=0x80; 0x50=0x20 |
| CSBK / data header / data block / any data type | payload (12, 18 or 24 bytes) to page 2 0x00; 0x41=0x80; 0x50 = DT << 4 (LCSS 0) |
| Slot not ours | 0x41=0x00 |

The CRC depends on 0x40 bit 3 (CRC_MCU_Control):

- Clear: the chip computes the CRC (masked per data type) for headers, CSBKs, and LCs.
- Set: the chip sends the 96 info bits exactly as written. The MCU supplies the CRC, and bad CRCs go out as given.

Change bit 3 only when the next burst needs a different setting (read-modify-write of 0x40). The chip still does the
FEC (BPTC, trellis), slot type, and sync in both cases. Restore bit 3 to 0 at the end of the transmission; rewriting
0x40=0xC3 does it. Bit 3 also affects the receiver's deframer: inbound headers decoded while the chip frames a burst
with bit 3 set come out bad.

The idle message has its own place in the TX RAM: page 2 from 0x18. Writing it there keeps the LC at 0x00 for the
voice bursts that follow.

### 6.2 Starting a transmission

Every transmission writes 0x21=0xA2 (polite to own CC, clear the encode buffer) and 0x22=0x86 (start encode, I2S).

- **Direct mode.** From idle, with the modem interrupts masked, write 0x21 and 0x22 as above, unmask, then write
  **0x40=0xE3** (active timing). The timeslot interrupts start on the chip's own timing; the next one starts the burst
  sequence.
- **Through a repeater, already receiving it** (its hang time or a call). Stay in passive timing. When the slot
  tracking is stable with no disagreement and the slot that just started is the other one, lock to ours, write
  0x21=0xA2, 0x22=0x86, 0x41=0x00, and start the sequence at the next timeslot interrupt.
- **Through a repeater, nothing heard.** Wake it first (section 6.5).
- **Data.** As voice, but the sequence sends the queued bursts instead of headers and voice.

With polite access (0x21=0xA2), the chip refuses to send while our CC is busy on the channel (send-rejected
interrupt). A transmission that must go out in a busy channel (an SFR repeat, a reply in a repeater's hang time on a
single frequency) writes 0x20=0x00 and 0x21=0x02 (impolite).

### 6.3 Voice sequence (one entry per timeslot interrupt)

| # | Next slot | Writes |
|---|---|---|
| 1 | ours | LC to page 2; 0x41=0x80; 0x50=0x10 (header 1) |
| 2 | other | 0x41=0x00 |
| 3 | ours | 0x41=0x80; 0x50=0x10 (header 2) |
| 4 | other | 0x41=0x00; start capturing microphone PCM |
| 5 | ours | 0x41=0x80; 0x50=0x10 (header 3) |
| 6 | other | 0x41=0x00. If PTT is released and the next burst would be A, go to 9 |
| 7 | ours | voice burst n (A..F) with the next encoded AMBE, or silence if the encoder is behind; first superframe of a call with a talker alias: the alias or LC in page 2 (section 6.4) |
| 8 | | back to 6 with n+1 mod 6 |
| 9 | ours | (LC again if talker alias was on) silence AMBE; 0x41=0x80; 0x50=0x20 (terminator) |
| 10 | other | direct: arm the 29 ms PA-off timer, restart the receiver. Repeater: 0x40=0xC3 (passive timing), 0x41=0x00, arm the 29 ms timer. Hold the slot lock on our slot for the capture timeout so a reply comes back on it |
| 11 | ours | the terminator goes out in this slot. Direct: back to idle (clear the slot sync). Repeater: 0x41=0x50, receive |

A transmission is 3 voice LC headers in our slot (180 ms of lead-in), then whole superframes, then the terminator. It
always ends at a superframe boundary: PTT release takes effect only before burst A.

Each 40 ms of PCM (two 20 ms buffers) gives one 9-byte AMBE frame, and the voice burst takes three. Encode in a 1 ms
tick task ahead of the timeslot interrupt; a 4-burst ring of encoded frames is enough. The AMBE codec is a binary
supplied per radio ([AMBE codec](../dev/codec.md)). Don't compile its glue with optimization that changes its inline
assembly: at -Os it crashes DMR TX.

### 6.4 LC content

The LC is 12 bytes to page 2: FLCO (0x00 group, 0x03 private), FID, service options, destination ID (3 bytes), source
ID (3 bytes), and 3 bytes of 0. The chip computes the RS(12,9) parity unless CRC_MCU_Control is set. For privacy, the
FID and service options carry the privacy bit ([dmr-modem.md](dmr-modem.md) 3).

For talker alias, page 2 holds the call LC during the first superframe of each pair. In the other superframe it holds
one embedded data block:

- A talker alias block, cycling the header (FLCO 4) and blocks 1-3 (FLCO 5-7), 7 bytes of text each. The header
  carries the format and length in byte 2, and 6 characters.
- FLCO 8 for GPS (7 bytes): longitude as 25-bit two's complement scaled 2^25/360, latitude 24-bit scaled 2^24/180.

The chip spreads the 72-bit embedded LC over bursts B-E.

### 6.5 Repeater wake-up (BS activation)

1. Write page 2: B8 00 00 00 00 00 00 + source ID (3 bytes) + 00 00 (BS_Dwn_Act CSBK, LB=1, BS address 0), 0x50=0x30
   (CSBK), 0x41=0x80, 0x40=0xE3 (active timing). One CSBK goes out in our slot.
2. At the next timeslot interrupt, arm the 29 ms PA-off timer, and restart the receiver (passive).
3. Wait for the repeater: the received TC must toggle correctly more than 3 times in a row. Then treat it as the
   "already receiving the repeater" case (6.2).
4. With no answer in 600 ms (the retry period), send the CSBK again, up to the codeplug's wake attempt count, then
   fail.

### 6.6 Data transmission

A list of up to 64 bursts (CSBKs, headers, blocks) goes out one burst per slot on our slot, with an idle slot between
(our slot only), starting from the first "our slot" step of the sequence (no voice headers, no terminator). After the
last burst, the end steps 10-11 restore passive timing and the receiver, and clear CRC_MCU_Control. The timeout is
10 s plus 60 ms per burst, which covers a repeater wake-up. A received-data interrupt for a data burst is queued (8
deep is enough; the packet CRC catches overflow) and handled outside the interrupt.

**End of a transmission.** The transmission is over at the timeslot interrupt where our next slot comes with nothing
queued and the end asked for, if the last burst's PA has dropped and the slot starting isn't a TX slot. Waiting for the
interrupt after that one leaves the slot after our last burst closed (0x41 = 0) with the RF still on the carrier, and an
answer that starts as our last burst ends is missed (for example, a link ACK to confirmed data, whose report comes
4.5 ms after the next timeslot interrupt).

In direct mode, the end then holds the receiver instead of restarting it: 0x40 = 0xC3 (passive timing from our active
one), 0x41 = receive, and the software state reset as at a restart. A burst taken holds the receiver as any reception
does. With nothing taken, the restart (reason "tx end") comes from the poll 8 ms after the next timeslot interrupt,
once that slot's report is no longer due (a restart at the interrupt itself loses it), or 35 ms after the last
interrupt if they stop. An answer a slot later, off our timing by a few ms, is taken after the restart: the held timing
gives only late class 0 reports for it. Through a repeater, or after a transmission that set up no burst, the restart
comes at once. Diagnostics: `rs ... h` counts the holds.

With the hold, an answer right after our last burst, one slot later, or five slots later is received on the first
try in most cases.

### 6.7 Which slot to transmit in

Through a repeater, the driver transmits in the channel's timeslot, from the CACH. In direct mode, the chip's own timing
defines the slots, and the "our slot" phase is the one the sequence starts in. Single-frequency repeater users transmit
in the other slot than the one heard: when keyed while hearing the repeater, start the sequence from the step that sets
up the slot heard, so the first burst lands in the opposite slot.

## 7. Timing quirks found on air

1. **"Transmitting" must come from the modem slot state.** The PA and the RF chip's TX mode toggle every burst
   (section 4.3), and putting the RF chip in RX between bursts clears any "transmitting" flag that follows it. A
   status reported from that flag dips to idle for one poll mid-call, and stopping the transmission on it cuts the
   call. Report "transmitting" from the first header's set-up to the end of the terminator's slot (steps 1-11 above,
   plus the data states).
2. **The terminator's slot is step 11, not step 10.** Treating step 11 as "already finished" ends every call before
   its terminator is on air. Only after step 11 is the transmission over.
3. **Ending a call (PTT release or a host's end of call) must not stop the RF.** Clear only the "transmission enabled"
   request (as a PTT release does), and let the sequence finish the superframe and the terminator. Switch the RF to
   receive only once the modem slot state is idle. If a voice frame handed to the modem hasn't been sent yet, wait for
   it, or the modem sends silence in its place.
4. **No start-up silence.** Feeding silence frames before the first real voice delays every call by 360 ms per
   superframe of silence. Start with the real first frame; the 3 headers are the lead-in.
5. **A host's end of call** (terminator) ends the call once the voice buffer is empty, or 360 ms after the last voice
   with none.
6. **Every voice burst number arrives with the voice bit set** (0x9..0xE): matching 1..6 exactly drops every burst.
7. **A second received-data interrupt per voice burst** (the EMB report): de-duplicate within 45 ms.
8. **While the chip transmits, its reports about bursts it hears are unreliable.** That covers the CRC flag, CC and
   sync type, the data type (a second report per slot with another type), and the info bits of headers and
   terminators (bytes 5-11). Every CSBK and data header heard while sending fails its CRC by 11-23 bits spread over
   the burst. A deferred re-read of 0x51 at the next timeslot interrupt often reads 0x00. Treat what is heard while
   transmitting as suspect; an SFR holds data calls until it stops sending.
9. **Bursts in step.** A received-data interrupt 3-5 ms after the timeslot interrupt means the sender is in step;
   25-30 ms means out of step: resynchronize the receiver (section 5.1).
10. **The color code probe busy-waits 3 ms in the interrupt** after rewriting 0x40; don't run it while transmitting.
11. **SPI contention.** Interrupt and thread code share the U_SPI with a simple in-use flag; a transfer attempted
    while another is running is refused (counted on SFR builds). Mask the modem interrupts around thread-level
    transfers.
12. **Retuning the instant "transmitting" clears is safe** once (1)-(3) hold: 0.70-0.73 s after the host writes its
    terminator (buffered voice, the rest of the superframe, and the terminator), never before the terminator is on
    air.

## 8. FM mode on the HR_C6000

The HR_C6000 does the FM modulation (two-point) and the audio paths in FM; the AT1846S does the RF.

**FM TX**: 0x10=0x80, 0xE0=0xC9, 0xE4=0x20+FM mic gain, 0xC2=0x00, 0xE5=0x1A, 0x25=0x0E, 0x26=0xFE, 0x83=0xFF,
0x87=0x00, 0x45 / 0x46 = FM calibration pair, 0x48=0x00, 0x04 = calibration Mod2 offset, 0x49=0xFF, 0x4A=0xFF, 0x35 =
deviation, 0xA0 = CTCSS or DCS deviation, 0x3F=0x04, 0x34=0x3C, 0x3E=0x08, page 1 0x50=0x00 and 0x51=0x00, 0x60=0x80.

| Band / bandwidth | 0x35 deviation | CTCSS 0xA0 | DCS 0xA0 | DTMF 0xA0 |
|---|---|---|---|---|
| UHF (> 300 MHz), 25 kHz | 0x24 | 0x09 | 0x02 | 0x24 |
| UHF, 12.5 kHz | 0x12 | 0x06 | 0x01 | 0x12 |
| VHF, 25 kHz | 0x73 (0x68 is about the distortion limit) | 0x18 | 0x06 | 0x75 |
| VHF, 12.5 kHz | 0x37 | 0x10 | 0x04 | 0x3A |

**FM RX**: 0x60=0x00, 0xE0=0x89, 0x10=0x80, 0x34=0x3C, 0x81=0x19, 0x85=0x00, 0x26=0xFD. Audio feed-through 0x36=0x02
(0x00 mutes); 0x10 goes back to 0x6E for DMR.

**Codec in FM mode.** With 0x10 bit 7 set (FM), the codec's line out carries the chip's own FM receive audio, and 0x36
bit 1 switches it on (the manual: in FM mode, receive opens the voice channel's codec switch). I2S PCM (beeps, prompts)
doesn't reach the line out then, with the bit set or clear. A board whose FM audio comes from elsewhere puts 0x10 back
to DMR after the FM receive rows (`hrc6000_fm_rx_pcm()`), so PCM plays in FM receive as it does in DMR.

**Tones (0xA1 mode).** The driver writes 0x80 for off, 0x88 for CTCSS, 0x84 for DCS, and 0x82 for DTMF. The manual's
register table differs: 0xA1 is `fm_mod`, bit 7 is reserved and must be 0, bit 6 is MSK, and its appendix selects CTCSS
with 0x08, CDCSS 0x04, DTMF 0x02, 2-tone 0x10, and 5-tone 0x20. With bit 7 set (the init's 0x80), a board that feeds
the chip a 450 kHz IF never gets a DMR sync, though the chip's RSSI (0x43 / 0x44) follows the signal. Such a board calls
`hrc6000_tones_off()`, which writes 0x00 after the power-on init.

- CTCSS TX: 0xA1=0x88, 0xA8 = tone index. RX: 0xA1=0x88, 0xA7=0x10 (threshold), 0xD3=0x07 / 0xD2=0xD0 (sampling depth
  2000 = 250 ms at 8 kHz), 0xD4 = tone index. Detected: 0x93 bit 0.
- DCS TX: 0xA1=0x84, 0xA2 = 0x00 inverted / 0x08 normal, 0xAB = code bits 7-0, 0xAC = code bit 8. RX: 0xA1=0x84, 0xA2 =
  0x04 inverted / 0x00 normal, 0xD4 = code bits 7-0, 0xD3 = (code bit 8 << 4) + 0x03, 0xD2=0x20 (depth 800 = 100 ms).
- DTMF TX: save 0xA1, 0xA0, and 0xD1; write 0xA1=0x82, 0xA0 = deviation, 0xA4=0xFA (tone time), 0xA3=0x19 (gap, 2 ms
  units), 0xD1=0x01 (one code), 0xAF = code << 4, then 0x60=0x00 and 0x80 to send. Restore the three registers after
  (the high bits of 0xD1 matter). 0xA4 = 0xFA is 500 ms, not continuous; one code is at most 510 ms and ends in about
  20 ms of silence ([hrc6000-registers.md](hrc6000-registers.md) 7.6).
- Tone table (page 1, extended addresses, value = f x 65536 / 32000, low byte first): 0x11A/0x11B 697 Hz (0x0593),
  0x11C/D 770 (0x0629), 0x11E/F 852 (0x06D1), 0x120/1 941 (0x0787), 0x122/3 1209 (0x09AC), 0x124/5 1336 (0x0AB0),
  0x126/7 1477 (0x0BD1), 0x128/9 1633 (0x0D10). For any DTMF pair, put its values in the 697 and 1209 entries, send
  DTMF "1", and restore the entries after.
- A single tone (1750 Hz burst, roger beep, AFSK; `hrc6000_tx_pair`): 2-tone mode (0xA1 = 0x10, bit 7 kept), every
  entry 0x11A-0x123 set to the tone, 0xA4 = 0xA5 = 0xA6 = 0xFF, 0xA3 = 0, code "A-B" (0xAF = 0x0A, 0xAE = 0xB0): one
  gapless tone of at least 25.5 s. The driver starts the sequence again before it runs out
  ([hrc6000-registers.md](hrc6000-registers.md) 7.8).

### 8.1 FM audio paths in the manual

The manual documents more places FM audio can come from and go to than the codec. The firmware doesn't use these paths
for FM; `tests/hil/test_audio_probe.py` exercises them. On an AT1846S board they behave as follows:

- The RX FM buffers hold 8 kHz big-endian 16-bit receive audio, ungated, after 0x34's de-emphasis and filters, one
  half per 32 ms with 0x82 bit 1 (once 0x81 unmasks it). Section 10.6 uses them.
- Keyed, 0x82 bit 7 comes every 32 ms, and the codec writes its samples into the TX buffers (read back through page 6,
  U_SPI OPMode 110). The TX buffers are a tap: no setting makes them the modulation source, even with the MCU
  rewriting each half within 4 ms.
- MCU audio goes on air through the MOD1 offset 0x04 instead (section 2 table). 0x06 bit 2 switches the FM chain's
  input to the external codec port.
- The prompt tone FIFO drains at 8000 samples a second whether 0x06 bit 1 is set or not, and doesn't reach the
  modulator either, with or without 0xE3 bit 0.
- LINEIN2 (0xE0 bit 5) puts a 2.5-2.6 kHz tone of its own on air when enabled during FM TX: keep it off.

| Item | Manual | Meaning |
|---|---|---|
| FM data RAM | TX RAM table (5.2): "FM data address 1 / 2", 0x030-0x22F and 0x230-0x42F, 512 bytes each, "can be written externally for FM send; voice data can also come internally from the codec". The RX RAM table has the same two buffers | a ping-pong buffer each way over U_SPI (page 2, 11-bit addresses); each half is 256 samples, 32 ms |
| 0x82 in FM mode | bit 7 "sending data request", bit 1 "receiving data", bit 2 "FM function detection" | the buffer handshake. FM RX sets 0x81 = 0x19, which masks bits 7 and 1 |
| 0x06 bit 2 CodeCMode | 0 built-in codec, 1 external codec (appendix A2: chosen for the FM path) | external: the FM chain's audio goes through the external-codec I2S port (pins 17-20: LRCK, BCLK, ADCDAT, DACDAT, chip as master, 8 kHz from 0x30-0x33, BCLK and LRCK enabled by 0x36 bit 7 = 0). That isn't the vocoder I2S port (pins 34-37) the MCU uses; pins 17-20 double as RF enable outputs |
| 0x06 bit 1 OpenMusic | "prompt tone" path (appendix A1) | the MCU writes 16-bit DAC samples over U_SPI (command 0x03, address 0x00, 32 samples per write) into a 64-sample FIFO, polling 0x88 bit 0 every 1 ms. Speaker only |
| 0xE3 bit 0 | "codec internal DAC and ADC pass-through" | loops the DAC into the ADC |
| 0x36 bit 7 | external codec BCLK / LRCK off | |
| 0xD7 bit 1 | msk_voice_send_en: "send FM voice" (vs. MSK signaling), in the MSK controls | no effect on the TX RAM |

## 9. Single-frequency repeater (SFR) facts on the HR_C6000

These facts hold for the SFR of [sfr.md](sfr.md):

- To repeat on the other slot, write the outbound sync pattern (page 1 0x04, for example the direct mode TS2 data
  sync) before keying, and restore the MS pattern after. Save and restore 0x10, and keep 0x11=0x80. Use impolite
  access (0x20=0x00, 0x21=0x02).
- Voice bursts carry the embedded LC from the call's LC, with CRC_MCU_Control clear so that the chip makes the
  checksum. Headers, terminators, CSBKs, and data go with CRC_MCU_Control set (verbatim), except the idle message (no
  checksum: the bit clear, written to page 2 0x18).
- Repeats go out 90.02-90.05 ms after each inbound burst: a burst reported just after the timeslot interrupt that set
  up the next outbound slot goes out in the one after. Bursts are 31.3 ms with 0.2 ms ramps, within 25 Hz.
- 0x10 bit 2 (relay) gives BS framing with a CACH whose AT and TC come from 0x52 (written) and short LC from TX RAM
  0x24 (9.1). An ETSI single-frequency BS uses guard-time framing without a CACH (TS 102 361-1 4.6.2).
- 0x41 bit 3 (CC_Match_Ctrl) lets the chip deframe any CC (5.4).
- The AT1846S TX/RX switch per burst over I2C takes about 0.5 ms from the interrupt.

### 9.1 Continuous BS outbound (split duplex TX half)

`drivers/modem/hrc6000/hrc6000_bs.c` (`CONFIG_HOTSPOT_SPLIT`, [split-duplex.md](split-duplex.md) 5.4) takes the chip
over from the slot engine while the split duplex TX half runs. The engine's three ISRs, its watchdog, halt, and
`is_transmitting` hand over to it while `e->bs` is set.

- **Start.** Write the color code, page 1 0x04 = BS data sync, 0x20 = 0x00, 0x21 impolite, 0x22 = 0x86, 0x41 = 0x00,
  then 0x40 = 0xE3 (own timing: the timeslot interrupt runs free every 30 ms).
- **Each slot.** Every timeslot interrupt while keyed sets up the next slot from that slot's queue, or the idle
  message on page 2 0x18 (counted as an underrun): the MCU CRC bit of 0x40 per burst kind, the payload (voice on page
  3, LC / data on page 2 0x00), 0x41 = 0x80, and 0x50. Page 2 0x00 is shared by both slots, so a slot's embedded LC is
  written back before its voice A-E whenever the other slot's header or data overwrote it. The first burst after a
  key-up is an outbound TS1 slot (outbound slot = ((tick - base) & 1) ? TS1 : TS2).
- **PA.** The PA is keyed once: at the RF TX interrupt, or at the first slot's set-up when that interrupt came before
  it. A key the board couldn't do is tried again at the next slot. The PA drops at the first timeslot interrupt after
  the unkey, or by the watchdog when no timeslot interrupt comes for 200 ms.
- **Stop.** Write 0x10 / 0x11 back to 0x6E / 0x80, the MS sync, and 0x40 = 0xC3; the slot engine restarts the
  receiver.

**The CACH.** The manual has the MCU give the BS downstream CACH's AT and TC in 0x52 (written) and the short LC in TX
RAM 0x24-0x28, for the chip to encode. In layer 2 framing the CACH on air stays all zero whatever is written there, in
every 0x10 / 0x11 / 0x50 combination (9.2). The **physical layer mode** (0x10 = 0x66, LayerMode 00) gives a working
CACH, though the manual documents that mode only for receive:

- The chip modulates TX RAM page 2 as given, 288 bits per slot (24 CACH, 264 burst, MSB of the first byte first), from
  0x00-0x23 and 0x24-0x47 in alternate slots, and takes its half at the slot start.
- The slot interrupt, 0x41 = 0x80, and the PA keying work as in layer 2.
- The driver writes each slot's frame (`bsOutboundRawSlot`) to both halves, because which half is which slot isn't
  known, and writes nothing to 0x50, 0x52, or the vocoder.
- Variant 1 keeps the layer 2 framing (0x52 and the short LC written for every burst) for comparison; variant 255 adds
  up to four raw page 4 pairs at key-up. Host test: `tests/host/hrc6000BsTest.c`.

**The key-up** (physical layer framing). In this mode, the chip raises the RF TX interrupt once per key-up, anywhere in
the slot (5.8-30.1 ms after the timeslot interrupt), not ahead of each slot, and it sends TX RAM whenever the PA is up.
So the PA can come up in the slot before the first frame, or inside the first frames. Three rules keep that from doing
harm on air:

| Rule | Why |
|---|---|
| At the key and while it waits for the TS1 slot, both halves hold an idle TS2 burst with the default CACH (LCSS last, no short LC) | the slot before the first frame otherwise sends whatever TX RAM last held: an out-of-sequence CACH |
| The first two slots set up get the default CACH, not the core's | a CACH clipped by the rising PA can decode as LCSS first; with the core's fragments after it, the decoder assembles a short LC with a bad CRC (about one key-up in three). The next fragment breaks the sequence. The first short LC comes 60 ms later |
| A board whose key-up is slower than the CACH times it from the slot clock (`stm32_hrc_dmr_bs_boundary_us`) | full power after the slot's sync and ahead of the next CACH |

Timed from the slot clock, full power still lands inside the first frames in about half the key-ups, so the timeslot
interrupt's place against the slot on air varies between key-ups. The first burst can then carry a clipped CACH, which
the rules above make harmless.

**Restarting the slot timer** (ALIGN, [split-duplex.md](split-duplex.md) 3.8). Write 0x41 = 0x00, 0x40 = 0xC3, 0x41 =
0x20, 0x00, 0x20, 0x00 (the 5.1 sync drop), then 0x40 = 0xE3 at the chosen time. The first timeslot interrupt comes
1.18-1.24 ms after that write and every 30 ms from it, and none comes during the wait. Clearing TxEn and RxEn (0x40 =
0x03), or 0xC3 then 0xE3 without the sync drop, stops the timer only sometimes: interrupts keep coming during the wait,
and the old phase comes back. The driver takes no interrupt while it waits, and gives the requested slot to the
boundary that the time since the write names (a slot per 30 ms, from 15 ms on). The diagnostics line `bs` shows the
restarts (`rs`), interrupts during waits (`ri`), and the first three after the last write (`r1`-`r3`, µs).

**The layer mode for the whole session.** 0x10 (the physical layer mode) is written at the start, ahead of `0x40 =
0xE3`, and back to 0x6E at the stop, never at a key-up. Written at the first keyed slot, it moves the timeslot
interrupt: the next one comes 12.9 or 19.1 ms after the last, and every one after keeps 30 ms from it. The clock is
then 12-17 ms off the ALIGN grid for the whole call, and the 15 ms same-slot filter counts one slot too many or too
few, so the call goes out in the other grid slot. With the mode set from the start, keyed interrupts stay within
0.6 ms of the grid.

In this mode the restart's first interrupt comes 22-30 ms after the write instead of 1.2 ms, numbered as the slot after
the one named for the write. Writing 0x10 = 0x66 just before or just after the `0xE3` write, or in layer 2 for the stop
and sync drop, changes nothing (25-30 ms). Switched at the new clock's first boundary, the next interrupts come 64-89 ms
after the write. So one restart lands up to a few ms off at random, and learning the delay doesn't help: the core snaps
again until a landing is inside the aligned bound ([split-duplex.md](split-duplex.md) 3.8), and an ALIGN to a grid the
clock is already on restarts nothing.

**No slewing.** The manual lists no slot length trim, so the driver offers no `bs_slew`, and INFO capability bit 2
stays clear. A keyed TX half reports its drift (ALIGN flag 10) and is re-aligned by a restart at the next unkey.

### 9.2 BS framing modes

This table gives what each framing of the continuous outbound puts on air (the variants of
[split-duplex.md](split-duplex.md) 10.1). It's why the driver uses the physical layer mode (9.1).

| Framing | On air |
|---|---|
| Layer 2 framing, 0x10 / 0x11 variants (0x6E, 0x4E, 0x6A; 0x11 0x80, 0x00, 0x40), nothing written to the CACH inputs | the continuous outbound right (30.00 ms, one key-up, BS sync, our color code); the CACH all zero (a valid TACT codeword), TC never alternating |
| The same with AT / TC in 0x52 and the short LC in TX RAM 0x24-0x28 every burst, LCSS also in 0x50 bits 1-0, raw 0x56 / 0x57 | unchanged: CACH all zero, no short LC |
| Layer 2 with Decode_Mode 00 (0x40 = 0xE0), our 288 bits at TX RAM 0x00 | the chip's own framing, CACH all zero |
| Physical layer mode (0x10 = 0x66), our 288 bits at TX RAM 0x00 | exactly our frames (sync, slot type, payload, CACH with TC, LCSS and the short LC fragments; AT following SET_AT), but every other slot only: bursts 60 ms apart, the parity changing between key-ups |
| ... and also at 0x24 | both slots: TC alternating in every slot, AT following SET_AT, every short LC decoded good |
| ... also at 0x30, 0x48, 0x60, page 6 0x00 (not 0x24) | the other slot repeats a stale frame (whatever 0x24 last held) |
| 0x00 and 0x24, voice LC headers queued on TS2 with idle on TS1 | no sync or slot type errors: the half on air isn't disturbed by the next frame written over it at the slot start |

So in the physical layer mode the chip sends TX RAM 0x00-0x23 and 0x24-0x47 in alternate slots and latches its half at
the slot start.

## 10. Physical layer pipe

`drivers/modem/hrc_common/hrc6000_air.c` (`CONFIG_AIR`, derived from `CONFIG_P25`) is the board's air interface
provider (`hal_air.h`): symbols in air order, with soft values and time stamps, for modes the core frames itself.

- The chip runs in its physical layer mode (0x10 bits 4-3 = 00), and only modulates and slices.
- A session halts the slot engine and takes its three interrupt lines and its task. Its state lives in the halted
  engine's event queue (and the soft modem of 10.6 in its transmit queue), so a session costs no RAM of its own.
- Close puts the DMR receive set-up back (0x10 = 0x6E, the MS pattern in the correlator, 0x40 = 0xC3), and the slot
  engine restarts the receiver (5.1).
- The same provider serves the HR_C5000 (10.7). Host test: `tests/host/hrc6000AirTest.c`.

### 10.1 What a profile gets

| Profile | Fit |
|---|---|
| 4800 symbols/s, continuous, 2 or 4 levels | TX |
| ... with a 24-symbol sync of outer symbols only, on air all through a transmission (not `AIR_SYNC_START_ONLY`), recurring every 1728 symbols or less, outer deviation 1700-2900 Hz | also RX, SOFT, NEEDS_SYNC |
| 2400 symbols/s | TX, RATE_DOUBLED (receive at half rate doesn't sync reliably) |
| a profile with TX and no RX above, on a board with the FM copy (10.6) | also the soft modem's receive fit on the copy's band (`air_softmodem_rx_fit`) |
| any shape but RRC 0.2 | SHAPE_APPROX: the pulse is the chip's own |
| transmit deviation outside 1050-2400 Hz (C4FM: of the target below) by more than a tenth | DEV_APPROX |
| TDMA, 1200 symbols/s, 3 levels | none |

The board adds UNTESTED until its `stm32_hrc_dmr_board_t.air_verified` is set.

### 10.2 Receive

Set-up: 0x41 = 0x00, 0x10 = 0x02, the pattern in aux 1:0x47-0x4C, 0x5F = 0xF0, 0x40 = 0x40, 0x41 = 0x40 (soft
records). 0x36 = 0 for the whole session, so nothing of the receiver reaches the codec's line out.

- **Correlator.** The correlator holds one pattern (aux 1:0x47-0x4C, write only, 48 bits as the dibits go on air; the
  DMR value is D5D7F77FD757). Only one of the comparators frames at a time, so the profile's first suitable 24-symbol
  sync goes there. Each inner symbol in the pattern weakens the match: with 4 of 24 it finds about two syncs in three,
  with 8 none at the threshold aux 1:0x54 = 0x78. Lowering the threshold to 0x50 finds them, but triggers on noise.
- **Records.** A record is 144 symbols (30 ms), read as 288 bytes from page 2 0x30 when the system interrupt shows
  0x82 bit 0, 2.4 ms after the timeslot interrupt. 0x51 bits 1-0 = 2 when the correlator fired in it, else 0; 0x5F
  bits 1-0 say which comparator.
- **Symbols.** Each symbol is two bytes: bits 5-0 of the first are the level, 6-bit two's complement; the second only
  repeats the slice. The hard bits the chip slices are 5-20 times worse than slicing the levels, so the driver slices
  its own.
- **Order.** The record's order depends on 0x51 (chip symbol ranges, in air order):

  | 0x51 bits 1-0 | Air order |
  |---|---|
  | 2 (sync) | 0-11, 46-94, 12-16, 22-45, 17-21, 95-143: the pattern starts at air symbol 66 |
  | 0 | 0-11, 36-89, 12-35, 90-143 |

- **Levels.** Levels are about 11 counts per kHz of deviation (outer about ±22 at 1944 Hz), with an offset of a few
  counts that follows the carrier's. The counts per kHz depend on the board's receiver; a board with 1.4 times as
  many puts the inner symbols at the slicer's threshold. So the driver takes the outer level from each detected sync,
  as half the distance between the means of its +3 and -3 symbols: at once on a new signal, and a quarter of the way
  per sync while in step. 11 counts per kHz is the start value, and a sync that reads under half or over twice it is
  ignored. The driver removes a slow average of the offset and scales the outer level to `AIR_SOFT_OUTER`; hard
  symbols are sliced at 0 and two thirds of it. With that, the symbol error rate on a clean C4FM signal is under
  0.1 %.
- **Placement.** A record is always the 144 symbols before its boundary, and the chip moves the boundary by whole
  symbols at a sync. After a signal's first sync, the next record comes 128 symbols later and repeats 16; the first
  sync found in step comes 160 symbols after the record before it, and 16 are lost. The first 4 to 12 symbols of both
  records aren't the signal's. The driver places every record by its boundary's time. Repeated symbols are dropped (a
  chunk shorter than 144). Up to 192 lost symbols are delivered as chunks of erasures (soft 0, `sync_at` -1), so a
  framer loses those symbols and not its frame. That covers the 16, a re-arm's hole (124 symbols with a sync every 6
  records), and one record lost. A step of one symbol is within the interrupt's jitter and is left alone.
- **Start and end.** Nothing is delivered before the first sync. After one, records keep coming for 0.7-0.9 s, and
  for about 2 s after a signal ends. With the pattern every 288, 864, or 1728 symbols, nothing is lost.
- **Re-arm.** The chip's timing takes a step of 0.5 ms but not one of 1 ms; after that it delivers records on its own
  timing and never fires again. The driver re-arms it after K records in a row without a sync (K = the sync spacing in
  records + 1): 0x41 = 0x60, then 0x40 40 ms later. It's back about 0.24 s later, and a re-arm with no signal is
  harmless.
- **Time.** t_us of symbol 0 = the timeslot interrupt before the record - 30 ms + half a symbol. `time_accuracy_us` is
  3000.
- **Chunk flags.** GAP is set on the first chunk after open, `rx_restart`, or a transmission; when more symbols were
  lost than erasures stand in for; and when the task was behind and a record was dropped. FREE_RUN is set when more
  records than the sync spacing passed without a sync. `rssi_dbm` is 0.

### 10.3 Transmit

Set-up at `tx_begin`: 0x41 = 0x00, 0x10 = 0x66, 0x20 = 0x00, 0x21 = 0x02 (impolite), 0x22 = 0x86, the profile's idle
in both TX RAM halves, then 0x40 = 0xE3 (own timing). The first timeslot interrupt comes up to 30 ms later (the
receive set-up's slot clock runs on; about 28.7 ms after a stop with 0x40 = 0x03), then every 30 ms.

- **Halves.** The chip sends TX RAM page 2 0x00-0x23 and 0x24-0x47 in alternate slots, 288 bits each, first bit 7 of
  the first byte, and reads a half while it sends it. A half written at the timeslot interrupt that starts its slot
  loses about its first 32 bits. So each interrupt writes only the half the next slot reads, and 0x41 = 0x80.
  Counting the timeslot interrupts from the 0xE3 write (the first is 1), slot n + 1 is set up at interrupt n and reads
  0x00 when n is odd: the first slot set up reads 0x00. This holds in every key-up, with the first interrupt anywhere
  from 2 to 29 ms after the write, and after transmissions of an odd and an even number of slots. A chunk written to
  the other half goes out in the slot then starting, with its first 32 bits those of the chunk two slots before. 0x42
  doesn't tell the halves apart: it reads 0xA0 at every interrupt of a transmission after the first.
- **Lead-in.** The modulator sends TX RAM only in a transmit slot. In any other slot it rests about 3 kHz below the
  channel. A PA keyed in slot 1 sends that as a bare carrier, and the step onto the channel falls under the first
  chunk's symbols (a C4FM receiver then loses the first unit). So interrupt 1 sets slot 2 up with idle, the PA is
  keyed at interrupt 2 as that slot starts, and the first chunk goes out in slot 3. The PA is held for the whole
  transmission.
- **RF TX interrupt.** In this mode the RF TX interrupt comes once per key-up, anywhere in the slot. It keys nothing
  before slot 2 starts; after that it only retries a key the board couldn't do.
- **Air delay.** A slot boundary to that slot on air is 2.2 ms. `AIR_EV_TX_STARTED` gives the first chunk's first
  symbol by that.
- **Refill.** Each chunk taken from the queue gives `AIR_EV_TX_SPACE` (events in a row are one), so the core refills
  the queue before it runs dry.
- **End.** An empty queue mid-transmission sends idle and gives one `AIR_EV_TX_UNDERRUN`. The last chunk is padded
  with idle, one idle slot follows it, and the PA drops at the next timeslot interrupt, so about 28 ms of idle follow
  the last symbol. Then come `AIR_EV_TX_DONE`, the deviation pair back, and the receive set-up. With no timeslot
  interrupt for 200 ms, the transmission is dropped the same way.
- **Deviation.** Mod1 (0x46) moves the outer deviation linearly, about 23.8 Hz per step, through zero; Mod2 (0x45)
  must keep its ratio to Mod1 (about 0.51). `tx_begin` reads the pair the board set for the channel (calibrated for
  1944 Hz) and scales both by target / 1944: from 0x52 / 0x2A, 1800 Hz is 0x4C / 0x27 and 2400 Hz is 0x65 / 0x34. For
  C4FM, the target is the deviation / 0.946: the chip's pulse is DMR's, and a C4FM receiver reads 0.946 of its outer
  level. 0x04 moves the carrier (about +24 Hz per step); 0x47 and 0x48 do nothing here.

### 10.4 The provider's rules

| Rule (`hal_air.h`) | How |
|---|---|
| 1. Raw receive never to the speaker | open needs the chip in DMR, where the speaker carries the codec's PCM only, and writes 0x36 = 0. The copy set-up (10.6) puts the chip in FM with 0x36 = 0 written first. The board turns the FM feed-through on again only for its audio(true), which the core keeps off for the session |
| 2. Microphone, tones, beeps never to the modulator | in the physical layer mode the modulator is fed from TX RAM only |
| 3. Never keys | the PA goes up only through the board's DMR keying, which refuses unless the core's tx() armed it |
| 4. close restores FM and DMR | the DMR receive set-up and the slot engine again; leaving DMR under a session ends any transmission, and entering again gives the receive set-up back |

### 10.5 Receive limits and transmit rates

- A sender whose symbol clock is 28 ppm off is followed, at 0.4 % symbol errors against 0.1 % with a matching clock.
- A sync that comes off the record grid (for example a header 396 symbols before the first voice unit) isn't detected;
  the re-arm K records later brings the receiver back on the next one.
- Transmit carries C4FM voice with every unit decoded, voice units 180 ms apart, and the outer deviation 7 % over
  1800 Hz with the calibrated pair. A trunked call runs on it ([p25-trunking.md](p25-trunking.md)).
- NXDN ([nxdn.md](nxdn.md)) transmits at both rates: 2400 symbols/s with an outer deviation of 1012 Hz, and 4800
  symbols/s with 2399 Hz. The correlator doesn't receive NXDN: it needs a sync of 24 outer symbols that comes again
  within `RX_SPACING_MAX` (10.2), and NXDN has a 10-symbol frame sync word. A board with the FM copy receives it
  through the copy (10.6); on another board the fit gives NXDN transmit only.

### 10.6 Receive through the FM copy

The FM copy serves a profile the correlator can't take, on a board whose RF chip gives the HR_C6000 the unfiltered
discriminator in its DMR set-up (`stm32_hrc_dmr_board_t.air_tap`). The chip's FM receive RAM is a copy of what its FM
demodulator hears; the driver feeds the core's soft modem (`core/air/air_softmodem.c`,
[air-interface.md](air-interface.md)) from it. Transmit stays as in 10.3, so a profile the chip can't send gets no fit
at all.

Set-up (`copy_on`): 0x41 = 0x00, 0x36 = 0x00, 0x60 = 0x00, 0x34 = 0x00 (every receive audio filter off), 0x10 = 0x80
(FM), 0x81 with bit 1 set (the FM receive RAM's interrupt), 0x83 = 0xFF. 0x34 and 0x81 are read first and written back
at close.

- The RAM is page 2 0x030: two halves of 256 samples, 16 bits big endian, 8 kHz. 0x82 bit 1 comes when a half is full,
  every 32 ms. With 0x10 back at its DMR value the RAM stops filling, so the chip stays in FM for the whole session.
- The interrupt doesn't say which half. The driver reads each half's last sample at every interrupt: the half whose
  last sample changed is the full one. When both or neither changed (a steady input), it's the other half than the
  last one (`half_fixes` counts the times the RAM disagreed with that order).
- The interrupt copies the full half into a ring of two halves, stamped with its time (the last sample is the
  interrupt's, the first 255 samples earlier). A half that finds the ring full is dropped (`overruns`), and the half
  after it starts the soft modem over, as does the first half after open, `rx_restart`, or a transmission.
- The task runs the soft modem on each half. Its state lies in the halted slot engine's transmit queue, at the first
  address aligned for it (the queue is bytes; an unaligned state faults).
- The band is 0 to 3400 Hz. `rx_counts_per_khz` is 0, so the soft modem takes the level from each sync. Positive is
  above the carrier.
- For a transmission, the chip leaves FM for the set-up of 10.3, and `rx_setup` after it is `copy_on` again.
- The same ring is the board's `hal_baseband_t` receive tap while no session is open (`stm32_hrc_baseband`).
- `rx_load` counts the soft modem's time for a session that receives this way, and reports false for one the
  correlator serves.

The codec's PCM stream to the MCU runs on in this set-up (a beep's blocks come at the same rate as in DMR), and decoded
voice goes to the codec's DAC as in DMR. NXDN calls at 4800 and 9600 bit/s decode through it, voice included
(`test_nxdn_rx_from_bench`, `test_nxdn_rx_speech`), and a DMR call is received right after sessions of both kinds.

### 10.7 The HR_C5000

The HR_C5000 user manual has the same physical layer mode: 0x10 LayerMode (bits 4-3) = 00, the 0x82 bit 0 interrupt
for physical layer receive only, the 30 ms timeslot interrupt in the physical layer and data link modes, and 36 bytes
of physical layer data in page 2 (addresses 0-35). The slot engine drives the same 0x40 and 0x41 slot control on both
chips. So the provider runs on the HR_C5000 unchanged, and the chip description (`hrc_chip_t`) gives what differs:

| | HR_C6000 | HR_C5000 | Where |
|---|---|---|---|
| 0x5F sync type (`sync_reg`) | written 0xF0 at `rx_setup` | not written: the comparators run as reset left them | `rx_setup` |
| Transmission start (`tx_begin`) | the chip's own: 0x20 = 0, 0x21 = access, 0x22 = 0x86 (encoder start) | the access policy alone (0x20 = 0, 0x21 = 0x02): the vocoder runs on the MCU | `hrc6000_air_tx_begin` |
| 0x10 after close (`mode_per_channel`) | 0x6E | the chip's `dmr_mode`; the engine writes the channel's value again when it runs | `hrc6000_air_close`, `hrc6000_dmr_run` |
| FM copy (10.6) | yes | none documented: no `air_tap`, so the correlator profiles only | the board |
| Physical layer receive (`air_tx_only`) | yes | none (below): the fit drops `AIR_FIT_RX`, `AIR_FIT_SOFT` and `AIR_FIT_NEEDS_SYNC`, and an open session doesn't write the correlator pattern | `hrc6000_air_fit_tap`, `hrc6000_air_open` |

On the HR_C5000, transmit works: the provider keys P25 and NXDN from the TX RAM halves as on the HR_C6000. Receive
doesn't: in an open session with the receiver on, 0x82 stays 0, and the chip gives no physical layer record
(`airp.records=0`, view `air` in [debuglink.md](debuglink.md)). No value of 0x40, 0x41, or 0x81 starts the records,
and 0x81 reads back 0 after a write. So a radio with this chip transmits the correlator profiles and can't receive
them. The chip description says so (`air_tx_only`), and the provider reports those profiles as transmit only. P25
needs receive, so the radio doesn't offer it; NXDN stays for transmit, and scan skips a channel the radio can't hear.
A board that gives the provider an FM copy (`air_tap`) still receives through the soft modem.

The HR_C5000 manual leaves these out, and the provider uses the HR_C6000 values for them:

- 0x40 = 0x40 / 0x41 = 0x40 for soft records, and the record format at page 2 0x30 (144 levels of 6 bits).
- The correlator pattern at aux 1:0x47-0x4C (the provider writes it at open and the MS pattern back at close).
- The second TX RAM half at page 2 0x24: the manual gives 36 bytes.
- 0x36 = 0 at open (on this chip 0x36 also carries the I2S clock set-up).
- The 0x45 / 0x46 two-point pair and its scale with deviation.

A board with this chip leaves `air_verified` unset, so its fits carry `AIR_FIT_UNTESTED` (10.1).
