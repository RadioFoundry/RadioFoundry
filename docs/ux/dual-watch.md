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
  the knob, digits and Quick settings act on it, as they act on the one channel of the normal Home.
- A radio with one receiver (every radio today) alternates between A and B, so a call on one can be missed while it
  is on the other. Home says so ("Alternating A and B"). A radio with two receivers hears both ("Listening to both");
  that is what `ui_svc_receivers()` > 1 is for. No radio reports two yet. The AT-D578UV, which has two receiver chips
  ([hardware-anytone.md](../spec/hardware-anytone.md)), would be the first.
- The UI takes any channel of any zone or the VFO. Today's radio core alternates only between two channels of **one
  zone**: for any other B, `ui_svc_dual` refuses and the radio shows "This radio watches 2 channels of one zone: Pick
  B from this zone". Other zones and the VFO need the receive-set model of #96 in the core. The screens, keys and
  gallery below show the finished UI.

## 2. Turning it on and off

| How | Does |
|---|---|
| **\***, **7** (Quick tile Scan), or Tools › Scan | Opens **Scan**: **1** Dual watch (with the last B: "+ Metro DMR", or "+ VFO"; the first time "Pick B"), **2** Scan this zone ("Scan the band" in the VFO). Tile 7 says "Dual" while it runs, and Tools › Scan says "Dual watch" |
| **\***, **7**, **1** | Dual watch with the last B (3 presses). The first time: the B picker |
| **#** on the Scan screen's dual-watch row | Pick another B |
| **A/B** key (a side key function) | Off: starts dual watch with the last B (or the picker). On: selects the other channel |
| **Dual watch** key (a side key function) | On (last B, or the picker) / off |
| **Back** on Home | Off, staying on the selected channel (a banner or VFO entry goes first, as on the normal Home) |
| **\***, **7** or Tools › Scan while it runs, **Scan** key, long SK2 | Off, staying on the selected channel (they do not start a zone scan) |

<img src="screens/160x120i5/dual_chooser.png" width="200" alt="Scan: dual watch or the zone">
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
| Tag | The letter in a box as tall as the pane |
| 1 | Channel number and name (the VFO: its frequency); at the right, the state pill |
| 2 | What PTT talks to (DMR: "TG 3100 USA"), or the frequency (FM); while a call comes in, the caller and the signal in dBm |
| 3 | Frequency, timeslot and color code, or tone and width (where there is room) |
| 4 | The zone (where there is room: B may be in another zone than A). Where row 3 has room, the zone sits at its right instead |

**Which channel PTT uses** (asked for: "easy to tell which one transmits"): the selected pane's tag is filled and its
pill says **PTT**. The other pane has no pill. While transmitting, the tag and pill turn red and say **TX**, and the line under the panes says
"TALKING on A" with the time.

**Where a call is coming in**: that pane's tag and text turn green, its pill says **RX**, row 2 names the caller, and
the line under the panes says "Receiving on B" with the meter. The other pane dims.

**Where the last call came in**: under the panes, the last calls, newest first, each with its pane's letter: "B Joe
Smith TG 91 10s ago". On displays without room for the list (128x64), the pane the last call came in on says how long
ago in amber: "Heard 7s ago" in its first row where it fits, else "7s ago" at the right of its second row. For 10 s
after a call on the other channel, the hint row says "# 1 talk on B  Back stop"; otherwise "# options  Back stop".

The status strip shows **DUAL** (amber) and the selected channel's mode (the radio's own mode alternates). Every color comes with a word (P5).

**Large text** (Settings › Display): one line per channel, "A COUNTY DMR" / "B Metro DMR", as large as fits, the
selected one marked with a bar, green while a call comes in on it.

### 3.1 Per display class

The layout picks the richest set of rows that fits, never a smaller font for a row than the normal Home uses for it,
and both names in one font (the largest both fit, never smaller than row 2; a longer name is shortened):

| Class | Rows per pane | Under the panes |
|---|---|---|
| 240x320, 240x320i10x5 (DM-32UV) | name (medium or large), target (medium), details, zone | state and the last calls, as many as fit |
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
| **#** | Dual watch actions: **1** Talk on B (or A), **2** Change A, **3** Change B, **4** Caller (the last caller's actions; dim until a call was heard), **5** Stop, stay on A (or B). Change A and B open the picker |
| **\*** | Quick settings of the selected channel |
| **Back** | Stops, staying on the selected channel |

<img src="screens/160x120i5/dual_actions.png" width="200" alt="Dual watch actions">

### 4.1 Per radio

No board assigns A/B or Dual watch by default. Every side key can take either one in Settings › Keys (press or
hold). What each radio has without that:

| Radio | Switch A / B | Start |
|---|---|---|
| DM-1701 | `#` **1**, or a side key set to A/B | `*` `7` `1` |
| DM-1801 Pro | **Left** / **Right** (the A/B key is Next zone by default; it can be reassigned) | `*` `7` `1` |
| DM-32UV | **Left** / **Right** | `*` `7` `1` |
| MD-380 | `#` **1**, or a side key set to A/B | `*` `7` `1` |
| DB25-D PRO (no number keys) | A side key set to A/B | A side key set to Dual watch or A/B, or Tools › Scan with the knob |
| Radios planned (GD-77 family, MD-UV380, MD-9600) | Not decided: A/B on a side key or mic key | |

Open question 2 asks whether SK2 long (Scan today) should become Dual watch on the DM-1701 class.

## 5. Rules

- Dual watch is a kind of scan to the radio: starting it stops a zone scan, and while it runs the Scan key (or
  `*` `7`, or Tools › Scan) stops it instead of starting a zone scan.
- If the radio ends the watch (hotspot mode, a channel change from the debug link), Home says "Dual watch stopped".
- Today's radio core ends the watch when you transmit; the UI starts it again after the transmission. #96 moves
  this into the core.
- A channel picked for A or B can't be the other one's.
- Changing A or B clears the calls listed for it (they were on the old channel).
- The last B is remembered until power off; the Scan screen offers it.
- PTT is never moved to the other channel by a call there: "# 1 talk on B" is one press away, and the
  selection stays where you put it.

## 6. Press budgets (uiFlowTest)

| Task | Presses |
|---|---|
| Start with the last B | 3 (`* 7 1`) |
| Start, B from this zone by number | 5 (`* 7 1 5 OK`) |
| Start, B on the VFO | 7 |
| Start, B in another zone | 8 |
| Talk on B | 1 (A/B key, Right) or 2 (`# 1`) |
| Stop | 1 (Back), 2 (`* 7`) |
| Zone scan | 3 (`* 7 2`; was 2) |

## 7. Open questions

1. A zone scan costs one more press (`* 7 2`): is the Scan screen worth it, or should Dual watch get its own tile?
2. SK2's default on the DM-1701 class: keep Scan on hold, or make it Dual watch?
3. Should a call on B keep the selection on A (today), or offer a timed "talk back on B" like the reply window?
4. Should the last B be kept in the codeplug across power off?
