<!-- SPDX-License-Identifier: MIT -->
# MD-380: hosting the AMBE+2 codec

The merge moves the code to sector 11 (Hosting), or for a 35.14 file next to its tables in sector 10; the placed
layout is checked on a host.

Rule: the radio runs exactly the codec its own TYT firmware shipped. The user's machine extracts it from a firmware
file the user supplies. No codec bytes go in this repository or in released binaries.

## Firmware files

| version | file | size | sha256 | code window |
|---|---|---|---|---|
| **D002.032** | `MD-380-D2.32(AD).bin` (TYT "Firmware 2.32", model string "JST51") | 994816 (0xF2E00) | `e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43` | 0x08050C44 |
| D35.14 | `TYT-72310-11-MD380-D35.14.bin` (MD-380) | 947712 (0xE7400) | `ff6c3d79b205391c9c2096f8e4367c514c1e1e4dac7d421ed69077e5b42eaf2f` | 0x08087A10 |
| S35.14 | `TYT-72310-11-MD390-S35.14.bin` (MD-390, GPS) | 951808 (0xE8400) | `167e79f18e78d7d31460dc577c4efd0cb881344f5ee44a4c9a30798ead0fb79d` | 0x08088AC4 |

Both 35.14 files use the MD-380 key and the single-segment container. They carry a different codec build from
D002.032's: the MD-UV380 family's, with its entry offsets ([codec-mduv380.md](codec-mduv380.md), "Entry points"; FEC
decode +0xB78), behaving bit-identically to it, and three table windows (35.14, below). Other versions (for example the
D13.20 / S13.20 family) sit at other addresses and need their own profile.

## Container and encryption

The file is a TYT "OutSecurityBin" container in its single-segment form ([hardware-md380.md](hardware-md380.md) 14.1):
segment count word 0xFFFFFFFF at 0x7C, one segment (0x0800C000, 0xF2C00) at 0x80, the payload at 0x100, then 0x100
trailing bytes. The payload is XORed with the MD-380's 1024-byte bootloader key:

```
plain[i] = file[0x100 + i] ^ key[i % 1024]
```

So for an application address A, file offset = A - 0x0800C000 + 0x100 and the key index = (A - 0x0800C000) % 1024.
The decrypted image has SP 0x2001DC10 and reset vector 0x080FA969.

### Key recovery

The key (SHA-256 `5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd`, first bytes 2E DF 40 B5) was
derived from the firmware file alone, checked by that hash. The plain column vote (most common cipher byte per key
column) is wrong in 352 of the 1024 columns: the image is far less zero-dominated than the DM-1701's (0x00 is only
12 % of the plain bytes). In every column the true key byte is among the 8 most common cipher bytes, though.

What converges is a byte-pair refinement trained on the file itself: plain firmware byte pairs (Thumb-2 halfwords,
little-endian addresses) are strongly skewed, with even and odd offsets distinct. Learn a pair model from the 40 % of
columns with the clearest vote, re-pick every other column among its 8 vote candidates, then repeat with the model
learned from the whole image until nothing changes. Two rounds give the exact key. `rfoundry/stockfw.derive_key` does
this when the plain vote fails (about 5 s in Python), so neither rfoundry nor `tools/codec_extract.py` needs a key file
for the MD-380. The key file is never stored in the repository.

## Codec in the D002.032 file

| window | flash | length | file offset | key phase | sha256 (decrypted) | CRC-32 |
|---|---|---|---|---|---|---|
| code | 0x08050C44..0x080614C8 | 0x10884 | 0x44D44 | 0x044 | `00d9565dc459c47119690e47c35daf9ae1a89c13bb3513d966390e0835b0c0ad` | 0x32F0A7C1 |
| tables | 0x0808FC64..0x0809AC5C | 0xAFF8 | 0x83D64 | 0x064 | `9c7b66b08a6d397565c70551df9097870757f8de719b8d31525eba72e1db4840` | 0x91E708C1 |
| small table 1 | 0x080D0FC8..0x080D0FEC | 0x24 | 0xC50C8 | 0x3C8 | `d69f60c456d316fd4506d66f706a093a172792e97f21a7d4eb69b94cbf91a1d2` | 0x7AAFE1D3 |
| small table 2 | 0x080F9698..0x080F96B0 | 0x18 | 0xED798 | 0x298 | `6679872ccda55797e8d51692e9a59bcc0ac2c87eaff31736302976be446a6375` | 0x33E82E70 |
| small table 3 | 0x080F9E74..0x080F9E88 | 0x14 | 0xEDF74 | 0x274 | `0af022f016edcb28a3b29d60abdf42846456f22b8b4579a14b2069c7f6f31017` | 0xFFF0E930 |
| small table 4 | 0x080FD490..0x080FD4B0 | 0x20 | 0xF1590 | 0x090 | `b6bc2f43d9a7779d7a8ac938db131b049f72620bc6d288db19bd9ecc15a8c812` | 0xDA18C5EF |

First word of the code window: 0x4FF0E92D (`push.w {r4-r11, lr}`), as on the DM-1701.

What the windows hold:

* **Code window.** The codec library from 0x08050C44, and at its end the compiler's 64-bit shift helpers
  (0x08061480..0x080614C6) that the codec calls. 0x08051C28..0x08053538 inside it is non-codec code (calls out, a CRC routine); the codec never executes it, and it is carried only to keep the window contiguous.
* **Table window.** The codec's constant tables, plus two short codec code islands placed among them by the linker,
  as on the DM-1701 (whose table window holds a codec function at 0x080D5A80): 0x08092954..0x080932EE (reached through
  a code pointer at 0x08055034 that holds 0x08092A4D, and calling back into the code window with BL) and
  0x080963C4..0x0809647C (a Golay (23,12) encoder and decoder). The rest between the tables is other data and code
  (for example a flash sector address table at 0x08095B70); the codec doesn't read it. The window ends with the last
  table (0x0809AC34, 40 bytes).
* **Small tables.** Constants the compiler merged into other read-only data: 0x080D0FCA (6 halfwords), 0x080D0FD8 and
  0x080D0FE4 (FEC decode); 0x080F9698 (12 halfwords, encode ECC); 0x080F9E74 (10 halfwords); 0x080FD490..0x080FD4AB
  (8 + 8 + 8 + 4 bytes). Each window is cut to whole tables (the DM-1701 image holds the same bytes inside its single
  table window). The codec faults without each of them.

## Entry points and calling convention

All AAPCS, Thumb, arguments beyond four on the stack in order, results in r0. Same library as the DM-1701's: the
prologues, argument lists and stack argument offsets match; only the addresses differ.

| call | address | offset from 0x08050C44 | DM-1701 offset | arguments |
|---|---|---|---|---|
| encode | 0x08050D90 | +0x14C | +0x130 | (bits out, 0, PCM in, 80, stack: flags, half, 0x2000, E) |
| encoder set | 0x08050E24 | +0x1E0 | +0x1C0 | (E, 128, -20) |
| encoder init | 0x08050ED8 | +0x294 | +0x284 | (E, 1, 1) |
| FEC init | 0x08051140 | +0x4FC | +0x4CC | (F, 1) |
| encode ECC | 0x08051158 | +0x514 | +0x4E8 | (bit buffer, same buffer, 0, F_enc): 49 bits in, 72 air bits out |
| decode | 0x08051248 | +0x604 | +0x5D8 | (PCM out, 80, bits in, 0, stack: flags, half, D) |
| decoder set | 0x080512F0 | +0x6AC | +0x688 | (D, 128, -20) |
| decoder init | 0x08051334 | +0x6F0 | +0x6D0 | (D, 1) |
| encoder parameters | 0x08051498 | +0x854 | +0x848 | (E, -22, 20, -10, stack: -40, 0x4000, 3276, 3276, 1310, 10, 20) |
| FEC decode | 0x0805170C | +0xAC8 | +0xB54 | (49 bits out, 0, 72 bits in, 1, stack: flags, F_dec): returns 0 good, 4 C0 uncorrectable |

Bits are one per halfword in all buffers, the frame is two calls of 80 samples (half 0, then half 1). Flags: encode
see [Encode flags](#encode-flags); decode 0 for a good frame, 4 (repeat the previous frame) or 8. These are the
DM-1701's semantics ([codec-dm1701.md](codec-dm1701.md), "Per-frame calls").

### Encode flags

The encode call's flags word (stack argument 0) is a set of feature bits, not a "first frame" marker. Measured on
the DM-1801 Pro build and checked bit for bit against this MD-380 build; the DM-32UV runs the same build as the
DM-1801 Pro.

| Bit | Effect |
|---|---|
| 0x0040 | Noise suppression. Noise alone: 2130 -> 486 RMS. Between words in noisy speech: 1611 -> 522. It also lowers steady tones |
| 0x0100 | Turns the input DC-removal filter off. Never set |
| 0x0800 | Noise gate. Acts only while E[0x138A] is not 0 |
| 0x1000 | Tone detection: a steady tone or tone pair is sent as an AMBE tone frame and is decoded at the right frequency. No effect on speech |
| 0x2000 | Input AGC: quiet speech about 4x louder, loud speech reduced. Its parameters are E+0x16A2..0x16B6, left at their values after init. The defaults have not been tested on air |
| 0x4000 | Test mode: one fixed frame whatever the input |
| others | No measurable effect |

Radios use one of two patterns, each a constant value on every call of a transmission: 0x800, or 0x1840
(0x1000 | 0x800 | 0x40). A word that changes from call to call is not one of them. A host that passes 0x1840 on
the first call and 0x800 afterwards gets the 0x800 behavior: over 234 frames the output equals 0x800 on every call.

What RadioFoundry passes (`drivers/vocoder/ambe2_flags.h`, used by the MD-380 and DM-1701 hosts and by
`platform/hrc7000/vocoder.c` for the DM-1801 Pro and DM-32UV): one word on every call, 0x800 plus 0x40 for
`VOCODER_OPT_NOISE_REDUCTION`, 0x1000 for `VOCODER_OPT_TONE_FRAMES` and 0x2000 for `VOCODER_OPT_AGC`. Noise
reduction and tone frames are on by default and AGC is off, which gives 0x1840 (0x3840 with Mic AGC on). 0x100 and
0x4000 are never set ([dmr-voice.md](dmr-voice.md) 6).

Cross-check with the 0x2000 bit: with the same values in the AGC parameter block, all 43 tested flag words give the
same output on the two builds. The difference seen earlier with 0x2000 set came from different values in that
block (state), not from different code.

### Init

The encoder block E has its FEC state at E+0x16E4 and the decoder block D has its FEC state at D+0x7F8: the same offsets as the DM-1701's. Store the
constants (0x1840, 0x2000, -22, 20, -10, -40, 0x4000, 3276, 3276, 1310, 10, 20, 128, -20) and then calls
encoder init (E, 1, 1), FEC init (E+0x16E4, 1), decoder init (D, 1), FEC init (D+0x7F8, 1), encoder parameters,
encoder set (E, 128, -20), decoder set (D, 128, -20), and one decode of an all-zero frame with flags 8. RadioFoundry runs the same
sequence on its own blocks.

### State sizes (measured)

| block | size | notes |
|---|---|---|
| decoder state D | 0x7EC bytes | 0x7E8 faults (decoder init) |
| encoder state E | 0x16E0 bytes | 0x16DC faults (encoder init) |
| FEC state | 0x44 bytes each | 0x40 faults; only for the codec's own FEC routines |
| stack | 4046 bytes peak from the entry | 4234 through RadioFoundry's call veneer and vocoder; allow 4.5 KB plus the caller |

The blocks are passed by pointer and can live anywhere. No fixed RAM address is touched: with only the windows, the
state blocks, the buffers and a stack mapped, init and 300 frames of encode, ECC, FEC decode (with 0 to 4 injected bit
errors per frame, random frames, flags 8) and decode ran without a fault. The executed code contains no FPU
instruction.

## Position dependence

* Calls inside the codec are PC-relative, but they cross between the code window and the code islands in the table
  window (BL from 0x08092E18 and others to 0x08056xxx), and the encoder reaches 0x08092A4C through an absolute code
  pointer. The code also reaches its tables through 49 distinct absolute literals at 70 sites, into the table window
  and all four small windows.
* The literals and the code pointer point into the table windows, so they stay valid while the tables stay at their
  original addresses. Moving the code alone needs two kinds of rewrite: the two movw/movt pairs in the code that point
  into the code, and the 12 calls (BL) from the islands back into the code. The codec map
  (`tools/rfoundry/rfoundry/codec_maps/md380/md380-d2032.json`) lists both, and the merge applies them
  ([codec-placement.md](codec-placement.md), "STM32 radios").

## Hosting

Run in place from flash, no copy. The merge moves the code to the start of sector 11 and leaves every table window at
its original address ([codec-placement.md](codec-placement.md), "STM32 radios").

| window | placed at | length | CRC-32 as placed |
|---|---|---|---|
| code | 0x080E0000 (sector 11) | 0x10884 | 0x067E6A77 (self references rewritten) |
| tables | 0x0808FC64 (sector 8, original address) | 0xAFF8 | 0x2A5C9EE1 (the 12 island calls re-encoded) |
| small tables 1-4 | original addresses (sectors 10 and 11) | as above | unchanged (0x7AAFE1D3, 0x33E82E70, 0xFFF0E930, 0xDA18C5EF) |
| descriptor | 0x080FFF80 (end of sector 11) | 128 | its own (`platform/stm32f4/codec_desc.h`), tag 0x2032 |

* **Flash map.** The application gets sectors 3-7, 0x0800C000..0x0807FFFF, 464 KB in one piece, and
  `boards/md380/memory.ld` asserts that the image ends before 0x08080000. The codec uses sectors 8, 10 and 11. Sector
  9 (0x080A0000..0x080BFFFF) holds nothing. The firmware file keeps the code at 0x08050C44, inside sectors 6 and 7, so
  that layout leaves the application only 275 KB (it stops inside sector 6, where the code starts).
* **Updates.** No codec byte shares a sector with the application, so an update writes sectors 3-7 only and keeps the
  codec (`boards/md380/board.toml`: `keep_codec = true`; the STM32 stub `stub/f4stub.py` owns sectors 3-7). The codec
  is merged on a first install from the manufacturer's firmware, or when it can't be kept or checked. It comes from the firmware file, or from
  the copy the flashing tools keep: rfoundry and the web flasher read the codec from a radio running RadioFoundry over the
  debug link (READ_MEM), in the original or the placed layout, check it against the profile hashes and save it on the
  computer (rfoundry's config directory, the browser's IndexedDB; `rfoundry/codecsave.py`,
  `site/static/js/flasher/codecsave.js`).
* **Boot check (`platform/stm32f4/hrc/vocoder.c`, this build's profile in `boards/md380/vocoder_md380.c`).** Once,
  before the first call: the descriptor (its own CRC, tag 0x2032, the code and every table window at the addresses
  and with the CRCs above), then the signature word 0x4FF0E92D at 0x080E0000, then the CRC-32 of every window (about
  112 KB). Otherwise the vocoder reports no codec and the radio is FM only. Entry points are offsets from the code start (encode +0x14C, decode +0x604, the rest in
  `vocoder_md380.c`), the same offsets as in the original layout.
* **Profile id.** `md380-d2032-s11` (debug link VERSION `codec=`), for a codec from any supported file: the descriptor's
  tag tells the builds apart. A build from before the move reports `md380-d2032`, so the tools know the codec isn't
  where the new layout keeps it and ask for the firmware file.

### 35.14

The main table window stays at its original address in sector 9, the code goes right after it, the two small windows
stay at their original addresses in sector 11 (`stockfw.py` PROFILES, `boards/md380/vocoder_md380.c`):

| build | tag | code (address, length, CRC-32) | tables (address, length, CRC-32) | small tables (address, length, CRC-32) |
|---|---|---|---|---|
| D35.14 | 0x00443514 | 0x080C45D0, 0x10366, 0x83A0361F | 0x080B7868, 0xCD68, 0x810CC7B3 | 0x080ECC98, 0x87C, 0xA5F0D272; 0x080F0C14, 0xE, 0xEFDB66A5 |
| S35.14 | 0x00533514 | 0x080C5710, 0x10372, 0xD953C7A8 | 0x080B8928, 0xCDE8, 0x68B2CC40 | 0x080EDDD8, 0x87C, 0xA2B85A33; 0x080F1D54, 0xE, 0xEFDB66A5 |

* Sectors 9, 10 and 11 (sector 8 stays empty); the application keeps sectors 3-7, so "keep codec" works the same way.
  An update keeps sectors 8-11 whichever build the codec came from.
* Code and tables as placed were checked on a host over a 300 frame stress run: the same hash as D002.032's and the
  DM-1701's codec, from the whole image, from the windows only and from the placed layout.
* **Calls.** Through the generic veneer `platform/stm32f4/codec_call.c`, as on the DM-1701, with the init sequence and
  per-frame calls above. The state blocks live in CCM.

## Extraction recipe (D002.032)

1. Check the file's SHA-256 (`e1a1a11a...`), its size 0xF2E00, and the single-segment table (0x0800C000, 0xF2C00).
2. Get the key: a key file checked by SHA-256 (`5254139c...`), or derive it from the file (above).
3. Decrypt each of the six windows: file offset = A - 0x0800C000 + 0x100, key index (A - 0x0800C000) % 1024. Check
   each window's SHA-256 and the code's first word 0x4FF0E92D.
4. Write the table windows at their original addresses, with the 12 island calls in the main window re-encoded for the
   moved code. Write the code at 0x080E0000 with its two movw/movt self references rewritten, and the descriptor at
   0x080FFF80. Check the placed CRC-32s (Hosting). 0xFF fills the rest after the application image. The codec map
   drives the rewrites. The merge happens on the user's machine (`make` with the firmware file in `codecs/`,
   `tools/codec_extract.py`, rfoundry or the web flasher), never in a release.

## Verification on a host

The windows were run on the host with only the six windows, the state blocks, the I/O buffers and a stack mapped, so any
other access faults.

* For a 1 kHz tone (30 frames) and a 300 frame stress run (noise, silence, full scale sweeps, injected bit errors,
  random frames, flags 8), the MD-380 codec's air bits and decoded PCM are bit-identical to the DM-1701 codec's. Their
  FEC decode results agree (0 and 4).
* RadioFoundry's own `boards/md380/vocoder_md380.c` and `platform/stm32f4/codec_call.c`, compiled with the firmware
  flags, were run against the windows of the merged image that `make BOARD=md380` writes, with a deliberately
  misaligned caller stack. `present()` passes, and the encoded bits and decoded PCM over 30 frames are bit-identical
  to calling the codec directly.
* The placed codec (code at 0x080E0000, rewrites applied), run with only its placed windows mapped, gives the same air
  bits and PCM as the original layout over 60 frames, plain and stressed, with the same FEC results and a stack peak
  of 4046 bytes.

## Only a radio can check

* Audio: TX and RX voice quality and levels through the MD-380's own audio path (the I2S format, gain staging).
* Timing: encode and decode within 20 ms at the MD-380's clock, and the audio task's stack (5 KB) under the codec's
  4.2 KB plus the audio callback.
* That D002.032's bootloader accepts an image that writes sectors 3-8, 10 and 11 in one session, and that nothing
  else in flash (for example the bootloader's own checks) cares about the codec sectors.
* Voice TX and RX with the moved code, and an update in keep mode that leaves the codec working.
* Calls against the HR_C5000 path with real frames, including the FEC decode results on air.

## Open items

* On-air check of the encode flags (0x800 against 0x1840): speech in a quiet room and with background noise, a
  1 kHz tone and a DTMF key into the mic, received on a second radio or the SDR bench. AGC (0x3840): a quiet and a
  loud talker, levels and pumping between words; whether the parameter block's values after init are usable.

* Other firmware versions (D13.20 and similar) need their own profile; D002.032, D35.14 and S35.14 are supported.
* On a radio: voice through a 35.14 codec as placed (untested on the radio).
* Moving the table windows too (rewriting the `table_refs`) would free sector 8 and pack the codec into sector 11
  ([codec-placement.md](codec-placement.md)). The application has 464 KB and doesn't need it yet.
