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
commercial applications, and are just as useful for amateur use.

**How can I help?**

Open an issue or pull request, or donate a new radio model for testing.

**Why doesn't the release include the codec?**

The AMBE+2 codec DMR voice needs is DVSI's, and ODR can't distribute it. Your radio already has it, in its stock
firmware. odrflash leaves it in place when it's already there, or takes it from your radio's stock firmware file on
your own computer ([Installing](02-installing.md)).

**Can I use ODR without the codec?**

Yes. FM works fully, DMR calls show on the screen, and DMR data (messages, call alert, radio check) and hotspot mode
work. DMR voice is off.

**Will ODR damage my radio?**

Flashing writes only the processor's application and codec sectors. The bootloader and the factory calibration are
never written, so you can go back to stock ([Installing](02-installing.md#going-back-to-the-stock-firmware)). ODR is early software:
test it before you depend on it, and check that you transmit only where you are licensed to.

**Does qdmr work with ODR?**

Yes, through a compatibility layer: qdmr's open-firmware driver reads and writes the codeplug
([Programming with qdmr](15-programming-qdmr.md)).

**Is basic privacy secure?**

No. Basic privacy scrambles voice with a fixed key. It keeps casual listeners out, nothing more.
