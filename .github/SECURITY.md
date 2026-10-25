<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Security

## Reporting a vulnerability

Report it privately: the repository's **Security** tab, **Report a vulnerability**. Don't open a public issue or pull
request for it.

Include what is affected (firmware and radio, the flasher, the web tools, the site, CI), the version or commit, and
the steps to reproduce. Expect an answer within a week. A fix ships in a release, and the advisory is published with
it, crediting you unless you'd rather not be named.

## In scope

- The firmware: anything a received transmission, a network peer (hotspot, DMR IP, the web control page), a USB host or
  a codeplug can do that it shouldn't, such as a crash, code execution or transmitting without the user's action.
- `tools/rfoundry` and the web flasher: a crafted image or file that runs code on the computer or bricks a radio
  past recovery.
- The site and the CI workflows: anything that lets a pull request or a visitor change what is built, published or
  served.

## Not a vulnerability

- DMR, FM and APRS carry no encryption or authentication on the air; anyone can listen or transmit. That is the
  standard, not a bug.
- Anything that needs the radio in the bootloader with a person at the keys.
