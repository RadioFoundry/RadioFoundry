<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Usability tests

This page describes how the UI is checked against the [design principles](principles.md) and the budgets in
[Task flows](task-flows.md). There are two layers:

1. **Automated checks** in the host simulator, on every build.
2. **Moderated sessions** with people, in the simulator and on radios.

## 1. Tools

The UI runs unchanged on the host. The tools are in `tests/host/`; for how to build and run them, see
[The UI simulator and the gallery](../dev/testing.md#the-ui-simulator-and-the-gallery).

| Tool | What it does |
|---|---|
| `uiSim` | Draws the UI for one display class (`--geom`) and key map (`--map K`, `M`, `V`, `F`, or `P`), runs a key script from Home, and writes the screen to a PNG. The script tokens also inject traffic: a group or private call, a message, the end of a call |
| `uiSim --gallery DIR` | Draws every gallery screen at every display class, in each theme on color displays (`make -C tests/host ui-gallery` writes `docs/ux/screens/`) |
| `uiSim --layout` | The layout check: the gallery screens, the settings pages, and the worst-case screens (longest names, IDs, aliases, messages, every status mark) on every display class and theme. Fails on any problem |
| `uiSim --ellipsis` | Lists every text that the gallery screens and the settings pages shorten with an ellipsis |
| `uiFlowTest` | Runs each task flow as a key script and checks the screen state the flow ends in (`screen=`, `focus=`, `toast=`, and so on), its press budget, and the codeplug writes. Also runs the screen crawler |

## 2. Automated checks

`uiFlowTest` runs these checks on every build (`make -C tests/host`, and `tools/ci.sh`):

| Check | What it does | Pass |
|---|---|---|
| Press budgets | Runs each flow in [Task flows](task-flows.md) as a script on the reference key maps and counts the presses | Every flow at or under its budget. A flow impossible on a key map that has it fails (P1, P10) |
| Operate only | Runs the flows that only operate the radio | Zero codeplug writes (P7) |
| Dead ends | The crawler checks that `BACK` leaves every screen reachable from Home and that long `BACK` reaches Home | Zero failures (P4) |
| Depth | The crawler measures the depth of field-use screens | At most 3 (P4) |
| Status strip | The crawler checks that every screen draws the status strip (or the title row that carries the state pill) and that PTT shows TX | Every screen (P2) |
| Long presses | Long `BACK`, long `QUICK`, and long `CONTEXT` on every screen | Home, lock, and the talk target picker on a DMR channel, everywhere (P3) |
| Private calls | The Transmitting screen during a private and a group call | Names the private call's contact and tells it apart from a group call (P2) |

`uiSim --layout` adds the layout check: no display class's list or strip spacing drops below its normal pitch, and
nothing clips except with a middle ellipsis (P5).

## 3. Moderated sessions

### Participants

Each round has at least 6 people (more for a major UI change):

- 2 new to DMR (who have used an FM handheld, or none).
- 2 regular DMR users of another radio brand.
- 1 existing RadioFoundry user.
- 1 tester with gloves, for the glove subset.

### Session setup

1. Plan 45 minutes, one radio class per participant, half in the simulator and half on a radio (alternate the order).
2. Give no training beyond a 1-minute introduction ("This is PTT; this is the volume knob"). Leave the hint row on.
3. Ask the participant to think aloud. Read each task as a goal, not as steps ("Talk to Joe privately", never "Use
   Find").
4. Note hesitations (no press for over 5 s), wrong turns, and comments. Identify the participant by a code, never a
   name.

### Tasks

The targets are for regular DMR users; new users get twice the time target. "Presses" is the budget in
[Task flows](task-flows.md); the target in a session is the budget + 2, to allow one exploration step.

| # | Task (as read to the participant) | Success criterion | Target presses (K / M / V) | Target time |
|---|---|---|---|---|
| T1 | Go to channel 12. | Home shows Ch 12 | 3 / 3 / 3 | 10 s |
| T2 | Switch to the zone "Simplex". | The zone shows on Home | 5 | 15 s |
| T3 | Say something on TG 91 once, then make sure you're back on the normal talkgroup. | TX on TG 91; Home back to the channel default | 3 + 1 | 20 s |
| T4 | Make TG 91 the normal talkgroup for this channel. | Codeplug written, banner seen | 3 | 15 s |
| T5 | Someone called you a moment ago. Answer them. (scripted call) | TX to the caller's TG | 1-2 | 5 s |
| T6 | Call Joe Smith privately. | Private call TX to Joe's ID | 4 | 15 s |
| T7 | Call the ID 9991234 privately. | Private call TX to that ID | 9 | 25 s |
| T8 | Reply "OK" to Joe by text. | Message sent to Joe | 4 | 15 s |
| T9 | Read the message that arrived. (scripted) | Message body shown | 1-3 | 10 s |
| T10 | Check whether Joe's radio is on. | Radio check sent; the participant reads the result aloud | 2-5 | 15 s |
| T11 | Turn the power down to low. Then turn the squelch up two steps (FM channel). | Both changed on Home | 3 / 5 | 20 s |
| T12 | Turn on privacy for this channel. | Privacy icon on Home | 3 | 10 s |
| T13 | Scan this zone (Tools > Scan); skip the noisy channel; stop. | Scan started, channel skipped, scan stopped | 3 + 2 + 1 | 30 s |
| T14 | Listen on 446.000 MHz. | VFO at 446.000 | 10 | 30 s |
| T15 | Lock the keys, then unlock them. | Locked and unlocked | 2 | 10 s |
| T16 | Does the radio have a GPS fix? What is your grid square? | Correct answer | 0-3 | 15 s |
| T17 | Start the hotspot (USB host connected), then stop it. | Hotspot started and stopped | 4 + 2 | 30 s |
| T18 | Who was talking a moment ago, and from where? (scripted call with alias) | Name or callsign and alias stated | 0 | 5 s |
| T19 | Switch this channel to FM and back. | Mode toggled twice | 4 | 10 s |
| T20 | Listen to everything on this timeslot, whatever the talkgroup. | Hear all on | 1-2 | 10 s |
| T21 | Switch to timeslot 2 and color code 7 for now. | Temporary TS2 CC7 on Home | 7 | 20 s |
| T22 | (any screen, after T21) Get back to the start screen and undo the temporary changes. | Home, defaults restored | 2 | 5 s |
| T23 | Five-second test: show Home for 2 s, then cover it. "Which channel? Where does PTT go? Is the battery OK?" | 3 of 3 correct | - | 2 s look |
| T24 | Emergency (radio with emergency configured, into a shielded setup or dummy load). Start, then cancel. | Started and canceled; no accidental start in other tasks | 1 + 1 | 10 s |
| T25 | Listen to this channel and to "Metro DMR" at the same time (channel 5 of this zone). Then stop. | Dual watch on with A = this channel, B = channel 5; then off, staying on the selected channel | 4 + 1 | 25 s |
| T26 | (during T25, scripted call on B) A call came in on the other channel. Which channel does your PTT talk on? Answer the call there. | The participant names the selected channel (A); TX on B after `#`, `1` | 2 + 1 | 15 s |
| T27 | Listen to this channel and to the VFO at the same time. | Dual watch on with B = the VFO | 6 | 30 s |

| Subset | Tasks | Target |
|---|---|---|
| Gloves (winter or work gloves) | T1, T3, T5, T11, T15, T24 | No extra errors compared with bare hands |
| Dual watch (radios built with `CONFIG_DUAL_WATCH`) | T25, T26, T27 | Score T25 and T26 as a pair |

### Record for each task

| Item | Values |
|---|---|
| Success | Completed, completed with help, or failed (gave up, or three times the time target) |
| Presses and time | From the end of the instruction to the success event |
| Errors | Wrong turns (a screen entered and left without effect), invalid presses, accidental TX, accidental codeplug write, lost context (the participant states the wrong talk target) |
| SEQ | The single ease question (1-7), after each task |

### After the session

1. Give the SUS (System Usability Scale) questionnaire.
2. Ask three open questions: What was hardest? What would you change first? What did you expect to find somewhere
   else?
3. File each problem as an issue labeled `ux` with the task number, the principle it breaks, and a severity:

   | Severity | When |
   |---|---|
   | Blocker | Two or more people failed the task |
   | Major | Over budget, or a wrong turn, for two or more people |
   | Minor | Comments |

## 4. Checks on a radio

| Check | Method | Pass |
|---|---|---|
| Readability | Home at arm's length (60 cm), indoors and in direct sun, by 3 people | Channel and talk target read correctly in one glance |
| Tones | Play each tone; participants name it from a list | 90% correct for talk permit, busy, invalid, message, low battery |
| Pocket test | Radio in a pocket and in a belt holster, walk 10 minutes, keys unlocked and locked | No TX, and no settings change when locked. With keys unlocked, record what changed: at most the channel or volume |
