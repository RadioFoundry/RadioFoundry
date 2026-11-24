<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
# Test voice set

Speech for testing every digital voice mode, in the frame formats each mode carries. The speech is synthesized
(text to speech), not a recording of a person. The HIL voice tests send these files by default
([tests/hil/README.md](../../hil/README.md)).

Two phrases:

| Set | Speech | Duration | 20 ms frames |
|-----|--------|----------|--------------|
| `test_transmission` | a short spoken test transmission | 2.08 s | 104 |
| `ten_second_countdown` | a test transmission with a countdown, for longer overs | 10.8 s | 540 |

## Files

Each set has the same six files. Every file holds whole 20 ms frames, so the frame counts above hold for each format.

| File | Format | Size (short / countdown) | Modes |
|------|--------|--------------------------|-------|
| `<set>.pcm` | 16-bit signed little-endian, 8 kHz, mono, no header | 33,280 / 172,800 B | the source; mic injection, any encoder |
| `<set>.wav` | the same PCM in a WAV file | 33,324 / 172,844 B | listening |
| `<set>.ambe` | AMBE+2 (3600 x 2450 bit/s), 9-byte frames with FEC (72 bits), back to back, no header | 936 / 4,860 B | DMR, NXDN |
| `<set>_decoded.wav` | the `.ambe` frames decoded back to audio | 33,324 / 172,844 B | reference: what a DMR or NXDN radio should play |
| `<set>.imbe` | full rate (7200 x 4400 bit/s), 11-byte frames of the 88 information bits, first bit first, no FEC, back to back, no header | 1,144 / 5,940 B | P25 Phase 1 |
| `<set>_imbe_decoded.wav` | the `.imbe` frames decoded back to audio by `core/mbe` | 33,324 / 172,844 B | reference: what a P25 radio should play |

The `.ambe` 9-byte frame is the one the HIL tests' `--ambe` option and the SDR bench's voice jobs take. NXDN carries
the same AMBE+2 frames.

The `.imbe` frame is `IMBE_BYTES` in [core/mbe/imbe.h](../../../core/mbe/imbe.h) (`P25_IMBE_BYTES` in the P25 code):
the frame the firmware's P25 voice path hands the call layer before the FEC. `tools/bench/p25tool.py gen --voice-file`
takes it.

## Regenerating the IMBE files

The `.imbe` files and their decoded WAVs come from the `.pcm` through the firmware's own full-rate encoder and decoder
(`core/mbe`, fixed point, the same output on every host):

```
make -C tests/host voice-data
```

The generator is [tests/host/imbeVoiceGen.c](../../host/imbeVoiceGen.c). `make -C tests/host` runs it with `--check`,
which fails when a committed `.imbe` no longer matches the encoder's output. After a change to the encoder, regenerate
and commit both files of each set.

## Git

The repository ignores `*.ambe` and `*.wav` so that no codec material or recording is committed by accident. These
files are tracked by name (`git add -f`). Git still shows changes to them.
