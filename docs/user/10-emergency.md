<!-- SPDX-License-Identifier: MIT -->

# Emergency

The emergency features are built and pass the host tests. They haven't been tested on air yet. Don't rely on them
for safety.

## When someone else declares an emergency

When the radio hears an emergency alarm, or plays a call marked as an emergency, to you or to a talkgroup you listen
to, it opens the emergency screen and plays an alert tone (a warble of about 1 s). The screen shows EMERGENCY and the
station's name or ID.

<img src="../ux/screens/160x120i5/emergency.png" width="200" alt="An emergency received">

| Key | Does |
|---|---|
| **OK** | Acknowledge: closes the screen ("Emergency acknowledged") |
| **#** | Closes the screen and opens the actions for that station: talk to them, call them privately, radio check |
| **Back** | Hides the screen |

The same radio's emergency again within 30 s of its last activity (repeated alarms, the call that follows) doesn't
raise a second alert.

## Declaring an emergency

The radio core can send an emergency alarm and start an emergency call with a hot microphone. By default it sends the
alarm 3 times, then keys the transmitter for 10 s so the people listening hear you without you pressing **PTT**. Every
call you make until you cancel carries the emergency flag and goes to the channel's talk target. Pressing **PTT**
during the hot mic takes over the transmission.

On the DM-1701, hold the **top side key** (above **PTT**) for 1.5 s. The screen says "EMERGENCY sent: hold again to
cancel". Hold it again for 1.5 s to cancel ("Emergency cancelled"). A short press of the same key still changes the
power level, and letting go before 1.5 s never starts an emergency. Where it can't start (see below) the radio says
"Can't start emergency here: DMR channel and DMR ID needed". The DM-32 has no emergency key yet.

Developers can also start and cancel it over the debug link:

```
tools/odr_debug.py data emergency start
tools/odr_debug.py data emergency cancel
```

The mode (alarm only, alarm then call, call only), the target, the number of alarms and the hot mic time are set the
same way (`tools/odr_debug.py data em-settings`). See [docs/spec/dmr-emergency.md](../spec/dmr-emergency.md).

An emergency can't start on an FM channel, the VFO, a receive-only channel, in hotspot mode, or without a DMR ID.
