<!-- SPDX-License-Identifier: MIT -->

# Gotchas: DMR modem and radio bring-up

What goes wrong when a radio is brought up, and what code has to do so it doesn't. Each entry gives the symptom, the
cause, the rule for new code and the chips it applies to. Entry numbers are referenced from code and other pages, so
they stay fixed.

Entries name the chips they apply to: HR_C6000 with the AT1846S (STM32F405 or MK22 hosts), HR_C5000 (STM32F405, single
band) and HR_C7000 (C-SKY CK803S SoC). Which radios carry which chips is in [radios.md](radios.md), and what has been
checked on each is in [feature-matrix.md](../feature-matrix.md). A fact about one radio is on its hardware page.
Status words: "on air" means verified with an SDR or a second radio; "built" means compiled and host tested only.

| Section | Topic |
|---|---|
| 1, 2 | DMR modem timing: HR_C6000, HR_C7000 |
| 3 | TX/RX state and PA handling |
| 4, 5 | AMBE codec calling, data bursts, CRCs and rates |
| 6, 7 | Privacy, ARS, TMS, LRRP, UDT |
| 8, 9 | Hotspot / MMDVM, USB and CDC-NCM |
| 10, 11, 12 | Flash and storage, calibration, memory and RAM limits |
| 13, 14 | Build and toolchain, SFR |
| 15, 16, 17 | Testing practice, input, scan |

## 1. DMR modem timing: HR_C6000

**1.1 Startup silence before the first voice frame.**
Symptom: every hotspot call went on air about 1.2 s longer than its audio; the host's buffer didn't drain for
1.2-1.4 s after keying. Cause: six silence frames (360 ms, one superframe) were queued before the first voice frame, on
top of a 360 ms network timeout and a 720 ms stop delay. Rule: start voice straight after the voice LC headers; never
pad a transmission with silence superframes. Radios: all HR_C6000 / HR_C5000 / HR_C7000.

**1.2 The chip raises more than one Sys (RX) interrupt per slot.**
Symptom: a voice B reported 12 ms after a header, a header twice 26 ms apart, a "terminator" right after voice A that
ended a call early. Cause: the EMB parsing interrupt (sync class 0, data type 0x0D / 0x0E) and other extra reports
arrive for the same slot, sometimes with a wrong type. Rule: accept only the first report per slot; drop any report
within 45 ms of the last one (28 ms when listening to both slots); never map data types 0x0D / 0x0E to voice E / F.
Radios: HR_C6000 confirmed on air; the HR_C7000 hook applies the same 45 ms rule.

**1.3 Voice burst numbering lives in the low 3 bits.**
Symptom: after tightening the voice filter to data types 1-6, no RF voice reached the host at all. Cause: real voice
bursts are reported with bit 3 set (0x9-0xE). Rule: take the burst position (A-F = 1-6) from the low 3 bits, and filter
duplicates by timing (rule 1.2), not by the high bit. Radios: HR_C6000; HR_C5000 numbering (1-6 or 0-5) still unknown
on air.

**1.4 The decoded LC at the first header is still the previous call's.**
Symptom: calls went up to the network under the last talker's IDs. Cause: the chip's LC buffer is only refreshed once
the timeslot is agreed; on the first header it holds the old call. Late entry from the embedded LC has the same trap
(the stale embedded LC of the previous call). Rule: take a call's LC from the header burst's own info bits; for late
entry accept an embedded LC only when it differs from the stale one or is confirmed. Radios: HR_C6000, HR_C5000,
HR_C7000.

**1.5 Burst timing comes from the timeslot interrupt; a burst is "in step" 2-10 ms after it.**
A received burst is reported 3-5 ms after the timeslot interrupt when the chip's slot timing matches the call, and
25-30 ms after when it doesn't (locked to noise before the call). Rule: time every RX report against the timeslot
interrupt; if it is out of step, resynchronize the receiver instead of transmitting on that timing (repeats came out
68 ms instead of 90 ms after the inbound burst, over the next one). Radios: HR_C6000.

**1.6 The DMO receiver locks to one slot.**
Symptom: whole header runs and data calls lost (only 8 bursts of a call reported). Cause: in direct mode the driver
writes "no receive" for the other slot; locked to the wrong slot (noise, a preamble) it misses everything until its
390 ms timeout. Rule: anything that must hear both slots (repeater, scanner, DCDM) keeps the chip receiving every slot
and derives slot parity itself from timeslot interrupt counts.

**1.7 The color code probe busy-waits in the ISR.**
Cause: a first RX path writes reg 0x40 and spins 3 ms inside the Sys interrupt to probe the color code. Rule: no
busy waits in modem interrupts; never probe while transmitting. Radios: HR_C6000; the HR_C7000 equivalent was removed
too.

**1.8 Access policy: polite to own color code after the first TX.**
Symptom: a repeater (SFR) refused to send while the inbound slot was busy. Cause: after the driver has transmitted,
reg 0x21 = 0xA2 makes the chip polite to our color code, and it rejects the send ("send request rejected"
interrupt). Rule: decide politeness explicitly per transmission (regs 0x20 / 0x21); repeater transmissions and replies
in a repeater's hang time must be impolite.

**1.4a The embedded LC reaches page 2 after F's first report (working conclusion, unconfirmed).**
Symptom: a late entry with a new talker (an LC different from the chip's sticky one) was taken a superframe late, at
the second F (13 of 36 bursts played instead of 18-19; HIL 2026-10-10 on c8c9b23). Cause, as far as one run shows: the
driver reads page 2 at F's first report (the EMB one) and page 2 still held the old LC then. Rule: until a reception
has its LC, look at page 2 again at the next burst A (and at F's repeated report and the post-access interrupt); the
driver logs every look (`dle` lines, `lt` diagnostics line) so the next HIL run shows exactly when the LC appeared.
Radios: HR_C6000.

**1.9 Restore reg 0x10 to the driver's value, not the init value.**
After a special mode, writing back the init value (0x6B) instead of the running value (0x6E: relay, aligned, slot 1)
broke subsequent operation. Rule: save and restore modem mode registers around any temporary change.

**1.10 The CACH TC is valid only after two received slots in a row.**
Symptom: through a duplex repeater, the slot lock built from the TC (0x52 bit 2) resynced on its own while the
outbound was steady, and a lock that was right was thrown away. Cause: once locked, the driver closes the other slot,
so every later compare reads the TC after a slot not received. On the HR_C6000 those reads agreed with the lock 43% of
the time (5485 reads), against 557 of 557 for reads after two received slots (bench, 2026-10-28). The HR_C5000 has
the same weakness in another form: its TC stops changing while a slot is closed (`tc_last_rx`). Rule: compare only
when the slot that ended and the one before it were both opened for receive; after our own transmission, a closed
slot or a lock, skip the first read. Radios: HR_C6000, HR_C5000.

## 2. DMR modem timing: HR_C7000

**2.1 The TX interrupts must be hooked.** Without the TX timeslot interrupt (vector 62) and the RF TX interrupt (vector 64) connected, the TX handler is never called and DMR TX never starts. TX states must run on the
TX timeslot interrupt.

**2.2 Write the modem registers once at init.** RF_CONTROL 0x41F1A; RF_MODE stays 0x03269860. Entering DMR turns the PA
bias DAC on and pulses WORK_MODE 0x80 then 0x6E (layer 2, slots chosen by LAYER2_TXRX_CTRL; layer 3 never used). RX
(re)start uses LAYER2_SLOTON with TXRX 0x800000 / 0x40 and VOICE_EMB_CTRL 2; TX start sets LAYER2_CONTROL 0xC3, with no
bit 31 write. Confirmed on a radio.

**2.3 Interrupt masking.** Disable / enable the modem interrupts through PIC_MASK bits 29-31 and PIC_MASK_1 bit 0, not
vector numbers 93-95. Setting PIC_MASK_1 bits 29-31 (vectors 93-95) in a "critical section" masks nothing at all.

**2.4 PA per burst by a one-shot timer.** PA band select and bias are switched per burst by a one-shot timer, with a
configurable delay (default 28 ms, range 1-35 ms); the FD6818 must be put into TX for DMR and the audio path pin 0x2E
set per mode. Repeater wake is the BS activation CSBK sent in two consecutive slots with the PA keyed.
A wake burst sent with the PA unkeyed (the repeater hears a bare carrier). RF_CONTROL 0x41F1A (was
0x40A0A) moves the RF TX interrupt from about 1.0 ms to 3.1 ms before the burst so the FD6818 has time to switch; the PA timer uses Timer4 because Timer3 drives the ADC. TX needs active timing: LAYER2_CONTROL 0xC3 at TX start
(0xC0 is passive, for RX); arm RX with LAYER2_TXRX_CTRL 0x40, not 0x8040. TX audio encoding must run from the tick, or the transmission is silence.

**2.5 Burst numbering and CRCs.** The C7000 reports the current voice burst as 1-6 for A-F (manual); it adds the CRC of
CSBKs, MBC headers and data headers itself (write 80 bits), data blocks go as given; received data bursts come from
receive RAM at 0x160004B0.

**2.7 Set the XIP flash mode even when the PLL is already running.** The vendor bootloader leaves the PLL on but the
flash controller in single-bit mode at divider 4. Skipping quad mode and divider 2 with the PLL steps made all code run
from flash about 6 times slower: the idle radio used about 80% of the CPU and voice encoding starved the other tasks.
Undo both before a soft reset or the stub entry: the soft reset keeps the divider, and the ROM can't read flash in
single-bit mode at divider 2.

**2.8 Don't set a receive UART's baud rate while its line is held low.** The UART ignores a line control write while
it is busy, and an unpowered GPS module holds its RX line low, a break the UART keeps receiving. Setting the baud rate
again when switching the GPS back on left the UART deaf on a board with a GPS module. Set it up once, before the module is first
powered; to switch on again, clear the RX FIFO and status (`hrc_uart_rx_clear`) and enable the interrupt.

The host port can't wait for a quiet line: it is set up at every boot, and after a reset the host may still be
sending (the next request of a test run). On a board whose host port is a UART, a reset with the host polling left the debug link
deaf until a power cycle (bench 2026-10-22; quiet resets came back every time): the divisor write was dropped and the
low divisor byte went out as data. So every HR_C7000 UART set-up (`platform/hrc7000/uart_format.h`) drains the RX FIFO, writes,
reads LCR, DLL and DLH back, and tries again until they stick (about 100 ms at most).

## 3. TX/RX state and PA handling

**2.6 HR_C5000 register differences.** Configuration page 0 (page 4 on the HR_C6000); reg 0x10 = 0x6B simplex,
0x4F repeater (never the C6000's 0x6E, which audio routing code wrote mid-DMR); no 0x21 / 0x22 vocoder writes at TX
start (set once at init); no reg 0x5F, so DMO / RMO come from the channel; the MCU must write the embedded LC LCSS in
reg 0x50 for voice B-E (0x08 0x19 0x2B 0x3B 0x4A 0x58 for A-F) or receivers can't assemble the embedded LC; regs
0x14-0x16 hold the radio's own DMR ID (a fixed value is wrong); voice SPI is hardware SPI2 with chip select on
PB12, 27 bytes per burst with command 0x03 / 0x83 (the flash SPI bus never reaches the chip); TX
reg 0x26 = 0xFE with DMR_SW low, RX 0x26 = 0xFD. Running on a radio since (see the feature matrix).

**3.1 "Transmitting" must come from the modem's slot state, not the PA.**
Symptom: GET_STATUS dipped to idle for one poll during a call; the end of every call was lost. Cause: on the STM32
radios the PA is switched burst by burst and the RX switch between our own bursts cleared the "transmitting" flag (it
followed the PA); the hotspot then disabled transmission while the chip still had the rest of the superframe and the
terminator queued. Rule: derive "transmitting" from the modem's slot state machine (TX_START_1 through TX_END_3_DMO);
end a transmission like a PTT release (clear the request) and switch to receive only once the modem is idle. Radios:
all; on air on an HR_C6000 board (24 of 24 calls clean).

**3.2 TX_END_3 is the terminator's own slot.**
Symptom: treating TX_END_3 as "finished" made every call end "lost" (24 of 24) on the SDR. Rule: the transmission is
over only after the TX_END_3 slot has gone by. (wrong), 7df0f50 (revert).

**3.3 Don't reset the DMR state machine at every TX/RX switch.** On the HR_C5000 the RF TX / RX helpers re-ran the full
DMR mode set (resetting the chip's DLL / state machine) right after the TX ISR had started a transmission. Rule: mode
setup once per mode change; per-burst switching only touches the RF registers.

**3.4 PA off timing.** Ending a burst at "the first timeslot interrupt more than 20 ms later" has an unknown phase on
the HR_C5000; a 30 ms one-shot timer from the RF TX interrupt is the approach used, with the timeslot interrupt as a 35 ms fallback. A board that doesn't route the RF RX interrupt to the MCU can only drop the PA at
the next timeslot interrupt (about 1.3 ms into the following slot); a board that does route it can end the burst on
time.

**3.5 The AT1846S +3 kHz TX offset is a sequencing hack.** The driver adds 3 kHz to the TX frequency to compensate an
offset caused by turning the receiver on between the frequency write and TX. A switch that writes the frequency with
the receiver off and goes straight to TX must not add it (bursts were +2.93 kHz off). Rule: measure TX frequency per
switching sequence; don't carry a fixed offset. RFoundry's direct sequence (RX straight to TX, no idle, which a
single frequency repeater needs) corrects the same shift with the HR_C6000 MOD1 offset, because the AT1846S applies a
frequency write only coming out of idle.

**3.6 Listen before talk for machine traffic.** Data, ACKs and CSBK answers collided with the far end. Rule: direct
mode: no voice, data burst or carrier for 120 ms; repeater: 180 ms without data bursts and wait for the hang time to
end. Replies give up after 3 s, new traffic after 10 s. The modem "slot state" end-of-call timeout is voice fade
tolerance (200-400 ms) and is not a channel-clear signal.

**3.7 Only saved settings count for background services.** ARS registered from an unsaved menu edit, then deregistered
when the user backed out. Rule: services read committed settings, never in-progress edits.

**3.8 Report "our call ended" from the modem state, not from PTT.** Location-after-call reports trigger on the end of
the voice TX state; a flag must keep the end of a data transmission from triggering another report, and none are sent
in hotspot mode.

**3.9 Data TX deadline.** 10 s plus 60 ms per burst covers repeater wakeup; abort if wakeup fails. In repeater mode
queue a data burst only on the channel's timeslot and once per slot.

**3.10 AT1846S voice source is reg 0x3A bits 14:12.** Symptom: APRS AFSK and the 1750 Hz burst keyed a carrier with
no tone (SDR bench: 0.6 s at 145.090 MHz with about 25 Hz RMS deviation; the burst's 300-3000 Hz band 226 Hz peak while
the CTCSS on the same carrier showed 2 kHz). Cause: `tx_tone` wrote the tone select into bits 6:4, which are squelch
and sub-audio detect enables; voice_sel (bits 14:12: 000 nothing, 001 tone1, 010 tone2, 011 both, 100 microphone,
AT1846S programming guide, TX voice channel select) stayed 000, and the HR_C6000 microphone input was muted. Rule:
tones go out with 0x3A bits 14:12 = 001 and 0x7A bit 14 (single tone enable), both put back after. Radios: any
AT1846S board where the HR_C6000 modulates the voice.

**3.11 Don't double the AT1846S tone deviation for 25 kHz.** The 25 kHz mode already doubles FM deviation. The
bandwidth table also doubled reg 0x59 bits 5:0 (0x0B90 to 0x0BA0), so CTCSS went out 4.0× the 12.5 kHz level (366 to
1464 Hz, SDR bench). With 0x0B90 for both it's 368 and 733 Hz (2.0×). `test_fm_tx_bandwidth` checks the ratio.

**3.12 Every TX state needs its own way out.** Symptom: on an HR_C5000 board, PTT on a simplex DMR channel left the radio
keyed for good: no RF, PTT release, keys and the time-out did nothing, only removing the battery helped. Cause: the
HR_C5000 gave no timeslot interrupts, so the modem queued the headers and the first voice burst and never sent one.
PTT release and the time-out only ask for the terminator at the next superframe boundary, and with no burst going
out that boundary never came; the only tx_done guard was in the ending state, which the call never reached. Rule:
every TX state ends on its own when the modem stops. Voice now aborts after `DMR_CALL_TX_STALL_MS` (1.5 s) without a
burst taken (`dmr-voice.md` 2). Radios: all; seen with the HR_C5000. Tests: `test_tx_modem_stalled` (dmrCallTest),
`test_dmr_stalled` (radioCoreTest).

**3.13 A crash or a reset must not leave the PA keyed.** A radio faulted while transmitting; the crash capture
soft-reset the chip, the radio came back stopped before the display was lit and drew about 15 W with the PA keyed
until a power cycle (2026-10-21). Neither the fault path nor the reset turned the transmitter off, and the board's
pin set-up ran after the clock set-up. Rule: every fault handler, crash hook, software reset and stub jump calls
`board_rf_safe()` first, and so does `main()` before the clocks ([crash-log.md](crash-log.md) 8). A new board must
provide it, and a new reset or jump path must call it.

## 4. AMBE codec calling

**4.1 The STM32 codec glue must be built at -O0.** The codec entry calls were inline assembly that depended on the
unoptimized stack frame; at -Os DMR voice TX crashed the radio. Rule for new code: call the codec through a proper ABI
shim (explicit function pointers or a hand-written assembly veneer with declared clobbers), never through inline asm
that assumes a frame layout. Radios: STM32 boards; MK22 likewise.

**4.2 HR_C7000: inline asm with unbound register variables.** Same class of bug: replaced by calls through C function
pointers at fixed offsets from the codec base (entry 0, encode 0x260, ECC 0x1E34 from 0x03061000).

**4.3 De-interleave table error (arr2[12] = 45 should be 46).** One entry duplicated another, so bit 46 (the MSB of
the C1 Golay(23,12) codeword) was never written and the decoder ran on uninitialized stack; Golay hid it at the cost of
one of C1's three correctable errors. Rule: build the 72-bit AMBE+2 de-interleave from ETSI / mbelib and test that the
tables cover all 72 codeword bits exactly once. Radios: all.

**4.4 No memory clobber around the codec.** The codec call asm had no "memory" clobber; anything that modifies codec
buffers between calls (privacy XOR on the 49 parameter bits, between the second encode call and the ECC call) needs a
compiler barrier.

**4.5 Codec presence check by signature word.** The firmware enables DMR only if the codec's first word matches (STM32:
0x4FF0E92D, a Thumb-2 push at the codec entry). Without the codec the build is FM only and must say so loudly. Rule:
verify the codec region by hash or signature at boot and at build time; never ship an image with the codec, never commit one.

**4.6 Codec placement is per board.** The merge tool moves the codec to where the board's memory map puts it and
rewrites the absolute addresses inside it ([codec-placement.md](codec-placement.md)), so a position copied from a
vendor image is wrong for a RadioFoundry build. The real flash budget is the gap before the codec, not the linker's
percent used. Each radio must use the codec from its own vendor firmware.

**4.7 MK22 .sgl container truncation.** The old bin2sgl header (from 3.1.8) stops at 0x77001 bytes and cuts off
the end of the codec section (to 0x7C090); the V4.3.6 header allows 0x78801.

## 5. Data bursts, CRCs and rates

**5.1 Send data with CRC_MCU_Control (HR_C6000 reg 0x40 bit 3).** With the bit set the chip sends the 96 info bits as
given (it still does FEC, slot type, sync), so any CSBK / header / block and its CRC can be sent. Clear it for bursts
without a checksum (idle). It also appears to affect the deframer: received headers decoded while the chip framed a
burst with the bit set came out corrupt.
The state that rewrites reg 0x40 at the end of a transmission also clears the bit, so any cached copy of it must be
invalidated at the start of each burst list.

**5.2 HR_C5000 and HR_C7000 add CSBK / header CRCs themselves.** Write 10 bytes (80 bits) for CSBK, MBC header and data
header; blocks go as given (12 / 18 / 24 bytes). There is no CRC_MCU_Control on the HR_C5000. Consequence: a burst with a deliberately wrong or
non-standard CRC can't be sent on those chips (the chip "fixes" it).

**5.3 Rate 1 has no FEC.** Rate 1 packets fail on a single RF bit error (TMS 1 of 3 over a real path); Rate 3/4 is
Trellis coded and corrected 1-2 bit errors per block on air. Rule: default to Rate 1/2, offer 3/4, use Rate 1 only on
clean links.

**5.4 Data blocks without their own CRC only count inside a data transmission.** Noise decodes as Rate 1/2 / 3/4 / 1
blocks. Accept blocks only within 400 ms of a header, CSBK, MBC or previous block. Equally, never drop received data blocks on the modem's per-burst CRC flag: the packet CRC-32 decides
(455e466).

**5.5 Data list sizing.** A data header must always set how many bursts follow (preamble count + header's blocks); the
old code compared with the preamble's count and stopped after the header (blocks never sent). A list goes out when
complete or 180 ms after its last frame, max 64 bursts (`DMR_DATA_MAX_BURSTS`); frames that can't be sent are NAKed, not silently ACKed.

**5.6 TMS ACK sequence number is 7 bits.** The ACK carried only the low 5 bits, so senders numbering past 31 never
matched. Bytes must equal a MOTOTRBO capture (example 00 04 9F 00 95 20 for s/n 53), sent as its own transmission after
the ETSI response, with preambles.

**5.7 Reply queue, not a single slot.** ACKs written by the modem task and read by the main task in one slot were
overwritten by a second message. Use a single-producer / single-consumer queue (4 entries usable with one slot kept
free, entries older than 5 s dropped), with compiler barriers so the entry is written before the index store.

**5.8 Duplicate message suppression.** A sender that missed our ACK resends: ACK again but don't store or ring again
when radio, sequence number and text match within 60 s.

## 6. Privacy

**6.1 Apply the keystream where the burst position is known.** Static keystream schemes (Motorola BP, Anytone 16-bit
BP) XOR the 49 AMBE+2 parameter bits by superframe position: on TX when the burst is handed to the modem, on RX before
the vocoder, after the FEC decode. The privacy voice LC uses FID 0x10 and service options 0x40.

**6.2 Burst numbering must be right for privacy.** A wrong A-F numbering (1-6 vs 0-5) breaks decryption and voice frame
A; it is unverified on the HR_C5000 and HR_C7000.

**6.3 ISR-safe contact lookup.** The privacy contact's ID must come from a cache that is safe to read in the modem ISR.

**6.4 PI headers are dropped by the receive path.** MI-based schemes need the modem to send and receive a PI header
(data type 0); late entry in burst F is an open risk. Research note, privacy feasibility.

## 7. ARS, TMS, LRRP, UDT

**7.1 ARS has no standard destination.** Every system programs its own "ARS Radio ID"; with none set, send nothing.
Registration and deregistration are unconfirmed private IPv4/UDP 4005 at Rate 1/2 with two preambles.

**7.2 Bounded retries.** MOTOTRBO retries forever, which floods a channel with no server. Retry 30 s, 1, 2, 4, 8 min
(6 tries), then wait for a change.

**7.3 ARS response details.** A response with the ACK/NAK bit (0x40) set is a refusal. The octet after the header is the
refresh time in 30-minute units; 0 means no refresh expected; no octet means the 30-minute default.

**7.4 Power-off deregistration needs time on air.** Hold the power-off screen up to 3 s until the deregistration is on
air (listen before talk can hold it); a supply cut can't do it. Tests: `testArsPowerOff`
(dmrDataServiceTest), `test_power_off_ars` (radioCoreTest).

**7.5 Settings without a reset.** Old builds reused spare settings bits and struct tail padding (with a static size
assertion) to add fields without wiping user settings; where the layout couldn't absorb them the settings magic was
bumped and settings reset once (magic 0x4781, struct 0x70 to 0x80). New firmware: version its settings and migrate
instead of resetting.

**7.6 UDT location and LRRP.** UDT NMEA: DPF 0, format 5, opcode 0x1A, one Rate 1/2 block, CRC-CCITT unmasked (an assumption;
Motorola / Hytera receivers may differ). Positions older than 30 s go out without the fix flag. LRRP
periodic interval minimum 5 s, distance checked every interval (30 s default), at most 2 triggers, emergency and one
shot refused (0x0A).

**7.7 TMS and compressed LRRP.** TMS (header, UDP port 4007) has its own switch, `CONFIG_DMR_TMS`
(`docs/build-config.md`). No compressed "short location" LRRP mode is implemented.

## 8. Hotspot / MMDVM

**8.1 The host's terminator ends the call.** The old hotspot ignored terminators and ran a 360 ms network timeout plus a
720 ms stop delay. A call shorter than the buffering minimum starts on its terminator.

**8.2 Type frames from the burst, not the control byte.** MMDVMHost writes control byte 0 on every frame; typing from
it sent data as voice, ignored terminators, and NAKed the 6 idle frames before each call. Terminator / start detection
by slot type only matched color code 1. Rule: classify from the sync (BS, MS, TS1, TS2; voice or data) and the slot
type's systematic bits; ACK and drop idle frames and PI headers.

**8.3 Back-to-back network calls.** A new call's header while the last call was still going out was merged into one
transmission under the old LC. Hold the new call's voice behind the old call, end the old call with its terminator,
then key again with the new headers. Another call's header while buffering must replace the LC.

**8.4 Never overwrite unsent frames.** A full voice buffer overwrote the oldest frames and the status space went
negative (255); the serial / UDP receive ring overwrote unread frames on bursts over about 2 KB. NAK (5) or drop and
count.

**8.5 RF uplink: every header, late entry, fill, LOST.** MMDVMHost and Motorola send up to 8 headers; sending one and
then a 300 ms timer produced DMR_LOST mid-call and two network calls. Send every header; late entry gets a header made
from the embedded LC, and the superframe so far as silence from A so the voice sync resets MMDVMHost's sequence (MMDVMHost
drops the rest of a superframe after a gap); one terminator; DMR_LOST 500 ms after the last burst; keep FID and service
options from the heard header.

**8.6 Status semantics.** GET_STATUS transmitting stays set from buffering until the modem's last burst is on air and
while data is queued or on air (bit clears 0.70-0.73 s after the host's terminator); the free space is the smaller of
voice buffer and data list; carrier detect (0x40) from the receiver noise level while not transmitting. SET_FREQ resets
the hotspot state, so a host must wait for idle.

**8.7 Out-of-band frequencies.** The amateur band check rejected SET_FREQ outside amateur bands (435-438 MHz always
refused); commercial use needs a configurable band plan.

**8.8 Shared buffers.** The RF uplink frames and the network voice queue shared one buffer, and RF frames survived a
transmission (stale headers later). Separate the RX queue (lock-free from the ISR) from the TX buffer and clear per
direction change.

**8.9 One MMDVM module, not per-radio copies.** The hotspot was copied per radio tree and the copies drifted: two
ports missed four or more fixes for days. Keep one MMDVM implementation behind the modem API.

**8.10 MMDVM over a UART has no packet boundaries.** Frame by the 0xE0 start byte and the length byte and resync on
0xE0; a request starting with 0xE0 switches the port to hotspot mode. A 39-byte frame takes about 3 ms at 115200, so
write replies from the main task, not an ISR.

**8.11 Sizes and suffixes.** GET_VERSION's description buffer had one byte left with a long build ID and a client
suffix; size it for any suffix (SFR adds " SFR TS1>TS2 TS"). The hotspot reuses the data service's TX burst list, which
is only safe because the two modes exclude each other: make that explicit. An RX overflow flag that compares bytes with frames is never true.

**8.12 SFR enable is evaluated after both SET_CONFIG and SET_FREQ** (duplex flag clear and RX = TX), in either order;
the uplink uses the inbound slot's command (DMR_DATA1 / DMR_LOST1 for TS1).

**8.13 CW ID side effect.** The HR_C6000 tone configuration must be restored after a CW ID or DMR
fails until a power cycle.

**8.14 A ring buffer needs storage for every place it counts.** The split duplex TX half's per-slot ring had
`HRC6000_BS_Q + 1` places (so full and empty differ) but `HRC6000_BS_Q` entries. At the spare place the queue stored
nothing and the ISR read past the array, so every fourth TS2 burst went out as a zeroed burst, a PI header on air
(TS1's overrun landed in TS2's first entry and looked right). `hrc6000BsTest` test_ring_wrap queues through many wraps.

**8.15 The HR_C6000's layer 2 BS framing sends an empty CACH.** Writing the CACH inputs the manual names (AT / TC in
0x52, the short LC in TX RAM 0x24) changes nothing on air: TC never alternates, so a radio can't tell TS1 from TS2 on
the split duplex outbound. The physical layer mode (0x10 LayerMode 00) sends TX RAM as given, from two 36-byte halves
(0x00, 0x24) in alternate slots; a frame written only to 0x00 goes out in every other slot. Build the whole slot and
write it to both halves (`modem-hrc6000.md` 9.1, 9.2). and the one after.

## 9. USB and CDC-NCM

**9.1 STM32 OTG_FS doesn't have the endpoints for ACM + NCM.** Choose serial or network mode (VID 0x1FC9, PID 0x0096 NCM
only, with an IAD) and tunnel the serial protocol over UDP 3334. The MK22 can do a composite (PID 0x0095).

**9.2 macOS waits for NetworkConnection.** macOS only selects the NCM data alternate setting after a NetworkConnection
notification: send speed and connection on configuration, and repeat until the host selects the data interface.

**9.3 Re-enumerate when the link never comes up.** After power on macOS sometimes never configured or never selected the
data interface. Re-enumerate up to 6 times, 3 s apart, also when never configured; stop once the link is up. Root cause
still open (needs a USB capture).

**9.4 NTB parser limit counts NDPs, not datagrams.** macOS packs back-to-back datagrams into one NTB; a loop guard that
counted datagrams dropped everything after the eighth. Guard against NDP chain loops by counting NDPs.

**9.5 macOS treats both ends of a /31 as broadcast.** Plain sends to the radio failed (EACCES without SO_BROADCAST). Use a
/32 host address with an option 121 route to the radio (host xor 1).

**9.9 STM32 OTG_FS FIFO budget for NCM.** The 0x140-word FIFO RAM needs a split like RX 0x80, TX0 0x30, TX1 0x80 and
0x10 for the notification endpoint; NCM needs two interfaces and the iMACAddress string (MAC from the MCU unique ID).

**9.10 Composite and alternate settings.** The MK22 composite needs device class 0xEF / 0x02 / 0x01 (IAD); NCM's data
interface has an empty alternate 0 and its endpoints only in alternate 1; SET_INTERFACE on the NCM interfaces must be
accepted (the 2022 code stalled it) and GET_INTERFACE must report the current alternate. Code shared between USB
personalities must check that the class handle of the active personality exists (the serial transmit path dereferenced
a NULL CDC handle in network mode).

**9.11 Persisted identifiers are append-only.** Stored IDs (settings bits, menu numbers saved in quick keys, the side key `keyfn_t` values, struct
fields) may only be appended, never renumbered.

**9.6 Start USB after settings load.** The USB mode depends on settings; init USB only after they are read. Use the
user's DMR ID for the host address, not the per-channel / hotspot override.

**9.7 Windows 10 needs MS OS 1.0 descriptors** (string 0xEE "MSFT100", vendor code 0x47, compatible ID WINNCM). The MK22
NXP stack's SetInterface refused the NCM interfaces and had to accept them.

**9.8 UDP broadcasts.** Only forward broadcasts for UDP ports 4000-4099 so a host's own discovery traffic never keys
the radio.

**9.12 TinyUSB NCM: call tud_network_recv_renew after every received frame.** The class driver hands over the next
datagram of an NTB only after it. macOS packs several frames into one NTB in a burst (a TCP close and a new connect),
and with one receive NTB the link then went dead both ways for good, re-enumeration included. Also: TinyUSB after
0.21.0 fixes a DWC2 race on the TX FIFO empty interrupt mask (an IN transfer that never completes).

## 10. Flash and storage

**10.1 The HR_C7000 data flash driver was stubbed.** Read returned an uninitialized value and program / erase did nothing, so
codeplug, settings and calibration never loaded or saved. Real driver: SPI0 master, the chip select as a GPIO, poll the busy bit.

**10.2 Place the open-firmware area clear of the radio's own data.** The area on the external data flash keeps the radio's own codeplug and factory calibration untouched; it overlaps a region the original firmware may use for voice recording:
verify before switching back and forth. Never write the
factory calibration sector. A status read needs the chip select, and write enable must be sent before a write.

**10.3 Calibration init must not write.** A calibration init that fills a table from stubbed security registers and writes it to flash would, with real flash writes, overwrite the local calibration copy.

**10.4 Spare codeplug bytes get zeroed by a CPS.** SFR settings live in channel spare bytes guarded by a flag bit
(flag 1 bit 0x02); a CPS that rewrites them with 0 turns SFR off.

## 11. Calibration

**11.1 Reference oscillator sign (HR_C5000 board).** regs 0x47 / 0x48 / 0x04 = 0x7F minus the calibration value, DAC2 =
cal × 4 + 0x600 (sign was inverted, DAC2 fixed at mid scale). The calibration read also overflowed its 512-byte buffer
with a third 256-byte block.

**11.1a Codec table decoder.** Its run-length table expansion subtracted from the remaining length once per
output byte instead of per input pair, so tables were only partly expanded.

**11.2 HR_C7000 factory calibration.** In a 4 KB sector of the data flash whose last byte is 2: TX I/Q center,
two point modulation bias, modulation per band, low / mid / high PA drive per band interpolated over frequency;
fallback to a local copy, then a fixed drive (1408).

## 12. Memory and RAM limits

**12.1 CCM RAM isn't zeroed at startup.** Buffers placed in the STM32 CCM must be initialized explicitly. A linker script
may not map CCM at all: placing data in an unmapped .ccmram section is silently wrong.

**12.2 Zero-initialized buffers in .data waste flash.** 25 MK22 buffers in a .data section stored 39.5 KB of zeros in
flash; putting them in a .bss section of the same RAM bank saved 34.5 KB.

**12.3 Budgets.** An STM32F405 board's main RAM had about 2.4 KB free and CCM about 196 B (the 38 KB notification buffer dominates);
MK22 2026 doesn't fit before 0x54000 with GNU + newlib-nano (Redlib is smaller) unless satellite / APRS are compiled
out (then 18.6 KB free; SFR leaves 6.2 KB). An HR_C7000 board must fit between 0x0300C000 and 0x03061000 (about 9-20 KB spare,
timing-critical code -O2, the rest -Os). Design the new firmware with feature switches and per-radio budgets from day
one.

**12.4 Memory regions belong to the board.** Shared code hard-coded linker section names (a CCM section, the MK22's
second RAM bank); the HR_C7000 SoC has one RAM. The board descriptor must say where large buffers go.

**12.5 Task stacks overflow without a sign.** A 768-byte buffer in `codeplug_contact_find` overflowed a
start task at boot: the radio core's set-up runs there (`radio_sm_init` > `tune` > `dmr_configure` > `find_name`),
and a DMR VFO whose talk target is a contact reaches the search. The radio hung before the debug link came up. The
host tests run on large pthread stacks and FreeRTOS' overflow hook only spins. Rule: no large buffers on a task's
stack (static, under the lock that already serializes the caller); `make stack-check` runs in ci.sh and fails a task
whose deepest chain doesn't fit ([stack-check.md](../dev/stack-check.md)).

## 13. Build and toolchain

**13.1 csky GCC 14 on CK801 runs away.** Build newlib for ck803s only. Toolchain built from source (binutils, GCC 14,
newlib) on Linux arm64, about 45 minutes.

**13.2 Stale objects.** Make didn't rebuild objects when HEAD changed, so the embedded GITVERSION went stale; sources
synced to a VM within the same second as a build linked stale objects (1 s mtimes). A .SECONDARY rule kept missing
objects from being rebuilt. Rule: version string generated every build, dependency files, clean CI builds; pass
GITVERSION explicitly where there is no .git.

**13.3 Case-insensitive file systems.** Make's built-in .S to .s rule deleted an assembly source on APFS. Disable
built-in suffix rules. Build note.

**13.4 Reproducible images.** Pinning the version string and build date made byte-for-byte comparisons of refactors
possible (build_all --compare). Keep that ability.

**13.5 One hardware version needed main.c unoptimized** in one configuration; one image per board version;
the wrong build is harmless but deaf.

**13.6 Compile features out with switches, never by commenting.** The HR_C7000 base shipped its hotspot disabled by
eleven commented-out blocks (request handler, reply queue, GET_VERSION, RAM placement), and the port's global rename of
chip-prefixed names leaked into other platforms' types. Satellite / APRS were later made compile-time switches with
no-op entry points (about 27 KB).

**13.7 A GPS power pin that is a flash chip select elsewhere.** On non-GPS units the GPS power pin may be a flash chip select: drive it only after
NMEA has been heard (listen 5 s at power on). A UART framing or noise error stops DMA reception for good unless the
error path restarts it; accept any NMEA talker and an 82-character line plus CR LF.

**13.8 Canceled menu edits must re-apply hardware state** (GPS and USB modes) after restoring the old values.

**13.9 No git stash in a worktree with symlinked submodules.** In an agent worktree whose `third_party/*` are symlinks
to another checkout, `git stash` (to build a baseline for a size comparison) took the symlinks away for the stash's
duration: the baseline build failed on `FreeRTOS.h`, and git refuses most operations on the paths ("expected submodule
path not to be a symbolic link"). The pop put them back. Build a baseline from `git archive HEAD` into a scratch
directory with its own `third_party` symlinks instead, and delete it afterwards.

**13.10 Link order changes LTO code.** `$(wildcard)` returns files in an order that depends on the make: GNU make 4.3
(CI) sorts it, macOS's make 3.81 doesn't. With LTO the object order on the link line steers partitioning and
inlining, so the same commit and the same Arm GNU Toolchain release gave different code on the two: `make stack-check`
disagreed by 8 to 16 bytes on a task (CI's ui task over its stack while the Mac's passed). Rule: sort every
`$(wildcard)` that feeds the build (sources, generated tables); `tools/stack_check.py` reads its `.ci` files in name
order for the same reason. The figures then match CI byte for byte.

**13.11 An STM32F4 clone with 3 priority bits.** The Geehy APM32F405/407 implements 3 NVIC priority bits, not the
STM32F4's 4, and keeps only the top 3 of each priority byte. FreeRTOS checks at scheduler start that
`configMAX_SYSCALL_INTERRUPT_PRIORITY` has no bit the hardware lacks; at 5 (0x50) it had, so the check failed every
boot, before the display or USB came up: a black screen that only the update mode survives. Rule: keep
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` even (`platform/stm32f4/main.c` asserts it at build time), and don't
let correctness depend on two interrupts one apart (4 and 5, 6 and 7) preempting each other: on the clone they are
one level. STM32F4 boards; built, not run on a clone.

## 14. SFR (single frequency repeater)

**14.1 The HR_C6000's RX status is unreliable while it transmits.** CRC flag, color code (reg 0x52), sync type
(reg 0x5F), data type (a second Sys interrupt) and a header or terminator's info bytes 5-11 come out wrong while
keyed; CSBK and data headers heard while keyed failed CRC by 11-23 bits. Rule: check bursts heard while keyed with
their own checksum (RS(12,9), CRC-CCITT with masks CSBK 0xA5A5, data header 0xCCCC); rebuild headers and terminators
from the call's LC; hold data calls and repeat them whole after 130 ms quiet.

**14.2 Own echo and phantom voice.** The chip reports voice in empty inbound slots with what we sent. Drop voice or
data matching anything we sent in the last 250 ms; voice counts only within a voice call; after a terminator, ignore
headerless voice for 10 s; a stale voice A after F with recent AMBE is dropped.

**14.3 Repeat timing.** A burst is ready just after the timeslot interrupt that sets up the next outbound slot, so the
repeat goes out 90 ms later (measured 90.02-90.05 ms). Read bursts at their Sys interrupt (deferred reads found reg 0x51
cleared). The raw 264-bit RX RAM areas (0x30 / 0x60) only held a fill pattern in this mode.

**14.4 No idle fill inside a call; idle bursts without CRC_MCU_Control.**

**14.5 Late entry needs A then B 60 ms later** (a lone voice A is noise).

**14.6 A synthesizer retuned per burst** is the open risk (about 1.25 ms guard time); the HR_C5000 isn't ported
(register page 0 and no CRC_MCU_Control). The HR_C7000 is (14.11).

**14.7 SFR channels never switch to repeater mode on BS sync.** RMO slot lock needs a CACH every slot and an SFR sends
none. On an MK22 board the repeater ends its own burst at the RF RX interrupt once the PA has been on 20 ms.

**14.8 The idle message's info bits come from table D.2, I(95) first.** `SFR_IDLE_INFO` once had bytes 8 and 11 as
`CF` and `41`; table D.2 gives `FF 83 DF 17 32 09 4E D1 E7 CD 8A 91`. The BPTC round trip passed anyway, so only a
check against the table catches it (sfrTest).

**14.9 Hearing the inbound slot while keyed (HR_C6000 + AT1846S, on air).** Three things, each lost most of the call on air: the slot
between our bursts must be written "receive" (0x41 = 0x50), not 0x00; the AT1846S must go back to RX as it goes to TX,
through receiver off (reg 0x30 idle), the RX frequency, the band's RX supply, then RX on (one write straight from DMR TX
to RX on heard 6-36 of a call's bursts, varying run to run; the RX supply goes off while the PA is up); and a gap in
the inbound call leaves our slot empty, never an idle burst (14.4: one missed burst then cascaded into DMR_LOST).
HIL 2026-10-12.

**14.10 Repeat latency and header runs.** The runner keeps two outbound slots queued in the modem; in a call's gap
those are placeholders, and a burst heard next queued behind them went out 210 ms after the inbound one instead of
90 ms. Rule: a burst heard while placeholders wait takes the oldest one's place (`tx_fill`). A caller's header run is
identical copies: the own-echo rule (same kind and payload within 250 ms) dropped 6 of the bench's 8 headers. Rule:
in an open inbound call, voice headers and terminators are never echo-checked (the SFR glue never did).

**14.11 HR_C7000 SFR (on the SDR bench 2026-10-16).** Four things, each lost the call or part of it:
- Writing LAYER2_CONTROL to key up (0xC3, or 0xC4 for TX slots that follow the receiver) left the receiver without slot
  sync for about 1 s: 5 of the bench's 8 headers and the first 10 voice bursts were never heard. Rule: leave it passive
  and start the TX slot from the receive slot (LAYER2_SLOTON b5), then send in the slot numbered for our TS
  (modem-hrc7000.md 2.5).
- `DMR_BURST_EMPTY` was armed as a burst, so a gap in the inbound call went on air as data type 15. Rule: an empty slot
  arms nothing and is received.
- Without `tx_fill` the repeat after a gap ran 210 ms late (14.10), and the bench's silent call (the same AMBE every
  superframe) then matched our own copy in the 250 ms echo window: 3 bursts dropped as echo.
- Every terminator heard while keyed came with the chip's RS flag and was dropped in the driver. Rule: while sending,
  the driver passes headers and terminators on with `crc_ok` false and the runner checks them itself (14.1).

**14.12 An HR_C6000 SFR user hears one copy of a call (SDR bench, 2026-10-19).** In direct mode the chip
decodes the voice stream of the slot it locked onto. Reports for the other slot during the call are stale status
(class 0, the same values again and again), not the other copy. So the user modes "First copy" and "Dual-path RX" only fall
over from one copy to the other between calls or after a loss, never take frames from both. Not timing (the repeat
sat 0.13 ms off the talker's slot grid) and not the repeat's sync (clean TS2 voice sync); a stronger repeat made it
worse. Rule: don't count on both copies from an HR_C6000; hearing both needs the raw RX RAM path (sfr.md 3).

**14.13 The HR_C7000 repeat carries bit errors (SDR bench, 2026-10-19).** The repeater's own receiver takes errors while it switches
TX/RX every burst: on air 212 of 216 repeated voice frames needed Golay corrections and 4 were uncorrectable, where
the talker's direct copy was clean. It plays, but it is the reason a user would want the direct copy (sfr.md 4).

**14.14 An AT1846S SFR keys straight from RX (SDR bench, 2026-10-19).** For any radio with an AT1846S and an HR_C6000
([feature-matrix.md](../feature-matrix.md)):
- Going through idle (reg 0x30) at every switch (14.9) makes the synthesizer settle again each slot. The repeat then
  lost 5-10 of 72 inbound frames, with about 190 bit errors. With RX and TX on one frequency, switching 0x30 straight
  between RX and DMR TX keeps it locked: no frames lost, 48-68 bits. The band's RX supply still follows the PA.
- Straight from RX the carrier is about 3.07 kHz low, and a frequency write can't correct it (3.5). Raise the HR_C6000
  MOD1 offset (4:0x04, about 75 Hz a step at 461 MHz) for the burst and restore it for RX. On the board measured that is 42
  steps over the calibrated value (41: -57..-15 Hz, 42: +16..+60 Hz), at 461.1375 MHz only. Check it at
  other frequencies and on each radio.
- Key 1 ms after the RF TX interrupt, which comes about 1.28 ms before the slot. 0.6-1.2 ms all did the same; keying
  at the interrupt lost frames. Dropping the PA earlier (28.5-30 ms) changed nothing.
- VHF and split frequencies keep the idle sequence (not measured).

**14.15 An HR_C7000 SFR user needs the voice-access flag on both slots.** The chip writes the voice RAM only for a
slot decided with TXRX_CTRL 0x8040; with 0x40 the burst is reported, but its voice RAM still holds the last burst
decoded. The driver gave the flag to the chip slot of the last voice burst only. A user hearing both copies gets voice
on both slots in turn, so the flag was always on the other slot, and every burst read the same stale bytes: the
listener played the call's first decoded burst over and over while every counter looked clean (frames, bursts, no
loss). That is the hand test of 2026-10-20 (an HR_C7000 repeater and two users, 1 clean call of about
30). Rule: each chip slot with a voice header or voice burst in the last 390 ms gets the flag; a terminator clears its
own slot (`hrc7000DmrTest` test_rx_sfr_both_copies). The frame counters can't show this: an HIL check has to look at
the payloads (or the audio) the listener plays.

This rule picks which slots carry a call; 14.16 picks which slot an interrupt decides. They compose at each RX slot
interrupt: the decided slot is LAYER2_STATUS b30 (XOR 0), and it gets 0x8040 if that slot is in the voice set, else
0x40. The test models the chip the same way (b30 names the slot about to be decided), so it fails both on a flag that
follows the last voice burst and on the old polarity.

**14.16 HR_C7000 voice-access slot polarity.** The RX timeslot interrupt decides the slot LAYER2_STATUS b30 shows
then (`HRC7000_RX_NEXT_SLOT_XOR` 0), not the one after it. With the flag on the other slot the chip never writes the
voice RAM, and every frame of every call reads the same stale bytes: silence (`voice.rx_peak` about 6), exactly 3 bit
errors a frame, every other counter clean. Two boards found it and set 0 in their board config; a third kept the driver's
old default of 1 and played every DMR call silent (2026-10-22, bench speech: peak 6 with 1, 32768 with 0). Rule: 0 is the driver default; `test_dmr_voice_speech` checks the peak, a frame counter can't.

## 15. Testing practice

- An SDR with a burst analyzer (timing, sync, color code, frequency offset) caught every timing bug above; host tests
  against independent decoders caught the protocol ones. Keep both.
- On-air debug counters readable over the host protocol, plus runtime switches, let the SFR work go through 17 test
  rounds without reflashing for each experiment.

## 16. Input

**16.1 Rotary encoder counted on the edges of one line.**
Symptom: on a board with a rotary encoder the knob sometimes skipped a step, counted two, or stepped back (RadioFoundry, 2026-10-10). Cause: one
count per edge of B with A read in the interrupt; a knob leaving a detent and settling back counts a step and a step
back, and bounce edges merged by the single pending flag count twice. Rule: interrupt on both lines (or poll both fast)
and decode with a table that counts only whole detents at a rest state and ignores samples where both lines changed
(`drivers/input/quadrature`). Radios: all with an encoder. Also:
when the UI's event queue is full, carry the detents over to the next scan instead of dropping them.

**16.2 PTT on the programming cable's serial line.**
Symptom: a debug link host polling the radio started DMR transmissions on its own (RadioFoundry, 2026-10-15). Cause: the
two-pin K-type programming jack carries the host's serial data on the same line as PTT, so every 0 bit pulls the PTT
pin low, and two low key scans pass the debounce. Rule: on a radio whose PTT shares the host's RX line, count the pin
low as PTT only after a quiet time with no good UART byte (`drivers/input/sharedptt`). A real press holds the line
low, which the UART reads as a break, never a good byte. Radios: every board with a K-type jack wired this way; three
are confirmed. A filter of three low scans in a row still let debug link polling key one of them: hundreds of PTT
presses, each with a key beep, in one SFR test session (2026-10-19). Many handhelds wire the K-type jack this way, so
check every new radio (adding-a-radio.md, bring-up order). The gate alone still leaked during PPP, hands off: 2
presses in 100 s (2026-10-22). A key scan that ran late caught up with two scans in the same millisecond, and both
read the low bits of the first byte of a burst 65 ms after the last one, before the UART had reported it. Rule:
debounce the shared pin by time, not by scan count. The pin must read low, and not gated, for 20 ms
(`SHARED_PTT_HOLD_MS`). `tests/hil/test_ptt_host_traffic.py` checks it on the radio.

**16.3 PTT on an empty mic jack.**
Symptom: a radio powered on without its hand mic transmitted at once (RadioFoundry, issue #230). Cause: the mic's PTT
line has no pull-up the firmware can set, and with nothing in the jack it reads low from power-on, which is a press.
Rule: on a PTT line that comes through a connector, count a low as a press only once the line has read high since
start-up, something plugged in (`drivers/input/plugptt`). Pulling the plug while armed still reads as a held press;
the transmit timeout ends it, and with TX timeout Off nothing does until power-off. Radios: one confirmed. Check every
radio with a mic jack: power on with the jack empty and watch the raw PTT pin (adding-a-radio.md, bring-up order).

**16.4 Encoder interrupt lost for good.**
Symptom: the encoder stopped after a while; every key still worked (RadioFoundry, 2026-10-30). Cause: not found. The
GPIO bank had both encoder bits pending, enabled and unmasked, and the handler never ran, so the interrupt controller
had most likely masked the source; a fallback handler that masks a source it has no handler for is one way. Rule: on a
board whose encoder runs on one level interrupt, the key scan checks that a pending encoder interrupt is taken. Pending
on two scans running with no handler entry between them, keep the controller and bank state for the debug link, serve
the bank from the scan and unmask the source (the steps of the stall window may be lost). Count every source the fallback handler masks. Radios: the HR_C7000
boards with an interrupt-driven encoder; one confirmed.

## 17. Scan

**17.1 Scanning DMR direct mode: the carrier is on air every other slot.**
Symptom: the zone scan never held on a DMR call already in progress (HIL 2026-10-10). Cause: the noise reading follows
the 30 ms bursts, so the reading at the end of the dwell, or any poll of the probe, often fell between two bursts and
the probe ended at once; and a 700 ms probe was shorter than a late entry that has to wait for a second superframe's LC
(about 1.1 s from the retune). Rule: a carrier seen in the last 150 ms counts; bursts with the channel's color code
extend the probe to 1.5 s (`docs/spec/radio-features.md` 2).

**17.2 A DMR retune without a receiver restart keeps the last transmitter's slot timing.**
Symptom: a field report (build b0b69d2) on an HR_C7000 radio: the scan never stopped on a DMR transmission; FM
scanning worked. Cause: the HR_C6000 boards restart the DMR receiver in their own `rx()` on a new frequency; the
HR_C7000 boards didn't, and that chip keeps the slot timing it took from the last DMR signal it heard. On the next
channel it then decodes nothing from another transmitter until its own stale restart, 390-600 ms later
(`modem-hrc7000.md` 5.3), longer than the 150 ms dwell. With a noise reading that shows the DMR carrier the probe
outlasts it, only later; without one, the stop ends at the dwell on every visit (`radioFeaturesTest`,
`test_scan_dmr_kept_timing`). Rule: the radio core calls `hal_dmr_modem_t.rx_restart` at every new DMR frequency while
the stack runs. Whether the RF chip's noise reading follows a DMR carrier in DMR mode is a board fact to check on the
radio: the DMR scan relies on it to probe within the dwell.
