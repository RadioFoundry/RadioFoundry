<!-- SPDX-License-Identifier: MIT -->

# FAQ

**Why allow commercial use?**

Commercial use is the reason $40 DMR radios exist. If everything DMR were ham-only, a decent radio would still cost
$500. The project authors don't make money from RadioFoundry, so nobody loses when someone else uses it in a product. The
project doesn't sell DMR firmware. The goal is a decent firmware that everyone can use.

**Why reinvent the wheel?**

RadioFoundry covers most of these radios with one core under a license that lets anyone use it, so work on one
radio helps every other.

**Why another radio firmware?**

DMR has many features that inexpensive radios rarely implement. They are useful for commercial applications and for
amateur use.

**How can I help?**

Any way you can:

- **Test and report.** Run RadioFoundry on your radio and [report what breaks](https://github.com/radiofoundry/radiofoundry/issues/new?template=bug_report.yml). If the radio restarted,
  include the crash log from **Settings > Advanced > Crash log** ([settings](17-settings.md#crash-log)).
- **Help others.** Answer questions and share what works in the
  [community group](https://www.facebook.com/groups/radiofoundry/) on Facebook.
- **Ask.** [Request a feature](https://github.com/radiofoundry/radiofoundry/issues/new?template=feature_request.yml) or [a radio](https://github.com/radiofoundry/radiofoundry/issues/new?template=radio_request.yml) you'd like supported.
- **Contribute.** Send pull requests for code or docs ([contributing](../dev/contributing.md)).
- **Donate or lend a radio.** This helps the most, especially older models that are hard to find now, because every
  port needs the hardware on the bench.

**The web flasher or the Codeplug Editor failed. How do I report it?**

On radiofoundry.org most errors are reported automatically: the message says so and gives a reference. Type what you
were doing in the box under it and click **Send Details**; the developers review every report and work on the
underlying problem. To send more, or where reporting is off or blocked, use **Report a Problem**.

Click **Report a Problem** under the error message, describe what you were doing, and download the report. It's a zip
file with the steps the page took, the errors and your browser's details. Email it to the developers, attach it to a
GitHub issue or share it in the [community group](https://www.facebook.com/groups/radiofoundry/). If the problem is reading or writing a codeplug, tick the box that includes it: a codeplug holds your
callsign, DMR ID and contacts, so it's left out unless you add it. Developers can also open the browser's console
(F12): every error is printed there with its details.

**Writing a codeplug fails with "The radio refused to write the codeplug". What now?**

If the radio came straight from its original firmware, choose **Write to Radio** again: the editor offers **Prepare
Radio**, which erases the original firmware's data from the area RadioFoundry keeps its settings in. Without that, the radio
refuses every write and forgets its settings when it's turned off. If the error comes back after that, use **Report a
Problem**.

**Why doesn't the release include the codec?**

The AMBE+2 codec DMR voice needs is DVSI's, and RadioFoundry can't distribute it. Your radio already has it, in its stock
firmware. rfoundry leaves it in place when it's already there, or takes it from your radio's stock firmware file on
your own computer ([Installing](02-installing.md)).

**Can I use RadioFoundry without the codec?**

Yes. FM works and DMR calls show on the screen. DMR data (messages, call alert, radio check) and hotspot mode work.
DMR voice is off.

**Will RadioFoundry damage my radio?**

Flashing writes only the processor's application and codec sectors. The bootloader and the factory calibration are
never written, so you can go back to stock ([Installing](02-installing.md#going-back-to-the-stock-firmware)). RadioFoundry is early software.
Test it before you depend on it, and transmit only where you are licensed to.

**Does qdmr work with RadioFoundry?**

Yes, through a compatibility layer. qdmr's open-firmware driver reads and writes the codeplug
([Programming the radio](04-programming.md)). On a radio whose codeplug sits where qdmr's driver can't write, use the Codeplug Editor ([Radio reference](23-radios.md#programming)).

**Can I program the radio without installing anything?**

Yes. The Codeplug Editor on the project's web site reads and writes the radio from a Chromium-based browser (Chrome,
Edge, Opera) and builds the DMR ID database from RadioID.net
([The Codeplug Editor](04-programming.md#the-codeplug-editor)).

**Is basic privacy secure?**

No. Basic privacy scrambles voice with a fixed key. It keeps casual listeners out, nothing more.
