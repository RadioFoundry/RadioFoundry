<!-- SPDX-License-Identifier: MIT -->
# Dual watch

Design for dual watch on Home ([#96](https://github.com/sfowlr/OpenDigitalRadio/issues/96)). Built with
`CONFIG_DUAL_WATCH` (on with `CONFIG_SCAN`; [build-config.md](../build-config.md)). Code: `core/ui/ui_dual.c` (behavior and
its screens), `ui_draw_home_dual` in `core/ui/ui_screens.c` (Home), `ui_svc_dual` (the radio).

## 1. What it is

The radio listens on two channels, **A** and **B**, and you talk on one of them. Each can be any channel of any zone,
or the VFO: dual watch is two frequencies, not the two timeslots of one DMR frequency.

- **A** is the channel you were on when dual watch started; **B** is the one you added.
- One of them is **selected**. PTT transmits on it, and the dual watch keeps going after the transmission. Up / Down,
  the knob, digits and Quick settings act on it, as they act on the one channel of the normal Home. A quick setting
  changed on the selected channel keeps the watch on.
- A radio with one receiver (every radio today) alternates between A and B, so a call on one can be missed while it
  is on the other. Home doesn't say so: the mode in the strip implies it, and the user guide has the caveat. A radio
  with two receivers (`ui_svc_receivers()` > 1, `ui_svc_dual_rx(pane)`) hears both at once (section 3.1). No radio
  reports two yet. The AT-D578UV, which has two receiver chips ([hardware-anytone.md](../spec/hardware-anytone.md)),
  would be the first.
- What the radio core does with the pair (any zone or the VFO, PTT, retunes) is in
  [radio-features.md 2.1](../spec/radio-features.md#21-dual-watch). The UI refuses only a channel the radio can't
  read, or the same channel twice: "Can't watch that channel: pick another".

## 2. Turning it on and off

| How | Does |
|---|---|
| **\***, **7** (Quick tile **Dual watch**) | On with the last B (2 presses), the first time the B picker; off while it runs. The tile says On or Off |
| Hold **SK2** (every radio with an SK2; the DB25-D PRO's tone key) | The same: on with the last B / off |
| **A/B** key (DM-1801 Pro's A/B key on a press; a side key function elsewhere) | Off: starts dual watch with the last B (or the picker). On: selects the other channel |
| **Dual watch** key (DM-1801 Pro's A/B key held; a side key function elsewhere) | On (last B, or the picker) / off |
| **Back** on Home | Off, staying on the selected channel (a banner or VFO entry goes first, as on the normal Home) |
| Tools › Scan, the **Scan** key function | A zone scan (the band in the VFO) in its place: dual watch stops on the selected channel, then the scan starts |
| **#** **3** while it runs | Another B (**Change B**) |

A zone scan is in Tools › Scan only (and on a side key set to Scan): tile 7 and SK2 hold are dual watch.

<img src="screens/160x120i5/dual_pick.png" width="200" alt="B picker">
<img src="screens/160x120i5/dual_zones.png" width="200" alt="Zone for B">

The **B picker** lists the channels of a zone, with their frequency and mode. The first row is the zone: OK on it
lists the VFO (with its frequency) and the zones; a zone goes back to the picker on that zone. The other pane's channel is shown dimmed with its letter ("A" when you pick B) and can't be picked. When the pane has a channel in this zone, it is focused and marked as the current one. Digits find a channel by number
or name, as in the zone's channel list.

## 3. Home

<img src="screens/240x320/home_dual.png" width="200" alt="Dual watch Home, 240x320">
<img src="screens/240x320/home_dual_rx_b.png" width="200" alt="A call on B">
<img src="screens/240x320/home_dual_tx.png" width="200" alt="Talking on A">
<img src="screens/240x320/home_dual_other_zone.png" width="200" alt="B in another zone">

Two panes of the same size, A over B: neither channel is second class. Each pane:

| Row | Shows |
|---|---|
| Tag | The letter in a box as tall as the pane; under the letter, an arrow (▶) on the selected pane only |
| 1 | Channel number and name (the VFO: its frequency); at the right, the PTT pill (selected pane) or TX |
| 2 | What PTT talks to (DMR: "TG 3100 USA"), or the frequency (FM); while a call comes in, the caller, and the RX pill at the right |
| 3 | Frequency, timeslot and color code, or tone and width (where there is room) |
| 4 | The zone (where there is room: B may be in another zone than A). Where row 3 has room, the zone sits at its right instead |

**Which channel PTT uses** (asked for: "easy to tell which one transmits"): the selected pane's tag is filled and has
the arrow, and its row 1 always has the **PTT** pill. The other pane has neither. While transmitting, the tag and pill
turn red and say **TX**, and the line under the panes says "TALKING on A" with the time.

**Where a call is coming in**: that pane's tag and text turn green, row 2 names the caller with an **RX** pill at its
right, and the line under the panes says "Receiving on B" with the meter. On the selected pane the pane shows both
PTT (row 1) and RX (row 2). The other pane dims.

**Under the panes**: the state line (TALKING or Receiving) when there is one, then the last calls, newest first, each with its pane's letter: "B Joe
Smith TG 91 10s ago". On displays without room for the list (128x64), the pane the last call came in on says how long
ago in amber: "Heard 7s ago" in its first row where it fits, else "7s ago" at the right of its second row. For 10 s
after a call on the other channel, the hint row says "# 1 talk on B  Back stop"; during talk back (section 4.2) "# 1
back to A  Back stop"; otherwise "# options  Back stop".

The status strip shows **DUAL** (amber) and the selected channel's mode (the radio's own mode alternates). It says RX while a call comes in on either pane. Every color comes with a word (P5).

**Large text** (Settings › Display): one line per channel, "A COUNTY DMR" / "B Metro DMR", as large as fits, the
selected one marked with an arrow at its left, green while a call comes in on it. The line receiving a call has its letter in a filled box.

### 3.1 Two receivers

With `ui_svc_receivers()` > 1, both panes can receive at once, each with its caller and RX pill. Under the panes there is
a line per receiving pane: "Receiving on A" with its meter, and "Receiving on B, muted" for the one that is not
heard; where there is room for one line only, "Receiving on A and B". The strip says RX. Gallery: `home_dual_two_rx`,
`home_dual_two_rx_large_text`. The simulator has `receivers:2`, `rxa:` / `rxb:` and the `rxaend` / `rxbend` tokens; uiFlowTest
checks a call on each, one ending (the other is then the one heard, and the ended call is listed), and talking on B.

<img src="screens/240x320/home_dual_two_rx.png" width="200" alt="Two receivers: a call on each">

### 3.2 Per display class

The layout picks the richest set of rows that fits, never a smaller font for a row than the normal Home uses for it,
and both names in one font (the largest both fit, never smaller than row 2; a longer name is shortened):

| Class | Rows per pane | Under the panes |
|---|---|---|
| 240x320, 240x320i10x5 (DM-32UV), 320x240 (GD-88) | name (medium or large), target (medium), details, zone | state and the last calls, as many as fit |
| 160x128 | name, target (medium) | one line: the state, or the last call |
| 160x120i5 (DM-1701) | name, target (small) | one line |
| 128x64 (GD-77) | name, target (small) | none: the last call's age is in its pane |

Every screen in every class is in the gallery (`docs/ux/screens/*/home_dual*.png`, `dual_*.png`).

## 4. Keys while it runs

| Key | Does |
|---|---|
| **PTT** | Transmits on the selected channel; dual watch goes on after |
| **Up / Down**, the knob | The selected channel moves through its zone (skipping the other one); on the VFO, a step |
| **Left / Right** (DM-1801 Pro, DM-32UV) | Selects A / B (a press; hold is the radio's user function) |
| **A/B** key | Selects the other channel |
| **#** | Dual watch actions: **1** Talk on B (or A), **2** Change A, **3** Change B, **4** Caller (the last caller and how long ago; OK opens their actions; dim until a call was heard), **5** Stop, stay on A (or B). Change A and B open the picker |
| **\*** | Quick settings of the selected channel |
| **Back** | Stops, staying on the selected channel |

<img src="screens/160x120i5/dual_actions.png" width="200" alt="Dual watch actions">

### 4.2 Talk back

**Settings › Talk › Dual watch talk back** (off by default). Off, a call on the other channel never moves PTT: "# 1
talk on B" is one press away. On, when a call ends on the other channel, that channel is selected ("Talk back on B")
so PTT answers there. The time starts over with each reply and each call there; 10 s after the last, the selection goes
back ("Talking on A"). **#** **1**, the A/B key or Left / Right go back at once and end it.

<img src="screens/240x320/home_dual_talk_back.png" width="200" alt="Talk back on B">

### 4.1 Per radio

SK2 held is Dual watch on every radio, and the DM-1801 Pro's A/B key is A/B (press) and Dual watch (hold). Every side
key can take A/B or Dual watch in Settings › Keys (press or hold).

| Radio | Switch A / B | Start / stop |
|---|---|---|
| DM-1701 | `#` **1**, or a side key set to A/B | `*` `7`, or hold SK2 |
| DM-1801 Pro | The **A/B** key, **Left** / **Right** | Hold **A/B**, `*` `7`, or hold SK2 (zones: hold Left / Right, or Zones) |
| DM-32UV | **Left** / **Right** | `*` `7`, or hold SK2 |
| MD-380 | `#` **1**, or a side key set to A/B | `*` `7`, or hold SK2 |
| DB25-D PRO (no number keys) | A side key set to A/B | Hold the tone key, or a side key set to Dual watch |
| Radios planned (GD-77 family, MD-UV380, MD-9600) | Not decided: A/B on a side key or mic key | |

## 5. Rules

- Dual watch is a kind of scan to the radio: starting it stops a zone scan, and a zone scan (Tools › Scan, the Scan
  key) stops it.
- If the radio ends the watch (hotspot mode, a channel change from the debug link), Home says "Dual watch stopped".
- A channel picked for A or B can't be the other one's.
- Changing A or B clears the calls listed for it (they were on the old channel).
- The last B is kept across power off (its channel number, or the VFO; settings-store.md 3), so `*` `7` starts with it
  at once. If its zone isn't known any more, the first zone holding that channel is used (A's zone first); a channel
  gone from the codeplug means the picker. The radio powers up on the selected channel, even when it was listening to
  the other one at the time.
- PTT is moved to the other channel by a call there only with talk back on (section 4.2), and only for its 10 s.

## 6. Press budgets (uiFlowTest)

| Task | Presses |
|---|---|
| Start with the last B (also after power off) | 2 (`* 7`), 1 (hold SK2) |
| Start, B from this zone by number | 4 (`* 7 5 OK`) |
| Start, B on the VFO | 6 |
| Start, B in another zone | 7 |
| Talk on B | 1 (A/B key, Right) or 2 (`# 1`) |
| Stop | 1 (Back, hold SK2), 2 (`* 7`) |
| Zone scan | 3 (Tools › Scan: `OK 6 1`; was 2 with `* 7`) |
| Talk back on | 10 (Settings › Talk) |

## 7. Decisions

Asked in this design's first round and decided by the owner (2026-10-21):

1. Tile 7 is Dual watch; a zone scan is in Tools › Scan only.
2. SK2 held is Dual watch (it was Scan).
3. A call on B keeps PTT on A by default; talk back (section 4.2) is a setting.
4. The last B is kept across power off.
5. The DM-1801 Pro's A/B key is A/B on a press and Dual watch on a hold (it was Next zone).
