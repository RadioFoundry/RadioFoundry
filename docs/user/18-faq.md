<!-- SPDX-License-Identifier: MIT -->

# FAQ

**Why allow commercial use?**

Why not? Commercial use is the reason we can buy $40 DMR radios. If everything DMR were ham-only, we would still be
paying $500 for a decent radio. Nobody "loses" here, because the project authors aren't trying to make any money off
of this. If someone can figure out a way to use it in their product and make money, great! We're not in the business
of selling DMR firmware, we just want everyone to have access to a decent one.

**Why reinvent the wheel?**

There is no current truly open source effort to make a firmware for most of these radios, and definitely not one
that tries to share this much code across all makes and models. Any prior major "open" firmware efforts have ended up
with source code being taken down by authors, licensing issues, arguments and fights, or key authors closed-sourcing
their additions to the project.

**Why another radio firmware?**

DMR is filled with cool features that cheaper radios almost never implement. These are incredibly useful for
commercial applications, and offer very powerful, useful capabilities for amateur use as well.

**How can I help?**

Open an issue or pull request, or donate a new radio model for testing.

**Why isn't the codec included?**

The AMBE+2 vocoder belongs to DVSI and isn't free to redistribute. Each radio's stock firmware carries a copy that
came with the radio. ODR's tools take the codec from the stock firmware for your radio, on your computer, so ODR never
distributes it.

**Can I use ODR without the codec?**

Yes. FM works fully, DMR calls show on the screen, and DMR data (messages, call alert, radio check) and hotspot mode
work. DMR voice is off.

**Why are there no release binaries?**

A firmware image with the codec merged can't be distributed, and the project is still early. When releases start,
each release will carry this manual as a PDF; the firmware still needs your own codec merge.

**Will ODR damage my radio?**

Flashing writes only the application area of the processor. The bootloader, the factory calibration and the stock
firmware download stay available, so you can go back to stock ([Installing](02-installing.md)). ODR is early software:
test it before you depend on it, and check that you transmit only where you are licensed to.

**Does qdmr work with ODR?**

Yes, through a compatibility layer: qdmr's open-firmware driver reads and writes the codeplug
([Programming with qdmr](15-programming-qdmr.md)).

**Is basic privacy secure?**

No. Basic privacy scrambles voice with a fixed key. It keeps casual listeners out, nothing more.
