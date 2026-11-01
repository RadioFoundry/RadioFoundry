<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Security

## Reporting a vulnerability

Report vulnerabilities privately. In GitHub,
under [the project's Security Advisories](https://github.com/RadioFoundry/RadioFoundry/security/advisories),
click [**Report a vulnerability**](https://github.com/RadioFoundry/RadioFoundry/security/advisories/new).
Don't open a public issue or pull request about it.

Include what is affected (the firmware, and on which radio; the flasher; the web tools; the site; CI), the version or commit, and
the how to reproduce the issue. Expect an answer within a week. We fix vulnerabilities in a release and publish the advisory with it.
The advisory credits you unless you’d rather not be named.

## In scope

- The firmware: input that makes the radio do something it shouldn’t, such as crash, run attacker-supplied code or transmit
  without the user asking it to. That covers a received transmission, anything on the network side (a hotspot, IP traffic over DMR,
  a browser on the web control page), the computer on its USB port, and a codeplug.
- `tools/rfoundry` and the web flasher: a crafted firmware image or other file that runs code on the computer, or "bricks" a radio
  so it can't be recovered.
- The site and the CI workflows: anything that lets someone, by opening a pull request or by visiting the site, 
  change what is built, published or served.

## Not a vulnerability

- DMR, FM and APRS have no encryption or authentication on the air, so anyone can listen or transmit. That’s how these modes work,
  not a bug.
- Attacks that need physical access to the radio. Anyone holding it can start the bootloader (keys held at power-on) and flash
  whatever they like; that’s by design.
