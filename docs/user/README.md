<!-- SPDX-License-Identifier: MIT -->

# OpenDigitalRadio user manual

How to install OpenDigitalRadio (ODR) and use it on the radio. The manual describes the Baofeng DM-1701. Each release
also carries this manual as a PDF, built from these files (`tools/build_manual.sh`).

<img src="../ux/screens/160x120i5/home_dmr.png" width="200" alt="Home screen on a DMR channel">
<img src="../ux/screens/160x120i5/quick_dmr.png" width="200" alt="Quick settings">
<img src="../ux/screens/160x120i5/hub.png" width="200" alt="The hub">

## Contents

1. [Introduction](01-introduction.md): what ODR is, supported radios, project status, conventions
2. [Installing](02-installing.md): odrflash, the codec, flashing the DM-1701, updating, going back to stock
3. [First boot](03-first-boot.md): built-in channels, programming the radio, checks
4. [Keys and navigation](04-keys-and-navigation.md): the five key rules, Find, temporary changes, key lock
5. [The Home screen](05-home-screen.md): what each region shows, receiving, transmitting
6. [Channels, zones and the VFO](06-channels-zones-vfo.md)
7. [FM](07-fm.md): squelch, monitor, tones, bandwidth
8. [DMR](08-dmr.md): talk targets, replying, Recent, contacts, timeslot and color code, hear all, privacy
9. [Messages, call alert and radio check](09-messages-and-signaling.md)
10. [Emergency](10-emergency.md)
11. [Scan and dual watch](11-scan.md)
12. [Settings](12-settings.md): every setting, its default and status
13. [Hotspot mode](13-hotspot.md): MMDVMHost and Pi-Star, SFR
14. [USB network mode](14-usb-network.md)
15. [Programming with qdmr](15-programming-qdmr.md)
16. [Battery and power](16-battery-and-power.md)
17. [Troubleshooting](17-troubleshooting.md)
18. [FAQ](18-faq.md)
19. [Glossary](19-glossary.md)
20. [APRS](20-aprs.md): positions on FM and over DMR, beacons, KISS and iGate with a computer
21. [Moving from stock or md380tools firmware](21-moving-from-stock-md380.md): MD-380 / MD-390 / RT3, keeping the codeplug

## Quick reference

| To | Press |
|---|---|
| Change channel | Channel knob, or **Up** / **Down** |
| Change zone | **P1** / **P2**, or **OK**, **4**, pick a zone |
| Quick settings | **\*** |
| Open the menu (hub) | **OK** |
| Act on the last caller | **#** |
| Recent calls | Hold **#** |
| Choose who to talk to | **SK2** (DMR) |
| Monitor | Hold **SK1** |
| Scan on / off | Hold **SK2** |
| Undo temporary changes | **Back** on Home |
| Save changes to the channel | Hold **OK** |
| Back to Home | Hold **Back** |
| Lock / unlock keys | Hold **\*** |

Developers: see the [developer documentation](../dev/README.md).
