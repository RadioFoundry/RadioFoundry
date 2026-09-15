<!-- SPDX-License-Identifier: MIT -->
# MD-380: hosting the stock AMBE+2 codec

Status: research, 2026-10-10. Static analysis of the decrypted stock image (byte matching against the DM-1701 codec,
literal cross references, targeted Thumb disassembly of the codec region and the stock voice wrapper), then
**verified by execution** in an instruction set emulator. Nothing has run on a radio.

Rule: the radio runs exactly the codec its own TYT stock firmware shipped. The user's machine extracts it from a stock
file the user supplies. No codec bytes go in this repository or in released binaries.

Every fact here comes from our own reading of the vendor image and the DM-1701 work ([codec-dm1701.md](codec-dm1701.md)).
No md380tools, OpenGD77, OpenRTX or other third-party source or symbol file was used. KD4Z's tools were not consulted.

## Stock files

| version | file | size | sha256 | code window |
|---|---|---|---|---|
| **D002.032 (only one so far)** | `MD-380-D2.32(AD).bin` (TYT "Firmware 2.32", model string "JST51") | 994816 (0xF2E00) | `e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43` | 0x08050C44 |

No other MD-380 stock file is at hand (the TYT download packages hold CPS installers, no firmware). Other versions
(for example the D13.20 / S13.20 family) will sit at other addresses and need their own profile.

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
recovered from the stock file alone, checked by that hash. The plain column vote (most common cipher byte per key
column) is wrong in 352 of the 1024 columns: the image is far less zero-dominated than the DM-1701's (0x00 is only
12 % of the plain bytes). In every column the true key byte is among the 8 most common cipher bytes, though.

What converges is a byte-pair refinement trained on the file itself: plain firmware byte pairs (Thumb-2 halfwords,
little-endian addresses) are strongly skewed, with even and odd offsets distinct. Learn a pair model from the 40 % of
columns with the clearest vote, re-pick every other column among its 8 vote candidates, then repeat with the model
learned from the whole image until nothing changes. Two rounds give the exact key. `odrflash/stockfw.derive_key` does
this when the plain vote fails (about 5 s in Python), so neither odrflash nor `tools/codec_extract.py` needs a key file
for the MD-380. The key file is never stored in the repository.

## How the codec was found

1. The DM-1701 V02.06 code window (0x08074714+0x1021C) and table window (0x080D19F0+0x12850) were cut into 8-byte runs
   and looked up in the decrypted MD-380 image. The code matches cluster at 0x08050C26..0x080614C8, the tables at
   0x0808FC64..0x0809C70E, with smaller clusters in sectors 10 and 11.
2. Instruction alignment (mnemonic sequences) of the DM-1701 codec head against 0x08050C44.. maps the DM-1701 entry
   functions one to one. Offsets differ by 0x14 to 0x30 (the same library, a slightly different build of the head; FEC decode moved by -0x8C)
   and the bulk of the library from DM-1701 +0x15B6 on is instruction-identical.
3. The stock voice wrapper at 0x08047C7C makes exactly the DM-1701 init sequence (same calls, same constants), which
   confirms every entry point and its arguments (below).
4. The windows were then cut from what the codec code actually references (every PC-relative literal that holds a
   flash address) and from execution coverage in the emulator.

## Codec in the stock image (D002.032)

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
  (0x08061480..0x080614C6) that the codec calls. 0x08051C28..0x08053538 inside it is stock code (calls out to the
  stock firmware, a CRC routine); the codec never executes it, and it is carried only to keep the window contiguous.
* **Table window.** The codec's constant tables, plus two short codec code islands placed among them by the linker,
  as on the DM-1701 (whose table window holds a codec function at 0x080D5A80): 0x08092954..0x080932EE (reached through
  a code pointer at 0x08055034 that holds 0x08092A4D, and calling back into the code window with BL) and
  0x080963C4..0x0809647C (a Golay (23,12) encoder and decoder). The rest between the tables is stock data and code
  (for example a flash sector address table at 0x08095B70); the codec doesn't read it. The window ends with the last
  table (0x0809AC34, 40 bytes).
* **Small tables.** Constants the compiler merged into other read-only data: 0x080D0FCA (6 halfwords), 0x080D0FD8 and
  0x080D0FE4 (FEC decode); 0x080F9698 (12 halfwords, encode ECC); 0x080F9E74 (10 halfwords); 0x080FD490..0x080FD4AB
  (8 + 8 + 8 + 4 bytes). Each window is cut to whole tables (the DM-1701 image holds the same bytes inside its single
  table window). The emulator faults without each of them.

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
0x1840 on the first call of a transmission and 0x800 after; decode 0 for a good frame, 4 (repeat the previous frame)
or 8. These are the DM-1701's semantics ([codec-dm1701.md](codec-dm1701.md), "Per-frame calls"); the D002.032 call
sites agree: encode at 0x08047FF0 (flags 0x800, half from the wrapper, the 0x2000 word, E), privacy XOR on the 49
bits, encode ECC at 0x08048038, FEC decode at 0x08048B0C, decode at 0x0804893A.

### Stock init (D002.032, function at 0x08047C7C)

The stock wrapper keeps the encoder block E at RAM 0x2000C730, its FEC state at 0x2000DE14 (E+0x16E4), the decoder
block D at 0x20011224 and its FEC state at 0x20011A1C (D+0x7F8): the same offsets as the DM-1701's. It stores the
wrapper constants (0x1840, 0x2000, -22, 20, -10, -40, 0x4000, 3276, 3276, 1310, 10, 20, 128, -20) and then calls
encoder init (E, 1, 1), FEC init (E+0x16E4, 1), decoder init (D, 1), FEC init (D+0x7F8, 1), encoder parameters,
encoder set (E, 128, -20), decoder set (D, 128, -20), and one decode of an all-zero frame with flags 8. ODR runs the same
sequence on its own blocks.

### State sizes (measured)

| block | size | notes |
|---|---|---|
| decoder state D | 0x7EC bytes | 0x7E8 faults (decoder init) |
| encoder state E | 0x16E0 bytes | 0x16DC faults (encoder init) |
| FEC state | 0x44 bytes each | 0x40 faults; only for the codec's own FEC routines |
| stack | 4046 bytes peak from the entry | 4234 through ODR's call veneer and vocoder; allow 4.5 KB plus the caller |

The blocks are passed by pointer and can live anywhere. No fixed RAM address is touched: with only the windows, the
state blocks, the buffers and a stack mapped, init and 300 frames of encode, ECC, FEC decode (with 0 to 4 injected bit
errors per frame, random frames, flags 8) and decode ran without a fault. The executed code contains no FPU
instruction. (The math library's errno at 0x2001D240 is written only from the stock `sqrtf` wrapper at 0x08056E4C,
which no codec function calls.)

## Position dependence

* Calls inside the codec are PC-relative, but they cross between the code window and the code islands in the table
  window (BL from 0x08092E18 and others to 0x08056xxx), and the encoder reaches 0x08092A4C through an absolute code
  pointer. The code also reaches its tables through 49 distinct absolute literals at 70 sites, into the table window
  and all four small windows.
* So the codec must sit at its stock addresses. Moving it means moving the code window and the table window by the
  same distance and patching every literal into either, plus the code pointer; the small windows could stay. That is
  possible but not needed for running in place, and it is not done.

## Hosting

Run in place from flash at the stock addresses, as stock does: no copy, no patching.

* **Flash map.** The code window starts at 0x08050C44, inside sector 6 (0x08040000..0x0805FFFF). The application
  therefore gets 0x0800C000..0x08050C43, 275 KB (281668 bytes), and `boards/md380/md380.ld` stops it there. The windows
  touch sectors 6, 7, 8, 10 and 11; sector 9 (0x080A0000..0x080BFFFF) is free. The earlier reservation of sectors 7-11
  was wrong: the codec starts 61 KB below sector 7.
* **Updates.** Sector 6 holds both the end of the application and the start of the codec, and the bootloader erases
  whole sectors. So the codec can't be kept across an update: every flash merges it from the stock file again
  (`boards.py` MD380: `keep_codec_supported=False`; odrflash writes sector 6 as "app+codec"). An application of at most
  208 KB (sectors 3-5) would make "keep" possible again.
* **Boot check (`boards/md380/vocoder_md380.c`).** The first word at 0x08050C44 must be 0x4FF0E92D, then the CRC-32
  of every window must match the table above (computed once, about 112 KB). Otherwise the vocoder reports no codec and
  the radio is FM only. Codec profile id: `md380-d2032`.
* **Calls.** Through the generic veneer `platform/stm32f4/codec_call.c`, as on the DM-1701, with the init sequence and
  per-frame calls above. The state blocks live in CCM.

## Extraction recipe (D002.032)

1. Check the file's SHA-256 (`e1a1a11a...`), its size 0xF2E00, and the single-segment table (0x0800C000, 0xF2C00).
2. Get the key: a key file checked by SHA-256 (`5254139c...`), or derive it from the file (above).
3. Decrypt each of the six windows: file offset = A - 0x0800C000 + 0x100, key index (A - 0x0800C000) % 1024. Check
   each window's SHA-256 and the code's first word 0x4FF0E92D.
4. Write them unmodified at their stock addresses after the application image (0xFF between). The merge happens on the
   user's machine (`make` with the stock file in `codecs/`, `tools/codec_extract.py` or odrflash), never in a release.

## Verification by execution

The decrypted image was run on the host in Unicorn (Thumb, Cortex-M4 model) with only the six windows, the state
blocks, the I/O buffers and a stack mapped, so any other access faults. Unicorn's memory access hooks corrupt Thumb IT
blocks, so the runs use block hooks only and check flash use by what is mapped.

* The same harness ran the DM-1701 V02.06 codec at its addresses. For a 1 kHz tone (30 frames) and a 300 frame stress
  run (noise, silence, full scale sweeps, injected bit errors, random frames, flags 8), the MD-380 codec's air bits
  and decoded PCM are **bit-identical** to the DM-1701 codec's. Their FEC decode results agree (0 and 4).
* ODR's own `boards/md380/vocoder_md380.c` and `platform/stm32f4/codec_call.c`, compiled with the firmware flags, were
  linked into a small harness and run against the windows of the merged image that `make BOARD=md380` writes, with a
  deliberately misaligned caller stack. `present()` passes, and the encoded bits and decoded PCM over 30 frames are
  bit-identical to the direct stock calls.

Scripts and notes: `the external reference folder` (outside the repository).

## Only a radio can check

* Audio: TX and RX voice quality and levels through the MD-380's own audio path (the I2S format, gain staging).
* Timing: encode and decode within 20 ms at the MD-380's clock, and the audio task's stack (5 KB) under the codec's
  4.2 KB plus the audio callback.
* That D002.032's bootloader accepts an image that writes sectors 6-8, 10 and 11 in one session, and that nothing
  else in flash (for example the bootloader's own checks) cares about the codec sectors.
* Calls against the HR_C5000 path with real frames, including the FEC decode results on air.

## Open items

* Other stock versions (D13.20 and similar) need their own profile; only D002.032 is supported.
* 275 KB for the application is tight (the default build uses 94 %). If the application outgrows it, the choices are
  relocating the codec (moving both windows and patching 70 literal sites and one code pointer) or a smaller
  application.
