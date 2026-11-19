<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# FAQ

For symptoms and their fixes, see [Troubleshooting](20-troubleshooting.md).

## About the project

### Why allow commercial use?

Commercial use is the reason $40 DMR radios exist. If everything DMR were ham-only, a decent radio would still cost
$500. The project authors don't make money from RadioFoundry, so nobody loses when someone else uses it in a product.
The project doesn't sell DMR firmware. The goal is a decent firmware that everyone can use.

### Why reinvent the wheel?

RadioFoundry covers most of these radios with one core under a license that lets anyone use it, so work on one radio
helps every other.

### Why another radio firmware?

DMR has many features that inexpensive radios rarely implement. They're useful for commercial applications and for
amateur use.

### How can I help?

Any way you can:

- **Test and report.** Run RadioFoundry on your radio and
  [report what breaks](https://github.com/radiofoundry/radiofoundry/issues/new?template=bug_report.yml). If the radio
  restarted, include the crash log from **Settings > Advanced > Crash log** ([Settings](17-settings.md#crash-log)).
- **Help others.** Answer questions and share what works in the
  [community group](https://www.facebook.com/groups/radiofoundry/) on Facebook.
- **Ask.** Request a
  [feature](https://github.com/radiofoundry/radiofoundry/issues/new?template=feature_request.yml) or a
  [radio](https://github.com/radiofoundry/radiofoundry/issues/new?template=radio_request.yml) that you'd like
  supported.
- **Contribute.** Send pull requests for code or docs ([Contributing](../dev/contributing.md)).
- **Donate or lend a radio.** This helps the most, especially with older models that are hard to find, because every
  port needs the hardware on the bench.

### Can I use RadioFoundry in my own project?

Yes, including in commercial and closed-source products. The license has one condition: keep our copyright and license
notices in the code you copy and with the firmware you ship. We also ask that you give your project its own name. If
you add a feature, we'd be glad to have it here, credited to you, on every radio whose hardware can run it. Start a
[discussion](https://github.com/radiofoundry/radiofoundry/discussions) or send a pull request. For details, see
[Using RadioFoundry in your project](../../FORKING.md).

### Was this project AI-generated?

No. This project is the result of thousands of hours of combined human effort from several contributors, spent on
radio teardowns, reverse engineering, software architecture, and testing. AI served as a pair programmer to speed up
support for more radios and to make the project's scope possible and sustainable.

## Installing and the codec

### Why doesn't the release include the codec?

DMR voice needs DVSI's AMBE+2 codec, and RadioFoundry can't distribute it. Your radio already has the codec, in its
original firmware. The installer leaves the codec in place when it's already there, or takes it from your radio's
original firmware file on your own computer. For details, see [Installing](02-installing.md).

### Can I use RadioFoundry without the codec?

Yes. FM works, and DMR calls show on the screen. DMR data (messages, call alert, radio check) and hotspot mode work.
DMR voice is off.

### Will RadioFoundry damage my radio?

Flashing writes only the processor's application and codec sectors. The installer never writes the bootloader or the
factory calibration, so you can go back to the original firmware
([Installing](02-installing.md#going-back-to-the-original-firmware)). Test RadioFoundry before you depend on it, and
transmit only where you're licensed to.

### How do I report a problem with the web installer or the Codeplug Editor?

See [Reporting a problem](20-troubleshooting.md#reporting-a-problem).

## Programming

### Does qdmr work with RadioFoundry?

Yes, through a compatibility layer. qdmr's open-firmware driver reads and writes the codeplug
([Programming the radio](04-programming.md)). On a radio whose codeplug sits where qdmr's driver can't write, use the
Codeplug Editor ([Radio reference](23-radios.md#programming)).

### Can I program the radio without installing anything?

Yes. The Codeplug Editor on the project's web site reads and writes the radio from a Chromium-based browser (Chrome,
Edge, Opera), and builds the DMR ID database from RadioID.net. For details, see
[The Codeplug Editor](04-programming.md#the-codeplug-editor).

### Can I use a phone?

On Android, yes: Chrome on Android runs the web installer and the Codeplug Editor. Plug the radio's USB cable or the
programming cable into the phone through a USB OTG adapter.

**Don't switch to another app or let the screen time out while a radio transfer is running.** Android slows a page in
the background, so a backup or an installation takes much longer and can fail. The pages keep the screen on while the
radio is connected.

| Point | Why |
|---|---|
| Cables with an FTDI chip and radios with their own USB port work. Other USB-serial cables (CH340, CP210x, PL2303) don't | Chrome on Android reaches USB serial devices only through WebUSB, and the page brings its own driver for each kind |
| After you pick the radio or cable in Chrome's list, accept Android's "Allow Chrome to access ..." prompt | Without it, the page can't open the device |
| Close any app that opens when you plug in the radio or cable, and don't pick it in Android's "open with" prompt | An app that holds the cable takes the radio's replies, and the page then reports that the radio didn't answer |
| Keep Chrome in front and the screen on | Android slows a page in the background to about one step a second |

On iPhone and iPad, no: iOS has no WebUSB or Web Serial.

## Privacy

### Is basic privacy secure?

No. Basic privacy scrambles voice with a fixed key. It keeps casual listeners out, nothing more.
