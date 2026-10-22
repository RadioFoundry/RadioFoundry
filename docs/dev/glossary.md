<!-- SPDX-License-Identifier: MIT -->
<!-- Generated from docs/glossary.json by tools/glossary.py. Edit the data file, not this page. -->
<!-- glossary: off -->

# Developer glossary

Protocol, hardware and firmware terms of the specs and developer docs. Terms the user guide uses are in the [glossary](../user/22-glossary.md).

## DMR

| Term | Meaning |
|---|---|
| <a id="t-ais"></a>AIS | Air interface specification, the ETSI name for DMR's over-the-air rules. |
| <a id="t-als"></a>ALS | Ambient listening service: a dispatcher turns on a radio's microphone remotely. |
| <a id="t-at"></a>AT | Access type: the TACT bit that tells a radio whether the slot is free to transmit in. |
| <a id="t-ber"></a>BER | Bit error rate: the share of received bits that were wrong. A measure of how clean a signal is. |
| <a id="t-bptc"></a>BPTC | Block product turbo code: the error correction on DMR headers and control blocks. |
| <a id="t-bs"></a>BS | Base station: the repeater or modem side of a DMR link, as opposed to the radio (MS). |
| <a id="t-bs-dwn-act"></a>BS_Dwn_Act | The CSBK that tells a repeater to start transmitting. It wakes a repeater. |
| <a id="t-burst"></a>Burst | One DMR transmission in a slot: 264 bits, with a sync pattern or embedded signaling in the middle. |
| <a id="t-c-ackd"></a>C_ACKD (C_NACKD, C_WACKD, C_QACKD) | A Tier III CSBK in which the TSCC accepts a request. |
| <a id="t-c-acku"></a>C_ACKU (P_ACKU) | A Tier III CSBK in which a radio acknowledges or answers, for example the authentication response. |
| <a id="t-c-ahoy"></a>C_AHOY | A Tier III CSBK that checks on a radio: an authentication challenge or the called party's radio check. |
| <a id="t-c-aloha"></a>C_ALOHA | The Tier III CSBK a trunked base station sends to announce the system and its slot timing. |
| <a id="t-c-rand"></a>C_RAND | The Tier III CSBK a radio sends to ask for something on the control channel: registration, a call. |
| <a id="t-c-sys-parms"></a>C_SYS_Parms | The Tier III system parameters a TSCC sends in its CACH (SLCO 0010): model, network, site, registration flag and slot counter. |
| <a id="t-c-syscode"></a>C_SYScode | The 16-bit system code of a Tier III system: MODEL, NET, SITE and PAR. C_ALOHA carries all of it, the CACH the top 14 bits. |
| <a id="t-cach"></a>CACH | Common announcement channel: 24 bits ahead of each burst a repeater sends. It carries the slot number and short link control. |
| <a id="t-codec"></a>Codec | A voice codec. DMR uses AMBE+2. |
| <a id="t-csbk"></a>CSBK (CSBKs) | Control signaling block: a one-burst DMR message for signaling such as call alert, radio check and Tier III control. [More](../spec/dmr-data.md) |
| <a id="t-csbko"></a>CSBKO | CSBK opcode: the field of a CSBK that says its kind. |
| <a id="t-dd"></a>DD (DD format) | Defined data format: the field of a short data header that says how its payload is coded (7-bit text, ISO 8859, UTF-16 and so on). |
| <a id="t-dgna"></a>DGNA | Dynamic group number assignment: the Tier III system moves a radio into a talkgroup over the air. |
| <a id="t-direct-mode"></a>Direct mode (DMO) | DMR radio to radio on one frequency, with no repeater. The standard calls it DMO. |
| <a id="t-dmrla"></a>DMRLA | DMR location area: how many of the most significant bits of SITE form SYS_AREA, the registration area. |
| <a id="t-dpf"></a>DPF | Data packet format: the field of a data header that says what follows (unconfirmed or confirmed data, UDT, short data). |
| <a id="t-dt"></a>DT | Data type: the field of a burst's slot type that says what the burst carries (header, terminator, rate 1/2 block and so on). |
| <a id="t-emb"></a>EMB | Embedded signaling: the 32 bits in the middle of a voice burst that carry link control. |
| <a id="t-etsi"></a>ETSI | European Telecommunications Standards Institute, which publishes the DMR standard. |
| <a id="t-fec"></a>FEC | Forward error correction: extra bits that let the receiver repair errors without a resend. |
| <a id="t-fid"></a>FID | Feature set ID: the byte that says whether a message is standard or manufacturer specific. |
| <a id="t-flco"></a>FLCO | Full link control opcode: the field of a link control message that says its kind. |
| <a id="t-foacsu"></a>FOACSU | Full off air call set up: OACSU with alerting, so the called radio rings and answers before voice. |
| <a id="t-fsn"></a>FSN | Fragment sequence number: counts the fragments of a data packet that was split. |
| <a id="t-lb"></a>LB | Last block: the flag that marks the final block of a CSBK or data message. |
| <a id="t-lc"></a>LC (embedded LC) | Link control: the call's addressing (source, destination, group or private) that DMR repeats through a voice call. |
| <a id="t-lcss"></a>LCSS | Link control start/stop: the 2-bit field of a burst's embedded signaling that says which fragment it carries. |
| <a id="t-llid"></a>LLID | Logical link ID: the 24-bit address (a DMR ID or a talkgroup number) in a data header. |
| <a id="t-lpcn"></a>LPCN | Logical physical channel number: the number of a channel in a Tier III channel plan. |
| <a id="t-mbc"></a>MBC | Multi block control: a DMR signaling message that spans several bursts. |
| <a id="t-mi"></a>MI | Message indicator: the value that changes with each call and keys its encryption. It travels in the PI header. |
| <a id="t-mpt-1327"></a>MPT 1327 (MPT) | The analog trunking standard that Tier III descends from. |
| <a id="t-ms"></a>MS | Mobile station: the radio, as opposed to the base station (BS). |
| <a id="t-oacsu"></a>OACSU | Off air call set up: signaling that sets up a call before voice is sent. The radio asks over the air and waits for the answer. |
| <a id="t-p-sys-parms"></a>P_SYS_Parms | The same system parameters a Tier III payload channel sends in its CACH (SLCO 0011). |
| <a id="t-pabx"></a>PABX | Private automatic branch exchange: an office telephone system. Tier III can connect calls to it. |
| <a id="t-pdu"></a>PDU (PDUs) | Protocol data unit: one packet or message of a protocol, as the standard defines its fields. |
| <a id="t-pf"></a>PF | Protect flag: the bit in link control that marks a protected (encrypted) call. |
| <a id="t-psn"></a>PSN | A 3-octet number XORed into the Tier III authentication response when a network uses it. |
| <a id="t-pstn"></a>PSTN | Public switched telephone network. Tier III can connect calls to it. |
| <a id="t-pv-grant"></a>PV_GRANT (TV_GRANT, BTV_GRANT) | A Tier III grant: the TSCC tells radios which payload channel carries a call. PV is a private voice call, TV a talkgroup voice call, BTV a broadcast talkgroup voice call. |
| <a id="t-rc"></a>RC | Reverse channel: signaling the receiver sends back to the transmitter during a call, in space the burst leaves free. |
| <a id="t-repeater-mode"></a>Repeater mode (RMO) | DMR through a repeater, which receives on one frequency and transmits on another. The standard calls it RMO. |
| <a id="t-said"></a>SAID (DAID) | Source address ID: the field of a compressed UDP/IP header that says where the source IP address comes from, for example from the LLID. |
| <a id="t-sap"></a>SAP | Service access point: the field in a DMR data header that says what the payload is (IP, short data, GPS and so on). |
| <a id="t-slco"></a>SLCO | Short link control opcode: the field of a short LC message that says its kind. |
| <a id="t-spid"></a>SPID (DPID) | Source port ID: the index in a compressed UDP/IP header that stands for a well-known UDP port. |
| <a id="t-sync"></a>Sync | The 48-bit pattern in the middle of a burst. It tells the receiver the burst's kind (voice or data, base station or radio) and its timing. |
| <a id="t-tact"></a>TACT | The 7 bits at the start of a CACH: AT, TC, LCSS and Hamming parity. |
| <a id="t-tc"></a>TC | TDMA channel: the TACT bit that names the timeslot, so a radio can tell TS1 from TS2. |
| <a id="t-tdma"></a>TDMA | Time division multiple access. A DMR channel carries two conversations by alternating them in 30 ms slots. |
| <a id="t-tier-ii"></a>Tier II | The conventional DMR tier of the standard (TS 102 361-2): repeaters and direct mode without trunking. |
| <a id="t-tier-iii"></a>Tier III | The trunked DMR tier of the standard (TS 102 361-4): a control channel assigns traffic channels. [More](../spec/dmr-tier3-plan.md) |
| <a id="t-tsc"></a>TSC | The controller of a Tier III system: the infrastructure side the radio talks to. |
| <a id="t-tscc"></a>TSCC | Trunked station control channel: the Tier III channel that carries control signaling. |
| <a id="t-tsccas"></a>TSCCAS | TSCC alternate slot: a second control slot of a Tier III site. |
| <a id="t-udt"></a>UDT | Unified data transport: DMR short data sent as a plain block of bytes. |

## RF and hardware

| Term | Meaning |
|---|---|
| <a id="t-adc"></a>ADC | Analog to digital converter. |
| <a id="t-afc"></a>AFC | Automatic frequency control: following a transmitter that is slightly off frequency. |
| <a id="t-agc"></a>AGC | Automatic gain control: the receiver adjusting itself to the signal level. |
| <a id="t-at1846s"></a>AT1846S | The RF transceiver chip that sets the frequency, deviation and power of many handheld DMR radios. Programmed over I2C. |
| <a id="t-cpu"></a>CPU | Central processing unit. |
| <a id="t-crc"></a>CRC | Cyclic redundancy check: a checksum that detects damaged data. |
| <a id="t-dac"></a>DAC | Digital to analog converter. |
| <a id="t-deviation"></a>Deviation | How far an FM transmitter's frequency swings from the channel center. It sets loudness and bandwidth. |
| <a id="t-dma"></a>DMA | Direct memory access: moving data between memory and a peripheral without the processor. |
| <a id="t-dsp"></a>DSP | Digital signal processor: a chip, or a processor core, that does the audio and modem math. |
| <a id="t-ecc"></a>ECC | Error correcting code: a code that repairs damaged bits, such as the one a NAND flash needs. |
| <a id="t-eeprom"></a>EEPROM | Electrically erasable programmable read-only memory: a small chip that keeps settings. |
| <a id="t-fifo"></a>FIFO | First in, first out: a queue of data between a producer and a consumer. |
| <a id="t-fsk"></a>FSK | Frequency shift keying: data as frequency steps. DMR uses 4-level FSK. |
| <a id="t-gpio"></a>GPIO | General purpose input/output: a pin the firmware can read or drive. |
| <a id="t-hr-c6000"></a>HR_C6000 (HRC6000, C6000) | The HRC6000 family: the baseband chip that codes and decodes DMR bursts in the DM-1701 and MD-380 class radios. |
| <a id="t-hr-c7000"></a>HR_C7000 (HRC7000, C7000) | The HRC7000 family: the baseband chip of the DM-32UV and DM-1801 Pro class radios. |
| <a id="t-i2c"></a>I2C (I²C) | Inter-integrated circuit: a 2-wire bus for small chips such as the RF transceiver. |
| <a id="t-i2s"></a>I2S | Inter-IC sound: the bus audio samples travel on between chips. |
| <a id="t-jtag"></a>JTAG | A debug and test port of many processors. |
| <a id="t-mcu"></a>MCU | Microcontroller: the processor that runs the firmware. |
| <a id="t-nand"></a>NAND | The flash chip type that holds large data, such as the codeplug and contacts, in some radios. |
| <a id="t-nor"></a>NOR | The flash chip type that holds code and can run it in place. |
| <a id="t-otg"></a>OTG (OTG_FS) | USB On-The-Go: the controller can act as the host or the device. |
| <a id="t-pa"></a>PA | Power amplifier: the stage that boosts the transmit signal to the output power. |
| <a id="t-pcm"></a>PCM | Pulse code modulation: audio as a stream of sample numbers. |
| <a id="t-pll"></a>PLL | Phase locked loop: the circuit that holds the radio's synthesizer on its frequency. |
| <a id="t-ppm"></a>ppm | Parts per million: a frequency error as a fraction of the frequency. At 450 MHz, 1 ppm is 450 Hz. |
| <a id="t-pwm"></a>PWM | Pulse width modulation: a pin switched on and off to give an average level, as for backlight or tone. |
| <a id="t-ram"></a>RAM | Random access memory: working memory that loses its content at power off. |
| <a id="t-rom"></a>ROM | Read-only memory: code fixed in the chip. |
| <a id="t-snr"></a>SNR | Signal to noise ratio. |
| <a id="t-spi"></a>SPI | Serial peripheral interface: the 4-wire bus the processor uses to talk to chips such as the modem. |
| <a id="t-sram"></a>SRAM | Static RAM: the processor's fast working memory. |
| <a id="t-stm32"></a>STM32 (STM32F4, STM32F405) | STMicroelectronics' family of ARM microcontrollers. The DM-1701 and MD-380 class radios use them. |
| <a id="t-swd"></a>SWD | Serial wire debug: the two-wire debug port of ARM processors. |
| <a id="t-synthesizer"></a>Synthesizer | The part that makes the radio's transmit and receive frequencies. |
| <a id="t-tcxo"></a>TCXO | Temperature compensated crystal oscillator: the stable clock the radio's frequency is derived from. |
| <a id="t-uart"></a>UART | Universal asynchronous receiver transmitter: a serial port. |
| <a id="t-vco"></a>VCO | Voltage controlled oscillator: the oscillator in a synthesizer, tuned by a voltage. |

## Vendor terms

| Term | Meaning |
|---|---|
| <a id="t-cai"></a>CAI | Common air interface: Motorola's name for the DMR air interface, also the name of its IP addressing on the air. |
| <a id="t-capacity-plus"></a>Capacity Plus (Cap+) | A MOTOTRBO single-site trunking mode in which the radios share a pool of repeater channels. |
| <a id="t-connect-plus"></a>Connect Plus | A MOTOTRBO multi-site trunking system with a central controller. |
| <a id="t-dcdm"></a>DCDM | Dual Capacity Direct Mode: Motorola's direct mode that carries two calls on one frequency, one per timeslot. |
| <a id="t-erdm"></a>ERDM | Motorola's style of single frequency repeater, with different inbound and outbound color codes. |
| <a id="t-ipsc"></a>IPSC | IP Site Connect: Motorola's linking of repeaters over IP. |
| <a id="t-linked-capacity-plus"></a>Linked Capacity Plus (LCP) | A MOTOTRBO system of several Capacity Plus sites joined by a network. |
| <a id="t-xnl"></a>XNL (XCMP) | A Motorola radio control protocol; XCMP commands travel over it. Neither is part of the standard. |

## Amateur and network

| Term | Meaning |
|---|---|
| <a id="t-ack"></a>ACK (NAK) | Acknowledgment: the reply that says a message arrived. |
| <a id="t-afsk"></a>AFSK | Audio frequency shift keying: data as two audio tones on an FM carrier. APRS at 1200 baud uses it. |
| <a id="t-dhcp"></a>DHCP | Dynamic Host Configuration Protocol: how a device gets its IP address on a network. |
| <a id="t-ip"></a>IP | Internet Protocol: the addressing and packet layer of networks. |
| <a id="t-ncm"></a>NCM (CDC-NCM) | Network control model: the USB class that makes the radio appear as a network adapter. |
| <a id="t-ppp"></a>PPP | Point-to-point protocol: IP over a serial link. [More](../spec/ppp.md) |
| <a id="t-tcp"></a>TCP | Transmission Control Protocol: IP connections with ordered, guaranteed delivery. |
| <a id="t-tnc"></a>TNC | Terminal node controller: the modem that turns packet data into audio tones and back. |
| <a id="t-udp"></a>UDP | User Datagram Protocol: IP packets with no delivery guarantee. |

## ODR and firmware

| Term | Meaning |
|---|---|
| <a id="t-ascii"></a>ASCII | The 7-bit character set of plain text. |
| <a id="t-bcd"></a>BCD | Binary coded decimal: each decimal digit in 4 bits. |
| <a id="t-board"></a>Board | The code and description of one radio model's hardware wiring: pins, chips, memory map. [More](architecture.md) |
| <a id="t-ci"></a>CI | Continuous integration: the checks every change must pass (tools/ci.sh). |
| <a id="t-debug-link"></a>Debug link | ODR's USB serial protocol for reading state and driving the radio from a computer. [More](debug-link.md) |
| <a id="t-feature-switch"></a>Feature switch | A CONFIG_ option that turns a feature's code in or out of the build. [More](../build-config.md) |
| <a id="t-gpl"></a>GPL | GNU General Public License. ODR does not use GPL code. |
| <a id="t-hal"></a>HAL | Hardware abstraction layer: the interfaces the core uses instead of touching a chip. [More](architecture.md) |
| <a id="t-hil"></a>HIL | Hardware in the loop: tests that run against a real radio, and a bench radio or SDR for the other side. [More](testing.md) |
| <a id="t-kb"></a>KB (MB) | Kilobytes (1024 bytes). |
| <a id="t-lto"></a>LTO | Link-time optimization: the compiler optimizes across files. It shrinks the firmware. |
| <a id="t-mit"></a>MIT | The permissive license ODR uses. |
| <a id="t-msb"></a>MSB (LSB) | Most significant bit (or byte): the first, highest-value one. |
| <a id="t-os"></a>OS | Operating system. |
| <a id="t-rtos"></a>RTOS | Real-time operating system: the scheduler that runs the firmware's tasks. |
| <a id="t-sd"></a>SD | Secure Digital: removable memory card. |
| <a id="t-sdr"></a>SDR | Software defined radio: a receiver that digitizes the radio spectrum so software can decode it. The SDR bench uses one. |
| <a id="t-sil"></a>SIL | Software in the loop: tests that run the firmware's core on the host with a simulated radio. |
| <a id="t-soc"></a>SoC | System on chip: the processor and its on-chip peripherals. |
| <a id="t-st"></a>ST | STMicroelectronics, the maker of the STM32 processors. |
| <a id="t-stub"></a>Stub | A small program that runs on the radio in place of the firmware so the computer can read or write flash without the stock bootloader. |
| <a id="t-usrp"></a>USRP | The UDP stream that carries a call's audio between ODR and a network gateway. |
| <a id="t-xip"></a>XIP | Execute in place: running code straight from flash. |
| <a id="t-xor"></a>XOR | Exclusive or: a bit operation that flips bits where the other operand has a 1. |
