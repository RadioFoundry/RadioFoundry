<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Recommended ham trunked systems

RadioFoundry recommends one DMR Tier III setup and one P25 trunked setup for hams in the US who start a trunked
system. Every ham system that follows the recommendation looks the same to a radio, so a radio set up once works on
any of those systems, and sites of different operators side by side never clash. The setups run on the BrandMeister
numbering: a ham's RadioID DMR ID is the radio's ID, and the talkgroups are BrandMeister's.

The Codeplug Editor fills a recommended system in (4), and qdmr YAML templates hold each one. The records are in
[dmr-trunking-codeplug.md](dmr-trunking-codeplug.md) and [p25-codeplug.md](p25-codeplug.md); what the radio does on
the systems is in [dmr-trunking.md](dmr-trunking.md) and [p25-trunking.md](p25-trunking.md).

## 1. Both

| Item | Recommendation | Why |
|---|---|---|
| Radio ID | The ham's DMR ID from RadioID.net | One ID everywhere: BrandMeister, conventional DMR, both trunked systems |
| Talkgroups | BrandMeister's numbers (91, 93, 3100, the state talkgroups 31xx, 9 for local) | A site linked to BrandMeister carries the same talkgroup on the same number |
| Registration | On: the radio registers, and registers its talkgroups | The site knows where each radio is and which talkgroups to carry |
| Authentication | Off, no key | Every licensed ham can use any site. A key would have to be handed out, and keys aren't shared |
| Encryption | None | Messages encoded to obscure their meaning are not allowed on the ham bands (47 CFR 97.113(a)(4)) |
| Transmit | On | A ham system is for talking. Listen only stays the choice for systems of other services |
| Frequencies | Any ham repeater pair, coordinated as any repeater | Grants and the channel plan carry frequencies (2, 3), so no national channel plan is needed |

## 2. DMR Tier III

C_SYScode ([dmr-trunking.md](dmr-trunking.md) 4) names the network: MODEL, NET, SITE, PAR. The radio takes a control
channel when MODEL and NET match its system, the site is in its site list (empty: any), and PAR admits its category.

| Item | Site | Radio | Why |
|---|---|---|---|
| Network model | Huge (3) | Huge | The most site numbers under one NET: 1024 |
| NET | 1 | 1 | One NET for every ham site in the country (below) |
| Site | 0-1022, chosen with the neighbors | Site list empty | Each site's number differs from those of the sites around. The radio takes any site |
| PAR, category | A and B | A | Any radio is admitted |
| Color code | 1 | 1 | The common ham DMR color code |
| Logical channel numbers | 4 × site + carrier (carrier 1 holds the control channel, 1-4) | The control channel's | No two sites announce one number for different frequencies |
| Grants | Absolute frequencies (LPCN 0xFFF with the appended block, [dmr-trunking.md](dmr-trunking.md) 3) | | Any frequency works without a channel plan in the radio |
| Learn the system from the air | | Off | The radio transmits, so the codeplug names the system |

**One NET for every site.** A NET per region or per state breaks roaming where two meet: a radio near a state line
would need one system for each side, and a metro area that spans the line would be two networks. With one MODEL and
NET, every ham site is a site of one network to the radio. The radio moves to a neighbor site that the current site
announces (Adjacent_Site, [dmr-trunking.md](dmr-trunking.md) 7.18) or whose control channel is among the system's,
up to 4 per system. Sites of one operator carry calls between them over their backbone. Sites of different operators
are separate backbones and still one network on the air: each site is its own registration area, so the radio
registers again at each site, and the current site carries the radio's calls.

**Why Huge, not Large.** Large has 16 NETs and 256 sites in each. A single ham NET under Large leaves 256 site
numbers for the country, which runs short where sites are dense. Huge leaves 1024 under one NET. Several operators'
networks still share the site space, which is why a site's number is chosen with its neighbors, the way a color code
or a repeater pair is.

**Logical channel numbers.** Grants carry the frequencies, but a site still announces its channels and its neighbors'
control channels by number. 4 × site + carrier gives every site its own four numbers (site 1022's fourth is 4092,
within 1-4094), so announcements from sites of different operators never clash.

**Addresses.** Tier III addresses are 24 bits. Every DMR ID and BrandMeister talkgroup is within the range for
individual and group addresses (up to 16776415), so both carry over as they are.

## 3. P25 trunked

No P25 trunked identity is in common use among hams; conventional P25 repeaters on the ham bands use NAC 0x293. The
recommendation keeps 0x293 on the control channel and takes a WACN of its own.

| Item | Site | Radio | Why |
|---|---|---|---|
| WACN | 0x32350 | 0x32350 | HAM in the Radix-50 code (0x3235), in the upper 16 bits as WACN names are written. Not 0xBEE00, the default many systems keep, nor another value of the 0xBEExx block. A radio set for a system of another service never takes a ham site, and a radio set for the ham WACN never takes a site of another service |
| System ID | 0x001-0xFFE, chosen with the neighbors | Any | Each operator's system differs from its neighbors'. The radio takes any ham system |
| RFSS, site | The operator's | | For display and roaming |
| Control channel NAC | 0x293 | 0x293 | The NAC ham P25 radios and hotspots already use |
| Unit ID | | The DMR ID (the radio-wide unit ID left at 0) | Unit IDs are 24 bits, so every DMR ID fits |
| Talkgroups | BrandMeister's numbers 1-65534 | | P25 talkgroups are 16 bits, and 65535 calls everyone. A BrandMeister talkgroup above 65535 has no P25 number: the system doesn't carry that talkgroup. A P25 channel with one calls talkgroup 1 instead, and the Codeplug Editor warns ([p25-codeplug.md](p25-codeplug.md)) |
| Encrypted calls | None sent | Skipped | Section 1 |
| Identifier plan | Sent on the control channel (below) | Learned | The radio needs no plan programmed |

The site's identifier plan has one row per band in use. Each row's transmit offset is the site's own repeater offset
(inbound minus outbound):

| Identifier | Band | Base | Spacing | Bandwidth |
|---|---|---|---|---|
| 1 | 2 m | 144.000 MHz | 5 kHz | 12.5 kHz |
| 2 | 1.25 m | 222.000 MHz | 5 kHz | 12.5 kHz |
| 3 | 70 cm | 420.000 MHz | 12.5 kHz | 12.5 kHz |
| 4 | 33 cm | 902.000 MHz | 12.5 kHz | 12.5 kHz |

Each row reaches its whole band with channel numbers of 12 bits, on the band's usual repeater raster.

## 4. In the Codeplug Editor

**Add Recommended Ham System** on the Systems tab adds a system of the type picked beside the button with the radio's side of
2 or 3 filled in. The button asks for the name, the control channel's frequencies and, for Tier III, the site's
number (which gives the control channel's logical channel number). The system form then opens with every setting to
check before the system is added. The values are the mode modules' (`ham` in `site/static/js/codeplug/mode_t3.js` and
`mode_p25.js`).

The templates are qdmr YAML files with the `radioFoundry` block ([qdmr-yaml.md](qdmr-yaml.md) 3.7): a radio ID, the
BrandMeister talkgroups, an RX group list, the system and one channel on the system.

| Template | System |
|---|---|
| [ham-tier3.yaml](../examples/ham-tier3.yaml) | Tier III (2) |
| [ham-p25-trunked.yaml](../examples/ham-p25-trunked.yaml) | P25 trunked (3) |

Tests: `site/tests/modes.test.mjs` (the recommended systems), `site/tests/qdmr.test.mjs` (each template opens without
notes, holds the recommended system, validates and encodes).
