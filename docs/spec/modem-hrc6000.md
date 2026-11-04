<!-- SPDX-License-Identifier: MIT -->

# HR_C6000 as a DMR modem

Facts about driving the Hongrui HR_C6000 as a DMR Tier II modem on a board with an STM32 host. Pins and the MCU side
are on the board's hardware page ([feature-matrix.md](../feature-matrix.md) lists the boards); the modem-neutral
behavior a DMR stack needs is in `dmr-modem.md`. This file is register-level: what to write, when, and what the chip
reports.

Full register reference: [hrc6000-registers.md](hrc6000-registers.md)

Sources: the HR_C6000 user manual (Hongrui; register chapter and the "Layer 2" application chapter) and on-air
measurements with an SDR watching (bit exact decode of every burst). Values marked "(unexplained)" are init values the
manual does not explain; keep them. Values marked "(unverified)" have not been confirmed on our hardware.

| Section | Topic |
|---|---|
| 1, 2, 3 | Interfaces, registers, power-on initialization |
| 4, 5, 6 | Interrupts, receive, transmit |
| 7 | Timing quirks found on air |
| 8 | FM mode |
| 9, 10 | Single-frequency repeater facts, open questions |

---

## 1. Interfaces

| Interface | Manual name | Use | Host side |
|---|---|---|---|
| Config SPI | U_SPI | every register page, LC / data RAM, sync pattern | bit-banged by the MCU |
| Voice SPI | V_SPI | the 27-byte AMBE voice frame for each voice burst (page 3) | STM32 SPI, mode CPOL=1 CPHA=0 (first edge), MSB first, prescaler 8 |
| I2S | I2S | 8 kHz PCM between the chip's built-in codec (mic ADC, line-out DAC) and the MCU, which runs the AMBE codec in software | see the board's hardware page |
| PWD pin | PWD | power down; drive low to run, wait 10 ms before the first SPI access | a GPIO |
| TIME_SLOT_INTER | timeslot interrupt | every 30 ms slot boundary once timing exists | an EXTI line, falling edge |
| SYS_INTER | system interrupt | events (burst decoded, TX start/end, late entry, ...) | an EXTI line, falling edge |
| RF_TX_INTER | RF TX interrupt | "key the RF now": comes shortly before each transmitted burst | an EXTI line, falling edge |
| RF_RX_INTER | RF RX interrupt | "RX window now" / end of TX burst | not routed on some boards (an MK22 board has it) |
| RF_TX_EN / RF_RX_EN | chip-timed RF enables | could switch the RF directly | not routed on some boards |

All three interrupt lines share one NVIC priority. On the STM32 radios it is 5, which is more urgent than the USB and
other EXTI lines at 6 (numerically lower is more urgent). Thread-level SPI traffic must not re-enter the handlers. A
thread that touches the modem masks the three EXTI lines and waits until any running handler has finished before it
starts its own transfers.

### 1.1 U_SPI framing

Each transaction is CS low, a command byte, an address, data bytes, CS high. Bytes are MSB first. The bit-banged
master sets MOSI, drives CLK low, then high (data sampled on the rising edge; CLK idles high). Reads sample MISO
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

The manual's command table lists page 2 as the RX ("receive end") RAM and page 6 as the TX ("write end") RAM. On air,
writes to page 2 land in the TX RAM and are sent bit exact. Page 6 has not been tried.

TX RAM layout (the manual):

| Address | Contents |
|---|---|
| 0x00-0x17 | LC, CSBK, data header, data blocks |
| 0x18-0x23 | the idle message |
| 0x24-0x28 | the CACH short LC: 28 bits in 0x24-0x26 and 0x27 bits 7-4, its CRC-8 in 0x28 |
| 0x29-0x2C | voice F's EMB fill |

V_SPI uses the same command / address framing (page 3, address 0x00, then 27 bytes; read command 0x83).

### 1.2 Clock

The PLL settings used match the manual's 29.4912 MHz crystal row (crystal frequency on the boards unverified):
PLL output 49.152 MHz, system clock 9.8304 MHz, codec clock 12.288 MHz, CLKOUT 24.576 MHz.

---

## 2. Registers used (page 4 unless noted)

Names are the manual's. Only the bits we rely on are listed.

| Reg | Name | Bits / values used |
|---|---|---|
| 0x00 | reset (active low per module: DMR, PHY, codec, FM, vocoder IF, MSK, I2S, built-in codec) | 0xFF at init (all running) then 0x2A (partial reset, unexplained) |
| 0x01 | H/W config | 0xF0 in DMR (two-point modulation TX, IF RX); 0xB0 then 0xF8 during init (unexplained). The same value is used on UHF and VHF |
| 0x02-0x05 | TX / RX I and Q offsets | 0 at init; 0x04 is rewritten in FM with the calibration "Mod2 offset". In two-point mode 0x04 is the MOD1 (reference crystal) offset and 0x02 the MOD2 (VCO) one (manual 4.7.2; the register table says MOD2 for both): 0x04 moves the FM carrier about 75 Hz a step, signed (bench 2026-10-12) |
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
| 0x53 / 0x54 (R) | RC info / EMB LCSS | not used |
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
bitwise inverse. Write the MS-sourced data sync for normal operation. The values are the ETSI ones, and the voice syncs
are their inverses.

| Data sync | Bytes |
|---|---|
| MS sourced (normal operation) | D5 D7 F7 7F D7 57 |
| BS sourced | DF F5 7D 75 DF 5D |
| direct mode TS1 | F7 FD D5 DD FD 55 |
| direct mode TS2 | D7 55 7F 5F F7 F5 |

---

## 3. Power-on initialization

PWD low, wait 10 ms, then in this order. Everything is page 4 unless a page is given. "(unexplained)" values are not
explained by the manual; keep them.

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

A page 1 burst from 0x60 (a table of 32 three-byte entries `nn EF 00`) is not needed. Don't send it.

### 3.1 Entering DMR (after FM, or at every DMR channel change)

0x01=0xF0, 0x45 / 0x46 = the DMR modulation pair from calibration, **0x10=0x6E**, 0xE0=0xC9, 0xE4=0x20+DMR mic gain,
0x26=0xFD. Then restart the receiver (section 5.1). In operation 0x10 is 0x6E, not the 0x6B written at init.

### 3.2 Flushing stale vocoder audio

Set the RX gain to -15 dB (any lower and the flush doesn't take), 0x06=0x22, 128 x 0x7F to page 3, 0x06=0x20,
restore the RX gain.

---

## 4. Interrupts

### 4.1 Timeslot interrupt (every 30 ms)

The interrupt comes at each slot boundary once the chip has timing. Passive timing (0x40=0xC3) derives the timing
from received bursts. When the signal goes away, it keeps counting from the last sync. Corrections within 1.25 ms are
applied in real time. Active timing (0x40=0xE3) generates the timing locally. The interrupt stops only when TxEn and
RxEn are both cleared.

At every timeslot interrupt the MCU decides the **next** slot and writes 0x41. For TX it also writes the payload and
0x50. These writes must land **before the slot after the next starts**. In the manual's timing, the chip starts
framing the next slot's burst at t1, and the MCU must write its data in the window t2 before t1. The driver writes
everything at the start of the timeslot handler, and the burst goes out in the following slot. A late setup slips the
burst by one slot (30 ms) or by a frame (60 ms).
While transmitting, the slot between our bursts gets 0x00. An SFR writes 0x50 there instead, because it hears its
inbound slot in that slot (`hrc6000_dmr_tx_listen`, `sfr.md` 3).

The CACH TC bit (0x52 bit 2) can be read here. The chip refreshes it only in slots being received. It is the TC of
the slot that just ended, not the one starting (5.3).

### 4.2 System interrupt

Read 0x82 (flags) and 0x52 (CC / CACH) first, handle each set flag, then write the value read back to 0x83.

| 0x82 bit | Meaning (DMR) | Sub-status | Use |
|---|---|---|---|
| 7 | send request rejected (channel busy and the access policy is polite) | none | the transmission didn't go out |
| 6 | TX start | 0x84: 7 voice, 6 OACSU, 5 encryption key update, 4 vocoder config done, 3 data, 2 partial retransmission, 1 retransmission, 0 vocoder init | read and ignored |
| 5 | TX end | 0x86: 7 service complete, 6 fragment-length ack sent, 5 OACSU timeout, 4 layer 2 "last moment to set up the next slot", 3 confirmed fragment sent, 2 short LC received, 1 BS activation timeout | read and ignored |
| 4 | post access (late entry) | none | a voice call already in progress has been acquired from its embedded signaling: start receiving as if a header had been heard |
| 3 | received data ("control frame parsed") | 0x51 | a burst was decoded: section 6 |
| 2 | received information (layer 3 service data) | 0x90 | read and ignored |
| 1 | voice abnormal exit | 0x98 | voice sync lost on slot 0 (bit 1) or slot 1 (bit 2): replace the next voice frame of that slot with silence |
| 0 | physical layer receive | none | not used |

The mask 0x81=0x19 enables bits 4, 3 and 0. Bit 7 (send rejected) still shows set in 0x82 when 0x82 is read on
another interrupt. The SFR debug build counts these. So 0x82 shows more flags than the mask lets interrupt.
Whether bit 7 raises the line by itself is unverified.

**Two interrupts per voice burst.** A voice burst produces one received-data interrupt for its EMB (SyncClass 0) and
one for the burst. A voice handler takes a burst number 1..6 at most once per 45 ms. It drops a second report that
comes within 45 ms of the last voice burst taken.

**Timing of a burst report.** For a burst received in step with the chip's slot timing, the received-data interrupt
comes 3-5 ms after the timeslot interrupt (2-10 ms observed). A delay of 25-30 ms means the chip's timing is out of
step with the sender, because it locked to noise or to another station. The receiver should then be resynchronized.

### 4.3 RF TX interrupt

The interrupt comes before each burst the chip is about to send. The handler keys the PA and switches the RF chip to
TX. On a board without the RF RX interrupt the MCU ends each TX burst itself. At the next timeslot interrupt, if the PA
has been on for more than 20 ms, the MCU turns it off and puts the RF chip back in RX. After the last burst of a
transmission, a one-shot 29 ms timer does the same. Keying takes the PA enables high and writes AT1846S register
0x30 = 0x60C6 (digital TX). Un-keying takes them low and writes 0x30 = 0x7026 (25 kHz) or 0x6026 (12.5 kHz). A board may key a fixed delay after the interrupt (a timer, 1 ms on one board) and switch from RX straight to TX or through idle; its hardware page has the sequences.

On such a board the RF chip's "transmitting" state therefore goes on and off **burst by burst**. Code that needs to
know whether a transmission is in progress must use the modem slot state (section 7.5), not the PA.

---

## 5. Receive

### 5.1 Restarting the receiver

0x40=0xC3, then 0x41=0x20, 0x41=0x00, 0x41=0x20, 0x41=0x50. (The manual's recipe: if the received CC doesn't match,
write 0x20 to drop the sync and search again.) Used after leaving TX in direct mode, after a call ends, after
color-code changes, and on any loss of state.

### 5.2 Per burst (received-data interrupt)

Read 0x51 and 0x5F (and 0x52 already read):

| Field | From | Notes |
|---|---|---|
| sync class | 0x51 bits 1-0 | 0 none (EMB report), 1 voice, 2 data, 3 RC |
| data type / voice burst | 0x51 bits 7-4 | data: the ETSI data type 0..15. voice: low 3 bits 1..6 = A..F |
| CRC error | 0x51 bit 2 | for voice it reflects the EMB / embedded checks; for data the burst CRC |
| PI | 0x51 bit 3 | the privacy indicator; bursts with it set are dropped by the voice path |
| color code | 0x52 bits 7-4 | |
| sync type | 0x5F bits 1-0 | MS / BS / TS1 direct / TS2 direct |

Payloads: page 2 from 0x00. 12 bytes for an LC (voice header, terminator, or the embedded LC once assembled), CSBK,
MBC, data header and rate 1/2 blocks; 18 bytes for rate 3/4 (the chip decodes the trellis code); 24 bytes for rate 1.
Voice: 27 bytes from page 3 on V_SPI (three 72-bit AMBE+2 frames with their FEC, as the codec expects).

- **Mode from sync type**: BS sync means a repeater; anything else is direct mode. The receiver follows it unless the
  channel forces direct mode (and always stays direct for SFR).
- **Data blocks have no CRC of their own** (the packet CRC-32 covers them): take rate 1/2, 3/4 and 1 blocks even when
  the CRC flag is set, and flag them. Drop CSBK, MBC and data headers with a CRC error.
- **Data on our slot only**: through a repeater, accept a data burst only when the slot number (from the TC bit) is
  ours and it differs from the previous burst's slot (a second report in the same slot is a duplicate). In direct
  mode, accept the bursts of the slot the receiver is locked to.
- **The LC registers are sticky.** Page 2 keeps the previous call's LC until the new one is decoded. On a new
  reception, the first LC read may be the last call's. Either wait for two superframes (burst A seen twice after the
  slot lock) before trusting it, or capture page 2 when the reception starts and accept an LC only when it differs
  from that capture, or after more than one superframe of voice.
  The driver captures page 2 at the first report of a quiet period (a status report before any burst is taken, for a
  late entry), else at the first burst.
- The voice header's LC and the terminator's LC are read at their bursts; during voice, the embedded LC appears in
  page 2 once the chip has assembled it from bursts B-E. Working conclusion from HIL 2026-10-10 (c8c9b23): not yet at
  F's first report (the EMB one): a new LC was first seen there at the second F. So until a reception has its LC the
  driver also reads page 2 at the next burst A, at F's repeated report and at the post-access interrupt (an LC that
  differs there is handed over with the next voice burst).
- **LC trace** (for the bench): every look at page 2 until the reception's LC is handed over is logged, with the ticks
  since the capture, the voice bursts taken and page 2's source ID. Debug link log lines
  `dle cap<how> <src>` (how: `0` status report, `A` lone burst A, `T` burst taken) and
  `dle <ev><res> @<ticks> v<voice> <src>` (ev: `F` burst F, `f` F's repeated report, `A` the next burst A, `P`
  post-access, `p` post-access just before the reception, `H` header; res: `n` differs from the capture, taken; `s`
  same, held back; `v` same, taken after more than one superframe; `k` differs, kept for the next voice burst; `c` CRC
  error flagged). Diagnostics lines `lc n= s= v= c=` (at F), `lc2 a= f= p= k=` (found at A, at F's repeat, at
  post-access; kept LCs handed over) and `lt <how><capture src> <ev><res><ticks>...` (the last reception).
- Talker alias and GPS embedded data (FLCO 4-8) appear in the same page 2 location as the embedded LC.

### 5.3 Timeslot tracking (repeater signals)

The TC bit of the CACH alternates 0/1 slot by slot. At each timeslot interrupt the driver toggles its own slot number
and compares it with the received TC. The TC read there belongs to the slot that just ended, so the slot starting is
the other one (`HRC6000_DMR_TC_IS_SLOT_NOW` 0). Read as the slot starting, a subscriber through a full duplex
repeater was one slot off both ways: it heard outbound TS2 as TS1 and transmitted its TS1 under outbound TS2 (bench,
2026-10-27). Read right, a subscriber on TSx receives outbound TSx and transmits under it: the aligned timing of ETSI
TS 102 361-1 5.1.1, table 5.1. The AT for TSx comes in the CACH before the other outbound slot (`dmr-modem.md` 1.6,
`split-duplex.md` 5.2).

The driver toggles its slot number at every timeslot interrupt, received or not, and compares only after a received
slot. Locked to one slot, the chip receives one slot in two, and a transmission skips the compare altogether: toggled
only at a compare, the number fell out of step and resynced onto the wrong slot mid-call.

| Parameter | Value |
|---|---|
| agreements needed for "stable" | more than 4 consecutive |
| "locked" | stable, then the first slot whose number is not the wanted one; from then on receive one slot in two |
| disagreements before resync | more than 3 (each disagreement also takes one agreement away) |

Once locked, write 0x41=0x50 before our slot and 0x41=0x00 before the other. The CACH is then not refreshed in the
other slot, so the TC compare is only meaningful before the lock. The driver compares only when the slot that ended and the one before it were both
received (gotchas 1.10): after a slot not received the TC read is junk (43% agreement on the bench), and those reads
used to resync a good lock. Once locked, the number is kept by the toggle alone until the reception ends (5.5).

In direct mode there is no CACH: lock to the slot of the first good burst (its number is meaningless) and receive one
slot in two from then on.

### 5.4 Color code acquisition (when the user's CC filter is off)

The chip only deframes bursts whose CC matches 0x1F unless 0x41 bit 3 is set. To follow an unknown CC: at each system
interrupt with an SNR margin of at least 5 dB, compare 0x52's CC with the last one seen (within 200 ms). On every
second match write the new CC to 0x1F and rewrite 0x40=0xC3 (through a repeater also the 0x41 resync sequence: 0x20,
0x00, 0x20, 0x50), then wait 3 ms (a busy wait in the interrupt: faster rewrites crashed the radio). After 8 matches
the CC is held. Release the hold after 5 s of no received audio while idle.

### 5.5 End of a reception

| Signal | Rule |
|---|---|
| Terminator with LC, good CRC | ends the call in **direct mode**. Through a repeater, ignore it: the repeater echoes our own terminators, which would end our next reception |
| no valid burst for 13 timeslot interrupts while receiving (about 390 ms) | call over; restart the receiver |
| no valid burst for 20 timeslot interrupts while idle | restart the receiver |
| no timeslot interrupt for 200 ms while not idle | the modem has stopped: re-initialize the DMR state and the receiver |
| slot captured but silent for the "capture timeout" (user setting, seconds) through a repeater, no slot filter | switch to the other slot |
| slot sync search timeouts | 2.5 s at start-up, 390 ms (one superframe + one slot) while scanning |

### 5.6 Late entry

The post-access interrupt (0x82 bit 4) comes when the chip acquires a voice call in progress from its embedded
signaling. If idle, with the CC held and matching: write 0x41=0x50 and start receiving as for a header. Skip voice
until two bursts A have passed, unless the source ID is already known and isn't our own. The LC comes from page 2 once
the embedded LC is complete (subject to the sticky-LC rule above). The driver reads page 2 at the interrupt and keeps an
LC that differs from the capture for the next voice burst (counted as `lc2 p=`, traced as `P`); whether the interrupt
comes on a late entry, and when relative to burst F, is what the trace shows.

### 5.7 Audio buffering

Decode the 27 bytes as three 20 ms frames. If, between two bursts A, fewer than 5 other voice bursts arrived, insert
one frame of silence so the playback buffer recovers instead of stuttering. If the PCM buffer is nearly full (fewer
than 3 free 20 ms buffers), skip decoding the next 6 frames. After a voice-abnormal interrupt on our slot, play
silence for the next frame. AMBE silence frame (9 bytes, repeated three times per burst): B9 E8 81 52 61 73 00 2A 6B.

---

## 6. Transmit

### 6.1 Burst set-up

For each burst, in the timeslot interrupt **before** its slot:

| Burst | Writes |
|---|---|
| Voice LC header | LC (12 bytes, section 6.4) to page 2 0x00; 0x41=0x80; 0x50=0x10 |
| Voice A..F | AMBE (27 bytes) to page 3 on V_SPI; 0x41=0x80; 0x50 = 0x08 \| (n << 4), n = 0..5. The chip adds the voice sync on A and the EMB + embedded LC fragments on B-F from the LC in page 2 |
| Terminator with LC | LC to page 2 if it changed; silence AMBE to page 3; 0x41=0x80; 0x50=0x20 |
| CSBK / data header / data block / any data type | payload (12, 18 or 24 bytes) to page 2 0x00; 0x41=0x80; 0x50 = DT << 4 (LCSS 0) |
| Slot not ours | 0x41=0x00 |

CRC: with 0x40 bit 3 (CRC_MCU_Control) clear the chip computes the CRC (masked per data type) for headers, CSBKs and
LCs; with it set the 96 info bits are sent exactly as written (the MCU supplies the CRC; bad CRCs go out as given).
Change bit 3 only when the next burst needs a different setting (read-modify-write of 0x40). The chip still does the
FEC (BPTC, trellis), slot type and sync in both cases. Restore bit 3 to 0 at the end of the transmission: rewriting
0x40=0xC3 does it. Bit 3 seems to affect the receiver's deframer too: inbound headers decoded while the chip was
framing a burst with bit 3 set came out bad.

The idle message has its own place in the TX RAM: page 2 from 0x18. Writing it there keeps the LC at 0x00 for voice
bursts that follow.

### 6.2 Starting a transmission

Common: 0x21=0xA2 (polite to own CC, clear the encode buffer), 0x22=0x86 (start encode, I2S).

- **Direct mode**: from idle, with the modem interrupts masked: 0x21, 0x22 as above, unmask, then **0x40=0xE3** (active
  timing). The timeslot interrupts start on the chip's own timing; the next one starts the burst sequence.
- **Through a repeater, already receiving it** (its hang time or a call): stay in passive timing; when the slot
  tracking is stable with no disagreement and the slot that just started is the other one, lock to ours, write
  0x21=0xA2, 0x22=0x86, 0x41=0x00, and start the sequence at the next timeslot interrupt.
- **Through a repeater, nothing heard**: wake it (section 6.5) first.
- **Data**: as voice, but the sequence sends the queued bursts instead of headers and voice.

Polite access: with 0x21=0xA2 the chip refuses to send while our CC is busy on the channel (send-rejected interrupt).
A transmission that must go out in a busy channel (an SFR repeat, a reply in a repeater's hang time on a single
frequency) writes 0x20=0x00 and 0x21=0x02 (impolite).

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

So a transmission is 3 voice LC headers in our slot (180 ms of lead-in), then whole superframes, then the terminator.
It always ends at a superframe boundary: PTT release takes effect only before burst A.

Encoding: each 40 ms of PCM (two 20 ms buffers) gives one 9-byte AMBE frame; the voice burst takes three. Encode in a
1 ms-tick task ahead of the timeslot interrupt; a 4-burst ring of encoded frames is enough. The AMBE codec code is a binary blob supplied per radio (`codec-placement.md`); its glue must not be compiled with
optimization that changes its inline assembly (it crashed DMR TX at -Os).

### 6.4 LC content

12 bytes to page 2: FLCO (0x00 group, 0x03 private), FID, service options, destination ID (3 bytes), source ID (3
bytes), 3 bytes 0. The chip computes the RS(12,9) parity unless CRC_MCU_Control is set. For privacy, the FID and
service options carry the privacy bit (see `dmr-modem.md`).

Talker alias: during the first superframe of each pair, page 2 holds the call LC; in the other, one talker alias block,
cycling header (FLCO 4) and blocks 1-3 (FLCO 5-7), 7 bytes of text each (the header carries the format and length in
byte 2 and 6 characters), and FLCO 8 for GPS (7 bytes: longitude as 25-bit two's complement scaled 2^25/360, latitude
24-bit scaled 2^24/180). The chip spreads the 72-bit embedded LC over bursts B-E.

### 6.5 Repeater wake-up (BS activation)

1. Write page 2: B8 00 00 00 00 00 00 + source ID (3 bytes) + 00 00 (BS_Dwn_Act CSBK, LB=1, BS address 0), 0x50=0x30
   (CSBK), 0x41=0x80, 0x40=0xE3 (active timing). One CSBK in our slot.
2. Next timeslot interrupt: arm the 29 ms PA-off timer, restart the receiver (passive).
3. Wait for the repeater: the received TC must toggle correctly more than 3 times in a row. Then treat it as the
   "already receiving the repeater" case above.
4. No answer in 600 ms (the retry period): send the CSBK again, up to the codeplug's wake attempt
   count, then fail.

### 6.6 Data transmission

A list of up to 64 bursts (CSBKs, headers, blocks) goes out one burst per slot on our slot, with an idle slot between
(our slot only), starting from the first "our slot" step of the sequence (no voice headers, no terminator). After the
last burst, the end steps 10-11 restore passive timing / the receiver and clear CRC_MCU_Control. Timeout: 10 s plus
60 ms per burst (covers a repeater wake-up). A received-data interrupt for a data burst is queued (8 deep is enough;
the packet CRC catches overflow) and handled outside the interrupt.

### 6.7 Which slot to transmit in

Through a repeater: the channel's timeslot, from the CACH. In direct mode the chip's own timing defines the slots and
the "our slot" phase is the one the sequence starts in. Single-frequency repeater users transmit in the other slot
than the one heard: when keyed while hearing the repeater, start the sequence from the step that sets up the slot
heard, so the first burst lands in the opposite slot.

---

## 7. Timing quirks found on air

1. **"Transmitting" must come from the modem slot state.** The PA and the RF chip's TX mode toggle every burst
   (section 4.3), and putting the RF chip in RX between bursts clears any "transmitting" flag that follows it. A status
   reported from that flag dipped to idle for one poll mid-call, and stopping the transmission on it cut the call.
   Report "transmitting" from the first header's set-up to the end of the terminator's slot (steps 1-11 above, plus
   the data states).
2. **The terminator's slot is step 11, not step 10.** Treating step 11 as "already finished" ended every call before
   its terminator was on air (24 of 24 calls lost their end on the SDR). Only after step 11 is the transmission over.
3. **Ending a call (PTT release or a host's end of call) must not stop the RF.** Only clear the "transmission
   enabled" request (as a PTT release does) and let the sequence finish the superframe and the terminator. Switch the
   RF to receive only once the modem slot state is idle. If a voice frame handed to the modem hasn't been sent yet,
   wait for it, or the modem sends silence in its place.
4. **No startup silence.** Feeding silence frames before the first real voice delayed every call by 360 ms per
   superframe of silence. Start with the real first frame; the 3 headers are the lead-in.
5. **A host's end of call** (terminator) ends the call once the voice buffer is empty, or 360 ms after the last voice
   with none.
6. **Every voice burst number arrives with the voice bit set** (0x9..0xE): matching 1..6 exactly drops every burst.
7. **A second received-data interrupt per voice burst** (the EMB report): de-duplicate within 45 ms.
8. **While the chip transmits, its reports about bursts it hears are unreliable**: the CRC flag, CC and sync type,
   the data type (a second report per slot with another type), and the info bits of headers and terminators (bytes
   5-11). Every CSBK and data header heard while sending failed its CRC by 11-23 bits spread over the burst. A
   deferred re-read of 0x51 at the next timeslot interrupt often reads 0x00. Treat what is heard while transmitting as
   suspect (an SFR holds data calls until it stops sending).
9. **Bursts in step**: a received-data interrupt 3-5 ms after the timeslot interrupt means the sender is in step;
   25-30 ms means out of step: resynchronize the receiver (section 5.1).
10. **The color-code probe busy-waits 3 ms in the interrupt** after rewriting 0x40; don't run it while transmitting.
11. **SPI contention**: the U_SPI is shared by interrupt and thread code with a simple in-use flag; a transfer
    attempted while another is running is refused (counted on SFR builds). Mask the modem interrupts
    around thread-level transfers.
12. **Retuning the instant "transmitting" clears is safe** once (1)-(3) hold: measured 0.70-0.73 s after the host
    writes its terminator (buffered voice + the rest of the superframe + the terminator), never before the terminator
    is on air.

---

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
(0x00 mutes); 0x10 back to 0x6E for DMR.

**Tones (0xA1 mode)**: 0x80 off, 0x88 CTCSS, 0x84 DCS, 0x82 DTMF.
- CTCSS TX: 0xA1=0x88, 0xA8 = tone index. RX: 0xA1=0x88, 0xA7=0x10 (threshold), 0xD3=0x07 / 0xD2=0xD0 (sampling depth
  2000 = 250 ms at 8 kHz), 0xD4 = tone index. Detected: 0x93 bit 0.
- DCS TX: 0xA1=0x84, 0xA2 = 0x00 inverted / 0x08 normal, 0xAB = code bits 7-0, 0xAC = code bit 8. RX: 0xA1=0x84, 0xA2 =
  0x04 inverted / 0x00 normal, 0xD4 = code bits 7-0, 0xD3 = (code bit 8 << 4) + 0x03, 0xD2=0x20 (depth 800 = 100 ms).
- DTMF TX: save 0xA1, 0xA0 and 0xD1; 0xA1=0x82, 0xA0 = deviation, 0xA4=0xFA (tone time, continuous), 0xA3=0x19 (gap,
  2 ms units), 0xD1=0x01 (one code), 0xAF = code << 4, 0x60=0x00 then 0x80 to send. Restore the three registers after
  (the high bits of 0xD1 matter).
- Tone table (page 1, extended addresses, value = f x 65536 / 32000, low byte first): 0x11A/0x11B 697 Hz (0x0593),
  0x11C/D 770 (0x0629), 0x11E/F 852 (0x06D1), 0x120/1 941 (0x0787), 0x122/3 1209 (0x09AC), 0x124/5 1336 (0x0AB0),
  0x126/7 1477 (0x0BD1), 0x128/9 1633 (0x0D10). A single arbitrary
  tone: put its value in both the 697 and 1209 entries and send DTMF "1"; restore the entries after.

### 8.1 FM audio paths in the manual (unverified)

What the manual documents about where FM audio can come from and go to, beyond the codec. None of it is used yet;
`tests/hil/test_audio_probe.py` tries it. Measured on an AT1846S board: the RX FM buffers hold 8 kHz big endian 16-bit receive audio, ungated, after 0x34's de-emphasis
and filters, one half per 32 ms with 0x82 bit 1 (once 0x81 unmasks it); keyed, 0x82 bit 7 comes every 32 ms and the
codec writes its samples into the TX buffers (read back through page 6, U_SPI OPMode 110): they are a tap, and no
setting tried (525, with the MCU rewriting each half within 4 ms) makes them the modulation source. MCU audio goes on
air through the MOD1 offset 0x04 instead (section 2 table), and 0x06 bit 2 does switch the FM chain's input to the
external codec port. The prompt tone FIFO drains at 8000 samples a second
whether 0x06 bit 1 is set or not, and doesn't reach the modulator either, with or without 0xE3 bit 0. LINEIN2 (0xE0
bit 5) puts a 2.5-2.6 kHz tone of its own on air when enabled during FM TX: keep it off.

| Item | Manual | Meaning |
|---|---|---|
| FM data RAM | TX RAM table (5.2): "FM data address 1 / 2", 0x030-0x22F and 0x230-0x42F, 512 bytes each, "can be written externally for FM send; voice data can also come internally from the codec". The RX RAM table has the same two buffers | a ping-pong buffer each way over U_SPI (page 2, 11-bit addresses). If the samples are 16-bit at 8 kHz, each half is 256 samples, 32 ms. Format, rate and the bit that picks the RAM over the codec are not stated |
| 0x82 in FM mode | bit 7 "sending data request", bit 1 "receiving data", bit 2 "FM function detection" | the buffer handshake. FM RX sets 0x81 = 0x19, which masks bits 7 and 1 |
| 0x06 bit 2 CodeCMode | 0 built-in codec, 1 external codec (appendix A2: chosen for the FM path) | external: the FM chain's audio goes through the external-codec I2S port (pins 17-20: LRCK, BCLK, ADCDAT, DACDAT, chip as master, 8 kHz from 0x30-0x33, BCLK and LRCK enabled by 0x36 bit 7 = 0). That is not the vocoder I2S port (pins 34-37) the MCU uses; pins 17-20 double as RF enable outputs, and their wiring on our radios is unknown |
| 0x06 bit 1 OpenMusic | "prompt tone" path (appendix A1) | the MCU writes 16-bit DAC samples over U_SPI (command 0x03, address 0x00, 32 samples per write) into a 64-sample FIFO, polling 0x88 bit 0 every 1 ms. Speaker only |
| 0xE3 bit 0 | "codec internal DAC and ADC pass-through" | loops the DAC into the ADC. With the I2S or prompt-tone path feeding the DAC, this may put MCU audio into the FM transmit chain |
| 0x36 bit 7 | external codec BCLK / LRCK off | |
| 0xD7 bit 1 | msk_voice_send_en: "send FM voice" (vs. MSK signaling), in the MSK controls (earlier notes said 0xDB, which the manual doesn't list) | bench 2026-10-12: no effect on the TX RAM |

---

## 9. Single-frequency repeater (SFR) facts on the HR_C6000

From our SFR work (on air in MMDVM SFR):

- Repeating on the other slot: write the outbound sync pattern (page 1 0x04, e.g. direct mode TS2 data sync) before
  keying and restore the MS pattern after; save and restore 0x10; keep 0x11=0x80; impolite access (0x20=0x00,
  0x21=0x02); voice bursts with the embedded LC from the call's LC (CRC_MCU_Control clear so the chip makes the
  checksum); headers / terminators / CSBKs / data with CRC_MCU_Control set (verbatim), except the idle message (no
  checksum: bit clear, written to page 2 0x18).
- Measured: repeats 90.02-90.05 ms after each inbound burst (a burst reported just after the timeslot interrupt that set
  up the next outbound slot goes out in the one after), bursts 31.3 ms with 0.2 ms ramps, within 25 Hz.
- 0x10 bit 2 (relay) gives BS framing with a CACH whose AT / TC come from 0x52 (written) and short LC from TX RAM
  0x24 (9.1). An ETSI single-frequency BS uses guard-time framing without a CACH (TS 102 361-1 4.6.2).
- 0x41 bit 3 (CC_Match_Ctrl) lets the chip deframe any CC while keyed (experiment switch, effect unconfirmed).
- The AT1846S TX/RX switch per burst over I2C takes about 0.5 ms from the interrupt.

### 9.1 Continuous BS outbound (split duplex TX half)

`drivers/modem/hrc6000/hrc6000_bs.c` (`CONFIG_HOTSPOT_SPLIT`, `split-duplex.md` 5.4) takes the chip over from the slot
engine while the split duplex TX half runs; the engine's three ISRs, its watchdog, halt and `is_transmitting` hand
over to it while `e->bs` is set. Start: color code, page 1 0x04 = BS data sync, 0x20 = 0x00, 0x21 impolite, 0x22 =
0x86, 0x41 = 0x00, then 0x40 = 0xE3 (own timing: the timeslot interrupt runs free every 30 ms). Every timeslot
interrupt while keyed sets up the next slot from that slot's queue (or the idle message on page 2 0x18, counted as an
underrun): the MCU CRC bit of 0x40 per burst kind, the payload (voice on page 3, LC / data on page 2 0x00), 0x41 =
0x80 and 0x50. Page 2 0x00 is shared by both slots, so a slot's embedded LC is written back before its voice A-E
whenever the other slot's header or data overwrote it. The first burst after a key-up is an outbound TS1 slot
(outbound slot = ((tick - base) & 1) ? TS1 : TS2). The PA is keyed once, at the first RF TX interrupt, and dropped
at the first timeslot interrupt after the unkey, or by the watchdog when no timeslot interrupt came for 200 ms. Stop:
0x10 / 0x11 back to 0x6E / 0x80, MS sync, 0x40 = 0xC3, the slot engine restarts the receiver.

The CACH: the manual has the MCU give the BS downstream CACH's AT and TC in 0x52 (written) and the short LC in TX RAM
0x24-0x28, and the chip encode it. On air (2026-10-12) the layer 2 CACH stays all zero whatever is written
there, in every 0x10 / 0x11 / 0x50 combination tried (9.2). What works is the **physical layer
mode** (0x10 = 0x66, LayerMode 00), which the manual documents only for receive: the chip then modulates TX RAM page
2 as given, 288 bits per slot (24 CACH, 264 burst, MSB of the first byte first), from 0x00-0x23 and 0x24-0x47 in
alternate slots, and takes its half at the slot start. The slot interrupt, 0x41 = 0x80 and the PA keying work as in
layer 2. The driver writes each slot's frame (`bsOutboundRawSlot`) to both halves, since which half is which slot
isn't known, and nothing to 0x50, 0x52 or the vocoder. Variant 1 keeps the layer 2 framing (0x52 and the short LC
written for every burst) for comparison; 255 adds up to four raw page 4 pairs at key-up. Host test: `tests/host/hrc6000BsTest.c`.

**Restarting the slot timer** (ALIGN, `split-duplex.md` 3.8; bench 2026-10-21): 0x41 = 0x00,
0x40 = 0xC3, 0x41 = 0x20, 0x00, 0x20, 0x00 (the 5.1 sync drop), then 0x40 = 0xE3 at the chosen time. The first
timeslot interrupt comes 1.18-1.24 ms after that write and every 30 ms from it, and none comes during the wait. Clearing
TxEn and RxEn (0x40 = 0x03) or 0xC3 then 0xE3 without the sync drop stopped the timer only sometimes: interrupts kept
coming during the wait and the old phase came back. The driver takes no interrupt while it waits and gives the
requested slot to the first boundary after the write; the diagnostics line `bs` shows the restarts (`rs`), interrupts
during waits (`ri`) and the first three after the last write (`r1`-`r3`, µs).

**No slewing.** The manual lists no slot length trim, so the driver offers no `bs_slew` and INFO capability bit 2
stays clear: a keyed TX half reports its drift (ALIGN flag 10) and is re-aligned by a restart at the next unkey.


### 9.2 BS framing bench results (2026-10-11 and 12)

467.375 MHz, lowest power, 10 s and about 335 bursts per run, with the framing variants of
[split-duplex.md](split-duplex.md) 10.1 and `sdrmon.py analyze --cach` on the SDR bench:

| Build / variant | On air |
|---|---|
| Layer 2 framing, 0x10 / 0x11 variants (0x6E, 0x4E, 0x6A; 0x11 0x80, 0x00, 0x40), nothing written to the CACH inputs | the continuous outbound right (30.00 ms, one key-up, BS sync, our color code); the CACH all zero (a valid TACT codeword), TC never alternating |
| The same with AT / TC in 0x52 and the short LC in TX RAM 0x24-0x28 every burst, LCSS also in 0x50 bits 1-0, raw 0x56 / 0x57 | unchanged: CACH all zero, no short LC |
| Layer 2 with Decode_Mode 00 (0x40 = 0xE0), our 288 bits at TX RAM 0x00 | the chip's own framing, CACH all zero |
| Physical layer mode (0x10 = 0x66), our 288 bits at TX RAM 0x00 | exactly our frames (sync, slot type, payload, CACH with TC, LCSS and the short LC fragments; AT following SET_AT), but every other slot only: 168 bursts, 60 ms apart, the parity changing between key-ups |
| ... and also at 0x24 | both slots: 335 bursts, TC alternating 334 of 334, AT following SET_AT, 83 short LCs decoded, none bad |
| ... also at 0x30, 0x48, 0x60, page 6 0x00 (not 0x24) | the other slot repeats a stale frame (whatever 0x24 last held) |
| 0x00 and 0x24, voice LC headers queued on TS2 with idle on TS1 | 168 idle (B), 154 idle and 15 headers (A), no sync or slot type errors: the half on air isn't disturbed by the next frame written over it at the slot start |

So in the physical layer mode the chip sends TX RAM 0x00-0x23 and 0x24-0x47 in alternate slots and latches its half
at the slot start. Earlier runs also found every fourth TS2 burst sent as a zeroed PI header, a driver bug (gotchas
8.14).

---

## 10. Open questions

- Page 6 (manual: TX RAM) vs page 2 for TX: page 2 works; page 6 untested.
- The meaning of the unexplained init values (0x10=0xF3 / 0x6B during init, the 0x11-0x3C bulk block, the page 1 tables).
- Whether the post-access interrupt is reliable for data calls (it is a voice mechanism in the manual).
- Crystal frequency on the boards (the PLL values match 29.4912 MHz).
- Passive full duplex (manual 5.4.4) for a true SFR / repeater: not tried.
