<!-- SPDX-License-Identifier: MIT -->

# Hotspot mode

In hotspot mode the radio is a DMR modem for a computer running MMDVMHost, Pi-Star or a similar host. The host
connects the radio to a DMR network (BrandMeister, TGIF and others); the radio carries the calls to and from radios
nearby on one frequency. The radio speaks the MMDVM modem protocol over its USB serial port.

Hotspot voice and data calls, both ways, are verified on air with the DM-1701 on the SDR bench. The MD-380's default image leaves hotspot mode out for space; **Tools >
Hotspot** is dimmed there.

## What you need

- A computer with MMDVMHost (Linux, macOS), or a Pi-Star style image on a Raspberry Pi, and the radio's USB cable.
- The codec isn't needed: the radio passes the DMR frames through without decoding the voice.
- **Settings > Connections > Hotspot allowed** on (the default).

## Setting up MMDVMHost

Point MMDVMHost's modem at the radio's serial port and run it as a simplex DMR hotspot. The parts of `MMDVM.ini` that
matter:

```
[General]
Duplex=0

[Info]
RXFrequency=438800000
TXFrequency=438800000

[Modem]
Protocol=uart
UARTPort=/dev/ttyACM0
UARTSpeed=115200

[DMR]
Enable=1
ColorCode=1

[DMR Network]
Enable=1
Slot1=0
Slot2=1
```

- The port is `/dev/ttyACM0` (or the next number) on Linux and `/dev/cu.usbmodem...` on macOS. The radio's USB ID is
  1FC9:0094.
- Use the same frequency for receive and transmit, inside the radio's bands (DM-1701: 136-174 and 400-480 MHz).
- The radio uses timeslot 2 only: enable network slot 2 and disable slot 1.
- The baud rate is ignored on USB; 115200 is fine.
- Leave the other modes (D-Star, YSF, P25, NXDN, POCSAG, FM) off. The radio refuses them; POCSAG is accepted but never
  transmitted.

On Pi-Star style images, choose a USB MMDVM modem on the radio's serial port and the settings above. This hasn't been
tested with Pi-Star yet.

The radio reports itself to the host as "OpenDigitalRadio DM-1701" and the firmware version.

## Starting

The radio enters hotspot mode by itself when the host opens the port and sends its first MMDVM request. The screen
changes to the hotspot screen.

<img src="../ux/screens/160x120i5/hotspot.png" width="200" alt="The hotspot screen">

The hotspot screen shows the state (STARTING, HOTSPOT, RX RF while a radio nearby talks, TX NET while a network call
goes out), the frequency, the color code and power, and the current or last call with its source ("RF" or "Net"), the
caller and the talkgroup.

**OK**, **5** (Tools), **3** (Hotspot) shows how to start it and, while it runs, a "Stop hotspot" row.

## While in hotspot mode

- **\*** switches between the hotspot screen and the diagnostics page.
- **PTT** doesn't transmit ("Hotspot mode: PTT off").
- Any other key shows "Hotspot: OK exits".
- If the host asks for a CW ID, the radio sends it in Morse on FM on the hotspot frequency (20 words per minute, 800
  Hz), then returns to DMR.

## Stopping

The radio leaves hotspot mode and returns to its channel when:

- you press **OK** or hold **Back** ("Leaving hotspot mode");
- the host closes the port, or the USB cable is unplugged;
- the host sends nothing for 20 s.

To stop the radio from entering hotspot mode at all, turn **Settings > Connections > Hotspot allowed** off.

## Single frequency repeater (SFR) mode

In SFR mode the hotspot repeats a call heard on timeslot 1 on timeslot 2 of the same frequency, and also sends it to
the network. Radios nearby in direct mode hear both. It is in the DM-1701 image, not in the MD-380's.

The host turns SFR mode on by running duplex with the receive and transmit frequencies equal (`Duplex=1`,
`RXFrequency` = `TXFrequency`). Setting `Duplex=0` or different frequencies turns it off.

Limitations today: the DM-1701 keys its transmitter for the whole repeat, so it misses the rest of an inbound call
after the first repeat. Not tested on air. The stand-alone repeater and the SFR user roles on a channel aren't built.
**Tools > Single freq. repeater** shows only an information page. Details: [docs/spec/sfr.md](../spec/sfr.md).

## Two radios as a duplex repeater

Experimental: two DM-1701s, each on its own USB port of one computer, act as one full duplex repeater, one
receiving, one transmitting. The computer runs `tools/split_duplex_host.py`. The slot numbering is still being
checked on the bench. Details:
[docs/spec/split-duplex.md](../spec/split-duplex.md).
