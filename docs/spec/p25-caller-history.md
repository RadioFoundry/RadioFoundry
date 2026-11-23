<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# P25 caller history

With `CONFIG_P25`, the UI adds selected P25 calls with a nonzero source ID to
Recent. It records the source, destination, group/private indication and last
observed time. With `CONFIG_P25_ALIAS`, the completed current alias updates the
entry even when it completes just before the call ends. Selected encrypted calls
can also supply caller metadata; their audio remains muted. Filtered calls do not
create an entry. Recent remains volatile and is cleared at boot.

P25 and DMR identities are separate even when their numeric source and destination
IDs match. The assignable Recent function opens the history in every mode. Received P25 entries open the
existing Actions screen as read-only details: alias or source ID in the title,
source ID, received talkgroup if applicable, and a P25 RX-only indication.
An alias completed while those details are open updates the title.
Recent rows show the alias (or numeric source ID) without a mode prefix, leaving
more horizontal room for the name. The details page retains the P25 RX-only label.

Selecting a P25 Recent entry opens those details. It never supplies a DMR focus
target, callback target, message target, contact-save action or talkgroup-picker
entry. DMR Recent behavior remains unchanged. This UI work changes neither the
radio ID nor any channel/system transmit permission and sends no registration or
radio traffic. See [the UI](ui.md) and [P25](p25.md) for the shared view and menus.

Completed selected aliases cross the engine/radio/UI boundary as a retained event,
with a session and call number. A call ending or a control-channel retune cannot
lose that event between UI ticks. The history consumer applies each event once to
its call. A newer played call from the same unit and destination reuses its last
verified name within the same receiver session. A newly received name replaces it.
With `CONFIG_P25_ALIAS_CACHE`, a separate radio-task RAM cache retains 64 validated
trunked aliases across receiver sessions. Its key is the received WACN, system ID,
and 24-bit unit ID; site and talkgroup changes do not change that identity. Unknown
or unverified system identity cannot populate or query the cache. Conventional
channels continue to use the session-scoped Recent lookup.

Completed selected alias events update the cache, including aliases received after
speech. A newly decoded name takes precedence over a cached name. A selected live
call without a new alias gets its cached name in the radio view, which both Home
and Recent consume. Lookup refreshes recency; the least recently used entry is
evicted at capacity. Radio initialization clears all entries. The cache uses 1540
bytes of RAM (64 24-byte entries plus count/padding), performs no flash writes,
and changes no transmit permissions, IDs, or registration behavior. This is not a
persistent alias database.
Filtered calls do not publish alias events. Event storage survives an
engine restart, but its session prevents use on the new channel's live display.

Assign Recent to an available press or long-press slot in Settings > Keys, or
open Recent from the menu. Existing assignments and caller-context behavior are unchanged.
Selecting an entry opens its read-only caller details. A completed alias remains
visible on idle Home for at most two seconds after UI receipt, without delaying
the control-channel return. A new call or channel suppresses that display.
