<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!--
The Support tab's FAQ (support/faq.html). One item: a "## " line with the question, then the answer under it, up to
the next "## ". Markdown: bold, lists and links. A link to a docs page is written from this file to
the page's Markdown (../../docs/user/20-troubleshooting.md#installing), so tools/check_doc_links.py checks it; a link
to another site starts with https://. Items show in the
order they are here. es.md, it.md and zh.md are the same in their languages, and a language's page shows only its own
file (a language without one shows this file): add a new item here first, then to the others, in English until it is
translated.
-->

## The radio is in another language after an update

Press Menu, then 7 (Settings), 2 (Display), then Up once to reach Language, the last row. Press OK and pick English. The web installer's Radio language choice sets the language the radio starts in, and a factory reset goes back to it. [More](../../docs/user/17-settings.md#display-key-2)

## The installer doesn't find my radio

The radio isn't in its bootloader, or the USB cable is charge-only. Turn the radio off, hold its bootloader keys and turn it on; the screen stays dark. Try another cable or USB port. On Windows the bootloader may need its driver. [More](../../docs/user/20-troubleshooting.md#installing)

## When I program the radio, the serial port chooser doesn't open or doesn't list my radio

- Close every other program that may have the port open: a CPS, qdmr, a hotspot host, another browser tab.
- Check that the computer lists the port as a serial port (on Windows, in Device Manager).
- A radio in USB network mode isn't listed. Use **Switch the radio to USB serial** on the page: it puts the radio in
  serial mode until it restarts.
- Cables with FTDI, CP210x and genuine Prolific chips work best. CH340 cables can be unreliable for backups and
  installs, and the installer warns about them. The Prolific Windows driver disables counterfeit chips, and many
  cables sold online for these radios have them.

[More](../../docs/user/20-troubleshooting.md#everyday-use)

## The screen stays dark after installing

The radio is still in its bootloader. Turn it off and on again without holding any key. [More](../../docs/user/20-troubleshooting.md#installing)

## The radio is stuck in update mode

Choose Radio Stuck in Update Mode in the web installer and pick the radio. It restarts the radio or says what to do. If nothing answers, take the battery out for a few seconds. [More](../../docs/user/02-installing.md#if-something-goes-wrong)

## If I don't like RadioFoundry, can I go back?

Yes. The installer backs up your radio and its original codeplug when it installs, and Restore puts the original firmware back from the manufacturer's firmware file. Keep a copy of that file. RadioFoundry never erases the factory bootloader, so as a last resort you can always start the bootloader by hand and load the factory firmware. [More](../../docs/user/02-installing.md#going-back-to-the-stock-firmware)

## How do I get to the VFO?

Three ways: Menu, 4 (Zones), where the VFO is the first row; a side key set to VFO/Channel in Settings > Keys (or the VFO/MR key where the radio has one); or * then 4 (To VFO) on a channel, which copies the channel into the VFO. [More](../../docs/user/07-channels-zones-vfo.md#the-vfo)

## I have trouble reaching my hotspot or one particular repeater on DMR

RadioFoundry uses your radio's factory frequency calibration, and hotspots are very sensitive to frequency. If your radio is a little off, set Settings > Advanced > Freq trim, on radios that have it. Measure the error first, best with a calibrated SDR, a frequency counter or a hotspot's BER test, and enter it with the sign reversed. [More](../../docs/user/17-settings.md#advanced-last-row)

## Does RadioFoundry do data modes?

Yes. Besides analog packet (APRS and a KISS TNC), RadioFoundry sends native DMR data: text messages compatible with commercial DMR radios, and IP data with ETSI's compressed UDP headers. The free takRF plug-in for ATAK gets you on the air in seconds with your programming cable and an inexpensive USB OTG cable for your phone or tablet. [More](https://takrf.com/dmr)

## What cables do I need for DMR data?

Only your radio's programming cable. To connect it to an Android phone or tablet, add a common USB OTG cable. [More](https://takrf.com/dmr)

## Tier III DMR trunking isn't working for me

It has been tested against real RadioFoundryGateway Tier III repeaters, duplex hotspots and software simulators, with the protocol implementations compared and validated against packet captures from commercial Tier III systems. Open an issue with the specifics: the system, its control and traffic channels, the system's settings in your codeplug and what the radio shows, with the support info from the Support page. [More](../../docs/user/09-dmr.md#trunked-systems)

## Will RadioFoundry support P25 Phase 2?

No. The RF transceiver components in these radios can't send or receive Phase 2's modulation (H-CPM and H-DQPSK). The same goes for TETRA. [More](../../docs/user/07-channels-zones-vfo.md#p25-trunked-channels)

## Can an FM-only radio do DMR or the other digital modes?

In general, no. It doesn't have the chips they need. [More](../../docs/user/23-radios.md)

## DMR calls show, but there is no audio

The screen says “No codec: voice off”: the radio has no usable codec. Install again with the stock firmware file for your radio. [More](../../docs/user/02-installing.md#no-codec-no-codec-voice-off)

## A call shows “Other TG: not played”

The talkgroup isn't the channel's talk target or in its listen list. Press Monitor, or add the talkgroup to the channel's RX group list. [More](../../docs/user/20-troubleshooting.md#everyday-use)

## My settings go back to the defaults

The radio found another firmware's data where RadioFoundry keeps its settings, and doesn't erase it on its own. [More](../../docs/user/20-troubleshooting.md#settings-are-not-kept)

## The radio restarted by itself

Look at Settings > Advanced > Crash log, and include its lines in your report together with the support info. [More](../../docs/user/17-settings.md#crash-log)
