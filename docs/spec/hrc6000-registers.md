<!-- SPDX-License-Identifier: MIT -->

# HR_C6000 register reference

A complete English register reference for the Hongrui HR_C6000 DMR / FM baseband chip, translated from the vendor's
user manual: Zhejiang Hongrui Communication Technology (浙江宏睿通信技术有限公司), *HR_C6000 用户手册*, Chinese,
updated edition (revision A2, 2014-04-26). The Chinese text is authoritative. The English machine translations that
circulate (the "updated translated English" PDF, the older translated PDF, the VK3KYY and G4EML documents) have
errors; section 9 lists the ones that matter.

This file is a fact reference: what the manual says each register, bit and RAM location does. How ODR drives the
chip, and what the bench has confirmed, is in [modem-hrc6000.md](modem-hrc6000.md); section 10 here repeats only
the bench facts that bear on a register's meaning. FM audio paths and their bench results are in
[hardware-dm1701.md](hardware-dm1701.md) 21.3 and 21.3.1.

Conventions:

| Mark | Meaning |
|---|---|
| R, W, R/W | access as the manual's table gives it; blank where the manual gives none |
| default | the manual's reset value; "-" where it gives none |
| "(manual section x)" | the fact comes from a chapter, not the register table |
| "(text layer only)" | present in the PDF's text layer but not visible on the rendered page |
| "(not stated)" | the manual says nothing about it |
| "(conflict)" | two places in the manual disagree; section 9.2 has the details |
| ODR page n | the U_SPI OPMode value ODR's driver calls "page" (section 1) |

---

## 1. U_SPI access (manual 4.4, 5.2)

The MCU reaches every register table and both RAMs over U_SPI (pins 55-58: U_SDO, U_SDI, U_SCLK, U_CS). The chip is
the SPI slave. SCLK is at most 4 MHz. Bytes are MSB first.

A transaction is: CS low, a command byte, an address, one or more data bytes, CS high.

### 1.1 Command byte

| Bit | Name | Meaning |
|---|---|---|
| 7 | IsRead | 1 = read, 0 = write |
| 6 | address extension | 0 = 8-bit address, 1 = 16-bit address field (11-bit address) |
| 5-3 | - | reserved |
| 2-0 | OPMode | which space the transaction addresses (table below) |

| OPMode | Space (manual) | ODR page | Notes |
|---|---|---|---|
| 000 | reserved | - | |
| 001 | auxiliary parameter table (附属参数配置表) | 1 | section 4 |
| 010 | write: TX RAM; read: RX RAM | 2 | section 2 |
| 011 | power-on tone / prompt tone (开机音或其他提示音) | 3 | the prompt tone FIFO, section 7.7 |
| 100 | system parameter table (系统参数表) | 4 | section 3 |
| 101 | external AMBE3000 vocoder registers | 5 | |
| 110 | write: RX RAM; read: TX RAM | 6 | |
| 111 | external AMBE1000 vocoder registers | 7 | |

So OPMode 010 is the normal direction (the MCU fills what the chip sends and reads what it received), and 110 is the
crossed direction (the MCU can read back the TX RAM, or preload the RX RAM).

### 1.2 Address

| Cmd bit 6 | Address field | Start address |
|---|---|---|
| 0 | one byte | that byte |
| 1 | two bytes, "high first" as Addr[15:8], Addr[7:0] | {Addr[2:0], Addr[15:8]}: the first byte is address bits 7-0, the low 3 bits of the second byte are address bits 10-8 |

The address auto-increments after each data byte for as long as CS stays low.

Manual examples:

| Operation | Cmd | Addr | Data |
|---|---|---|---|
| write 0x80 to system register 0x01 | 0x04 | 0x01 | 0x80 |
| read 2 bytes of RX RAM from 0x30 | 0x82 | 0x30 | (0x01, 0x02 returned) |

### 1.3 V_SPI (manual 4.6.1)

The vocoder SPI (pins 41-44) is a second slave port with the same framing, one operation per transaction: read
Cmd = 0x83, Addr = 0x00, 27 data bytes; write Cmd = 0x03, Addr = 0x00, 27 data bytes. SCLK is at most 4 MHz. Using
the HR_V3000 vocoder over V_SPI needs only 0x06 = 0x24 (manual).

ODR note: on U_SPI, OPMode 011 is the prompt tone FIFO (section 7.7). ODR's "flush page 3 with 0x06 = 0x22"
(modem-hrc6000.md 3.2) is, in the manual's terms, 64 prompt-tone samples of 0x7F7F written with the prompt tone path
open (0x06 bit 1).

### 1.4 Power down, reset and clocks (manual 4.1, 4.3, 4.4)

| Item | Fact |
|---|---|
| RESETn (pin 39) | power-on reset, active low, at least 0.1 µs |
| Software reset | write 0x00 bit 7 = 0: one Sys_clk period (1/9.8304 MHz); the chip sets the bit back to 1 itself |
| PWD (pin 47) | high = sleep, every internal clock stopped (the PLL also sleeps). Valid 100 ns after it rises |
| Leaving sleep | after PWD goes low, run from the crystal (PLL bypassed), wait more than 500 µs for the PLL, then switch to the PLL |
| Crystal | 1.5 V bias best, at least 2 Vpp (4.3.1); the characteristics table gives 12.288 MHz typical, 320 mVpp minimum |
| Interrupt pins | four, active-low pulses 3 Sys_clk wide |

---

## 2. RAM maps (manual 5.3)

Two 1.2 KB RAMs (0x000-0x4AF each), one per direction, reached with OPMode 010 / 110 and 11-bit addresses. The
manual's tables are for layer 2 mode. Areas overlap: the RAM is reused per mode.

### 2.1 TX RAM (表 5.2)

| Frame / use | Address | Contents |
|---|---|---|
| Voice LC header | 0x00-0x0B | 0x00-0x08: 72 control bits; 0x09-0x0B: 24 check bits (MCU optional) |
| Voice PI header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits (MCU optional) |
| Voice EMB (embedded LC) | 0x00-0x09 | 0x00-0x08: 72 control bits; 0x09 bits 7-3: 5 check bits (MCU optional). Prepared together with voice burst A |
| Voice A-F | 0x30-0x4A | 216 voice bits (27 bytes) |
| RC burst | 0x00-0x01 | 11 bits: 0x00 and 0x01 bits 7-5 |
| Voice terminator | 0x00-0x0B | 0x00-0x08: 72 control bits; 0x09-0x0B: 24 check bits (MCU optional) |
| CSBK | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits (MCU optional) |
| MBC header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits (MCU optional) |
| MBC intermediate | 0x00-0x0B | 96 control bits |
| MBC last | 0x00-0x0B | 96 control bits |
| Data header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits (MCU optional) |
| Rate 1/2 data | 0x00-0x0B | 96 data bits |
| Rate 3/4 data | 0x00-0x11 | 144 data bits (the manual's description column says "0x00-0x0B", a copy error) |
| Rate 1 data | 0x00-0x17 | 192 data bits (description column again says "0x00-0x0B") |
| Idle | 0x18-0x23 | 96 control bits |
| Short LC (CACH) | 0x24-0x28 | 0x24-0x26 and 0x27 bits 7-4: 28 control bits; 0x28: 8 check bits |
| Voice F EMB fill | 0x29-0x2C | voice burst F embedded fill, or 0x29 = superframe number (KeyID) and 0x2A bits 7-5 = encryption algorithm ID |
| Data / control burst embedded RC | 0x4B-0x50 | the 48-bit RC embedded in a data or control burst, or the 11-bit RC encoder input in 0x4B and the high 3 bits of 0x4C |
| C_RC burst (PDT) | 0x00-0x0A | 0x00 and 0x01 bits 7-5: 11 RC bits; 0x02-0x08: 56 control bits; 0x09-0x0A: 16 check bits |
| 196-bit information | 0x30-0x48 | 196 control bits |
| Test send (测试发送) | 0x00-0x48 | data for the transmit test mode |
| FM data buffer 1 | 0x030-0x22F | 512 bytes. "Can be FM transmit voice data written externally; the voice data can also come internally from the codec" |
| FM data buffer 2 | 0x230-0x42F | 512 bytes, same text |
| Encryption key stream | 0x495-0x4AF | 216 bits, 27 bytes |

The manual says nothing more about the FM buffers: not the sample format, the rate, the order of the two halves,
or which register makes the chip transmit from them instead of the codec. Section 8 collects what it does say.

### 2.2 RX RAM (表 5.3)

| Frame / use | Address | Contents |
|---|---|---|
| Voice LC header | 0x00-0x0B | 0x00-0x08: 72 control bits; 0x09-0x0B: 24 check bits |
| Voice PI header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits |
| Voice EMB (embedded LC) | 0x00-0x09 | 0x00-0x08: 72 control bits; 0x09 bits 7-3: 5 check bits |
| RC burst | (registers) | the decoded 11-bit RC is in registers (0x53 / 0x54) |
| Voice terminator | 0x00-0x0B | 0x00-0x08: 72 control bits; 0x09-0x0B: 24 check bits |
| CSBK | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits |
| MBC header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits |
| MBC intermediate | 0x00-0x0B | 96 control bits |
| MBC last | 0x00-0x0B | 96 control bits |
| Data header | 0x00-0x0B | 0x00-0x09: 80 control bits; 0x0A-0x0B: 16 check bits |
| Rate 1/2 data | 0x00-0x0B | 96 data bits |
| Rate 3/4 data | 0x00-0x11 | 144 data bits |
| Rate 1 data | 0x00-0x17 | 192 data bits |
| Idle | 0x00-0x0B | 96 control bits |
| C_RC burst (PDT) | 0x00-0x08 | 0x00-0x06: 56 control bits; 0x07-0x08: 16 check bits; the 11 RC bits are in registers |
| EMB_48_INFO_0 | 0x1F-0x24 | the 48-bit EMB field received in slot 0 |
| EMB_48_INFO_1 | 0x25-0x2A | the 48-bit EMB field received in slot 1 |
| Short LC | 0x2B-0x2F | the 36-bit CACH short LC received; the low 4 bits of 0x2F are unused |
| 264bit_info_0 | 0x30-0x50 | slot 0's current 264 decoded bits, including a voice burst's 216 bits |
| 264bit_info_1 | 0x60-0x80 | slot 1's current 264 decoded bits |
| 264bit_soft_0 | 0xA8-0x1AF | slot 0's 264 soft values, one per byte in bits 5-0 |
| 264bit_soft_1 | 0x1C8-0x2CF | slot 1's 264 soft values, one per byte in bits 5-0 |
| FM data buffer 1 | 0x030-0x22F | 512 bytes |
| FM data buffer 2 | 0x230-0x42F | 512 bytes |
| Decryption key stream | 0x495-0x4AF | 216 bits, 27 bytes |

Layer 3 service data (manual 5.1.1): after 0x82 bit 2 with sub-status 0x80 (all checks passed), the MCU reads the
RX RAM from 0x30; the length comes from the received header.

Physical layer mode (manual 5.4.6): each demodulated burst, 36 bytes, is stored from RX RAM 0x30. The layouts are
CACH 24 + slot type 20 + sync 48 + data 196 bits, or CACH 24 + sync / EMB / RC 48 + voice 216 bits; the CACH is
present only in continuous mode.

---

## 3. System parameter table (manual table 8.1, OPMode 100)

One table per functional group, in the manual's order. Addresses the manual's table omits are listed in 3.22 with
whatever other chapters say about them.

### 3.1 Reset (0x00)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x00 | W | DMRnRst | 0x00 | 7 | 0 = reset the DMR protocol. Lasts one system clock; the chip releases it itself (likewise for every bit here) |
| | | PHYnRst | | 6 | 0 = reset the physical layer |
| | | CodernRst | | 5 | 0 = reset the channel encoder / decoder |
| | | FMnRst | | 4 | 0 = reset FM |
| | | VoCoderRst | | 3 | 0 = reset the vocoder interface |
| | | MSKRst | | 2 | 0 = reset the MSK block |
| | | IISRst | | 1 | 0 = reset the I2S interface |
| | | CodeCRst | | 0 | 0 = reset the built-in codec; lasts 3 system clocks, released by the chip |

### 3.2 Hardware configuration (0x01-0x06)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x01 | R/W | RFTransIQMode | 0xB0 | 7 | IQ modes: 0 = TX I on DAC I, TX Q on DAC Q; 1 = swapped. Two-point mode: selects the symbol-to-deviation mapping (section 7.1) |
| | | RFRecvIQMode | | 6 | 0 = RX I from ADC I, RX Q from ADC Q; 1 = swapped |
| | | RFTransMode | | 5-4 | TX interface: 00 IF, 01 IF IQ, 10 baseband IQ, 11 two-point modulation |
| | | RFRecvMode | | 3-2 | RX interface: 00 IF, 01 IF IQ, 10 baseband IQ |
| | | - | | 1 | IQ balance test: 1 = output zero plus the offset values, 0 = normal |
| | | - | | 0 | ADC / DAC clock phase: 1 = inverted relative to the system clock, 0 = in phase |
| 0x02 | R/W | TransIsigCenter | 0x00 | 7-0 | IQ modes: TX I offset (added to DAC I, or DAC Q if 0x01 bit 7 = 1). Two-point mode: "MOD2 output offset", about ±422 mV in 3.3 mV steps (conflict, 9.2) |
| 0x03 | R/W | RecvIsigCenter | 0x00 | 7-0 | RX I offset (ADC I, or ADC Q if 0x01 bit 6 = 1). Digital range -127..127 (4.8.1) |
| 0x04 | R/W | TransQsigCenter | 0x00 | 7-0 | IQ modes: TX Q offset (DAC Q, or DAC I if swapped). Two-point mode: "MOD2 output offset" in the table, MOD1 in 4.7.2 (conflict, 9.2); about ±422 mV in 3.3 mV steps |
| 0x05 | R/W | RecvQsigCenter (the table prints "RecvIsigCenter") | 0x00 | 7-0 | RX Q offset (ADC Q, or ADC I if swapped). Range -127..127 |
| 0x06 | R/W | Vocoder | 0x40 | 7-6 | vocoder: 00 V_SPI vocoder, 01 AMBE3000, 11 AMBE1000 |
| | | DMRFrom | | 5 | 0 = one of the two external vocoders (AMBE3000 / AMBE1000) supplies the encoded voice; 1 = the vocoder on the V_SPI general interface does |
| | | VocoderFrom | | 4 | 0 = encoded / decoded voice packets go through the DMR protocol layer (normal TX); 1 = encoded voice goes straight into the vocoder's decode buffer (loopback test) |
| | | SPIFrom | | 3 | 0 = V_SPI reads voice data from the protocol layer (normal RX); 1 = V_SPI reads the vocoder's packets directly (voice recording) |
| | | CodeCMode | | 2 | 0 = built-in codec, 1 = external codec |
| | | OpenMusic | | 1 | 1 = open the path for the power-on tone or call prompts (section 7.7); 0 = closed |
| | | LocalVoCoderControl | | 0 | 0 = the system switches the vocoder; 1 = the MCU does |

### 3.3 IF, clocks and status (0x07-0x0F)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x07 | R/W | IFFreq2 | 0x0B | 7-0 | IF word bits 23-16. IF = word / 2^24 x 9.8304 MHz |
| 0x08 | R/W | IFFreq1 | 0xB8 | 7-0 | IF word bits 15-8 |
| 0x09 | R/W | IFFreq0 | 0x00 | 7-0 | IF word bits 7-0. Default word 0x0BB800 = 450 kHz |
| 0x0A | R/W | Clk_enb | 0x81 | 7 | Clk_in_sel: 1 = run directly from the crystal, 0 = from the PLL. After changing 0x0B / 0x0C wait more than 500 µs before switching back to the PLL |
| | | | | 6-1 | reserved |
| | | | | 0 | ClkOut_enb: 1 = drive CLKOUT (pin 15) |
| 0x0B | R/W | PLLM | 0x28 | 7-0 | PLL feedback divider M |
| 0x0C | R/W | PLLBP | 0xB3 | 7 | 0 = use the PLL, 1 = bypass (CLK = XTALI) |
| | | PLL SLEEP | | 6 | reserved in the table; 4.3.2 says 1 = PLL asleep (no output) |
| | | PLLDO | | 5-4 | PLL output divider: NO = 2^PLLDO |
| | | PLLN | | 3-0 | PLL input divider N |
| 0x0D | R/W | Voice_superframe | 0x02 | 7-4 | reserved |
| | | | | 3-0 | voice abnormality wait before exit, in superframes: the chip waits (value + 1) x 360 ms |
| 0x0E | R/W | - | - | - | reserved |
| 0x0F | R | FSKErro | - | 7-0 | FSK error or EVM statistic (selected by 0x48 bit 7); valid when read at the timeslot interrupt |

PLL (manual 4.3.2): CLK = XTALI x M / N / NO, NO = 2^PLLDO, with 1 MHz < XTALI / N < 25 MHz,
200 MHz < CLK x NO < 1000 MHz, M > 1, N > 1. CLK (49.152 MHz recommended) is divided into Sys_clk (0xB9), the codec
clock (0xBA) and CLKOUT (0xBB). Recommended settings (表 4.1):

| XTALI | 0x0B | 0x0C | PLL out | 0xB9 / 0xBA / 0xBB | Sys_clk / codec / CLKOUT |
|---|---|---|---|---|---|
| 12.288 MHz | 0x40 | 0x32 | 49.152 MHz | 0x05 / 0x04 / 0x02 | 9.8304 / 12.288 / 24.576 MHz |
| 29.4912 MHz | 0x28 | 0x33 | 49.152 MHz | 0x05 / 0x04 / 0x02 | 9.8304 / 12.288 / 24.576 MHz |

The three dividers behave as divide-by-value in this table (the manual gives no formula).

The appendix A1 text uses 0x0D, 0x0E, 0x0F and 0x38 as built-in codec registers (section 7.7), which the table
doesn't (conflict, 9.2).

### 3.4 System mode and RF timing (0x10-0x13)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x10 | R/W | ModulatorMode | 0x73 | 7 | 0 = DMR, 1 = FM |
| | | TierMode | | 6 | 0 = Tier I, 1 = Tier II |
| | | ContinueMode | | 5 | 0 = continuous, 1 = timeslot. In layer 2, must be 0 to receive the CACH |
| | | LayerMode | | 4-3 | 00 physical layer, 01 layer 2, 10 layer 3 |
| | | ISRepeater | | 2 | 0 = not a repeater, 1 = repeater |
| | | ISAligned | | 1 | 0 = offset (in non-repeater mode, offset means single-frequency repeating), 1 = aligned |
| | | RepeaterSlot | | 0 | layer 3: must be 1. Layer 2: 0 = slot 1, 1 = slot 2 |
| 0x11 | R/W | LocalChanMode | 0x80 | 7 | 1 = repeater mode: RepeatedSimplex; direct mode: DirectSimplex |
| | | | | 6 | 1 = repeater mode: RepeatedDuplex; direct mode: DirectDuplex |
| | | | | 5, 4 | no description |
| | | | | 3 | 1 = SingleFreqSimplex (text layer only) |
| | | | | 2, 1 | no description |
| | | | | 0 | 1 = receive digital and analog at the same time; 0 = digital or analog only |
| 0x12 | R/W | rf_pre_on | 0x00 | 7 | smooth ramp up / down of the signal at slot boundaries. In TX (4.7.1, 4.7.2): ramp start point 0 = 1.65 V, 1 = 0 V |
| | | | | 6 | two-point test: the DAC outputs a 40 Hz sine (4.7.2 says square wave) |
| | | | | 5-0 | RF_TX_INTER (RX-to-TX switch) advance before the 30 ms boundary, about 100 µs steps, 0-6.3 ms. The TX-to-RX advance is 0xC0 |
| 0x13 | W | Cend_band | 0x00 | 7-4 | RF_TX_EN / RF_RX_EN active-to-inactive advance before the 30 ms boundary, 100 µs steps, at most 11 (1.1 ms). Non-continuous mode only |
| | | | | 3-0 | RF_TX_EN / RF_RX_EN inactive-to-active delay after the boundary, 100 µs steps, at most 11. Non-continuous mode only |

### 3.5 Addresses and color code (0x14-0x1F)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x14 | R/W | LocalSreAdrressL | 0x01 | 7-0 | own address bits 7-0 |
| 0x15 | R/W | LocalSreAdrressM | 0x00 | 7-0 | own address bits 15-8 |
| 0x16 | R/W | LocalSreAdrressH | 0x00 | 7-0 | own address bits 23-16 |
| 0x17 | R/W | LocalGroupAdrressL | 0x33 | 7-0 | own group address bits 7-0 |
| 0x18 | R/W | LocalGroupAdrressM | 0xEF | 7-0 | own group address bits 15-8 |
| 0x19 | R/W | LocalGroupAdrressH | 0x00 | 7-0 | own group address bits 23-16 |
| 0x1A | R/W | LocalBSAdrressL | 0xFF | 7-0 | own BS address bits 7-0 |
| 0x1B | R/W | LocalBSAdrressM | 0xFF | 7-0 | own BS address bits 15-8 |
| 0x1C | R/W | LocalBSAdrressH | 0xFF | 7-0 | own BS address bits 23-16 |
| 0x1D | R/W | LocalUnaddress | 0xFF | 7-4 | unaddressed-call receive address |
| | | MaskUnaddr | | 3-0 | unaddressed-call match mask: a bit set to 1 is **ignored** in the address compare |
| 0x1E | R/W | LocalBroadCast | 0xFF | 7-4 | broadcast (all-call) receive address |
| | | MaskBroadCast | | 3-0 | all-call match mask: a bit set to 1 is ignored |
| 0x1F | R/W | LocalEMB | 0x10 | 7-4 | own color code |
| | | | | 3 | PI bit |
| | | | | 2 | monitor calls whose address doesn't match (text layer only) |
| | | | | 1-0 | ratio of PI (encryption) information to late-entry information in the EMB field: 00 no encryption info, 01 1:1, 10 1:2, 11 1:4 |

### 3.6 Access policy and vocoder control (0x20-0x24)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x20 | R/W | LocalAccessPolicy | 0xAA | 7-6 | 00 impolite, 01 polite to all, 10 polite to own CC (the manual doesn't say which service each 2-bit field covers) |
| | | | | 5-4, 3-2, 1-0 | same coding |
| 0x21 | R/W | LocalAccessPolicy1 | 0xA0 | 7-6, 5-4 | same coding as 0x20 |
| | | | | 3 | confirmed-packet response policy: 0 impolite, 1 polite |
| | | | | 2 | reserved |
| | | | | 1 | 1 = clear the vocoder encode buffer; the chip resets the bit to 0 |
| | | | | 0 | 1 = clear the vocoder decode buffer; self-clearing |
| 0x22 | R/W | EncodeStart | 0x01 | 7 | write 1: vocoder encoding starts |
| | | EncodeStop | | 6 | write 1: encoding stops |
| | | DecodeStart | | 5 | write 1: decoding starts |
| | | DecodeStop | | 4 | write 1: decoding stops |
| | | | | 3 | reserved |
| | | sel_I2S_mode | | 2 | 1 = the vocoder I2S interface is master, 0 = slave. Only when bit 1 = 1 |
| | | sel_I2StoI2S | | 1 | PCM to / from the vocoder over 1 = I2S, 0 = SPI (C_SPI) |
| | | Ambe1000_noise_enb | | 0 | AMBE1000 squelch: 0 = off |
| 0x23 | R/W | - | - | 7-0 | reserved |
| 0x24 | R/W | - | 0xF8 | 7-2 | reserved |
| | | AMBE1000FrameRept | | 1 | AMBE1000 frame repeat |
| | | AMBE1000ForceSilen | | 0 | AMBE1000: force the decoder silent |

### 3.7 Vocoder SPI, FM squelch and companding, TX advance, codec interface (0x2A-0x37)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x2A | R/W | spi_clk_cnt | 0x0B | 7-0 | C_SPI clock = codec clock / (2 x (value + 1)) |
| 0x2B | R | Sql_l | 0x00 | 7-0 | FM: squelch signal bits 7-0 |
| 0x2C | R | Sql_h | 0x00 | 7-0 | FM: squelch signal bits 15-8 |
| 0x2D | R/W | shift_radix | 0x0B | 7-4 | FM: 0 dB point of the expander (RX) |
| | | | | 3-0 | FM: 0 dB point of the compressor (TX) |
| 0x2E | R/W | tx_pre_on | 0x04 | 7-5 | reserved |
| | | | | 4-0 | TX advance in 100 µs units, to cancel the RF chain's delay so DMR bursts sit on the 30 ms boundary. The internal path has a fixed 400 µs; with no RF delay use 0x04 |
| 0x2F | R/W | par_ck_cnt | 0x00 | 7-0 | vocoder I2S master bit clock: I2S_CK_M = codec clock / (2 x (value + 1)); must be above 272 kHz |
| 0x32 | R/W | LRCK_CNT_H | 0x02 | 7-0 | I2S frame clock in master mode: I2S_FS_M = codec clock / (2 x ({0x32, 0x33} + 1)); must be 8 kHz. Also sets the external-codec LRCK (4.5.2) |
| 0x33 | R/W | LRCK_CNT_L | 0xFF | 7-0 | low byte of the above. Default 0x02FF gives 8 kHz from 12.288 MHz |
| 0x34 | R/W | FMBpfOn | 0xBE | 7 | 1 = 300-3400 Hz band-pass filter on (TX and RX) |
| | | FMCompressorOn | | 6 | 1 = compressor on (TX) / expander on (RX, 6.2) |
| | | FMPreEmphasisOn | | 5 | 1 = pre-emphasis on (TX) / de-emphasis on (RX, 6.2) |
| | | FMBandWidth | | 4 | FM TX bandwidth: 0 = 12.5 kHz, 1 = 25 kHz |
| | | FMBandWidth_adj | | 3 | FM with baseband IQ, IF or IF IQ TX: adjacent-channel filter bandwidth, 0 = 12.5 kHz, 1 = 25 kHz |
| | | FMBandWidth_r | | 2 | FM RX bandwidth: 0 = 12.5 kHz, 1 = 25 kHz |
| | | FMBandWidth_ctc | | 1 | CTCSS RX bandwidth: 0 = 12.5 kHz, 1 = 25 kHz |
| | | | | 0 | reserved |
| 0x35 | R/W | FM_dev_coef_t | 0x14 | 7-0 | FM TX deviation coefficient |
| 0x36 | R/W | mcu_alc_clk_down | 0x00 | 7 | 1 = stop the external codec's BCLK and LRCK |
| | | mcu_vspi_clk_down, mcu_vspi_cs_down | | 6-5 | vocoder C_SPI SCK: x1 = always off, 00 = always on, 10 = on while CS is low. When the vocoder PCM uses I2S, bit 6 = 1 enables I2S_CK and I2S_FS (4.6.1 says the opposite polarity, conflict, 9.2) |
| | | mcu_pkt_clk_down | | 4 | 1 = stop the vocoder packet interface clock |
| | | mcu_ctr_rst1000 | | 3 | AMBE1000 reset: 0 = system controlled, 1 = MCU controlled |
| | | mcu_val_rst1000 | | 2 | MCU's AMBE1000 reset level: 1 = high |
| | | | | 1 | FM receive: voice channel to the codec, 1 = on, 0 = off |
| | | | | 0 | reserved |
| 0x37 | R/W | mcu_control_shift | 0x00 | 7 | 1 = the MCU changes the DAC data level of the built-in / external codec (DALIN_VOL), 0 = unchanged |
| | | zoom | | 6 | with bit 7 set: 0 = quieter, 1 = louder |
| | | | | 5 | 0 |
| | | shift_size | | 4-0 | DAC data level change, 00001-11111, 1.5 dB steps (4.5.1: all zero is the minimum when turning down; when turning up, 0 means no change) |

### 3.8 FM deviation and limiter (0x3E, 0x3F)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x3E | R/W | FM_dev_coef_r | 0x08 | 7-0 | FM RX deviation coefficient. Chapter 6.1 says 0x3E (with mic gain "0x0F") adjusts the TX deviation when the FM modulator feeds baseband IQ or IF (conflict, 9.2) |
| 0x3F | R/W | TR_SIG_LIM | 0x10 | 7-0 | FM limiter coefficient (TX soft limiter, against over-deviation) |

### 3.9 PHY / DLL control (0x40-0x4F)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x40 | R/W | TxEn | 0x03 | 7 | TX synchronization enable: 1 = on |
| | | RxEn | | 6 | RX synchronization enable: 1 = on. Timing exists only while bit 7 or bit 6 is set |
| | | MasterMode | | 5 | 0 = passive timing (follows received syncs), 1 = active timing (own clock) |
| | | Layer2Slot | | 4 | the MCU tells the chip whether the current layer 2 slot is in use (1) or not (0); for a smooth change from layer 2 to layer 3 |
| | | CRC_MCU_Control | | 3 | 1 = the MCU computes the CRC / checksums; 0 = the chip does |
| | | | | 2 | reserved |
| | | Decode_Mode | | 1-0 | 00 = physical layer test without decoding, 11 = normal (non-test) operation |
| 0x41 | R/W | TxNxtSlotEn | 0x00 | 7 | 1 = transmit in the slot starting at the coming timeslot interrupt |
| | | RxNxtSlotEn | | 6 | 1 = receive in that slot |
| | | SyncFail | | 5 | 1 = no sync: the physical layer searches again |
| | | begin_v_layer2 | | 4 | layer 2 access success: write 1 to start receiving voice; write 0 in the slot that isn't receiving voice |
| | | CC_Match_Ctrl | | 3 | 0 = deframe only when the CC matches, 1 = deframe without a CC match |
| | | | | 2 | (not listed) |
| | | | | 1 | reserved |
| | | AutoTest | | 0 | 1 = physical layer test enable |
| 0x42 | R | TransSlot | - | 7 | 1 = the current slot transmits |
| | | RecvSlot | | 6 | 1 = the current slot receives |
| | | UsedSlot | | 5 | table: 0 = slot in use, 1 = not in use. 5.4.4 reads it the other way (conflict, 9.2) |
| | | | | 4-0 | reserved |
| 0x43 | R | RSSILevelH | - | 7-0 | RSSI bits 15-8 |
| 0x44 | R | RSSILevelL | - | 7-0 | RSSI bits 7-0 (appendix B: RSSI against ADC input) |
| 0x45 | R/W | Sig_Reduce | 0x00 | 7-0 | IQ modes: IQ amplitude (4.7.1: bits 3-0, 170 mV-2725 mV in 170 mV steps). Two-point: MOD2 amplitude, 8.75 mV-2240 mV in 8.75 mV steps |
| 0x46 | R/W | Phase_Reduce | 0x00 | 7-0 | two-point: MOD1 amplitude, 8.75 mV-2240 mV in 8.75 mV steps |
| 0x47 | R/W | Two_Point_Bias | 0x00 | 7-0 | two-point bias, low 8 bits of 10 (high 2 bits in 0x48 bits 1-0). 4.7.2 swaps the halves (conflict, 9.2) |
| 0x48 | R/W | Two_Point_Bias | 0x00 | 7 | 1 = 0x0F reports the FSK error statistic, 0 = EVM |
| | | | | 6-2 | reserved |
| | | | | 1-0 | two-point bias, high 2 bits. 4.7.2: with 0x25 bit 5 = 1 and bit 3 = 0, this 10-bit value sets MOD1's DC output while receiving, 0-3.3 V (for a reference crystal control voltage) |
| 0x49 | W | DEV_LIMITERH | 0xFF | 7-0 | TX deviation limit, bits 15-8: deviation above it is clipped to it |
| 0x4A | W | DEV_LIMITERL | 0x00 | 7-0 | TX deviation limit, bits 7-0 |
| 0x4B | R/W | Code_Type1 | 0x00 | 7-6 | coding of user data type 1011: 00 BPTC(196,96), 01 rate 3/4 trellis ("convolutional 3/4"), 10 none, 11 BPTC72 |
| | | | | 5-4 | same for data type 1100 |
| | | | | 3-2 | same for data type 1101 |
| | | | | 1-0 | same for data type 1110 |
| 0x4C | R/W | Code_Type2 | 0x00 | 7-6 | same for data type 1111 |
| | | | | 5-3 | reserved |
| | | data_embrc_ctrl | | 2 | RC in the EMB field of data / control bursts: 0 = taken from the CPU, 1 = RC-encoded internally |
| | | data_embrc_en | | 1 | 1 = embed RC in the sync field of data bursts |
| | | voice_burstF_emb_ctr | | 0 | voice burst F embedded info: 0 = from the CPU, 1 = RC-encoded internally |
| 0x4E | R | AD_Bias_I | - | 7-0 | measured DC offset of the ADC I input |
| 0x4F | R | AD_Bias_Q | - | 7-0 | measured DC offset of the ADC Q input |

### 3.10 DLL (0x50-0x5F)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x50 | W/R | LocalDataType | - | 7-4 | what the next TX slot carries: the data type, the voice burst, or RC (table below) |
| | | LocalVoD | | 3 | 0 = data, 1 = voice or RC |
| | | | | 2 | 1 = a data header is a "data header 2"; ignored for other types |
| | | LocalLCSS | | 1-0 | LCSS for this TX slot |
| 0x51 | R | DLLRecvDataType | - | 7-4 | data burst: the received data type. Voice burst: bit 7 = 1 and bits 6-4 = 1..6 for A..F |
| | | DLLRecvPI | | 3 | received PI |
| | | DLLRecvCRC | | 2 | 0 = checks passed, 1 = error |
| | | SyncClass | | 1-0 | 00 no sync, 01 voice, 10 data, 11 RC. A burst F EMB report uses SyncClass 0 (5.1.1) |
| 0x52 | R | DLLCC | - | 7-4 | received color code |
| | | CACH | | 3 | received AT |
| | | | | 2 | received TC |
| | | | | 1-0 | received CACH LCSS |
| 0x52 | W | CACH | - | 3 | AT for the chip's BS downlink |
| | | | | 2 | TC for the chip's BS downlink |
| 0x53 | R | RC_Info | - | 7-0 | RC bits 10-3, or the superframe number of burst F's info |
| 0x54 | R | RC_Info | - | 7-6 | LCSS of the voice burst's EMB |
| | | | | 5-3 | reserved |
| | | | | 2-0 | RC bits 2-0, or burst F's algorithm ID |
| 0x55 | R | Check_sum_reg | - | 7 | reserved |
| | | | | 6 | CRC8 error |
| | | | | 5 | Golay ("gdout") check error |
| | | | | 4 | QR check error |
| | | | | 3 | CS (5-bit checksum) error |
| | | | | 2 | CRC16 error |
| | | | | 1 | CRC24 error |
| | | | | 0 | CRC9 error |
| 0x5F | W/R | Sync_Detect_Ctrl | - | 7-4 | layer 2: which syncs may be detected, 1 = enabled. Bit 7 MS, bit 6 BS, bit 5 TDMA direct slot 1, bit 4 TDMA direct slot 2 |
| | | | | 3-2 | reserved |
| | R | Recv_Sync_Type | | 1-0 | sync received: 00 MS, 01 BS, 10 TDMA1, 11 TDMA2 |

LocalDataType codes (表 5.5, with LocalVoD):

| Burst | Bits 7-4 | VoD |
|---|---|---|
| voice LC header | 0001 | 0 |
| voice PI header | 0000 | 0 |
| voice A, B, C, D, E, F | 0000, 0001, 0010, 0011, 0100, 0101 | 1 |
| RC burst | 0110 | 1 |
| voice terminator | 0010 | 0 |
| CSBK | 0011 | 0 |
| MBC header | 0100 | 0 |
| MBC intermediate | 0101 | 0 |
| MBC last | 0101 | 0 |
| data header | 0110 | 0 |
| rate 1/2 data | 0111 | 0 |
| rate 3/4 data | 1000 | 0 |
| rate 1 data | 1010 | 0 |
| idle | 1001 | 0 |
| reserved | 1011-1111 (1011 is listed twice, 1101 not at all) | 0 |

These are the ETSI data type values. 0x4B / 0x4C give the coding of the user types 1011-1111.

### 3.11 Call control (0x60-0x63)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x60 | - | TransControl | - | 7-6 | 10 = voice TX request, 01 = data TX request. FM (6.1): 0x80 opens the analog TX path, 0x00 is receive (6.2) |
| | | | | 5-0 | reserved |
| 0x61 | - | LocalDestAdrressL | 0x03 | 7-0 | call destination address bits 7-0 |
| 0x62 | - | LocalDestAdrressM | 0x00 | 7-0 | bits 15-8 |
| 0x63 | - | LocalDestAdrressH | 0x00 | 7-0 | bits 23-16 |

### 3.12 Interrupts (0x81-0x90, 0x98)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x81 | W | InterClass1Mask | - | 7-0 | mask for 0x82: a bit at 0 masks that interrupt |
| 0x82 | R | InterRequestDeny | - | 7 | DMR: TX request rejected (channel busy under a polite policy). **FM: "send data request" interrupt** |
| | | InterSendStart | | 6 | DMR: TX start (sub-status 0x84). MSK: TX ping-pong buffer half empty ("half full") |
| | | InterSendStop | | 5 | DMR: TX end (sub-status 0x86). MSK: physical layer TX finished |
| | | InterLateEntry | | 4 | DMR: late entry. MSK: response / acknowledge interrupt |
| | | InterRecvData | | 3 | DMR: a control / data burst was parsed (status in 0x51). MSK: receive interrupt |
| | | InterRecvMessage | | 2 | DMR: service data received (sub-status 0x90). FM: FM function detection (tone matched) |
| | | InterQuit | | 1 | DMR: voice abnormal exit (type in 0x98). **FM: "receive data" interrupt** |
| | | InterPHYOnly | | 0 | physical-layer-only receive: data ready (BER testing) |
| 0x83 | W | InterClear | - | 7-0 | write 1 to clear the matching 0x82 bit: 7 rejected, 6 TX start, 5 TX end, 4 late entry, 3 control frame, 2 short message, 1 abnormal exit, 0 physical layer |
| 0x84 | R | SendStartVoice | - | 7 | voice TX start |
| | | SendStartCSBK1 | | 6 | OACSU request sent (first or retry) |
| | | KeyUpdateInterp | | 5 | end-to-end voice encryption: key update request (0x88 bits 5-4 say which) |
| | | Over_VocoderRespon | | 4 | AMBE3000 configuration done (MCU-configured external AMBE3000 only) |
| | | SendStartData | | 3 | data TX start |
| | | SendStartDataPart | | 2 | data partial retransmission |
| | | SendStartDataFull | | 1 | data full retransmission |
| | | config_done_pulse | | 0 | vocoder initialization done (external AMBE3000 / AMBE1000 only) |
| 0x85 | W/R | SendStartMask | - | 7-0 | enables for the 0x84 bits, same order |
| 0x86 | R | Tx_finished | - | 7 | service completely sent, voice or data (for confirmed data: after the good response) |
| | | tx_complet_data | | 6 | a fragment that needs no immediate response was sent |
| | | tx_denied_overtime_o | | 5 | voice OACSU wait timeout |
| | | rdy_lst_interp | | 4 | layer 2 "last moment": after it, a slot not fully set up can't be a TX slot (layer 2 only) |
| | | tx_complet_confirmed | | 3 | a fragment that needs a response was sent: start the response timer |
| | | short_lc_interp | | 2 | short LC received |
| | | tx_denied_overtime_bs | | 1 | BS activation timeout |
| | | lost_gps_pps | | 0 | GPS 1 PPS lost (5.1.1 calls bit 0 "none") |
| 0x87 | W/R | SendStopMask | - | 7-0 | enables for the 0x86 bits, same order |
| 0x88 | R | config_done_auto | - | 7 | 1 = the chip's automatic vocoder initialization is done |
| | | config_done_force | | 6 | 1 = it isn't done |
| | | key_update_interp | | 5 | voice encryption key update |
| | | emb_update_interp | | 4 | voice encryption EMB update |
| | | embF_update_interp | | 3 | burst F EMB info sent |
| | | | | 2-1 | reserved |
| | | FIFO_FULL | | 0 | the PCM FIFO the MCU writes is full: stop writing until it reads 0 (the prompt tone FIFO, 7.7) |
| 0x89 | W/R | rf_tx_interp_mask | - | 7 | RF_TX_INTER mask |
| | | rf_rx_interp_mask | | 6 | RF_RX_INTER mask (the table calls it "TX end RF interrupt") |
| | R | | | 5-0 | sync field bit error count |
| 0x90 | R | RecvDataCRC | - | 7 | whole-fragment CRC-32: 1 = good, 0 = bad |
| | | | | 6 | 1 = unconfirmed short message error |
| | | | | 5 | FM: function detection interrupt (5.1.1 gives the FM sub-status as 0x10, bit 4: conflict, 9.2) |
| | | | | 4-0 | reserved |
| 0x98 | W/R | voice_lostmask | - | 7-5 | voice abnormality interrupt mask |
| | R | voice_abnormal_state | | 2-0 | bit 2: slot 1 voice_sync_lost timeout; bit 1: slot 0 voice_sync_lost timeout; bit 0: layer 3 voice receive lost sync |

Sub-status values for 0x82 bit 2 (5.1.1): DMR 0x80 all checks passed (data from RX RAM 0x30), 0x00 check failed,
0x40 unconfirmed short message error.

### 3.13 Tone decoder and MSK receive status (0x93-0x97)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0x93 | R | decode_flag[7:0] | - | 7-0 | see below |
| 0x94 | R | decode_flag[15:8] | - | 7-0 | |
| 0x95 | R | decode_flag[19:16] | - | 3-0 | (bits 7-4 reserved) |
| 0x96 | R | RecBitLen | - | 7-0 | MSK: received data length in bits (128 during acquisition, 64 once synchronized) |
| 0x97 | R | - | - | 7-6 | reserved |
| | | TrainCodewordFlag | | 5 | MSK: 0 data codeword, 1 codeword with sync (training) sequence |
| | | TrainErrorBitNum | | 4-0 | MSK: training sequence bit errors, 0-31; valid when bit 5 = 1 |

decode_flag by mode (0xA1):

| Mode | Bits | Meaning |
|---|---|---|
| 5-tone | 19-0 | 5-tone decoder output |
| 2-tone | 11-8 | long tone output |
| 2-tone | 7-0 | the two tones' output |
| CTCSS / CDCSS | 0 | speaker enable (tone or code matched) |
| DTMF | 4 | DTMF decode finished |
| DTMF | 3-0 | DTMF code |
| XTCSS | 9 | XTCSS individual-call flag |
| XTCSS | 8 | XTCSS all-call flag |
| XTCSS | 7-0 | XTCSS type flags |

### 3.14 FM signaling (0xA0-0xAF)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0xA0 | - | subvoice_dev_coef | 0x0E | 7-0 | deviation of the sub-audio / signaling tone (A2: 0x0D CTCSS 350 Hz, 0x03 CDCSS 350 Hz, 0x39 DTMF / selcall 1.8 kHz) |
| 0xA1 | W/R | fm_mod | 0x08 | 7 | reserved, must be 0. Chapter 4.8 table 4.8 calls it "AF / non-AF receive mode" (section 7.2) |
| | | | | 6 | MSK mode |
| | | | | 5 | 5-tone |
| | | | | 4 | 2-tone |
| | | | | 3 | CTCSS |
| | | | | 2 | CDCSS |
| | | | | 1 | DTMF |
| | | | | 0 | XTCSS |
| 0xA2 | W/R | signal_std | 0x10 | 7-6 | XTCSS TX signaling length |
| | | | | 5-4 | CTCSS squelch tail (reverse burst): 00 off, 01 standard 240° phase shift, 10 non-standard 180° |
| | | | | 3 | CDCSS TX polarity: 1 inverted, 0 normal |
| | | | | 2 | CDCSS RX polarity: 1 inverted, 0 normal |
| | | | | 1 | CDCSS TX turn-off code (134.4 Hz tail) enable |
| | | | | 0 | send the XTCSS end message |
| 0xA3 | W/R | intertone_time | 0x05 | 7-0 | 2-tone / 5-tone: gap between tones, 100 ms steps. DTMF: gap between codes, 2 ms steps |
| 0xA4 | W/R | first_tone_length | 0x05 | 7-0 | 2-tone: first tone TX / RX time, 100 ms steps. 5-tone: each tone, 100 ms. DTMF: one code's TX time, 2 ms steps |
| 0xA5 | W/R | second_tone_length | 0x05 | 7-0 | 2-tone: second tone TX / RX time, 100 ms steps |
| 0xA6 | W/R | long_tone_length | 0x0A | 7-0 | 2-tone: long tone time, 100 ms steps |
| 0xA7 | W/R | FREQ_AMP_LIMIT | 0x10 | 7-0 | detection threshold of every FM tone decoder |
| 0xA8 | W/R | - | 0x01 | 7-0 | DTMF: DTMF_tone[7:0] (TX digits). CTCSS: ctcss_send_addr (TX tone index, 7.8) |
| 0xA9 | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[15:8]. XTCSS: xtc_send_hi, TX address |
| 0xAA | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[23:16]. XTCSS: xtc_send_low, TX type |
| 0xAB | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[31:24]. CDCSS: dcs_code[7:0] |
| 0xAC | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[39:32]. CDCSS: dcs_code[8] in bit 0 |
| 0xAD | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[47:40]. 2-tone / 5-tone: selcall_tone[7:0] |
| 0xAE | W/R | - | - | 7-0 | DTMF: DTMF_tone[55:48]. 2-tone / 5-tone: selcall_tone[15:8] |
| 0xAF | W/R | - | 0x00 | 7-0 | DTMF: DTMF_tone[63:56]. 2-tone / 5-tone: selcall_tone[19:16] in bits 3-0 |

### 3.15 AMBE1000 squelch packet (0xB0-0xB8)

| Addr | R/W | Name | Default | Meaning |
|---|---|---|---|---|
| 0xB0-0xB8 | W/R | Ambe1000_noise_reg0..8 | CE C9 32 E8 A4 06 2C F7 B4 | the 9 bytes of the AMBE1000 squelch (comfort noise) output packet |

### 3.16 Clocks (0xB9-0xBB)

| Addr | R/W | Name | Default | Meaning |
|---|---|---|---|---|
| 0xB9 | W/R | Sys_clk_Reg | 0x05 | system clock divider (Sys_clk) |
| 0xBA | W/R | Codec_clk_Reg | 0x04 | built-in codec clock divider |
| 0xBB | W/R | ClkOut_Reg | 0x02 | CLKOUT divider |

### 3.17 RF timing, codec level, pin multiplexing (0xC0-0xD0)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0xC0 | W/R | rf_pre_on_rx | 0x00 | 7-6 | reserved |
| | | | | 5-0 | RF_RX_INTER (TX-to-RX switch) advance before the 30 ms boundary, about 100 µs steps |
| 0xC1 | W/R | RF_LEVEL | 0x00 | 7 | RF_LEVEL source: 0 = generated inside the chip, 1 = written by the MCU |
| | | | | 6 | the MCU's RF_LEVEL value |
| | | | | 5 | layer 3: enable the 30 ms timeslot interrupt |
| | | | | 4-0 | reserved |
| 0xC2 | W/R | Codec_AGC_CTRL | 0x00 | 7 | 1 = mic gain AGC on |
| | | | | 6-0 | reserved |
| 0xC3 | W/R | CODEC_OPTIMALH | 0x08 | 7-0 | AGC: best codec output level, bits 15-8 (target mean level) |
| 0xC4 | W/R | CODEC_OPTIMALL | 0x00 | 7-0 | bits 7-0 |
| 0xC5 | W/R | CODEC_LOWLEVELH | 0x00 | 7-0 | AGC: lowest codec output level, bits 15-8 |
| 0xC6 | W/R | CODEC_LOWLEVELL | 0x64 | 7-0 | bits 7-0 (the table repeats "high 8 bits") |
| 0xC7 | W/R | RF_3TC_ON | 0x00 | 7 | 1 = pin 17 (LRCK) becomes the RF_3TC_EN output |
| | | | | 6 | edge to high: 1 = after the 30 ms boundary, 0 = before |
| | | | | 5-0 | that time, about 100 µs steps |
| 0xC8 | W/R | RF_3TC_OFF | 0x00 | 7 | reserved |
| | | | | 6 | edge to low: 1 = after the boundary, 0 = before |
| | | | | 5-0 | that time, 100 µs steps |
| 0xC9 / 0xCA | W/R | RF_5TC_ON / OFF | 0x00 | as 0xC7 / 0xC8 | pin 19 (ADCDAT) as RF_5TC_EN |
| 0xCB / 0xCC | W/R | RF_ANT_ON / OFF | 0x00 | as 0xC7 / 0xC8 | pin 16 (MCLK) as RF_ANT_EN |
| 0xCD / 0xCE | W/R | RF_3RC_ON / OFF | 0x00 | as 0xC7 / 0xC8 | pin 18 (BCLK) as RF_3RC_EN |
| 0xCF / 0xD0 | W/R | RF_5RC_ON / OFF | 0x00 | as 0xC7 / 0xC8 | pin 20 (DACDAT) as RF_5RC_EN |

Manual 4.5.2 (表 4.4, figure 4.15): any external-codec pin can be a GPIO output whose edges sit up to 6 ms either
side of the 30 ms boundary, in 100 µs steps. The figure labels its two cases "C7=0x81, C7=0x61" (high 100 µs early,
low 100 µs late) and "C7=0xC1, C7=0x01" (high 100 µs late, low 100 µs early). By the bit definitions the first value
of each pair fits 0xC7 and the second belongs in 0xC8 (whose bit 7 is reserved); 0x61 doesn't decode as 100 µs. Treat
the figure as illustrative.

### 3.18 FM tone decoder control (0xD1-0xD4)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0xD1 | W/R | dtmf_code_width | 0x04 | 7-5 | reserved |
| | | | | 4-0 | number of DTMF codes to send |
| 0xD2 | W/R | - | 0xD0 | 7-0 | sample_size[7:0]: tone decoders' sampling depth |
| 0xD3 | W/R | - | 0x07 | 7-5 | reserved |
| | | | | 4 | CDCSS RX code bit 8 |
| | | | | 3-0 | sample_size[11:8] |
| 0xD4 | W/R | - | 0x01 | 7-0 | CTCSS: RX tone index. CDCSS: RX code bits 7-0. XTCSS: RX address |

### 3.19 MSK (0xD5-0xD8, 0xDC-0xDD)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0xD5 | W/R | AcqEnable | 0x00 | 7 | 1 = the PHY starts acquisition. Edge triggered: the PHY clears it once it has seen it |
| | | OperationType | | 6 | 0 idle, 1 working |
| | | ChannelType | | 5-4 | 00 control channel, 01 traffic channel, 10 data channel, 11 reserved (BS: TX channel type; MS: both) |
| | | TranOrRecFlag | | 3 | 0 receive, 1 transmit |
| | | SoftReset | | 2 | 1 = reset (BS: TX block; MS: both) |
| | | | | 1-0 | reserved |
| 0xD6 | W/R | RemBitNum | 0x00 | 7-6 | reserved |
| | | | | 5-0 | MAC framing: bits left to send, 0-63 |
| 0xD7 | W/R | MultiMessageTransFlag | 0x00 | 7 | 1 = insert a reversal bit after the current codeword (multi-message, PHY framing only); edge triggered |
| | | RxInterMask | | 6 | 1 = mask the receive interrupt |
| | | CtrlDataInterMask | | 5 | 1 = mask the control-data interrupt |
| | | MacFrameEn | | 4 | 0 = PHY framing, 1 = MAC framing |
| | | MACTransFinishFlag | | 3 | 1 = last data: PHY framing stops after this codeword; MAC framing after this codeword plus RemBitNum bits |
| | | Unsolicited | | 2 | 0 = response message, 1 = unsolicited message (traffic channel) |
| | | **msk_voice_send_en** | | **1** | **0 = send MSK signaling, 1 = send FM voice** |
| | | | | 0 | reserved |
| 0xD8 | - | ChannelType_RX | - | 1-0 | BS only: RX channel type, coded as 0xD5 bits 5-4 (chapter 7 only) |
| | | SoftReset_RX | | 2 | BS only: 1 = reset the RX block (chapter 7 only) |
| | | | | 7-3 | reserved |
| 0xDC | - | TDMA_Slot_Num_H | - | 0 | trunking: multiframe number from the short LC, bit 8 |
| 0xDD | - | TDMA_Slot_Num_L | - | 7-0 | bits 7-0 |

### 3.20 Built-in codec (0xE0-0xE6)

| Addr | R/W | Name | Default | Bit | Meaning |
|---|---|---|---|---|---|
| 0xE0 | W/R | - | - | 7 | 1 = the MCU switches the codec blocks through these bits; 0 = the chip switches them |
| | | | | 6 | LINEIN1 enable |
| | | | | 5 | LINEIN2 enable |
| | | | | 4 | LINEOUT1 enable |
| | | | | 3 | LINEOUT2 enable |
| | | | | 2 | MIC_N enable |
| | | | | 1 | MIC_P enable |
| | | | | 0 | I2S mode: the chip works as slave, set to 1 |
| 0xE2 | W/R | - | - | 7 | default 0 |
| | | | | 6 | DAC switch, 0 in normal operation |
| | | | | 5 | codec bias switch, 0 normal; 1 stops the DAC and ADC (standby value 1) |
| | | | | 4 | ADC enable switch: 0 normal, 1 when the ADC isn't used |
| | | | | 3 | ADC mic amplifier switch: 0 normal, 1 when the ADC isn't used |
| | | | | 2 | anti-pop: set 1 ms after the DAC starts, clear 10 ms before it stops |
| | | | | 1 | DAC to output amplifier switch: 1 normal; 0 = no DAC signal at LINEOUT |
| | | | | 0 | codec power down: 0 normal, 1 standby |
| 0xE3 | W/R | - | - | 7-6 | default 01 |
| | | | | 5-4 | default 01 |
| | | | | 3-1 | default 001 |
| | | | | 0 | codec internal DAC-ADC pass-through: 1 = on, 0 in normal operation (direction not stated) |
| 0xE4 | W/R | - | - | 7-6 | LINEOUT gain: 00 0 dB, 01 2 dB, 10 4 dB, 11 6 dB |
| | | | | 5-4 | mic first stage: 00 0 dB, 01 -6 dB, other -12 dB |
| | | | | 3-0 | mic second stage: 3 dB steps above the first stage, 0000 lowest |
| 0xE5 | W/R | - | - | 7-1 | default 00001010 (the table gives 8 bits for a 7-bit field) |
| 0xE6 | W/R | - | - | 7 | HP_PREV_EN, default 0 |
| | | | | 6-5 | HP_TIME_SET, default 00 |
| | | | | 4 | POP_SEL_EN, default 0 |
| | | | | 3-0 | reserved |

Codec gain stages (4.5.1): MIC_VOL 0 / -6 / -12 dB before the ADC; ADLIN_VOL 0 to +36 dB in 3 dB steps before
the ADC; DALIN_VOL on the DAC data in 1.5 dB steps (0x37); HPOUT_VOL 0 / 2 / 4 / 6 dB (0xE4 bits 7-6). The
characteristics table gives the mic range as -12 to +24 dB in 3 dB steps and the digital volume as ±45 dB in
1.5 dB steps. Inputs: MIC_P / MIC_N differential, or LINEIN1 / LINEIN2 single ended, through one mux (K2) to the
ADC; the output path K1 drives LINEOUT1 / LINEOUT2. Table 4.3 says 0xE3 is "the K1 switch control".

### 3.21 Table 4.3 summary of codec registers

| Addr | Use (表 4.3) |
|---|---|
| 0xE0 | MCU control of the codec; LINEIN, MIC and LINEOUT enables |
| 0xE2 | DAC / ADC switches and codec power down |
| 0xE3 | K1 switch control |
| 0xE4 | LINEOUT gain (7-6), mic first stage (5-4), mic second stage (3-0) |
| 0x37 | DALIN_VOL: bits 5-0 in 1.5 dB steps (all zero is the minimum), bit 6 louder / quieter, bit 7 enable |

### 3.22 Addresses the register table omits

| Addr | What the manual says elsewhere |
|---|---|
| 0x25 | "DAC working control word" (4.7, 表 4.5). Bits 3 and 2 choose chip control of the DACs by TX slot; bits 5 and 4 let the MCU switch them. 4.7.2: bit 5 = 1 and bit 3 = 0 keep MOD1's DAC on all the time (the pairing of bits 4 and 2 with the other DAC is not stated) |
| 0x26, 0x27, 0x28 | "ADC working control word" (4.8, 表 4.8). Bits not stated; the ADCs can sleep in TX slots under chip or MCU control |
| 0x29 | (not stated) |
| 0x30, 0x31 | external codec BCLK = codec clock / (2 x ({0x30, 0x31} + 1)) (4.5.2). Defaults not stated |
| 0x38 | A1: written 0x00, "no change" (a built-in codec setting) |
| 0x39-0x3D | (not stated) |
| 0x4D | (not stated) |
| 0x56-0x5E | (not stated) |
| 0x64-0x80 | (not stated) |
| 0x8A-0x8F, 0x91, 0x92, 0x99-0x9F | (not stated) |
| 0xBC-0xBF | (not stated) |
| 0xD9-0xDB, 0xDE, 0xDF | (not stated) |
| 0xE1, 0xE7-0xFF | (not stated) |

---

## 4. Auxiliary parameter table (附属参数配置表, OPMode 001)

| Group | Addr | Name | Default | Meaning |
|---|---|---|---|---|
| TX data sync | 0x04-0x09 | DATA SYNC1-6 | DF F5 7D 75 DF 5D | 48-bit data sync the chip transmits (default: BS-sourced data sync). The chip sends voice sync as its inverse (modem-hrc6000.md 2) |
| TX RC sync | 0x0A-0x0F | RC SYNC1-6 | DF F5 7D 75 DF 5D | 48-bit RC sync transmitted |
| Scrambler | 0x24, 0x25 | scramble_reg0, 1 | 00 00 | voice encryption initialization |
| Descrambler | 0x26, 0x27 | dscramble_reg0, 1 | 00 00 | voice decryption initialization |
| RS init | 0x2A-0x2C | RS_H_INITREG0-2 | 00 | RS check initial value, voice header |
| | 0x2D-0x2F | RS_T_INITREG0-2 | 00 | RS check initial value, terminator |
| CRC init | 0x30, 0x31 | CSBK_INITREG0, 1 | 00 | CSBK CRC-16 initial value |
| | 0x3A, 0x3B | DATAH_INITREG0, 1 | 00 | data header CRC-16 initial value |
| | 0x3C, 0x3D | PI_INITREG0, 1 | 00 | PI header CRC-16 initial value |
| | 0x3E, 0x3F | MBC_INITREG0, 1 | 00 | MBC CRC-16 initial value |
| | 0x40 | CRC8_INITREG | 00 | CRC-8 initial value |
| RX MS data sync | 0x47-0x4C | DATA SYNC1-6 | D5 D7 F7 7F D7 57 | MS-sourced data sync the receiver looks for |
| CRC9 init | 0x4D-0x50 | CRC9_INITREG0-3 | 00 | CRC-9 initial value for confirmed data at every rate |
| DMR receive thresholds | 0x52 | RSSIBottonH | 0x1A | signal (energy) detection threshold bits 15-8 |
| | 0x53 | RSSIBottonL | 0xF0 | bits 7-0 |
| | 0x54 | MaxValue | 0x78 | sync (timing synchronization) detection threshold |
| | 0x55 | Sig_COR_VALUE | 0x67 | signal arrival detection threshold |
| RX BS data sync | 0x56-0x5B | DATA SYNC1_1-6_1 | DF F5 7D 75 DF 5D | BS-sourced data sync |
| CRC32 init | 0x5C-0x5F | CRC32_INITREG0-3 | 00 | CRC-32 initial value |
| Multi-group addresses | 0x60-0xBF | GroupAddr_L1/M1/H1 .. L32/M32/H32 | - | 32 group addresses of 24 bits (L, M, H per group) |
| MSK | 0x12A | TrainErrorThreshold | - | bits 5-0: training sequence decision threshold for acquisition and sync (7.3: set 5) |
| | 0x12B | DTBeforeTransAndRec | - | delay between the receive pulse and the TX start pulse, n / 19.2 ms (7.3: 160) |
| | 0x12C | NT | - | delay from MAC framing to the air (7.3: 103) |
| | 0x12D | ChannelDelay | - | longest delay, in bits, for the TSC's answer to an MS's unsolicited message on a traffic channel (7.3: 20) |
| RX RC sync | 0x12E-0x133 | RC SYNC1-6 | 77 D5 5F 7D FD 77 | RC sync received |
| RX TDMA1 sync | 0x134-0x139 | DATA SYNC1_2-6_2 | F7 FD D5 DD FD 55 | direct mode slot 1 data sync |
| RX TDMA2 sync | 0x13A-0x13F | DATA SYNC1_3-6_3 | D7 55 7F 5F F7 F5 | direct mode slot 2 data sync |

Addresses outside these rows are not in the table. Appendix A2 uses two more ranges of this table:

| Addr | Use (A2) |
|---|---|
| 0x102-0x119 | eight 24-bit tone detector coefficients, {high, mid, low} at {0x104, 0x103, 0x102} .. {0x119, 0x118, 0x117}: value = 2 cos(2 pi f / fs) x 2^22 |
| 0x11A-0x129 | eight 16-bit tone generator phase steps, low byte first ({0x11B, 0x11A} .. {0x129, 0x128}): value = f / 32000 x 2^16 |

Values are in section 7.8.

---

## 5. Hardware chapter facts on transmit and receive (manual 4.7, 4.8)

### 5.1 Transmit interface (表 4.5)

Two 10-bit single-ended DACs (pins 75 DAC_IVOUT/MOD1 and 74 DAC_QVOUT/MOD2 in the pin list).

| Register | Use in transmit |
|---|---|
| 0x01 bit 7 | TX port mapping (IQ swap; two-point: deviation sign) |
| 0x01 bits 5-4 | 00 IF, 01 IF IQ, 10 baseband IQ, 11 two-point |
| 0x02 / 0x04 | DC offset of the two outputs |
| 0x07-0x09 | IF word |
| 0x12 | bit 7 ramp, bit 6 two-point test tone, bits 5-0 RF_TX_INTER advance |
| 0x25 | DAC control word |
| 0x2E | TX advance (fixed internal delay 400 µs, so 0x04 with no RF delay) |
| 0x45 | two-point MOD2 amplitude (表 4.5 writes "MOD2 (DAC_IVOUT)") |
| 0x46 | two-point MOD1 amplitude (表 4.5 writes "MOD1 (DAC_QVOUT)") |
| 0x47 / 0x48 | 10-bit two-point bias |

| Mode (0x01 bits 5-4) | Facts |
|---|---|
| Baseband IQ (10) | Single-ended I and Q around 1.65 V into an IQ modulator (the example is a CMX991 with TXIN / TXQN at 1.65 V), through image filters. Up to 2725 mV differential for random data. 0x02 / 0x04: I / Q offset, about ±264 mV in 3.3 mV steps. 0x45 bits 3-0: both amplitudes, 170-2725 mV in 170 mV steps. 0x12 bit 7: ramp start 1.65 V (0) or 0 V (1) |
| Two-point (11) | Two op-amps shift and scale the DAC outputs into MOD1 (drives the reference crystal) and MOD2 (drives the VCO) (figure 4.21: pin 75 MOD1 to the crystal / PLL, pin 74 MOD2 to the VCO). Bias1 / Bias2 come from a DAC or digital potentiometer; an AD5165 sets the gain. Up to about 2240 mV differential. 0x04: MOD1 offset and 0x02: MOD2 offset, about ±422 mV in 3.3 mV steps (4.7.2; the register table says MOD2 for both). 0x46 MOD1 and 0x45 MOD2 amplitude, 8.75-2240 mV in 8.75 mV steps. 0x12 bit 6: 40 Hz test square wave. With 0x25 bit 5 = 1 and bit 3 = 0, MOD1's DAC stays on in receive and 0x47 / 0x48 set its voltage (0-3.3 V) |
| IF IQ (01) | Like baseband IQ; IF word = IF / Sys_clk x 2^24 (0x07, 0x08, 0x09) |
| IF (00) | The IF IQ pair combined into one single-ended output; same IF word |

Two-point symbol mapping (表 4.6, 4.7):

| Symbol | 0x01 bit 7 = 0 | 0x01 bit 7 = 1 |
|---|---|---|
| +3 | +1944 Hz | -1944 Hz |
| +1 | +648 Hz | -648 Hz |
| -1 | -648 Hz | +648 Hz |
| -3 | -1944 Hz | +1944 Hz |

Characteristics (表 3.4): two-point offset 1.228-2.072 V (1.65 typical), 3.3 mV resolution; amplitude
8.75-2240 mV, 8.75 mV resolution. IQ offset 1.386-1.914 V; amplitude 0.17-2.725 V in 0.17 V steps.

### 5.2 Receive interface (表 4.8, 4.8.1, 4.8.2)

Two 10-bit ADCs (pins 70 / 71 I, 63 / 62 Q).

| Register | Use in receive |
|---|---|
| 0xA1 bit 7 | AF receive mode or not; when not AF, 0x01 bits 3-2 choose the mode (the register table says this bit is reserved, must be 0) |
| 0x01 bit 6 | RX port mapping (IQ swap) |
| 0x01 bits 3-2 | 00 IF, 01 IF IQ, 10 baseband IQ |
| 0x03 / 0x05 | I / Q input offset, -127..127 |
| 0x07-0x09 | IF word (the BER test uses 0x0BD954, 455 kHz) |
| 0x26, 0x27, 0x28 | ADC control words |
| aux 0x52 / 0x53 | signal energy threshold, high / low byte |
| aux 0x54 | timing sync detection threshold |
| aux 0x55 | arrival detection threshold |

IF mode: the low IF goes into ADC_IVINP; ADC_IVINN is grounded or held at the IF bias. IF filter: ±7.5 kHz for
25 kHz channels, ±3.75 or ±4.5 kHz for 12.5 kHz. Receive IF 450 kHz typical, up to 1 MHz; Eb/N0 7 dB at 5 % BER.
Appendix B gives RSSI (0x43 / 0x44) against a 450 kHz DMR input: 0 at -34 dBm (0.048 V), 75 at -10 dBm, 790 at
0 dBm (1.5 V), 1980 at +3 dBm.

### 5.3 External codec and vocoder interfaces (4.5.2, 4.6)

| Item | Fact |
|---|---|
| External codec port | pins 17 LRCK, 18 BCLK, 19 ADCDAT, 20 DACDAT, 16 MCLK; the chip is the I2S master. Example codec ALC5621: LRCK 8 kHz, BCLK 512 kHz, CLKOUT (24.576 MHz) wired to MCLK and to the codec's clock. With the built-in codec, CLKOUT and MCLK stay unconnected |
| Pin reuse | every external-codec pin can be a timed GPIO output instead (0xC7-0xD0) |
| Vocoder I2S (pins 34-37: C_SDI/I2S_RX, C_SDO/I2S_TX, C_SCLK/I2S_CK, C_CS/I2S_FS) | master or slave. In SPI mode (0x22 bit 1 = 0) the chip is SPI master: C_SDO carries the codec ADC's voice to the vocoder, C_SDI brings the vocoder's data in |
| I2S master clocks | I2S_CK_M from 0x2F, I2S_FS_M from 0x32 / 0x33 (must be 8 kHz). I2S_CK_M > 34 x I2S_FS_M, codec clock >= 6 x I2S_CK_M |
| I2S master clock gate | 4.6.1: 0x36 bit 6 = 0 runs I2S_CK_M and I2S_FS_M, 1 stops them (the register table says 1 enables, conflict) |
| HR_V3000 | encoded voice over V_SPI, PCM over I2S (chip as I2S master), keys and frame sync over the MCU's UART. 0x06 = 0x24 |
| AMBE3000 / AMBE1000 | McBSP / CHS on pins 22-31; OPMode 101 / 111 reach their registers |

---

## 6. Interrupts and layer 2 timing (manual 5.1, 5.4)

### 6.1 Pins

| Pin | Use |
|---|---|
| TIME_SLOT_INTER (48) | every 30 ms once slot timing exists, until sync is lost (passive) or TxEn and RxEn are both cleared |
| SYS_INTER (49) | status and control events: read 0x82, handle, write 0x83. The layer 2 "Rdy_lst_inter" (0x86 bit 4) shares this pin |
| RF_TX_INTER (50) | only at a RX-to-TX slot change; advance from 0x12 |
| RF_RX_INTER (51) | only at a TX-to-RX slot change; advance from 0xC0 |

### 6.2 Layer 2 slot timing (figures 5.11-5.14)

| Time | Value | Meaning |
|---|---|---|
| t1 | 1.7 ms | the chip starts encoding and framing the next slot's burst |
| t2 | 27 ms | the window for the MCU to write the data and 0x41 for the next slot |
| t3 | under 3 ms (figure 5.11), 4 ms (5.12-5.14) | from slot end to the SYS_INTER with the decoded data |

At the timeslot interrupt (or at Rdy_lst_inter, if unmasked) the MCU sets 0x41 for the slot after the next; at the
following timeslot interrupt 0x42 reports what that slot is doing. 0x42 bits 7-5 (5.4.4): 001 working slot, TX and
RX off; 101 working slot, TX on; 011 working slot, RX on; xx0 not a working slot.

Active timing: 0x40 bit 5 = 1 with bit 7 or 6 set; the manual's TX value is 0xA3. Passive timing: 0x40 = 0x43 and
0x41 = 0x40 ("blind" continuous receive) until a burst is received; corrections within 1.25 ms are applied at once,
and the timing free-runs when the signal goes. The manual's recipe after a SYS_INTER: read the CC in 0x52; on a
mismatch write 0x41 = 0x20 to resynchronize; on a match decide the next slots from 0x51. Active full duplex: the
received bursts don't update the timing. Passive full duplex: they do.

Layer 2 setup (5.4.3): 0x10 = 0x68, 0x40 = 0x43, 0x41 = 0x40; the TX burst type goes in 0x50. With 0x40 bit 3 the
MCU supplies the check bits for the types in 表 5.5 (for example 80 CSBK bits plus its own 16-bit check in TX RAM
0-11); otherwise the chip follows the DMR standard.

### 6.3 Manual application examples (5.4.5)

| Service | Register sequence |
|---|---|
| Data TX | 0x10 = 0x6A; on request 0x40 = 0xA3; at a timeslot interrupt 0x41 = 0x80, 0x50 = 0x60, header in TX RAM 0x00-0x09; next 0x41 = 0x00; next 0x41 = 0x80, 0x50 = 0x70, 96 bits in 0x00-0x0B; repeat; finally 0x40 = 0x03 |
| Data RX | 0x10 = 0x6A, 0x40 = 0x43, 0x41 = 0x40; at SYS_INTER read 0x51 / 0x52 and the header's blocks-to-follow (RX RAM 0x08 bits 6-0); then 0x41 alternates 0x00 / 0x40 per slot; to stop, 0x40 = 0x03, 0x41 = 0x20 then 0x00 |
| Voice TX | 0x10 = 0x6A, 0x06 = 0x45; 0x40 = 0xA3; 0x41 = 0x80, 0x50 = 0x10 with the LC; next 0x41 = 0x00, 0x22 = 0x80 (encode on); then per our slot 0x50 = 0x08 (A), 0x19 (B), 0x2B (C), 0x3B (D), 0x4A (E), 0x58 (F), 0x41 = 0x00 between; terminator 0x50 = 0x20; then 0x41 = 0x00, 0x22 = 0x40 (encode off); finally 0x40 = 0x03 |
| Voice RX | 0x10 = 0x6A, 0x40 = 0x43, 0x41 = 0x40; on an LC header for us 0x22 = 0x20 (decode on); 0x41 alternates 0x50 / 0x00; on the terminator 0x22 = 0x10 (decode off) |

The voice TX example's 0x50 values carry the embedded LC's LCSS in bits 1-0 (B 01, C 11, D 11, E 10). The voice RX
example reads "0x50" where 0x51 is meant and compares the CC with 0x51 bits 7-4 (0x52 holds it); the data RX example
does the same with 0x51.

### 6.4 Physical layer BER test (5.4.6, 表 5.6)

| Reg | Value | Meaning |
|---|---|---|
| 0x01 | xxxx 0000 | IF receive |
| 0x07-0x09 | 0x0B, 0xD9, 0x54 | 455 kHz IF |
| 0x10 | 0000 0010 | physical layer, continuous (set bit 5 for slotted) |
| 0x40 | 0100 0000 | RX on, physical layer test mode |
| 0x41 | 0100 0001 | RX test enable (AutoTest) |

The chip stores each demodulated 36-byte burst at RX RAM 0x30 and raises 0x82 bit 0; the MCU compares it with what
was sent.

---

## 7. FM, analog signaling and MSK (manual 6, 7, appendix A)

### 7.1 FM transmit chain (6.1, figure 6.1)

0x10 bit 7 = 1 selects FM. FM is simplex only. 0x60 = 0x80 opens the analog TX path. The manual's chain:

mic → codec ADC → compressor (0x34 bit 6) → pre-emphasis (0x34 bit 5) → filter with soft limiter → FM modulator →
Tr_I / MOD1 and Tr_Q / MOD2.

| Block | Register | Fact |
|---|---|---|
| Band-pass | 0x34 bit 7 | 300-3400 Hz |
| Compressor | 0x34 bit 6, 0x2D bits 3-0 | syllabic: 2 dB in gives 1 dB out (60 dB range to 30 dB); 0x2D sets the 0 dB point. Use with an expander at the far end |
| Pre-emphasis | 0x34 bit 5 | TIA, +6 dB / octave over 300-3000 Hz |
| Low-pass with soft limiter | 0x34 bit 4 (and bit 3 for IQ / IF TX) | 2.55 kHz for 12.5 kHz channels, 3 kHz for 25 kHz |
| Deviation | 0x35, 0x3F | TX deviation coefficient; limiter. 6.1 says 0x3E and mic gain "0x0F" for baseband IQ / IF TX (conflict) |
| Sub-audio / tones | 0xA0, 0xA1 | added after weighting by 0xA0 |

Signaling carried on this chain: analog voice, CTCSS, CDCSS, DTMF, 2-tone, 5-tone and MSK.

### 7.2 FM receive chain (6.2)

0x60 = 0x00. IQ or IF in → filter, phase discriminator → audio filter → de-emphasis → expander → codec DAC.

| Block | Register | Fact |
|---|---|---|
| Low-pass | 0x34 bit 2 | 2.55 kHz (12.5 kHz channels) or 3 kHz (25 kHz) |
| De-emphasis | 0x34 bit 5 | TIA, -6 dB / octave over 300-3000 Hz |
| Expander | 0x34 bit 6, 0x2D bits 7-4 | 1 dB in gives 2 dB out; 0x2D sets the 0 dB point |
| Band-pass | 0x34 bit 7 | 300-3400 Hz |
| Squelch | 0x2B / 0x2C | squelch signal level (read only) |
| To the codec | 0x36 bit 1 | FM RX voice channel to the codec on / off |

### 7.3 Interrupts in FM mode (5.1.1, 8.1)

| 0x82 bit | FM meaning |
|---|---|
| 7 | send data request (发送数据请求中断) |
| 2 | FM function detection: a tone / code / selcall matched (sub-status 0x90) |
| 1 | receive data (接收数据中断) |

The manual pairs bits 7 and 1 with nothing else: no sub-status, no buffer index, no format.

### 7.4 CTCSS (6.1.1, 6.2.1, A2.2.1)

TX: sine table with phase accumulation; tone index 1-51 (62.5-254.1 Hz). On PTT release the tone continues about
155 ms with a phase reversal. RX: discriminator, offset calibration, limiter, 4th-order 300 Hz IIR low-pass,
magnitude against a threshold; a match sets 0x93 bit 0 and raises the FM interrupt.

| Step | Writes |
|---|---|
| mode | 0xA1 = 0x08 |
| TX deviation | 0xA0 = 0x0D (350 Hz) |
| tail | 0xA2 bits 5-4 = 01 (240°, default), 10 (180°), 00 off |
| RX threshold | 0xA7 = 0x10 |
| TX tone | 0xA8 = index |
| RX sampling depth | 0xD3 = 0x07, 0xD2 = 0xD0: 2000 samples (8 kHz, 250 ms) |
| RX tone | 0xD4 = index |
| speaker | on a match 0x36 = 0x72, else 0x36 = 0x80 |

| Index | Hz | Index | Hz | Index | Hz | Index | Hz |
|---|---|---|---|---|---|---|---|
| 1 | 67.0 | 14 | 107.2 | 27 | 167.9 | 40 | 62.5 |
| 2 | 71.9 | 15 | 110.9 | 28 | 173.8 | 41 | 159.8 |
| 3 | 74.4 | 16 | 114.8 | 29 | 179.9 | 42 | 165.5 |
| 4 | 77.0 | 17 | 118.8 | 30 | 186.2 | 43 | 171.3 |
| 5 | 79.7 | 18 | 123.0 | 31 | 192.8 | 44 | 177.3 |
| 6 | 82.5 | 19 | 127.3 | 32 | 203.5 | 45 | 183.5 |
| 7 | 85.4 | 20 | 131.8 | 33 | 210.7 | 46 | 189.9 |
| 8 | 88.5 | 21 | 136.5 | 34 | 218.1 | 47 | 196.6 |
| 9 | 91.5 | 22 | 141.3 | 35 | 225.7 | 48 | 199.5 |
| 10 | 94.8 | 23 | 146.2 | 36 | 233.6 | 49 | 206.5 |
| 11 | 97.4 | 24 | 151.4 | 37 | 241.8 | 50 | 229.1 |
| 12 | 100.0 | 25 | 156.7 | 38 | 250.3 | 51 | 254.1 |
| 13 | 103.5 | 26 | 162.2 | 39 | 69.3 | | |

### 7.5 CDCSS (6.1.2, 6.2.2, A2.2.2)

TX: the 12 data bits are Golay(23,12) coded and sent repeatedly, NRZ mapped, two interpolation filters, into the FM
modulator. RX: low-pass, FM demodulation, two low-pass stages, offset compensation, decimation at 7x the symbol
rate, hard decision, Golay decoding.

| Step | Writes |
|---|---|
| mode | 0xA1 = 0x04 |
| TX deviation | 0xA0 = 0x03 (350 Hz) |
| polarity / tail | 0xA2 bits 3-1 (default 001 per A2): bit 3 TX inverted, bit 2 RX inverted, bit 1 turn-off code |
| TX code | 0xAC bit 0 = code bit 8, 0xAB = bits 7-0. Code "114" is 0xAB = 0x4C: the octal code number as a 9-bit binary value |
| RX sampling depth | 0xD3 bits 3-0 = 0x3, 0xD2 = 0x20: 800 samples (8 kHz, 100 ms) |
| RX code | 0xD3 bit 4 = bit 8, 0xD4 = bits 7-0 |
| turn-off code detector | aux {0x104, 0x103, 0x102} = 0x7F499D (8341917): 2 cos(2 pi 134.4 / 8000) x 2^22 |
| speaker | as CTCSS: a match sets 0x93 bit 0; 0x36 = 0x72 on, 0x80 off |

On PTT release with the turn-off code enabled, a 134.4 Hz tone follows.

### 7.6 DTMF (6.1.3, 6.2.3, A2.2.3)

TX: two table-driven sines, the low group 2.5 dB below the high group, weighted and FM modulated. The manual's usual
timing is about 600 ms of quiet after PTT, then 50 ms per code and 50 ms gap. RX: eight-frequency magnitude analysis
behind a 3 kHz 4th-order IIR low-pass; each decoded code raises an interrupt.

| Step | Writes |
|---|---|
| mode | 0xA1 = 0x02 |
| TX deviation | 0xA0 = 0x39 (1.8 kHz) |
| code time | 0xA4 = 0x32 (100 ms, 2 ms steps) |
| gap | 0xA3 = 0x19 (50 ms, 2 ms steps) |
| RX threshold | 0xA7 = 0x0A |
| TX digits | 0xAF, 0xAE, ... 0xA8, up to 16 digits, 4 bits each, from the top: "1234" is 0xAF = 0x12, 0xAE = 0x34 |
| TX count | 0xD1 bits 4-0 = number of digits (4 for "1234") |
| RX sampling depth | 0xD3 bits 3-0 = 0x4, 0xD2 = 0x20: 1056 samples (32 kHz, 33 ms) |
| send | each new PTT sends the sequence once |
| receive | at each FM interrupt queue 0x93 bits 3-0; when 0x93 bit 4 = 1 compare the queue with the expected digits; 0x36 = 0x72 on a match, 0x80 otherwise |

The 4-bit codes for 0, *, #, A-D are not stated (the manual shows only the standard keypad grid).

| Hz | RX coefficient (aux, 24 bit) | Address | TX phase step (aux, 16 bit) | Address |
|---|---|---|---|---|
| 697 | 0x7ECD9D | 0x104-0x102 | 0x0593 | 0x11B-0x11A |
| 770 | 0x7E8A34 | 0x107-0x105 | 0x0629 | 0x11D-0x11C |
| 852 | 0x7E368C | 0x10A-0x108 | 0x06D1 | 0x11F-0x11E |
| 941 | 0x7DD245 | 0x10D-0x10B | 0x0787 | 0x121-0x120 |
| 1209 | 0x7C690D | 0x110-0x10E | 0x09AC | 0x123-0x122 |
| 1336 | 0x7B9F03 | 0x113-0x111 | 0x0AB0 | 0x125-0x124 |
| 1477 | 0x7AA7A8 | 0x116-0x114 | 0x0BD1 | 0x127-0x126 |
| 1633 | 0x7979F5 | 0x119-0x117 | 0x0D10 | 0x129-0x128 |

RX coefficient = 2 cos(2 pi f / 32000) x 2^22; TX step = f / 32000 x 2^16. Addresses are high byte first in the
first column, low byte at the lower address.

### 7.7 Prompt tone path (appendix A1)

For power-on, call and message tones. Speaker path only.

| Step | Writes |
|---|---|
| codec choice | 0x06 bit 2: built-in or external |
| external codec | 0x36 bit 7 = 0 (BCLK, LRCK on); 0x30 / 0x31 BCLK; 0x32 / 0x33 LRCK |
| built-in codec | 0x0D = 0x10 (codec in normal mode, AD / DA switched by the system; change for low power); 0x0E = 0x8E (MicEn, HPoutEn, LineoutEn; HPoutVol 0 dB); 0x0F = 0xB8 (ADLinVol and MicVol 0 dB); 0x38 = 0x00 |
| level | 0x37 = 0x00 (DAC data level unchanged) |
| open | 0x06 bit 1 = 1, then write samples at once |
| close | 0x06 bit 1 = 0 as soon as the tone ends |

Data: U_SPI Cmd = 0x03, Addr = 0x00, then 16-bit DAC samples (DACDATA), two bytes each. The FIFO is 64 samples deep.
Every 1 ms the MCU reads 0x88 bit 0; when it reads 0, the MCU writes 32 samples in one transaction (the figure
shows Data0-Data63, 64 bytes). Byte order is not stated.

### 7.8 FM path setup and selcall (appendix A2)

A2.1: 0x06 bit 1 = 0 and 0x10 bit 7 = 1 (prompt tone path closed, FM path open); 0x06 bit 2 chooses the codec, which
is then set up as in A1; 0x37 = 0x00.

2-tone (A2.2.4) and 5-tone (A2.2.5), CCIR1 by default:

| Step | 2-tone | 5-tone |
|---|---|---|
| mode | 0xA1 = 0x10 | 0xA1 = 0x20 |
| TX deviation | 0xA0 = 0x39 (1.8 kHz) | 0xA0 = 0x39 |
| tone time | 0xA4 = 0x05 first tone, 0xA5 = 0x05 second tone (500 ms, 100 ms steps) | 0xA4 = 0x05 each tone |
| gap | 0xA3 = 0x05 (500 ms) | 0xA3 = 0x05 |
| long tone | 0xA6 = 0x0A (1000 ms) | - |
| RX threshold | 0xA7 = 0x0A | 0xA7 = 0x0A |
| TX code | {0xAF bits 3-0, 0xAE bits 7-4}: a pair from A-D ("A-B") or one long tone ("long A") | {0xAF bits 3-0, 0xAE, 0xAD}: five tones ("A-B-C-D-E") |
| RX sampling depth | 0xD3 bits 3-0 = 0xC, 0xD2 = 0x80: 3200 samples (32 kHz, 100 ms) | same |
| receive | compare 0x93 with the two-tone address and 0x94 bits 3-0 with the long-tone address | compare {0x95 bits 3-0, 0x94, 0x93} |
| speaker | 0x36 = 0x72 on a match, 0x80 otherwise | same |

| Tone | Hz | RX coefficient | Address | TX phase step | Address |
|---|---|---|---|---|---|
| A | 1981 | 0x767041 | 0x104-0x102 | 0x0FD9 | 0x11B-0x11A |
| B | 1124 | 0x7CE537 | 0x107-0x105 | 0x08FE | 0x11D-0x11C |
| C | 1197 | 0x7C7B1E | 0x10A-0x108 | 0x0993 | 0x11F-0x11E |
| D | 1275 | 0x7C0285 | 0x10D-0x10B | 0x0A33 | 0x121-0x120 |
| E | 1358 | 0x7B7A03 | 0x110-0x10E | 0x0ADD | 0x123-0x122 |

Each sequence goes out once per PTT.

### 7.9 MSK (chapter 7)

TX: framed data at 1.2 kbit/s, mapped, interpolated to 19.2 kHz, MSK modulated and moved to a 1.5 kHz IF by an NCO
in one block (continuous phase), then into the FM modulator. RX (MPT1327-style, non-coherent): FM demodulation,
band-pass, differential phase detection, NCO, low-pass, a second differential detection, arrival detection and
timing sync, hard decision at 1.2 kbit/s.

MCU flow (7.3):

| Step | Registers |
|---|---|
| init | aux 0x12A TrainErrorThreshold = 5, 0x12B DTBeforeTransAndRec = 160, 0x12D ChannelDelay = 20, 0x12C NT = 103 |
| control channel TX | at a receive-slot interrupt fill the TX ping-pong buffer, then OperationType = 1, ChannelType = 0, TranOrRecFlag = 1, MacFrameEn (1 MAC framing, 0 PHY framing), Unsolicited = 0, MultiMessageTransFlag = 0 |
| on each half-full interrupt (0x82 bit 6) | PHY framing: send the next 64 bits, or set MACTransFinishFlag when done. MAC framing: 64 bits at a time; with the last (possibly short) block set MACTransFinishFlag and RemBitNum |
| traffic channel TX | as control, ChannelType = 1, Unsolicited = 1; between messages of a multi-message set MultiMessageTransFlag (PHY framing) |
| data channel TX | as control, ChannelType = 2 |
| RX | OperationType = 1, ChannelType 0 / 1 / 2, TranOrRecFlag = 0, AcqEnable = 1, RxInterMask = 0, CtrlDataInterMask = 0 (1 on the data channel); on each receive pulse read RecBitLen (0x96), TrainCodewordFlag and TrainErrorBitNum (0x97) and then the data |
| re-acquire | SoftReset 1 then 0 |

MSK-mode interrupts (0x82): bit 6 TX ping-pong buffer half full, bit 5 PHY TX finished, bit 4 response, bit 3
receive. Two interrupt lines are used: a receive-slot interrupt (which also stands for "demodulated data ready",
with a flag saying whether the data is valid) and a control-data interrupt (half-full and TX-finished, told apart
by register). The manual never says where the MSK TX ping-pong buffer is or how it is written.

---

## 8. FM transmit from MCU data: what the manual gives

The open question is how to make the chip transmit FM audio the MCU writes into TX RAM 0x030-0x42F. Everything the
manual says that touches it:

| Item | Source | What it says |
|---|---|---|
| TX RAM FM buffers 1 and 2 | 表 5.2 | 512 bytes each; "can be FM transmit voice data written externally; the voice data can also come internally from the codec" |
| 0x82 bit 7 in FM | 8.1 | "send data request" interrupt |
| 0x82 bit 1 in FM | 8.1 | "receive data" interrupt |
| 0xD7 bit 1 msk_voice_send_en | 8.1, 7.4.2 | 0 = send MSK signaling, 1 = send FM voice. Listed with the MSK controls, default 0x00 |
| 0xA1 bit 6 | 8.1 | MSK mode |
| 0xD5 OperationType / TranOrRecFlag / ChannelType | 8.1, 7.3 | MSK PHY state; "fill the TX ping-pong buffer, then set these" |
| 0x60 bits 7-6 | 8.1, 6.1 | 10 voice TX request, 01 data TX request; FM TX = 0x80 |
| 0x06 bit 2 | 8.1, A1, A2 | built-in or external codec for the FM path |
| 0x06 bit 1 | A1, A2.1 | prompt tone path; A2.1 closes it to open the FM path |
| 0x88 bit 0 | 8.1, A1 | the MCU-written PCM FIFO's full flag |
| 0xE3 bit 0 | 8.1 | codec internal DAC-ADC pass-through |

Nothing names a sample format, a rate, a buffer order or a bit that selects RAM over the codec ADC. The register
the manual pairs with "send FM voice" is **0xD7 bit 1**, not 0xDB (section 9.3).

---

## 9. Translation notes

### 9.1 Where the English translations are wrong or misleading

| Place | Chinese | Translation says | Correct reading |
|---|---|---|---|
| everywhere (pin list, chapter 6, A2) | 模拟 | "simulation", "simulates" (pin list: "Codec simulates a 3.3V power supply"; 6.1: "In simulation mode") | analog: "analog 3.3 V supply", "in analog mode" |
| feature list, figure 6.1, 6.1 | 加重 / 去加重 | "aggravation", "weighting and de-weighting", "Aggravate" | pre-emphasis / de-emphasis |
| 0x02 / 0x04 (G4EML) | 两点模式下：调整输出 MOD2 路偏置 (both rows) | 0x02 "transmit I or Mod1 output", 0x04 "Q or Mod2 output" | the table says MOD2 for both; 4.7.2 says 0x04 = MOD1, 0x02 = MOD2 (9.2) |
| 0x1D / 0x1E masks (G4EML) | 匹配码为 1 的对应 bit ... 予以忽略 | "1 = compare, 0 = ignore" | a mask bit of 1 means **ignore** that address bit |
| 0x11 bit 7 / 6 (G4EML) | 置 1 有效。在中继模式下表示 RepeatedSimplex；在直接模式下表示 DirectSimplex | "0 = Direct Simplex, 1 = Repeated Simplex" | 1 = simplex in either mode; repeater vs direct comes from the mode, not the bit |
| 0x11 bit 0 (G4EML) | 只开启数字或者模拟接收 | "only numbers or modes" | digital or analog receive only |
| 0x41 bit 3 (G4EML) | 0 表示接收需要 CC 匹配才做解帧处理，1 表示接收无需 CC 匹配便做解帧处理 | "0 = CC decode off, 1 = CC decode on" | 0 = deframe only on a CC match, 1 = deframe regardless of CC |
| 0x51 SyncClass 00 (G4EML) | 无同步头 | "Sync Header" | no sync |
| 0x85 (G4EML) | SendStart 中断的使能 | "mask bits for reg 0x86" | enables for 0x84 |
| 0x86 bit 3 (G4EML) | 需要等待反馈的确认数据包的一个 fragment 发送完成 | "confirmation packet received" | a fragment that needs a response was **sent** |
| 0x82 FM meanings (G4EML) | FM 模式下表示发送数据请求中断 / 接收数据中断 / FM 功能接收检测中断 | absent: bit 7 "request rejected", bit 1 "voice unusual exit" only | FM: bit 7 send data request, bit 2 FM tone detect, bit 1 receive data |
| 0x82 bit 1 FM (updated translation) | FM 模式下表示接收数据中断 | "it indicates that receiving data is interrupted" | a "receive data" interrupt, not an interruption |
| 0x36 bit 1 (G4EML) | FM 模式下接收开启语音通道 Codec 开关 | "Open FM Voice Channel" | FM **receive** voice channel to the codec on / off; nothing to do with TX |
| 0x37 bit 7 (G4EML) | mcu 控制内置与外置 Codec 的 DACDATA 变化 | "MCU Control Codec enable" | enables the MCU's DAC data level change (volume step), not codec control |
| 0x3E (G4EML) | FM 收端调制频偏系数 | "FM Modulation Frequency Offset" | FM **receive** deviation coefficient |
| 0x32 / 0x33 (G4EML) | 配置 I2S_FS_M 的时钟频率 | "Set Codec Operating Frequency" | I2S frame clock (LRCK) divider, must give 8 kHz |
| 0x2A (G4EML) | C_SPI 时钟 = Codec 时钟 / [2 x (spi_clk_cnt + 1)] | "Codec Clock / (value + 1)" | the factor 2 is missing |
| 0x42 bit 5 (G4EML) | 0 表示当前时隙是通信使用的，1 表示...不使用的 | "current timeslot active" (1 = active) | the table says 0 = in use (but see 9.2) |
| 0x0F access (G4EML) | 0x0F R | "R/W" | 0x0F is read only |
| 0xB9 default (G4EML) | 0x05 | 0x04 | 0x05 |
| 5.4.4 (all) | 盲收 | "blind reception" | continuous receive before any sync (searching) |
| 5.4.5 (all) | 忙收 | "busy receiving" | the same continuous receive-and-search state that 5.4.4 calls 盲收 ("blind" receive); not a busy channel |
| 1.2KRAM (all) | 发端 / 收端 RAM | "send end / receive end RAM" | TX RAM / RX RAM |

### 9.2 Conflicts inside the Chinese manual

| Item | One place | Other place |
|---|---|---|
| Two-point offsets 0x02 / 0x04 | 4.7.2: 0x04 = MOD1 offset, 0x02 = MOD2 offset. Figure 4.21: MOD1 drives the reference crystal, MOD2 the VCO | table 8.1: both 0x02 and 0x04 "adjust the MOD2 output offset" |
| MOD1 / MOD2 pins | pin list: 74 DAC_QVOUT/MOD2, 75 DAC_IVOUT/MOD1; figure 4.21 agrees | 表 4.5: 0x45 "MOD2 (DAC_IVOUT)", 0x46 "MOD1 (DAC_QVOUT)" |
| 0x47 / 0x48 bias halves | table 8.1 and 表 4.5: 0x47 low 8 bits, 0x48 bits 1-0 the high 2 | 4.7.2: "0x47[1:0] and 0x48[7:0], with 0x47[1:0] the high 2 bits" |
| 0x12 bit 6 test signal | table 8.1: 40 Hz sine | 4.7.2: 40 Hz square wave |
| 0x42 bit 5 polarity | table 8.1: 0 = slot in use | 5.4.4: bits 7-5 = xx1 is a working slot, xx0 is not |
| FM sub-status of 0x82 bit 2 | 5.1.1: 0x90 = 0x10 | table 8.1: 0x90 bit 5 (0x20) |
| 0x36 bit 6 I2S clock gate | table 8.1: 1 enables I2S_CK / I2S_FS | 4.6.1: 0x36[6] = 0 runs them, 1 stops them |
| 0x0D / 0x0E / 0x0F | table 8.1: voice superframe timeout, reserved, FSK error (read only) | A1: written 0x10, 0x8E, 0xB8 as built-in codec settings |
| FM TX deviation | table 8.1: 0x35 TX coefficient, 0x3E RX coefficient | 6.1: "0x3E and the mic gain 0x0F" adjust the deviation (IQ / IF TX) |
| 0xA1 bit 7 | table 8.1: reserved, must be 0 | 表 4.8: AF / non-AF receive mode |
| 0xA2 bits 3-1 default | A2.2.2.1: 001 | table 8.1: default 0x10 (bits 3-1 = 000) |
| 0xA2 bit list in A2.2.2.1 | "0xa2[2]" is listed twice | the second is bit 1, the turn-off code |
| 0x86 bit 0 | table 8.1: GPS PPS lost | 5.1.1: "none" |
| 0x05 name | table 8.1 prints RecvIsigCenter | it is the Q offset (RecvQsigCenter) |
| Rate 3/4 and rate 1 RAM rows | address column 0x00-0x11 / 0x00-0x17 | description column 0x00-0x0B |
| 表 5.5 codes | 1011 listed twice, 1101 missing | |
| Layer 2 mode value | 5.4.3: 0x10 = 0x68 | 5.4.5: 0x10 = 0x6A |

### 9.3 Conflicts with ODR's current docs and code

| Item | ODR | Manual |
|---|---|---|
| "send FM voice" bit | modem-hrc6000.md 8.1, hardware-dm1701.md 21.3 and `tests/hil/test_audio_probe.py` use **0xDB** bit 1 | 0xD7 bit 1 (msk_voice_send_en). 0xDB is not in the manual. The bench result "0xDB bit 1: no FM TX from RAM" says nothing about 0xD7 |
| CDCSS TX polarity | modem-hrc6000.md 8: 0xA2 = 0x00 inverted, 0x08 normal | 0xA2 bit 3 = 1 means TX inverted. The RX mapping (0x04 inverted) agrees with the manual |
| Page 1 table at 0x60 | modem-hrc6000.md 3: a 32-entry table of `nn EF 00` in the trace notes | aux 0x60-0xBF is the 32-entry multi-group address table; `nn EF 00` matches the group default 0x00EF33 pattern (L, M, H) |
| Page 3 flush | modem-hrc6000.md 3.2: "vocoder frame buffer" | U_SPI OPMode 011 with 0x06 bit 1 is the prompt tone FIFO (7.7) |

---

## 10. Bench notes

Only facts already recorded in [modem-hrc6000.md](modem-hrc6000.md) and [hardware-dm1701.md](hardware-dm1701.md) 21.3.1, plus
the 2026-10-12 two-point measurement. DM-1701 throughout.

| Register | Bench fact |
|---|---|
| OPMode 010 | writes land in the TX RAM and go out bit exact; OPMode 110 untried |
| 0x01 | 0xF0 in DMR (two-point TX, IF RX, bit 7 = 1) |
| 0x04 | **FM TX, two-point mode (2026-10-12):** writing 0x04 moves the carrier about 75 Hz per step, as a signed byte around the calibrated base 0xCF, linear over ±128. Firmware writing 0x04 at 37,500 writes a second sends tones of 300-3000 Hz, about 57 Hz of deviation per step at 1 kHz |
| 0x02, 0x03, 0x05, 0x45, 0x46, 0x47, 0x48 | stepped while keyed in FM: no carrier movement (2026-10-12) |
| 0x10 | 0x6E in DMR operation; 0x80 for FM. **Every FM value (0x81-0xFF) leaves the TX RAM unused** (2026-10-12). 0x66 (physical layer, timeslot) modulates TX RAM 0x00-0x23 / 0x24-0x47 as 288 raw bits per slot in alternate slots |
| 0x40 bit 3 | set: the 96 info bits go out verbatim (MCU CRC); also upsets the receiver's deframing while set |
| 0x41 | 0x80 send, 0x50 receive, 0x20 drop sync; passive timing corrects within 1.25 ms |
| 0x52 (W) | layer 2 BS CACH stays all zero on air whatever is written |
| 0x82 | bit 7 is seen set when read on other interrupts. FM, keyed: bit 7 every 32 ms (31.0 a second) once 0x81 unmasks it. FM receive: bit 1 every 32 ms once unmasked (0x81 = 0x19 masks it) |
| RX RAM FM buffers | 8 kHz, 16-bit big endian receive audio, ungated, after 0x34's processing; one 512-byte half per 32 ms, filled progressively |
| 0x34 | de-emphasis (bit 5), band-pass (bit 7) and the wider RX filter (bit 2) act on the RX RAM tap; a fixed sub-audio high-pass stays about 20 dB down below 300 Hz |
| TX RAM FM buffers | a 1 kHz tone in both halves, either byte order, never reached the air with: the flags unmasked, "0xDB" bit 1, 0x60 = 0x40 or 0xC0, 0x06 bits 0, 2, 3 or 4, 0xE0 bit 7 off, 0xE2 bit 4. The chip doesn't clear or rewrite the TX RAM |
| Prompt tone FIFO | drains at 8000 samples a second whether 0x06 bit 1 is set or not; never reaches the modulator, with or without 0xE3 bit 0 |
| 0xE0 bit 5 (LINEIN2) | puts a 2.49-2.61 kHz tone of its own on air during FM TX |
| 0xE3 bit 0 | no effect on FM TX from I2S; lowers FM RX on I2S |
| 0x25, 0x26, 0xE1, 0xE2, 0xE3, 0xE5, 0x0D, 0x0E, 0x0F, 0x38 | each bit flipped while keyed: no MCU audio on air |
| 0x36 | 0x02 / 0x00 feed-through; also gates the I2S receive stream |
| aux 0x04-0x09 | the TX data sync; voice sync is sent as its inverse |

The 2026-10-12 result fits the manual's 4.7.2 reading of 0x04 as the MOD1 offset: MOD1 drives the reference
oscillator, where a DC step moves the carrier, while a DC step on the VCO side (MOD2, 0x02) is pulled back by the
synthesizer loop. That is an inference, not a measurement of the wiring.
