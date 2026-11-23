<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Digital receive audio AGC

`CONFIG_RX_AGC` builds one shared controller for decoded receive speech. It follows `CONFIG_DMR` and can be disabled
independently. FM is outside this feature. Availability of digital modes is in the [feature matrix](../feature-matrix.md).

## Level and preferences

Each digital mode has its own target: Off, -36, -33, -30, -27, -25, -24, -21, -18, -15 or -12 dBFS. Off is the
initial preference and passes samples unchanged. Targets are RMS relative to a full-scale sample amplitude of 32768,
measured from 20 ms PCM blocks with their DC means removed. After initial acquisition, energy is averaged with a
1.5-second time constant over qualifying audio before conversion to dB. They are not peak levels or acoustic loudness.
User volume follows normalization. Local beeps, transmit audio and raw modem input are never normalized.

A mode stores the list index (0 = Off), not a protocol enum or a translated string. Append a new mode's setting;
never reorder the existing mode slots or target choices. Invalid preferences behave as Off.

## Controller

The audio task owns the controller and caller history. A block below -55 dBFS does not update the gain or history.
This is a level gate, not a voice classifier: sufficiently loud noise can pass it. Lost and concealed decoder frames
use the current gain but do not teach it. The first 15 qualifying blocks (300 ms of active audio) acquire with a
maximum gain change of 3 dB per block, using the cumulative mean energy of those blocks. Later blocks use an
exponential energy average with weight 1/75 for each new block (about 1.5 seconds of active audio). Boost then moves at no more than about 1 dB per second of active audio. Attenuation can move up to 1 dB per
block, so a quiet lead-in or stale caller estimate cannot leave a loud voice boosted for seconds. Both directions
follow the averaged input, rather than individual blocks. An error within 0.5 dB holds the gain.
The controller, display and caller history use this same input estimate. Pauses and concealed frames freeze the
averages and the requested gain; normal syllable dynamics remain in the output.
Gain is bounded to -24 through +24 dB. Samples are ramped within each block; a peak ceiling of -1 dBFS overrides the
ramp immediately when necessary. The target is best effort when the gain bounds or safety ceilings intervene.

An independent raw-RMS ceiling protects playback from corrupt blocks and stale boost before the averaged controller
settles. It includes DC energy and applies even to gated, lost or concealed frames. During the first 15 qualifying
speech blocks it caps each output block at the configured target. After acquisition it allows about 6 dB above the
target to preserve normal syllable dynamics. It limits both ends of the sample ramp immediately and records the
applied scale without teaching the gain estimate or caller history. RMS is rounded up and gain down so integer
rounding cannot weaken the ceiling. The existing -1 dBFS peak ceiling also applies. This bounds the loudness of
corrupt audio; it does not repair it. Off remains bit-exact.

Ten entries remember the most recently heard source IDs across the digital modes. The key is mode, source ID and a
network scope when supplied by the call path. Unknown source ID 0 never occupies an entry. After five qualifying
blocks, a smoothed input level updates the history and the mode's fallback. Pauses and concealment never replace it.
At the next call, the target and remembered input level determine the initial gain. Unknown callers start from the
mode's last stable input level, or unity without history. A late identity labels the running estimate without a gain
jump. History is RAM only and clears at boot. Changing the target preserves useful input-level history.

## Display and diagnostics

An optional receive row shows `AGC -37 > -26 dBFS`: actual input and output, before user volume, not the configured
target. Readings use the input and actual output energy averages, whole dB and about four updates per second. A pause holds the last reading for
one second, then shows dashes. Off says `AGC Off`. On a small display this row takes the optional alias/location row;
caller identity, call destination and RX state remain. Larger displays put it below the signal meter when space
allows. P25/NXDN large-text and all dual-watch layouts retain their usual information instead of adding the row. RF signal strength remains distinct from PCM audio level. Local sounds do not enter the readings.

The debug link exposes mode, enabled state, input/output level, gain, valid reading, acquisition and cache hit.

## Verification

`tests/host/rxAgcTest.c` checks bypass, acquisition, target changes, noise gate, concealment, peak protection, caller
reuse, ten-entry eviction, late identity, separation of modes and scopes, syllable-level stability, energy averaging,
sustained level changes, gain-rate bounds and recovery from a quiet lead-in or stale cached boost at low targets.
A -33 dBFS target sweep covers inputs from -40 through -15 dBFS, all modes and fresh, cached, fallback and concealed-startup histories:
actual PCM reaches within 0.5 dB of target by 300 ms and stays steady. Fault injection covers full-scale alternating
samples, positive and negative DC rails, impulses, silence, random data and biased signals, both as good and
concealed frames. Every startup block stays at or below the target; sustain blocks stay within the 6 dB guard. Playback integration tests exercise the
real voice callbacks. Hardware playback, timing and sound quality remain untested until an owner runs the HIL checks.
