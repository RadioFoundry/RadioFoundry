<!-- SPDX-License-Identifier: MIT -->
# Usability test protocol

How we check that the UI meets `principles.md` and the budgets in `task-flows.md`, release after release. Three layers:

1. **Automated checks** in the SDL simulator on every build (press budgets, dead ends, status strip, latency).
2. **Moderated sessions** with people, in the simulator and on real radios, before each release.
3. **Release metrics** tracked over time.

---

## 1. Tools

### SDL simulator

The host build of the UI (core UI + a simulated `hal/` display and input) runs in an SDL window. Requirements for testing:

- A **skin per reference radio** (DM-1701, DM-32, GD-77, MD-9600) drawing the real key layout and display size, with keyboard and mouse bindings, so testers press "the radio" and not a PC keyboard map they have to learn. The skin's key map is the board's real key map file.
- **Display class switch** (240x320 portrait/landscape, 160x128, 128x64) and a "glare" filter (reduced contrast) and "mono" filter.
- **Event log**: every logical action with gesture and timestamp, every screen enter/leave, every banner, every codeplug write, every TX start/stop, as JSON lines. This is the source of all metrics.
- **Scripted traffic**: a scenario file injects RX events (group call from ID 3101234 on TG 91 with talker alias, private call, text message, radio check request, emergency, GPS position) at set times, so every session sees the same radio world.
- **Script runner** (headless): replays a list of logical actions and asserts on screen state (named fields exposed by the UI model, not pixels). Used for the press-budget tests below.
- **Screen crawler** (headless): from Home, explores every reachable screen with breadth-first logical actions and records the graph.

### Real radios

- One of each reference radio with the release candidate firmware and the test codeplug (`tests/ux/codeplug-test.*`: 3 zones, 40 channels, 30 contacts, 20 talkgroups, listen lists, 2 privacy keys, canned messages).
- A second radio or SDR/hotspot to generate the scripted traffic over the air (DMR and FM), never on channels the team is not licensed for.
- A phone camera at 240 fps on a stand for latency, and a key-press counter: the firmware's debug build writes the same event log over USB serial as the simulator.

---

## 2. Automated checks (CI, every build)

| Check | What it does | Pass |
|---|---|---|
| Press budgets | Runs each flow in `task-flows.md` as a script on all four key maps; counts presses | every flow at or under its budget; impossible-on-a-map = fail (P1, P10) |
| Dead ends | Crawler verifies `BACK` leaves every screen and long `BACK` reaches Home | zero failures (P4) |
| Depth | Crawler depth of screens tagged "field use" | at most 3 (P4) |
| Status strip | Every screen reports the TX/RX pill as drawn; inject PTT on each screen | pill shows TX within 100 ms on every screen (P2) |
| Long-press lint | Action tables: long press bound only to the key's global meaning | zero violations (P3) |
| Codeplug writes | "Operate only" script (all 20 tasks, temporary variants) | zero codeplug writes (P7) |
| Confirmations | List of confirmation dialogs vs the destructive list | no extras (P7) |
| Error strings | Lint over user-facing error strings | each has an action or "see" reference (P9) |
| Latency | Simulator press-to-first-draw time (instrumented) | p95 < 50 ms on the host model of the slowest target CPU |
| Render fit | Each screen at each display class with the longest test strings | no clipping except middle ellipsis; channel name and talk target the two largest text items on Home (P5) |
| Voice prompts | Every screen and focus change has a prompt string | no missing prompts (P10) |

---

## 3. Moderated sessions

### Participants

Per release, at least 6 people (more for major UI changes):

- 2 new to DMR (have used an FM handheld or none),
- 2 regular DMR users of another radio brand (Anytone, TYT, Motorola),
- 1 existing OpenDigitalRadio user,
- 1 tester using voice prompts without looking (blindfold or a low-vision participant) for the accessibility tasks,
- plus, for the MD-9600, one session in a parked car with the mic only (and never while driving).

### Session setup

- 45 minutes, one radio class per participant, half in the simulator, half on the real radio (alternate the order).
- No training beyond a 1-minute "this is PTT, this is the volume knob" introduction. The hint row is on.
- Think aloud. The moderator reads tasks as goals, not as steps ("Talk to Joe privately", never "use Find").
- The event log is recorded; the moderator notes hesitations (no press for over 5 s), wrong turns and comments.

### Tasks

Targets are for regular DMR users; new users get 2x the time target. "Presses" = the budget in `task-flows.md`; the target in a session is budget + 2 (to allow one exploration step).

| # | Task (as read to the participant) | Success criterion | Target presses (K / M / V) | Target time |
|---|---|---|---|---|
| T1 | Go to channel 12. | Home shows Ch 12 | 3 / 3 / 3 | 10 s |
| T2 | Switch to the zone "Simplex". | Zone shown on Home | 5 | 15 s |
| T3 | Say something on TG 91 once, then make sure you're back on the normal talkgroup. | TX on TG 91; Home back to channel default | 3 + 1 | 20 s |
| T4 | Make TG 91 the normal talkgroup for this channel. | Codeplug written, banner seen | 3 | 15 s |
| T5 | Someone just called you. Answer them. (scripted call) | TX to the caller's TG | 1-2 | 5 s |
| T6 | Call Joe Smith privately. | Private call TX to Joe's ID | 4 | 15 s |
| T7 | Call the ID 3101234 privately. | Private call TX to that ID | 9 | 25 s |
| T8 | Reply "OK" to Joe by text. | Message sent to Joe | 4 | 15 s |
| T9 | Read the message that just arrived. (scripted) | Message body shown | 1-3 | 10 s |
| T10 | Check whether Joe's radio is on. | Radio check sent, result read aloud by participant | 2-5 | 15 s |
| T11 | Turn the power down to low. Then the squelch up two steps (FM channel). | Both changed on Home | 3 / 5 | 20 s |
| T12 | Turn on privacy for this channel. | Privacy icon on Home | 3 | 10 s |
| T13 | Scan this zone; skip the noisy channel; stop. | Scan started, channel skipped, stopped | 2 + 2 + 1 | 30 s |
| T14 | Listen on 446.000 MHz. | VFO at 446.000 | 10 | 30 s |
| T15 | Lock the keys, then unlock them. | Lock and unlock events | 2 | 10 s |
| T16 | Does the radio have a GPS fix? What is your grid square? | Correct answer | 0-3 | 15 s |
| T17 | Start the hotspot (USB host connected), then stop it. | Hotspot started and stopped | 4 + 2 | 30 s |
| T18 | Who was talking just now, and from where? (scripted call with alias) | Name/callsign and alias stated | 0 | 5 s |
| T19 | Switch this channel to FM and back. | Mode toggled twice | 4 | 10 s |
| T20 | Listen to everything on this timeslot, whatever the talkgroup. | Hear all on | 1-2 | 10 s |
| T21 | Switch to timeslot 2 and color code 7 for now. | Temporary TS2 CC7 on Home | 7 | 20 s |
| T22 | (any screen, after T21) Get back to the start screen and undo the temporary changes. | Home, defaults restored | 2 | 5 s |
| T23 | Five-second test: show Home for 2 s, cover it. "Which channel? Where will PTT go? Is the battery OK?" | 3 of 3 correct | - | 2 s look |
| T24 | Emergency (radio with emergency configured, in a shielded setup or dummy load). Start, then cancel. | Started and cancelled, no accidental start in other tasks | 1 + 1 | 10 s |

Accessibility subset (voice prompts on, no looking): T1, T3, T5, T6, T9, T11, T15, T18. Target: all completed, time at most 2x the sighted target.

Glove subset (winter or work gloves): T1, T3, T5, T11, T15, T24. Target: no extra errors versus bare hands.

### Per task, record

- Success: completed / completed with help / failed (gave up or 3x the time target).
- Presses (from the event log) and time from the end of the instruction to the success event.
- Errors: wrong turns (screen entered and left without effect), invalid presses, accidental TX, accidental codeplug write, lost context (participant states the wrong talk target).
- SEQ (single ease question, 1-7) after each task.

### After the session

- SUS (System Usability Scale) questionnaire.
- Three open questions: what was hardest, what would you change first, what did you expect to find somewhere else.

---

## 4. Real-radio checks per release

| Check | Method | Pass |
|---|---|---|
| Press-to-pixel latency | 240 fps video, 20 presses per key type, per radio | p95 < 50 ms, max < 150 ms (P8) |
| PTT-to-TX pill | video | < 100 ms |
| Readability | Home at arm's length (60 cm), indoor and in direct sun, by 3 people | channel and talk target read correctly in one glance |
| Tones | each tone played; participants name it from a list | 90% correct for talk permit, busy, invalid, message, low battery |
| Voice prompts | top 10 tasks blindfolded | all completed |
| Pocket test | radio in a pocket and a belt holster, walk 10 minutes, keys unlocked and locked | no TX, no settings change when locked; with keys unlocked, record what changed (should be channel/volume at most) |

---

## 5. Release metrics

Tracked in `tests/ux/metrics.csv` (one row per release per radio class), plotted in the release notes.

| Metric | Target | Source |
|---|---|---|
| Task success rate (moderated) | ≥ 90% overall, 100% for T1, T5, T9, T15, T18 | sessions |
| Median presses / budget (per task) | ≤ 1.2 | event log |
| Median time / target time | ≤ 1.0 | event log |
| Wrong turns per task | ≤ 0.5 | event log |
| Accidental TX or codeplug write | 0 | event log |
| SUS score | ≥ 80 (a "good to excellent" product) | questionnaire |
| Five-second test (T23) | ≥ 90% fully correct | sessions |
| Press-budget CI failures | 0 at release | CI |
| Dead ends found by crawler | 0 | CI |
| Latency p95 per radio | < 50 ms | camera / log |
| Confirm dialogs not on the destructive list | 0 | CI |
| Settings found in < 15 s (sample of 10) | ≥ 90% | sessions |
| Accessibility subset completion | 100% | sessions |

A release that regresses any metric by more than 10% against the previous release needs a note in the release notes explaining why, or a fix.

---

## 6. Running a session in the simulator (checklist)

1. Build the host simulator with the release candidate (`tools/` will hold the exact command).
2. Load `tests/ux/codeplug-test.*` and the scenario `tests/ux/scenario-session.*`.
3. Choose the participant's radio skin and display class.
4. Start the event log; note the participant code (no names in logs).
5. Read tasks in order T1..T24 (randomize T6-T12 between participants to spread learning effects).
6. After the session, run `tools/ux_report` (to be written) on the log: it produces the per-task presses, time, wrong turns and a press-by-press trace for each failure.
7. File each problem as an issue labeled `ux` with the task number, the trace, the principle it breaks and a severity (blocker: task failed by 2+ people; major: over budget or wrong turn by 2+; minor: comments).

## 7. Running on a real radio (checklist)

1. Flash the release candidate and the test codeplug. Debug build streams the event log over USB serial.
2. Set the traffic generator to the scenario (second radio or hotspot/SDR) on the test frequency only.
3. Same task list; the moderator triggers scripted traffic at the marked steps.
4. Record video of the screen and keys (for latency and for reviewing hesitation).
5. Same report and issue filing as the simulator.
