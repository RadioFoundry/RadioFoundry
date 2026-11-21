<!-- SPDX-License-Identifier: MIT -->

# Ruyage DP10 bring-up: review request

This is a request for review of a local DP10 port and its provenance, not a
board implementation or a claim that the port meets the clean-room rules. The
local build is labeled v1.3; it is not a RadioFoundry release. The prototype
was built against upstream commit `b7e877c1ce55024bb042e483f4b35acc24cb0631`.

## Provenance and review

The port was developed through hardware experiments and iterative tests, with
stock firmware analysis used as a reference. A local display profile describes
its initialization sequence as extracted from a program dump. This review
request contains no extracted sequence, firmware, codec, flash dump, private
settings or implementation. The local port has not had a file-by-file
provenance or project clean-room review. Please advise which observations can
be used as specifications, and what independent implementation and records
are needed before the board code can be proposed here.

## Owner-reported operation

On one radio the owner reports normal boot and working FM and DMR receive and
transmit, mode switching, APRS receive and decoded transmit, local codeplug
editing, and usable beep and APRS receive audio. Serial PPP connectivity,
UDP debug access, and a rigctl frequency change and restoration were tested
on an earlier local build. A program-only stock-to-local installation was
verified; the existing data flash was retained. These observations do not imply
testing on other units or complete coverage of all features.

## DMR transmit measurements

At 446 MHz, low power, into a dummy load with an RTL-SDR coupled to the
signal, an earlier local build had a median -3 dB burst falling edge 2.383 ms
before the analyzer's expected last-symbol end. A trial using a 30,500 us
PA-off timer measured a median 0.113 ms after that point across 30 bursts,
with zero unplaced. The opposite-slot samples were near the SDR noise floor;
this is not a calibrated slot-compliance measurement. The owner reported much
clearer Pi-Star Parrot playback and BER below 2%, versus 6.3% for one earlier
call. These were not controlled equivalent BER measurements.

A separate instrumented trial recorded a burst timer firing 30,511 us after PA
enable and clearing the PA control registers; it then skipped one normal off
callback 1,411 us later, when those registers remained clear. This exercises
one timer-before-callback case, not all interrupt ordering or timer failures.
The standalone v1.3 was restored after the trial.

## Remaining work

- File-by-file provenance and clean-room review, followed by an independently
  implemented port where required.
- Full RadioFoundry hardware-in-the-loop DMR bench suite; a compatible bench
  service was not available for this test.
- Calibrated deviation, frequency accuracy, power, and operation across units
  and bands; the SDR and hotspot are not calibrated RF test instruments.
- Full stock-origin installation including data migration, web installer
  validation, and long-duration reliability. The stock-to-local program test
  preserved previously installed local data.
- SFR and standalone hotspot operation with a second DMR radio, and additional
  PA guard, interrupt-ordering and PTT-release timing tests.
- Review shared settings/UI and audio changes, feature-off builds, translations,
  and the provisional APRS tocall before proposing any implementation.
