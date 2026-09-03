<!-- SPDX-License-Identifier: MIT -->

# Messages, call alert and radio check

Text messages, call alert and radio check work on DMR channels. They are built and pass the host tests, but haven't
been tested on air yet. None of them needs the codec. They need a DMR channel you may transmit on and your DMR ID.

## Receiving a message

When a message arrives, the radio shows a banner with the sender and the first words for 8 s. Press **#** while the
banner is up to open the message. The radio plays a short three-note tone with the banner unless **Settings > Sound >
Message tone** is off.

<img src="../ux/screens/160x120i5/banner_message.png" width="200" alt="A message banner">

After the banner, the status line shows "M" and the number of unread messages, and the hub shows "1 new" next to
Messages.

## Reading messages

1. **OK**, **2** (Messages). The newest message is first; unread ones are highlighted.
2. Focus a message and press **OK**. The radio shows the sender, how long ago it arrived and the text, and marks it
   read.
3. Below the text:

   | Row | Does |
   |---|---|
   | Reply "OK" | Sends "OK" to the sender |
   | Reply "On my way" | Sends "On my way" to the sender |
   | Call | Makes the sender the talk target for now |
   | Delete | Deletes the message. There is no undo |

**#** on a message opens the actions for its sender. **PTT** in an open message calls the sender.

<img src="../ux/screens/160x120i5/messages.png" width="200" alt="Messages">
<img src="../ux/screens/160x120i5/message_view.png" width="200" alt="A message">

The radio keeps the last 4 messages received, in memory. They are lost when the radio turns off.

## Sending a message

1. Open the actions for the person or talkgroup: **#** on Home for the last caller, or **#** on a contact.
2. Press **3** (Message, or "Message the group").
3. Pick a text: "OK", "On my way", "Call me" or "Can't talk now". Press **OK** to send.

<img src="../ux/screens/160x120i5/compose.png" width="200" alt="The message composer">

The radio shows "Sending to ..." and returns to Home. A message to a person asks for a confirmation and is tried twice;
the result comes as a banner: "Delivered to ..." or "No answer from ...", with "Not delivered: send it again" under it. The message
isn't kept for a retry: send it again from the actions. A message to a talkgroup has no confirmation.

Typing your own text isn't built yet: "Write..." is dimmed. Messages you send aren't stored in the radio.

The radio sends messages in the Motorola text message format (TMS), which most radios read, and reads both TMS and
ETSI short data text. Developers can switch sending to the ETSI format over the debug link
(`tools/odr_debug.py data settings ... sms-etsi`).

## Call alert

A call alert pages another radio: it rings there, even when that radio's user isn't listening to your talkgroup.

1. Open the actions for the person (**#** on Home, or **#** on a contact).
2. Press **5** (Call alert). The radio shows "Call alert to ..." and returns to Home.
3. The radio tries up to 3 times. The result is a banner: "... answered the alert" or "No answer from ...".

When someone sends you a call alert, the radio shows "Call alert from ..." with "PTT replies". Press **#** while the
banner is up for the caller's actions. Your radio acknowledges the alert unless **Settings > Connections > Answer call
alerts** is off.

<img src="../ux/screens/160x120i5/banner_call_alert.png" width="200" alt="A call alert banner">

## Radio check

A radio check asks another radio to answer, without its user doing anything. It shows whether that radio is on and in
range.

1. Open the actions for the person.
2. Press **4** (Radio check). The radio shows "Checking ...".
3. The result is a banner: "... answered (0.8 s)" with the round-trip time, or "...: no answer" with "Off, out of range
   or elsewhere". The radio tries up to 3 times.

<img src="../ux/screens/160x120i5/banner_radio_check.png" width="200" alt="A radio check result">

Your radio answers radio checks from others only when **Settings > Connections > Answer radio checks** is on. It is off
by default, because an answer makes the radio transmit without you asking.

## Registration (ARS) and location (LRRP)

ARS registers the radio with a presence server at power-up and after a channel change. It is off until a server ID is
set, and there is no menu item for it yet; developers set it over the debug link.

LRRP answers location requests from a dispatcher. The DM-1701 has no GPS, so it answers "no fix". **Settings >
Location > Answer position requests** turns the answers off.
