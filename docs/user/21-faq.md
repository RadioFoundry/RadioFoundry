<!-- SPDX-License-Identifier: MIT -->

# FAQ

**Why allow commercial use?**

Commercial use is the reason $40 DMR radios exist. If everything DMR were ham-only, a decent radio would still cost
$500. The project authors don't make money from ODR, so nobody loses when someone else uses it in a product. The
project doesn't sell DMR firmware. The goal is a decent firmware that everyone can use.

**Why reinvent the wheel?**

No other open source firmware project covers most of these radios, and none shares this much code across makes and
models. Earlier "open" firmware projects ended with source code taken down by its authors, licensing issues,
disputes, or key authors closing the source of their additions.

**Why another radio firmware?**

DMR has many features that inexpensive radios rarely implement. They are useful for commercial applications and for
amateur use.

**How can I help?**

Open an issue or pull request, or donate a new radio model for testing.

**The web flasher or the Codeplug Editor failed. How do I report it?**

Click **Report a Problem** under the error message, describe what you were doing, and download the report. It's a zip
file with the steps the page took, the errors and your browser's details. Email it to the developers or attach it to a
GitHub issue. If the problem is reading or writing a codeplug, tick the box that includes it: a codeplug holds your
callsign, DMR ID and contacts, so it's left out unless you add it. Developers can also open the browser's console
(F12): every error is printed there with its details.

**Writing a codeplug fails with "The radio refused to write the codeplug". What now?**

If the radio came straight from its original firmware, choose **Write to Radio** again: the editor offers **Prepare
Radio**, which erases the original firmware's data from the area ODR keeps its settings in. Without that, the radio
refuses every write and forgets its settings when it's turned off. If the error comes back after that, use **Report a
Problem**.

**Why doesn't the release include the codec?**

The AMBE+2 codec DMR voice needs is DVSI's, and ODR can't distribute it. Your radio already has it, in its stock
firmware. odrflash leaves it in place when it's already there, or takes it from your radio's stock firmware file on
your own computer ([Installing](02-installing.md)).

**Can I use ODR without the codec?**

Yes. FM works and DMR calls show on the screen. DMR data (messages, call alert, radio check) and hotspot mode work.
DMR voice is off.

**Will ODR damage my radio?**

Flashing writes only the processor's application and codec sectors. The bootloader and the factory calibration are
never written, so you can go back to stock ([Installing](02-installing.md#going-back-to-the-stock-firmware)). ODR is early software.
Test it before you depend on it, and transmit only where you are licensed to.

**Does qdmr work with ODR?**

Yes, through a compatibility layer. qdmr's open-firmware driver reads and writes the codeplug
([Programming the radio](15-programming-qdmr.md)). On the DM-32UV qdmr can't write the codeplug directly
([DM-32UV with qdmr](23-radios.md#dm-32uv-with-qdmr)).

**Can I program the radio without installing anything?**

Yes. The Codeplug Editor on the project's web site reads and writes the radio from a Chromium-based browser (Chrome,
Edge, Opera) and builds the DMR ID database from RadioID.net
([The Codeplug Editor](15-programming-qdmr.md#the-codeplug-editor)).

**Is basic privacy secure?**

No. Basic privacy scrambles voice with a fixed key. It keeps casual listeners out, nothing more.
