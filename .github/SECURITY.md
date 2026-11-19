<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Security

## Reporting a vulnerability

Report vulnerabilities privately. On GitHub, go to
[the project's security advisories](https://github.com/RadioFoundry/RadioFoundry/security/advisories) and click
[**Report a vulnerability**](https://github.com/RadioFoundry/RadioFoundry/security/advisories/new). Don't open a
public issue or pull request about it.

In the report, include what's affected (the firmware and the radio, the flasher, the web tools, the site, or CI), the
version or commit, and how to reproduce the issue. Expect an answer within a week. We fix vulnerabilities in a release
and publish the advisory with it. The advisory credits you, unless you ask not to be named.

## In scope

The following are in scope:

- The firmware: input that makes the radio do something it shouldn't, such as crash, run attacker-supplied code, or
  transmit without the user asking it to. This covers a received transmission, anything on the network side (a
  hotspot, IP traffic over DMR, a browser on the web control page), the computer on its USB port, and a codeplug.
- `tools/rfoundry` and the web flasher: a crafted firmware image or other file that runs code on the computer, or
  "bricks" a radio so it can't be recovered.
- The site and the CI workflows: anything that lets someone, by opening a pull request or by visiting the site,
  change what is built, published or served.

## Not a vulnerability

The following aren't vulnerabilities:

- Eavesdropping, spoofing a call sign or DMR ID, or breaking DMR basic privacy. These modes don't authenticate
  senders, and basic privacy is a weak scrambler, not encryption.
- Attacks that need physical access to the radio. Anyone holding it can start the bootloader (keys held at power-on)
  and flash any image; that's by design.
