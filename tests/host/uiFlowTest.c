// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The automated UI checks of docs/ux/usability-tests.md 2, in the host simulator:
// - press budgets: each task flow of docs/ux/task-flows.md as a script on the reference key maps, at most its budget
//   (impossible on a key map that should have it = failure);
// - operate only: flows that only operate the radio never write the codeplug (P7);
// - crawler: every screen reachable from Home is left by BACK, long BACK reaches Home, field-use screens are at most
//   3 levels deep, the status strip (or the title row that carries the state pill) is drawn, PTT shows TX (P2, P4);
// - long presses keep their one meaning on every screen: long BACK Home, long QUICK lock, long CONTEXT the talk target picker on DMR (P3);
// - the Transmitting screen names a private call's contact and tells it apart from a group call (P2).
#include <stdio.h>
#include <string.h>
#include "sim/ui_sim.h"
#include "ui/ui_app.h"
#include "ui/ui_screens.h"

static int fails, flows_run;

typedef struct {
	const char *name;
	const char *setup;	// not counted
	const char *script;	// counted
	const char *expect;	// checks after the script (expect: tokens)
	int budget;
	bool writes;		// the flow saves on purpose (else: zero codeplug writes)
} flow_t;

#define CALLER	"call:3101234,91,g wait:11000 "
#define TALK_BACK	"ok 7 3 down down down down down down down down down ok long:back quick 7 ctx 3 5 ok "	// on, and A 3 / B 5

static const flow_t flows[] = {
	{ "1 next channel", "", "up", "pos=4", 1, false },
	{ "1 channel 12 by number", "", "1 2 ok", "pos=12 screen=home", 3, false },
	// The status strip's USB word (#58): IP as a network adapter, with the link arrows once the computer has it up,
	// none on serial
	{ "1 USB network: IP on the strip, no link arrows", "usbnet:on", "", "shows=IP nomarkafter=IP", 0, false },
	{ "1 USB network brought up by the PC: the link arrows", "usbnet:up", "", "shows=IP markafter=IP", 0, false },
	{ "1 USB serial again: no word", "usbnet:on", "usbnet:off", "hides=IP", 0, false },
	{ "1 USB network: the PC brings the link up, the arrows appear", "usbnet:on", "usbnet:up", "markafter=IP", 0, false },
	{ "1 USB network: the link goes down, the arrows go", "usbnet:up", "usbnet:on", "shows=IP nomarkafter=IP", 0, false },
	{ "1 USB network: SCAN wins the strip's word", "usbnet:on", "ok 6 1", "scanning=1 shows=SCAN hides=IP", 3, false },
	{ "1 USB network from Tools: IP", "", "ok 6 4 ok", "usbnet=1 shows=IP", 4, false },
	{ "2 zone 2 positions away", "", "ok 4 down down ok", "zone=2 screen=home", 5, false },
	{ "3 one-shot TG, 2nd in picker", "", "tt down ptt", "txid=91 txpriv=0", 3, false },
	{ "3 one-shot typed TG 3100", "", "3 1 0 0 ptt", "txid=3100 txpriv=0", 5, false },
	{ "3 permanent TG", "", "tt down long:ok", "talk=91 writes=1", 4, true },
	{ "4 reply in the window", "call:3101234,91,g", "ptt", "txid=91", 1, false },
	{ "4 call back after the window", CALLER, "ctx ptt", "txid=91", 2, false },
	{ "4 3rd station in Recent", "call:3101234,91,g call:3105678,3100,g call:3102222,93,g wait:11000",
	  "ok 3 down down ptt", "txid=91", 5, false },
	{ "5 private call by name", "", "5 6 3 ptt", "txid=3101234 txpriv=1", 4, false },
	{ "5 private call by ID", "", "3 1 0 9 9 9 9 down ptt", "txid=3109999 txpriv=1", 9, false },
	{ "6 canned reply to last caller", CALLER, "ctx 3 ok", "sent=1 text=OK", 4, false },
	{ "6 canned to contact by name", "", "5 6 3 ctx 3 ok", "sent=1 text=OK", 6, false },
	{ "7 read message, banner up", "msg:3101234,Meet_at_the_north_lot", "ctx", "screen=message", 1, false },
	{ "7 read message later", "msg:3101234,Meet_at_the_north_lot wait:9000", "ok 2 ok", "screen=message", 3, false },
	{ "8 radio check last caller", CALLER, "ctx 4", "checks=1", 2, false },
	{ "8 radio check by name", "", "5 6 3 ctx 4", "checks=1", 5, false },
	{ "9 power one step", "", "quick 1 back", "power=8 screen=home", 3, false },
	{ "9 squelch 2 steps (FM)", "down", "quick 2 up up back", "sq=7 screen=home", 5, false },
	{ "10 privacy", "", "quick 6 back", "privacy=1 screen=home", 3, false },
	{ "11 scan start (Tools > Scan)", "", "ok 6 1", "scanning=1", 3, false },
	{ "11 nuisance skip", "ok 6 1", "ctx 1", "pos=4 scanning=1", 2, false },
	{ "11 scan stop", "ok 6 1", "back", "scanning=0", 1, false },
	// Dual watch (docs/ux/dual-watch.md): on in a few presses, PTT on the selected channel, the other one press away
	{ "11 dual watch: start, B the next channel", "", "quick 7", "dual=1 sel=A pos=3 bpos=4 screen=home", 2, false },
	{ "11 dual watch: start, then B by number", "", "quick 7 ctx 3 5 ok", "dual=1 sel=A pos=3 bpos=5 screen=home", 6, false },
	{ "11 dual watch: again with the last B", "quick 7 ctx 3 5 ok back back", "quick 7", "dual=1 sel=A", 2, false },
	{ "11 dual watch: B in another zone", "", "quick 7 ctx 1 zone+", "dual=1 sel=B bzone=1 screen=home", 5, false },
	{ "11 dual watch: B on the VFO", "", "quick 7 ctx 1 vfokey", "dual=1 bvfo=1 sel=B", 5, false },
	{ "11 dual watch: talk on B from #", "quick 7 ctx 3 5 ok", "ctx 1", "dual=1 sel=B uipos=5", 2, false },
	{ "11 dual watch: the A/B key", "quick 7 ctx 3 5 ok", "abkey", "dual=1 sel=B", 1, false },
	{ "11 dual watch: PTT keeps it on", "quick 7 ctx 3 5 ok ctx 1", "pttdown wait:1000 pttup wait:1000", "dual=1 sel=B pos=5", 2, false },
	{ "11 dual watch: Up moves the selected", "quick 7 ctx 3 5 ok", "up", "dual=1 sel=A pos=4", 1, false },
	{ "11 dual watch: stop stays on B", "quick 7 ctx 3 5 ok ctx 1", "back back", "dual=0 pos=5", 2, false },
	// One Back press says how to stop it; it goes on. A second one later is a first press again
	{ "11 dual watch: one Back keeps it", "quick 7 ctx 3 5 ok", "back", "dual=1 toast=Press_Back_again", 1, false },
	{ "11 dual watch: Back, later Back", "quick 7 ctx 3 5 ok", "back wait:2500 back", "dual=1 toast=Press_Back_again", 2,
	  false },
	// Back clears the selected channel's changes first, then asks
	{ "11 dual watch: Back undoes changes first", "quick 7 ctx 3 5 ok quick 2 back", "back", "dual=1 temporary=0 toast=Changes_undone",
	  1, false },
	{ "11 dual watch: quick 7 stops it", "quick 7 ctx 3 5 ok", "quick 7", "dual=0 pos=3", 2, false },
	{ "11 dual watch: a quick setting keeps it on", "down quick 7 ctx 3 5 ok", "quick 8 back", "dual=1 sel=A", 3, false },
	{ "11 dual watch, two receivers: a call on each", "receivers:2 quick 7 ctx 3 5 ok",
	  "rxa:3101234,91,g wait:300 rxb:3107777,3100,g wait:300", "dual=1 sel=A rx=A calls=0", 0, false },
	{ "11 dual watch, two receivers: A's call ends", "receivers:2 quick 7 ctx 3 5 ok",
	  "rxa:3101234,91,g wait:300 rxb:3107777,3100,g wait:300 rxaend wait:300", "dual=1 rx=B calls=1 last=A", 0, false },
	{ "11 dual watch, two receivers: talk on B", "receivers:2 quick 7 ctx 3 5 ok rxb:3107777,3100,g wait:300 rxbend",
	  "ctx 1", "dual=1 sel=B last=B uipos=5", 2, false },
	{ "11 dual watch: long SK2", "quick 7 ctx 3 5 ok back back", "long:user2", "dual=1 sel=A bpos=5", 1, false },
	{ "11 dual watch: Tools > Scan, a zone scan instead", "quick 7 ctx 3 5 ok", "ok 6 1", "dual=0 scanning=1", 3, false },
	{ "11 dual watch: the last B after power off", "quick 7 ctx 3 5 ok back back reboot", "quick 7", "dual=1 sel=A bpos=5",
	  2, false },
	{ "11 dual watch talk back on", "", "ok 7 3 down down down down down down down down down ok", "shows=Dual_watch_talk_back", 13,
	  false },
	{ "11 dual watch talk back: a call on B", TALK_BACK, "tune:b call:3101234,91,g wait:4000", "dual=1 sel=B", 0,
	  false },
	{ "11 dual watch talk back: then back on A", TALK_BACK, "tune:b call:3101234,91,g wait:4000 wait:11000",
	  "dual=1 sel=A", 0, false },
	{ "11 dual watch talk back: # 1 goes back at once", TALK_BACK "tune:b call:3101234,91,g wait:4000", "ctx 1",
	  "dual=1 sel=A", 2, false },
	// A reply target lapses with its window: B's pane shows B's own talk group once the receiver has gone back to A
	{ "11 dual watch: no stale reply target on B", "quick 7 ctx 3 5 ok wait:3000",
	  "tune:b call:3101234,91,g tune:a call:3107777,3100,g wait:4000", "dual=1 sel=A hides=World", 0, false },
	// B on the VFO, selected before the receiver has been there (a new selection doesn't retune): digits are a frequency
	{ "11 dual watch: digits on B the VFO, receiver on A", "quick 7 ctx 1 vfokey ctx 1 wait:3000 tune:a ctx 1", "4 4",
	  "dual=1 sel=B entry=44", 2, false },
	// Stopped with B selected, the radio stays on B: the next start keeps the pair (A the channel it is on)
	{ "11 dual watch: again after a stop on B", "quick 7 ctx 1 back back", "quick 7", "dual=1 sel=A apos=4 bpos=3", 2,
	  false },
	{ "11 dual watch: again after a stop on B and power off", "quick 7 ctx 1 back back reboot", "quick 7",
	  "dual=1 sel=A apos=4 bpos=3", 2, false },
	{ "11 dual watch: again on the last B's channel", "quick 7 back back up", "quick 7", "dual=1 sel=A apos=4 bpos=3", 2,
	  false },
	{ "11 dual watch key on and off", "quick 7 ctx 3 5 ok back back", "dualkey", "dual=1", 1, false },
	// Changes and requests act on the selected channel wherever the receiver is, and stay with it over hops
	{ "11 dual watch: power goes to the selected", "quick 7 ctx 3 5 ok tune:b", "quick 2 back", "dual=1 sel=A pos=3 temporary=1",
	  3, false },
	{ "11 dual watch: the screens show the selected", "quick 7 ctx 3 5 ok quick 2 back tune:b", "",
	  "dual=1 pos=5 temporary=0 uipos=3 uitemp=1", 0, false },
	{ "11 dual watch: changes kept over hops", "quick 7 ctx 3 5 ok quick 2 back", "tune:b tune:a", "pos=3 temporary=1", 0,
	  false },
	{ "11 dual watch: talk target for the selected", "quick 7 tune:b", "tt", "dual=1 screen=picker", 1, false },
	{ "11 dual watch: VFO entry over a hop", "quick 7 ctx 1 vfokey ctx 1 ctx 1", "1 4 6 tune:a 5", "dual=1 sel=B entry=1465",
	  4, false },
	// Up / Down with nothing to step to says why
	{ "11 dual watch: one channel in the zone", "zonesize:1,1 quick 7 ctx 1 zone+", "up",
	  "dual=1 sel=B toast=Only_one_channel", 1, false },
	{ "11 dual watch: the zone's other channel is A", "zonesize:0,2 quick 7 ctx 1", "up",
	  "dual=1 sel=B apos=1 bpos=2 toast=No_other_channel_here", 1, false },
	// Nothing ends it but the user: a channel change from elsewhere becomes the selected channel or selects the other
	{ "11 dual watch: a channel from elsewhere", "quick 7 ctx 3 5 ok", "dlpos:7 wait:1000", "dual=1 sel=A apos=7 bpos=5",
	  0, false },
	{ "11 dual watch: the other channel from elsewhere", "quick 7 ctx 3 5 ok", "dlpos:5 wait:1000",
	  "dual=1 sel=B apos=3 bpos=5 toast=Talking_on_B", 0, false },
	// The debug link: its stop is the user's (the watch stays off); A or B selected there is followed, and a channel it
	// sets after that goes to that one
	{ "11 dual watch: started by the debug link", "", "dldual:5 wait:1000", "dual=1 sel=A bpos=5 toast=Dual_watch_on", 0,
	  false },
	{ "11 dual watch: started by the debug link, a channel for B", "dldual:5 wait:1000", "dlsel:b wait:200 dlpos:7 wait:1000",
	  "dual=1 sel=B bpos=7", 0, false },
	{ "11 dual watch: started by the debug link, then its stop", "dldual:5 wait:1000", "dlstop wait:1500", "dual=0", 0,
	  false },
	{ "11 dual watch: stopped by the debug link", "quick 7 ctx 3 5 ok", "dlstop wait:1500",
	  "dual=0 pos=3 toast=Dual_watch_off:_on_A", 0, false },
	{ "11 dual watch: stopped by the debug link on B", "quick 7 ctx 3 5 ok ctx 1", "dlstop wait:1500",
	  "dual=0 pos=5 toast=Dual_watch_off:_on_B", 0, false },
	{ "11 dual watch: B selected by the debug link", "quick 7 ctx 3 5 ok", "dlsel:b wait:500",
	  "dual=1 sel=B toast=Talking_on_B", 0, false },
	{ "11 dual watch: B selected and a channel at once", "quick 7 ctx 3 5 ok", "dlsel:b dlpos:7 wait:1000",
	  "dual=1 sel=B apos=3 bpos=7", 0, false },
	{ "11 dual watch: a channel for B from the debug link", "quick 7 ctx 3 5 ok", "dlsel:b wait:200 dlpos:7 wait:1000",
	  "dual=1 sel=B apos=3 bpos=7", 0, false },
	{ "11 dual watch: Zones menu sets the selected", "quick 7 ctx 3 5 ok", "ok 4 down ok wait:1000",
	  "dual=1 sel=A azone=1 apos=1 bzone=0 bpos=5", 4, false },
	{ "11 dual watch: after hotspot mode", "quick 7 ctx 3 5 ok", "hotspot:on wait:1000 hotspot:off wait:1000",
	  "dual=1 sel=A apos=3 bpos=5", 0, false },
	{ "11 dual watch: a codeplug write", "quick 7 ctx 3 5 ok", "cpwrite wait:1000", "dual=1 sel=A apos=4 bpos=6 pos=4", 0,
	  false },
	{ "11 dual watch: B deleted", "quick 7 ctx 3 5 ok", "cpdel:5 wait:1000", "dual=0 pos=3 toast=Dual_watch_off:_on_COUNTY",
	  0, false },
	// It comes back at power on, as it was
	{ "11 dual watch: back after power off", "quick 7 ctx 3 5 ok reboot wait:1000", "", "dual=1 sel=A apos=3 bpos=5", 0,
	  false },
	{ "11 dual watch: back with B selected", "quick 7 ctx 3 5 ok ctx 1 reboot wait:1000", "", "dual=1 sel=B apos=3 bpos=5",
	  0, false },
	{ "11 dual watch: off stays off", "quick 7 ctx 3 5 ok back back reboot wait:1000", "", "dual=0", 0, false },
	// The VFO key and Copy to VFO act on the selected channel
	{ "11 dual watch: VFO key", "quick 7 ctx 3 5 ok", "vfokey", "dual=1 sel=A avfo=1 bpos=5", 1, false },
	{ "11 dual watch: VFO key back", "quick 7 ctx 3 5 ok vfokey", "vfokey", "dual=1 sel=A avfo=0 apos=3 bpos=5", 1, false },
	// Each channel has its own VFO: with B on one, the VFO key takes A to the other (apos / bpos: which VFO, 1 or 2)
	{ "11 dual watch: VFO key, B the VFO", "quick 7 ctx 1 vfokey ctx 1", "vfokey",
	  "dual=1 sel=A avfo=1 apos=2 bvfo=1 bpos=1", 1, false },
	{ "11 dual watch: VFO key, B the VFO, and back", "quick 7 ctx 1 vfokey ctx 1 vfokey", "vfokey",
	  "dual=1 sel=A avfo=0 apos=3 bvfo=1 bpos=1", 1, false },
	{ "11 dual watch: Copy to VFO", "quick 7 ctx 3 5 ok", "quick 4 wait:1000", "dual=1 sel=A avfo=1 bpos=5", 2, false },
	{ "11 dual watch: Copy to VFO, B the VFO", "quick 7 ctx 1 vfokey ctx 1", "quick 4 wait:1000",
	  "dual=1 sel=A avfo=1 apos=2 bvfo=1 bpos=1", 2, false },
	// The A/B key only switches; off, it says how to start
	{ "11 dual watch: A/B with the watch off", "", "abkey", "dual=0 pos=3 toast=Turn_on_dual_watch_first", 1, false },
	// The zone keys move the selected channel to another zone; the watch goes on
	{ "11 dual watch: next zone moves A", "quick 7 ctx 3 5 ok", "zone+", "dual=1 sel=A azone=1 apos=1 bzone=0 bpos=5 toast=A_zone:_Simplex", 1, false },
	{ "11 dual watch: previous zone moves B", "quick 7 ctx 3 5 ok ctx 1", "zone-", "dual=1 sel=B bzone=2 bpos=1 azone=0 apos=3", 1, false },
	{ "11 dual watch: home zone", "quick 7 ctx 3 5 ok zone+ zone+", "homekey", "dual=1 azone=0 apos=1 bzone=0 bpos=5", 1, false },
	// The first channel of the zone is the other pane's: the next one
	{ "11 dual watch: the zone's first channel is A", "down down quick 7 ctx 3 5 ok ctx 1 zone+", "homekey",
	  "dual=1 sel=B azone=0 apos=1 bzone=0 bpos=2", 1, false },
	{ "11 dual watch: the zone key on the VFO", "quick 7 ctx 1 vfokey ctx 1 ctx 1", "zone+", "dual=1 sel=B bvfo=0 bzone=0 bpos=1", 1, false },
	{ "11 dual watch: Up/Down on zone moves the selected", "ok 7 8 down down ok long:back quick 7 ctx 3 5 ok", "up",
	  "dual=1 azone=1 bzone=0", 1, false },
	// task-flows.md 12 says 3: the VFO row is first but Channels opens on the current zone (flow 2), so one Up
	{ "C channels: every channel", "", "ok 5", "screen=channels focus=3_", 2, false },
	{ "C a channel's settings", "", "ok 5 ok", "screen=channel", 3, false },
	// The channel's last rows are Go to it and Save as new channel: two Ups from the first row (lists wrap)
	{ "C go to it from its settings", "ok 5 down down ok", "up up ok", "screen=home pos=5", 3, false },
	// task-flows.md 21 and 22 (ui.md 2.1). On channel 3 (DMR) the timeslot is the ninth row, the TX contact the tenth
	{ "21 timeslot of the channel in use, saved", "", "ok 5 ok down down down down down down down down ok long:ok", "writes=1 toast=Saved_channel_3",
	  13, true },
	{ "21 TX contact opens the picker", "ok 5 ok down down down down down down down down down", "ok", "screen=chpick", 1, false },
	{ "21 a change asks before discarding", "ok 5 ok down down down down down down down down ok", "back", "screen=discard", 1, false },
	{ "21 keep editing", "ok 5 ok down down down down down down down down ok back", "ok", "screen=channel", 1, false },
	{ "21 discard goes back to Channels", "ok 5 ok down down down down down down down down ok back", "down ok", "screen=channels", 2, false },
	{ "21 the name opens text entry", "ok 5 ok", "ok", "screen=text", 1, false },
	// Back deletes in text entry, and leaves once the field is empty (principles 4's one exception)
	{ "21 Back deletes in text entry", "ok 5 ok ok 4 6", "back", "screen=text focus=COUNTY_DMRG", 1, false },
	{ "21 Back on an empty name leaves", "ok 5 ok ok", "back back back back back back back back back back back",
	  "screen=channel", 11, false },
	// Text entry (ui.md 2.1): * deletes, multi-tap letters move on with another key, # cycles ABC -> 123 -> abc
	{ "21 * deletes the name", "ok 5 ok ok", "quick quick quick quick quick quick quick quick quick quick long:7", "focus=7 screen=text", 11, false },
	{ "21 a name by multi-tap", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "9 2 2 2 7 7 7 2", "focus=WCRA", 8, false },
	{ "21 # to digits", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "ctx 3 1 3", "focus=313", 4, false },
	{ "21 # twice to lower case", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "ctx ctx 4 4 4", "focus=i", 5, false },
	{ "21 more presses: digit, then lower case", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "2 2 2 2 2", "focus=a", 5, false },
	{ "21 1 cycles punctuation", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "1 1 1 1", "focus=,", 4, false },
	{ "21 a slow * deletes, no key lock", "ok 5 ok ok 4 6", "long:quick", "focus=COUNTY_DMRG locked=0", 1, false },
	{ "21 a held digit types itself", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "long:7", "focus=7", 1, false },
	{ "22 new from current settings, saved", "", "ok 5 up up up ok long:ok",
	  "writes=1 toast=Saved_as_channel_41", 7, true },
	// The channel editor's rules (ui.md 2.1). Channel 1 is an FM repeater (146.940, TX 600 kHz below), channel 3 DMR
	// simplex; on channel 3 the own ID is row 12 and the TX time-out row 17
	{ "E new RX keeps the repeater offset", "ok 5 up up ok down down down", "ok 1 4 7 0 0 ok",
	  "shows=147.000 shows=146.4 shows=600_kHz", 7, false },
	{ "E new RX: an offset off the bands makes it simplex", "ok 5 up up ok down down down", "ok 1 3 6 0 0 ok",
	  "shows=136.000 hides=135.4 hides=-600", 7, false },
	{ "E RX refuses a digit no band starts with", "ok 5 up up ok down down down", "ok 2", "toast=Bands:", 2, false },
	{ "E RX needs the MHz digits", "ok 5 up up ok down down down", "ok 1 4 ok", "toast=Type_at_least screen=channel",
	  4, false },
	{ "E own ID refuses a digit past 16777215", "ok 5 ok down down down down down down down down down down down",
	  "ok 1 6 7 7 7 2 1 6", "toast=IDs_go_up shows=1677721", 9, false },
	{ "E own ID: Back takes back a digit", "ok 5 ok down down down down down down down down down down down",
	  "ok 5 5 5 1 back ok", "shows=555 hides=5551", 7, false },
	{ "E own ID: at most 8 digits", "ok 5 ok down down down down down down down down down down down",
	  "ok 0 0 0 0 0 0 0 0 0 0", "shows=00000000 hides=000000000", 11, false },
	{ "E time-out in steps of 15", "ok 5 ok down down down down down down down down down down down down down down down "
	  "down", "ok 7 ok", "toast=Not_changed", 3, false },
	{ "E time-out spinner", "ok 5 ok down down down down down down down down down down down down down down down down",
	  "ok up up ok", "shows=30_s", 4, false },
#if CONFIG_SUBTONE
	// Channel 1's RX tone is row 9 (none), its TX tone row 10 (100.0): the spinner walks the standard tone list
	{ "E RX tone spinner from none", "ok 5 up up ok down down down down down down down down", "ok up ok",
	  "shows=67.0", 3, false },
	{ "E TX tone spinner, Up", "ok 5 up up ok down down down down down down down down down", "ok up ok",
	  "shows=103.5 hides=100.0", 3, false },
	{ "E TX tone spinner, Down", "ok 5 up up ok down down down down down down down down down", "ok down ok",
	  "shows=97.4", 3, false },
#endif
	{ "E DMR needs a TX contact to save", "ok 5 ok down down down down down down down down down", "ok up ok long:ok",
	  "toast=Pick_a_TX writes=0", 4, false },
	{ "E a name to save", "ok 5 ok ok quick quick quick quick quick quick quick quick quick quick", "ok long:ok",
	  "toast=Give_it_a_name writes=0", 2, false },
	{ "E nothing changed to save", "ok 5 ok", "long:ok", "toast=No_changes writes=0", 1, false },
	{ "E go to it asks for a save first", "ok 5 ok down down ok", "up up up up up ok", "toast=Save_first screen=channel",
	  6, false },
	{ "E saved: Back leaves without asking", "ok 5 ok down down ok", "long:ok back", "screen=channels writes=1", 2,
	  true },
	{ "E save as new takes the next free number", "ok 5 ok up ok", "ok", "toast=Saved_as_channel_42 writes=2", 1, true },
	{ "E the new channel is in the list", "ok 5 up up up ok long:ok", "back up", "focus=41_COUNTY_DMR_2", 2, true },
	{ "E new from the VFO is named by its frequency", "ok 4 up ok", "ok 5 ok", "shows=146.520 shows=41", 3, false },
	{ "C go to it from the list", "ok 5 down", "ctx", "screen=home pos=4", 1, false },
	{ "12 into the VFO", "", "ok 4 up ok", "vfo=1 screen=home", 4, false },
	{ "12 446.000 in the VFO", "ok 4 up ok", "4 4 6 0 0 0 ok", "freq=446000000", 7, false },
	{ "13 lock", "", "long:quick", "locked=1", 1, false },
	{ "13 unlock", "long:quick", "long:quick", "locked=0", 1, false },
	{ "14 GPS details", "", "ok 6 2", "screen=info", 3, false },
	{ "15 hotspot page", "", "ok 6 3", "screen=info", 4, false },
	{ "15 stop hotspot", "hotspot:on", "ok", "hotspot=0", 2, false },
	// Tile 4 on a channel copies it to the VFO (docs/spec/dmr-voice.md 2.2); the VFO's own tile 4 stays Mode
	{ "18 Copy to VFO from a DMR channel", "", "quick 4", "vfo=1 mode=dmr screen=home toast=Copied_to_VFO temporary=0",
	  2, false },
	{ "18 Copy to VFO from an FM channel", "down", "quick 4", "vfo=1 mode=fm freq=147060000 toast=Copied_to_VFO", 2,
	  false },
	{ "18 Copy to VFO: a temporary color code goes along", "quick 3 7 ok back", "quick 4", "vfo=1 cc=7 temporary=0",
	  2, false },
	{ "18 Copy to VFO: the talk target goes along", "tt down ok", "quick 4", "vfo=1 talk=91 temporary=0", 2,
	  false },
	{ "18 Copy to VFO: the channel is unchanged", "quick 3 7 ok back quick 4", "ok 4 ok", "vfo=0 cc=1 temporary=0",
	  3, false },
	{ "18 Copy to VFO writes no codeplug", "quick 3 7 ok back", "quick 4", "writes=0", 2, false },
	{ "19 hear all latched", "", "quick 5", "hearall=1", 2, false },
	{ "19 hear all off on BACK", "quick 5 back", "back", "hearall=0", 1, false },
	{ "19 hear all ends with the channel", "quick 5 back", "up", "hearall=0", 1, false },
	{ "21 VOX on (FM)", "down", "quick 6", "vox=1", 2, false },
	{ "21 VOX off again", "down quick 6", "6", "vox=0", 1, false },
	{ "21 monitor latch (FM)", "down", "quick 5", "monitor=1", 2, false },
	{ "3 focus follows the row", "", "tt down", "focusid=91", 2, false },
	{ "3 focus cleared on Home", "tt down", "long:back", "focusid=0", 1, false },
	{ "20 timeslot", "", "quick 2 back", "ts=2 screen=home", 3, false },
	{ "20 color code 7", "", "quick 3 7 ok", "cc=7", 4, false },
	{ "20 save to channel", "quick 2", "long:ok", "writes=1", 1, true },
	// Settings the radio core acts on reach it (ui_svc_apply), key presses ask for a key beep
	{ "S TX timeout to the radio", "", "ok 7 3 down ok", "radio_tot=300", 5, false },
	{ "S backlight time to the radio", "", "ok 7 2 down ok", "radio_backlight=60", 5, false },
	{ "S key beep", "", "up", "beeps=1", 1, false },
	{ "S long press time to input", "", "ok 7 8 ok up ok", "longpress=700", 6, false },
	{ "S auto lock after 30 s", "ok 7 4 down down ok long:back", "wait:31000", "locked=1", 0, false },
	{ "S auto lock waits for keys", "ok 7 4 down down ok long:back", "wait:20000 up wait:20000", "locked=0", 1, false },
	// DMR repeater roaming (docs/spec/dmr-roaming.md 6.2): Tools 8
	{ "R roaming on for this zone", "", "ok 6 6 1", "roamzone=1 roam=1 toast=Roaming_on_for", 4, false },
	{ "R roaming off again", "ok 6 6 1", "1", "roamzone=0 roam=0 toast=Roaming_off", 1, false },
	{ "R roam now", "ok 6 6 1 long:back", "ok 6 6 2", "roams=1 screen=home toast=Moved_to", 4, false },
	{ "R lock to this site", "ok 6 6 1", "3", "roam=3 toast=Locked_to", 1, false },
	{ "R roam now ends the lock", "ok 6 6 1 3", "2", "roam=1 roams=1", 1, false },
	{ "R not on the VFO", "ok 4 up ok", "ok 6 6 1", "roamzone=0 toast=Roaming_works", 4, false },
	// The survey and beacon rows (set33 survey, set34 beacon interval, set36 wake probe): radio-wide, OK cycles
	{ "R site survey per site", "", "ok 6 6 4", "set33=2", 4, false },
	{ "R beacon interval", "", "ok 6 6 5", "set34=120 toast=Must_match", 4, false },
	{ "R wake probe on", "", "ok 6 6 7", "set36=1 toast=Wake_probe_on", 4, false },
	{ "T22 Home and undo", "quick 2 back ok 6", "long:back back", "screen=home temporary=0", 2, false },
	// Settings that act now (uifix). set<N> = ui_setting_t N: 7 theme, 31 privacy key type, 32 privacy key
	{ "S PTT on when locked (default)", "long:quick", "ptt", "tx=1 pttblock=0", 1, false },
	{ "S PTT off when locked", "ok 7 4 down down down ok long:back long:quick", "ptt",
	  "tx=0 pttblock=1 toast=PTT_is_off", 1, false },
	{ "S PTT back with the unlock", "ok 7 4 down down down ok long:back long:quick", "long:quick ptt",
	  "tx=1 pttblock=0", 2, false },
	{ "S Up/Down on Home: zone", "ok 7 8 down down ok long:back", "up", "zone=1 screen=home", 1, false },
	{ "S Up/Down on Home: talk target", "ok 7 8 down down ok ok long:back", "up", "talk=91 toast=Talking_to", 1,
	  false },
	{ "S Up/Down: back to the default", "ok 7 8 down down ok ok long:back up", "down", "talk=3100", 1, false },
	{ "S hear all by default", "ok 7 3 down down down down down down down ok long:back", "up up", "hearall=1 pos=5", 2, false },
	{ "S hear all by default: BACK", "ok 7 3 down down down down down down down ok long:back up up", "back", "hearall=0", 1, false },
	{ "S mic noise reduction off", "", "ok 7 3 down down down down down down ok", "set58=0", 10, false },
	// Mic AGC on: Mic gain (the row above) shows "AGC", is dimmed and says why
	{ "S mic AGC on", "", "ok 7 3 down down down down down ok", "set59=1 shows=AGC", 9, false },
	{ "S mic gain while AGC is on", "ok 7 3 down down down down down ok", "up ok", "toast=Mic_AGC_sets set13=5", 2, false },
	{ "S message tone", "", "msg:3101234,Hi", "beep=2", 0, false },
	// Vibrate (Settings > Sound, radio-features.md 4.1): ui_vibe_t 0 message, 1 call, 2 emergency
	{ "S vibrate on a message", "", "msg:3101234,Hi", "vibes=1 vibe=0", 0, false },
	{ "S vibrate on a call alert", "", "alert:91", "vibes=1 vibe=1", 0, false },
	{ "S vibrate on a private call", "", "privcall:91", "vibes=1 vibe=1", 0, false },
	{ "S vibrate on an emergency", "", "emergency:91", "vibes=1 vibe=2", 0, false },
	{ "S vibrate off", "ok 7 1 down down down down down ok long:back", "msg:3101234,Hi", "vibes=0 beep=2", 0,
	  false },
	{ "S low battery warning", "", "battery:14", "banner=Battery_14%", 0, false },
	{ "S privacy key typed", "", "ok 7 4 ok down ok 1 2 3 ok", "set31=1 set32=123", 10, false },
	{ "S privacy key out of range", "ok 7 4 ok down", "ok 3 0 0 ok", "toast=Not_changed set32=0", 5, false },
	{ "S privacy key needs a type", "ok 7 4 down", "ok", "toast=Pick_a_key", 1, false },
	{ "S number typed", "", "ok 7 3 down down ok 3 ok", "sq=5 set12=3", 8, false },
	{ "S voice prompts not built", "ok 7 1 down down down", "ok", "toast=Voice_prompts", 1, false },
	{ "S mode tile in the VFO", "ok 4 up ok", "quick 4", "toast=VFO:_DMR mode=dmr temporary=0 vfo=1", 2, false },
	{ "S Copy to VFO key", "", "copykey", "vfo=1 screen=home toast=Copied_to_VFO", 1, false },
	{ "S Copy to VFO key, long press", "", "long:copykey", "vfo=1 toast=Copied_to_VFO", 1, false },
	{ "S Copy to VFO key in the VFO", "ok 4 up ok", "copykey", "vfo=1 toast=Already_in_VFO", 1, false },
	{ "S Copy to VFO: tile 4 in the VFO is Mode", "copykey", "quick 4", "toast=VFO:_FM mode=fm vfo=1", 2, false },
	{ "S VFO/MR key: channel to VFO and back", "", "vfokey vfokey", "vfo=0 toast=Back_to_channels", 2, false },
	{ "S VFO/MR key: into the VFO", "", "vfokey", "vfo=1", 1, false },
	// The VFO's step: Auto follows the width (12.5 kHz narrow, 25 kHz wide); one set in Settings > Talk stays
	{ "S VFO step: auto, wide", "vfokey", "up", "freq=146545000", 1, false },
	{ "S VFO step: auto, narrow", "vfokey quick 8 back", "up", "freq=146532500", 1, false },
	{ "S VFO step: 2.5 kHz", "ok 7 3 down down down ok long:back vfokey", "up", "freq=146522500", 1, false },
	// Quick settings in the FM VFO: tile 8 is Width, as on a channel
	{ "Q FM VFO: Width on 8", "vfokey", "quick 8", "toast=Bandwidth", 2, false },
	{ "S VFO mode back to FM", "ok 4 up ok quick 4 back", "quick 4", "toast=VFO:_FM mode=fm temporary=0", 2, false },
	{ "S VFO DMR: color code 7", "ok 4 up ok quick 4 back", "quick 3 7 ok", "cc=7 temporary=0", 4, false },
	{ "S VFO DMR: timeslot 2", "ok 4 up ok quick 4 back", "quick 2 back", "ts=2 temporary=0", 3, false },
	{ "S VFO DMR: talk to a typed TG", "ok 4 up ok quick 4 back", "quick 8 down ok 3 1 0 0 ok", "talk=3100 temporary=0",
	  9, false },
	{ "S VFO DMR: the mode stays across a channel", "ok 4 up ok quick 4 back ok 4 ok", "ok 4 up ok", "mode=dmr vfo=1", 4,
	  false },
	{ "S scan in the VFO: the band", "ok 4 up ok", "ok 6 1", "toast=Scanning_the_band scanning=1", 3, false },
	{ "S scan on a channel: the zone", "", "ok 6 1", "toast=Scanning_this_zone scanning=1", 3, false },
	{ "S USB network on", "", "ok 6 4 ok", "usbnet=1 toast=USB_is_a_network screen=home", 4, false },
	{ "S USB network off", "ok 6 4 ok", "ok 6 4 ok", "usbnet=0 toast=USB_is_a_serial", 4, false },
	{ "S message failed text", "", "", "screen=home", 0, false },
	// APRS (docs/spec/aprs.md 10): Tools 9, one row each
	{ "A APRS page", "", "ok 6 8", "screen=aprs", 3, false },
	{ "A send position now", "", "ok 6 8 1", "aprs_sent=1 toast=Sending_position", 4, false },
	{ "A send without a callsign", "aprs:nocall", "ok 6 8 1", "aprs_sent=0 toast=Set_your_callsign", 4, false },
	{ "A beacons on", "", "ok 6 8 2", "aprs_auto=1 toast=Beacons_on", 4, false },
	{ "A beacons off again", "ok 6 8 2", "2", "aprs_auto=0 toast=Beacons_off", 1, false },
	{ "A next region", "", "ok 6 8 3", "aprs_region=1", 4, false },
	{ "A regions skip an unset custom", "ok 6 8 3 3 3 3 3", "3", "aprs_region=0", 1, false },
	{ "A regions with a custom frequency", "aprs:custom ok 6 8 3 3 3 3 3", "3", "aprs_region=6", 1, false },
	{ "A power as the channel", "ok 6 8", "down down down", "shows=As_channel aprs_power=0", 3, false },
	{ "A power of its own", "", "ok 6 8 4", "aprs_power=1", 4, false },
	{ "A path shows the default", "ok 6 8", "down down down down", "shows=WIDE1-1,WIDE2-1 aprs_path=2", 4, false },
	{ "A path: the next choice", "", "ok 6 8 5", "aprs_path=3 shows=WIDE2-1", 4, false },
	{ "A path: WIDE2-2 then none", "ok 6 8 5 5", "5", "aprs_path=0 shows=None", 1, false },
	{ "A path wraps to WIDE1-1", "ok 6 8 5 5 5", "5", "aprs_path=1", 1, false },
	{ "A interval", "", "ok 6 8 6", "aprs_interval=60", 4, false },
	{ "A SmartBeaconing", "", "ok 6 8 7", "aprs_smart=1", 4, false },
	{ "A send via DMR", "", "ok 6 8 8", "aprs_via=1", 4, false },
	{ "A position page", "", "ok 6 8 9", "screen=aprs-pos", 4, false },
	{ "A position source", "ok 6 8 9", "1", "aprs_pos=1", 1, false },
	{ "A no fixed position yet", "", "ok 6 8 9", "shows=Not_set", 4, false },
	{ "A position page found by latitude", "", "5 2 8 4 8 8 3 3", "shows=APRS_position", 8, false },
	{ "A latitude while typed", "ok 6 8 9", "2 3 3 7 4", "shows=33.74 aprs_lat=0", 5, false },
	{ "A fixed latitude", "ok 6 8 9", "2 3 3 7 4 9 0 ok", "aprs_lat=337490000", 8, false },
	{ "A fixed longitude west", "ok 6 8 9", "3 0 8 4 3 8 8 0 down ok", "aprs_lon=-843880000", 10, false },
	{ "A longitude's leading zero by itself", "ok 6 8 9", "3 8 4 3 8 8 0 down ok", "aprs_lon=-843880000", 9, false },
	{ "A longitude of 100 or more", "ok 6 8 9", "3 1 2 2 5 ok", "aprs_lon=1225000000", 6, false },
	{ "A latitude digits not typed are zeros", "ok 6 8 9", "2 4 5 ok", "aprs_lat=450000000", 4, false },
	{ "A latitude past 90 refused", "ok 6 8 9", "2 9 5", "toast=Up_to_90", 3, false },
	{ "A latitude after a refused digit", "ok 6 8 9", "2 9 5 ok", "aprs_lat=900000000", 4, false },
	{ "A latitude BACK takes a digit back", "ok 6 8 9", "2 3 9 back 3 ok", "aprs_lat=330000000", 6, false },
	{ "A latitude BACK leaves unchanged", "ok 6 8 9", "2 1 back back", "aprs_lat=0 screen=aprs-pos", 4, false },
	{ "A position in DMR calls", "", "ok 6 8 0", "aprs_dmrgps=1 toast=Position_goes_out", 4, false },
	{ "A KISS on USB", "ok 6 8", "up up ok", "aprs_kiss=1", 3, false },
	{ "A stations heard", "", "ok 6 8 up ok", "screen=aprs-heard count=2", 5, false },
	{ "A not in this build", "aprs:off", "ok 6 8 2", "toast=APRS:_not aprs_auto=0", 4, false },
	// The crash log and its report (docs/spec/crash-log.md): Settings > Advanced > Crash log, after the restart
	{ "C crash log from Advanced", "crash:on", "ok 7 0 down down down down ok", "screen=crash shows=Fault_3 crash_seen=1",
	  8, false },
	{ "C crash: Off neither asks nor sends", "crash:on wait:2500", "", "screen=home crash_sends=0 crash_seen=0", 0, false },
	{ "C crash: Ask prompts after the restart", "crash:ask wait:2500", "", "screen=crash shows=USA?",
	  0, false },
	{ "C crash: send from the prompt", "crash:ask wait:2500", "ok", "screen=home crash_sends=1", 1, false },
	{ "C crash: not now from the prompt", "crash:ask wait:2500", "down ok", "screen=home crash_sends=0 crash_seen=1", 2,
	  false },
	{ "C crash: Auto sends without a prompt", "crash:auto wait:2500", "", "screen=home crash_sends=1 crash_seen=1", 0,
	  false },
	{ "C crash: send needs a target", "crash:on ok 7 0 down down down down ok", "1", "crash_sends=0 toast=Pick_Send_to",
	  1, false },
	{ "C crash: Send to picks a contact", "crash:on ok 7 0 down down down down ok", "2 ok", "screen=crash shows=USA", 2,
	  false },
	{ "C crash: send from the menu", "crash:on ok 7 0 down down down down ok 2 ok", "1", "crash_sends=1", 1, false },
	{ "C crash: After a crash cycles", "crash:on ok 7 0 down down down down ok", "3", "shows=Ask", 1, false },
	{ "C crash: cleared", "crash:on ok 7 0 down down down down ok", "4", "crash_have=0 shows=No_crash_recorded", 1, false },
	{ "C crash: nothing to clear", "ok 7 0 down down down down ok", "4", "toast=none shows=No_crash_recorded", 1, false },
	{ "C crash: nothing to send", "ok 7 0 down down down down ok", "1", "toast=No_crash crash_sends=0", 1, false },
	{ "C crash: a send that can't start says so", "crash:on crashtx:refuse ok 7 0 down down down down ok 2 ok", "1",
	  "toast=Can't_send crash_sends=0", 1, false },
	{ "C crash: Ask without a target asks for one", "crash:on ok 7 0 down down down down ok", "3",
	  "shows=Ask toast=Pick_Send_to_as_well", 1, false },
	{ "C crash: Auto says when the report is out", "crash:auto wait:2500", "", "toast=Crash_report_sent", 0, false },
	{ "C crash: Auto says when the report failed", "crash:auto crashtx:fail wait:2500", "",
	  "toast=Crash_report_not_sent crash_sends=1", 0, false },
	{ "C crash: Auto that can't start stays quiet", "crash:auto crashtx:refuse wait:2500", "",
	  "screen=home crash_sends=0 crash_seen=0 toast=none", 0, false },
	{ "C crash: no prompt off Home", "crash:ask ok wait:2500", "", "screen=hub crash_seen=0", 0, false },
	{ "C crash: the prompt once back on Home", "crash:ask ok wait:2500 back wait:100", "", "screen=crash shows=USA?", 0,
	  false },
	{ "C crash: a seen record isn't asked about", "crash:ask ok 7 0 down down down down ok long:back wait:2500", "",
	  "screen=home crash_sends=0", 0, false },
	// Digit keys while transmitting (#37)
	{ "T FM transmitting: a digit does nothing (#37)", "down pttdown", "5", "screen=home toast=none", 1, false },
	{ "T FM transmitting: a held digit does nothing (#37)", "down pttdown", "long:5", "screen=home toast=none", 1, false },
	{ "T DMR transmitting: a digit does nothing (#37)", "pttdown", "5", "screen=home toast=none", 1, false },
	{ "T after the transmission a digit opens Find again", "down pttdown pttup", "5", "screen=find", 1, false },
	{ "T a transmission without PTT (beacon) leaves the digits alone", "down autotx", "5", "screen=find", 1, false },
	{ "T a refused transmission (RX only) leaves the digits alone", "ok 4 down ok up up up up up up up up pttdown", "5", "zone=1 pos=9 screen=find", 1, false },
};

// GD-77 key map on its 128x64 monochrome screen only
static const flow_t flows_mono[] = {
#if CONFIG_UI_COLOR
	{ "S theme on a monochrome screen: dimmed", "", "ok 7 2 down down ok", "toast=Colors_are_for set7=0", 6, false },
	{ "S highlight color on a monochrome screen: dimmed", "", "ok 7 2 down down down ok",
	  "toast=Colors_are_for set49=0", 7, false },
	{ "S filled status bar on a monochrome screen: dimmed", "", "ok 7 2 down down down down ok",
	  "toast=Colors_are_for set50=1", 8, false },	// unchanged: on by default
#else
	{ "S no color rows on a monochrome screen", "", "ok 7 2", "hides=Theme hides=Highlight hides=Filled", 3, false },
#endif
};

// DM-1701 key map only: the top side key, long waits
static const flow_t flows_k[] = {
	// Dual watch on the side keys: SK2 is A/B pressed, Dual watch held; P1 / P2 (zones) and the knob act on the
	// selected channel
	{ "11 dual watch: SK2 pressed is A/B", "quick 7 ctx 3 5 ok", "sk2", "dual=1 sel=B", 1, false },
		{ "11 dual watch: SK2 pressed with the watch off", "", "sk2", "dual=0 toast=Turn_on_dual_watch_first", 1, false },
		{ "11 dual watch: SK2 held starts it", "quick 7 ctx 3 5 ok back back", "long:sk2", "dual=1 sel=A bpos=5", 1, false },
		{ "11 dual watch: SK2 held stops it", "quick 7 ctx 3 5 ok", "long:sk2", "dual=0", 1, false },
		{ "11 dual watch: P2 moves the selected", "quick 7 ctx 3 5 ok", "user4", "dual=1 azone=1", 1, false },
		{ "11 dual watch: P1 moves the selected", "quick 7 ctx 3 5 ok", "user1", "dual=1 azone=2", 1, false },
		{ "11 dual watch: Up/Down on zone, the knob still steps", "ok 7 8 down down ok long:back quick 7 ctx 3 5 ok", "enc+",
	  "dual=1 azone=0 apos=4", 1, false },
#if CONFIG_UI_COLOR
	{ "S theme light (color screens)", "", "ok 7 2 down down ok", "set7=1", 6, false },
	{ "S highlight color red (the first after Default)", "", "ok 7 2 down down down ok", "set49=1", 7, false },
	{ "S filled status bar off (on by default)", "", "ok 7 2 down down down down ok", "set50=0", 8, false },
#else
	// Built without CONFIG_UI_COLOR: Display has no color rows; the third row is Large text
	{ "S no color rows in this build", "", "ok 7 2", "hides=Theme hides=Highlight hides=Filled", 3, false },
	{ "S large text is the third row", "", "ok 7 2 down down ok", "set8=1", 6, false },
#endif
#if CONFIG_SUBTONE
	{ "E TX tone spinner, the knob", "ok 5 up up ok down down down down down down down down down", "ok enc+ enc+ ok",
	  "shows=107.2", 4, false },
#endif
	{ "E emergency: hold the top key", "", "long:emerg", "toast=EMERGENCY_sent", 1, false },
	{ "E emergency: hold again cancels", "long:emerg", "long:emerg", "toast=Emergency_canceled", 1, false },
	{ "E top key short: power", "", "user3", "power=5", 1, false },
	// Side keys (Settings > Keys rows 4-9, core/input/keyassign.h; set42 = side key 1 ... set47 = top key hold)
	{ "K side key 1: the next choice", "ok 7 8", "down down down ok", "set42=1 shows=Nothing", 4, false },
	{ "K side key 1: monitor", "ok 7 8 down down down ok", "ok", "set42=2", 1, false },
	{ "K side key 1 hold while it monitors (DMR)", "ok 7 8", "down down down down ok", "set43=1", 5, false },
	{ "K side key 1 hold once it doesn't", "ok 7 8 down down down ok down", "ok", "set43=1", 1, false },
	{ "K side key 2 hold", "ok 7 8", "down down down down down down ok", "set45=1", 7, false },
	{ "K top key hold: emergency after scan", "ok 7 8 up ok ok ok ok ok ok", "ok", "set47=8 shows=Emergency", 1,
	  false },
	{ "K top key hold: copy to VFO", "ok 7 8 up ok ok ok ok ok ok ok ok ok", "ok", "set47=14 shows=Copy_to_VFO", 1,
	  false },
	{ "K top key hold: call back last", "ok 7 8 up ok ok ok ok ok ok ok ok ok ok", "ok", "set47=12 shows=Call_back", 1,
	  false },
	{ "K top key hold wraps to default", "ok 7 8 up ok ok ok ok ok ok ok ok ok ok ok ok ok", "ok", "set47=0", 1, false },
	{ "K a key set to power", "", "powerkey", "power=5", 1, false },
	{ "K a key set to power, long", "", "long:powerkey", "power=5", 1, false },
	{ "K a key set to the next zone", "", "zone+", "zone=1", 1, false },
	{ "K a key set to the previous zone", "", "zone-", "zone=2", 1, false },
	{ "K a key set to scan, from a menu", "ok 7", "scankey", "scanning=1", 1, false },
	{ "K talk target on a long press", "", "long:tt", "screen=picker", 1, false },
	{ "K monitor key on DMR: digital monitor", "", "mon", "hearall=1 toast=Digital_monitor_on", 1, false },
	{ "K monitor key on DMR, again: off", "mon", "mon", "hearall=0 toast=Digital_monitor_off", 1, false },
	{ "K a key set to key lock", "", "lockkey", "locked=1", 1, false },
	{ "K key lock unlocks too", "lockkey", "lockkey", "locked=0", 1, false },
	{ "K call back: the last talkgroup", CALLER, "cbkey ptt", "txid=91 txpriv=0", 2, false },
	{ "K call back: nothing heard", "", "cbkey", "toast=No_calls", 1, false },
	// The volume key of a radio without a knob (UI_FEAT_VOLUME, "softvol"): the editor, the encoder / Up / Down, the ends
	{ "V volume key opens the editor", "softvol", "volkey", "screen=volume vol=8", 1, false },
	{ "V volume up and down", "softvol volkey", "up up down", "vol=9 screen=volume", 3, false },
	{ "V volume stops at the top", "softvol volkey", "up up up up up up up up up up", "vol=16", 10, false },
	{ "V volume down to mute", "softvol volkey", "down down down down down down down down", "vol=0", 8, false },
	{ "V volume stops at mute", "softvol volkey", "down down down down down down down down down down", "vol=0", 10, false },
	{ "V volume OK closes it", "softvol volkey up", "ok", "screen=home vol=9", 1, false },
	{ "V volume BACK closes it", "softvol volkey up", "back", "screen=home vol=9", 1, false },
	{ "V volume key again closes it", "softvol volkey up", "volkey", "screen=home vol=9", 1, false },
	{ "V volume closes after a pause", "softvol volkey", "up wait:3500", "screen=home vol=9", 1, false },
	{ "V volume stays while keys come", "softvol volkey", "up wait:2000 up wait:2000", "screen=volume vol=10", 2, false },
	{ "V volume from a menu, back returns to it", "softvol ok", "volkey back", "screen=hub", 2, false },
	{ "V a knob radio: no editor", "", "volkey", "screen=home toast=Volume:_use", 1, false },
	{ "V volume is in Settings > Sound", "softvol ok 7", "ok", "shows=Volume", 1, false },
	{ "K home zone: back to the first zone", "zone+ zone+", "homekey", "zone=0 toast=Home_zone", 1, false },
	{ "K home zone: already there", "", "homekey", "zone=0 toast=Home_zone", 1, false },
	{ "K home zone: out of the VFO", "ok 4 up ok", "homekey", "vfo=0 zone=0", 1, false },
	{ "K a key set to key lock, long", "", "long:lockkey", "locked=1 toast=Keys_locked", 1, false },
	{ "K key lock unlocks on a long press", "long:lockkey", "long:lockkey", "locked=0 toast=Keys_unlocked", 1,
	  false },
	{ "K locked: the monitor key still works", "lockkey", "mon", "hearall=1", 1, false },
	{ "K locked: call back is ignored", CALLER "lockkey", "cbkey", "talk=3100 toast=Keys_locked", 1, false },
	{ "K unlocked again: call back works", CALLER "lockkey lockkey", "cbkey", "talk=91 toast=Talking_to", 1, false },
	{ "K call back: a private call talks to the caller", "call:3101234,1,p wait:11000", "cbkey ptt",
	  "txid=3101234 txpriv=1", 2, false },
	{ "K call back: a caller in Contacts by its saved name", "call:3101234,1,p wait:11000", "cbkey",
	  "talk=3101234 toast=Talking_to_Joe_Smith", 1, false },
	{ "K call back: a caller not in Contacts by its ID", "call:3109999,1,p wait:11000", "cbkey ptt",
	  "txid=3109999 txpriv=1", 2, false },
	{ "K call back: a group call names its talkgroup", CALLER, "cbkey", "talk=91 toast=Talking_to", 1, false },
	{ "K call back on FM: DMR only, nothing changes", CALLER "down", "cbkey", "talk=3100 toast=Call_back:_DMR", 1,
	  false },
	{ "K call back skips what we sent", CALLER "ptt wait:3000", "cbkey", "talk=91 toast=Talking_to", 1, false },
	{ "K call back: only our own call so far", "ptt wait:3000", "cbkey", "toast=No_calls", 1, false },
	{ "H hold # opens the talk target picker", "", "long:ctx", "screen=picker", 1, false },
	{ "H hold # then a typed TG", "", "long:ctx up ok 3 1 0 0 ptt", "txid=3100 txpriv=0", 8, false },
	{ "H hold # on FM: DMR only", "down", "long:ctx", "screen=home toast=Talk_target:_DMR", 1, false },
	{ "H FM monitor held: the pill says RX, MON stays", "down", "mondown", "shows=RX shows=MON", 1, false },
	{ "H FM monitor released: IDLE again", "down mondown", "monup", "shows=IDLE hides=MON", 0, false },
	{ "H DMR monitor: digital monitor, the pill stays IDLE", "", "mon", "shows=IDLE", 1, false },
	// The encoder: forward = the next channel on Home, the next row down a list, the next quick tile; back = up
	{ "N encoder: next channel", "", "enc+", "pos=4 screen=home", 1, false },
	{ "N encoder: previous channel", "", "enc-", "pos=2 screen=home", 1, false },
	{ "N Up/Down on zone: the knob still steps the channel", "ok 7 8 down down ok long:back", "enc+",
	  "zone=0 pos=4 screen=home", 1, false },
	{ "N Up/Down on zone: Up steps the zone", "ok 7 8 down down ok long:back", "up", "zone=1 screen=home", 1, false },
	{ "N Up/Down on talk target: the knob still steps the channel", "ok 7 8 down down ok ok long:back", "enc+",
	  "talk=3100 pos=4 screen=home", 1, false },
	{ "N encoder: down a list", "ok", "enc+ enc+", "focus=Recent", 2, false },
	{ "N encoder: back up a list", "ok enc+ enc+", "enc-", "focus=Messages", 1, false },
	{ "N encoder: wraps up to the last row", "ok", "enc-", "focus=Settings", 1, false },
	{ "N encoder: a number goes up", "ok 7 2 ok", "enc+", "focus=Brightness", 1, false },
	{ "P auto power off warns", "ok 7 7 down ok long:back", "wait:1741000", "banner=Switching_off poweroff=0", 0,
	  false },
	{ "P auto power off", "ok 7 7 down ok long:back", "wait:1801000", "poweroff=1", 0, false },
	{ "P a key keeps it on", "ok 7 7 down ok long:back wait:1700000", "up wait:200000", "poweroff=0", 1, false },
	{ "P the knob holds it", "ok 7 7 down ok long:back wait:1801000", "offfailed", "poweroff=0 banner=Couldn't", 0,
	  false },
};

// Few-key radios (7.5): fallbacks
static const flow_t flows_f[] = {
	{ "F next channel", "", "up", "pos=4", 1, false },
	{ "F quick settings (long OK)", "", "long:ok", "screen=quick", 1, false },
	{ "F last caller (Hub item 0)", CALLER, "ok ok ptt", "txid=91", 3, false },
	{ "F zone", "", "ok down down down down ok down ok", "zone=1", 8, false },
	// A key set to key lock on a key map with no QUICK: long Back still unlocks as well
	{ "F a key set to key lock", "", "lockkey", "locked=1 toast=Keys_locked", 1, false },
	{ "F a key set to key lock, long", "", "long:lockkey", "locked=1", 1, false },
	{ "F key lock unlocks", "lockkey", "lockkey", "locked=0 toast=Keys_unlocked", 1, false },
	{ "F key lock unlocks on a long press", "lockkey", "long:lockkey", "locked=0", 1, false },
	{ "F locked: long Back unlocks", "lockkey", "long:back", "locked=0", 1, false },
	{ "F locked: call back is ignored", CALLER "lockkey", "cbkey", "talk=3100 toast=Keys_locked", 1, false },
	// Text entry without a keypad (ui.md 2.1): Down steps the last character, OK adds one, long OK keeps; long OK on
	// the channel saves
	{ "F text entry by Up / Down", "ok down down down down down ok ok ok", "down ok long:ok",
	  "screen=channel shows=COUNTY_DMQQ", 3, false },
	{ "F long OK saves the channel", "ok down down down down down ok ok ok down ok long:ok", "long:ok",
	  "writes=1 toast=Saved_channel_3", 1, true },
	{ "F no side keys to set","ok up ok up up up ok", "down down down ok", "toast=This_radio_has_no set42=0", 4,
	  false },
};

// The MD-9600's front panel alone on its 128x64 screen (information-architecture.md 7.4): no digits, P1 / P2 as QUICK /
// CONTEXT and named so in the hints, the encoder, P3 / P4 / the orange key as side keys
static const flow_t flows_p[] = {
	{ "P next channel", "", "up", "pos=4", 1, false },
	{ "P next channel by the knob", "", "enc+", "pos=4", 1, false },
	{ "P hints name P1, not *", "", "", "shows=P1_Quick", 0, false },
	{ "P quick settings on P1", "", "quick", "screen=quick", 1, false },
	{ "P key lock on a long P1", "", "long:quick", "locked=1", 1, false },
	{ "P key unlock on a long P1", "long:quick", "long:quick", "locked=0", 1, false },
	{ "P call back on P2", CALLER, "ctx ptt", "txid=91", 2, false },
	{ "P radio check last caller", CALLER, "ctx down down down ok", "checks=1", 5, false },
	{ "P one-shot talk group on P4", "", "tt down ptt", "txid=91 txpriv=0", 3, false },
};

static void run_flow(const flow_t *f, const sim_geom_t *g, sim_map_t map, const char *mapname)
{
	char err[300];
	char exp[300];
	sim_reset(g, map);
	flows_run++;
	if (!sim_run(f->setup, err, sizeof err)) {
		printf("FAIL %s [%s] setup: %s\n", f->name, mapname, err);
		fails++;
		return;
	}
	uint32_t writes0 = sim_rec.codeplug_writes;
	sim_count_reset();
	if (!sim_run(f->script, err, sizeof err)) {
		printf("FAIL %s [%s]: %s\n", f->name, mapname, err);
		fails++;
		return;
	}
	int n = sim_presses();
	// expect tokens: "a=b c=d" -> "expect:a=b expect:c=d"
	char *o = exp;
	const char *p = f->expect;
	while (*p && o < exp + sizeof exp - 16) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		memcpy(o, "expect:", 7);
		o += 7;
		while (*p && *p != ' ' && o < exp + sizeof exp - 2)
			*o++ = *p++;
		*o++ = ' ';
	}
	*o = 0;
	if (!sim_run(exp, err, sizeof err)) {
		printf("FAIL %s [%s]: %s\n", f->name, mapname, err);
		fails++;
	}
	if (n > f->budget) {
		printf("FAIL %s [%s]: %d presses, budget %d\n", f->name, mapname, n, f->budget);
		fails++;
	}
	if (!f->writes && sim_rec.codeplug_writes != writes0) {
		printf("FAIL %s [%s]: wrote the codeplug while only operating (P7)\n", f->name, mapname);
		fails++;
	}
}

// ---- crawler ----

#define CRAWL_MAX	400
static const char *const crawl_keys[] = { "ok", "down", "up", "ctx", "quick", "1", "2", "3", "4", "5", "6", "7", "0" };
#define NKEYS	(sizeof crawl_keys / sizeof crawl_keys[0])

typedef struct {
	char path[96];
	char sig[40];
} node_t;
static node_t nodes[CRAWL_MAX];
static int n_nodes;

#define WORLD	"msg:3101234,Meet_at_the_north_lot call:3105678,3100,g wait:11000 "

static bool replay(const sim_geom_t *g, sim_map_t map, const char *path, ui_app_status_t *st)
{
	char err[300], buf[200];
	sim_reset(g, map);
	snprintf(buf, sizeof buf, WORLD "%s", path);
	if (!sim_run(buf, err, sizeof err))
		return false;
	ui_app_status(st);
	return true;
}

static void signature(const ui_app_status_t *st, char *sig, size_t len)
{
	snprintf(sig, len, "%s/%u/%s", st->screen, st->depth, st->locked ? "L" : "");
}

static void crawl(const sim_geom_t *g, sim_map_t map, const char *mapname)
{
	ui_app_status_t st;
	n_nodes = 0;
	if (!replay(g, map, "", &st))
		return;
	snprintf(nodes[0].path, sizeof nodes[0].path, "%s", "");
	signature(&st, nodes[0].sig, sizeof nodes[0].sig);
	n_nodes = 1;
	for (int i = 0; i < n_nodes; i++) {
		if (strlen(nodes[i].path) > 60)
			continue;
		for (size_t k = 0; k < NKEYS && n_nodes < CRAWL_MAX; k++) {
			char path[96];
			snprintf(path, sizeof path, "%s%s%s", nodes[i].path, nodes[i].path[0] ? " " : "", crawl_keys[k]);
			if (!replay(g, map, path, &st))
				continue;	// a key this map doesn't have
			if (strcmp(st.screen, "confirm") == 0 && strstr(path, "ok ok"))
				continue;
			char sig[40];
			signature(&st, sig, sizeof sig);
			bool seen = false;
			for (int j = 0; j < n_nodes && !seen; j++)
				seen = strcmp(nodes[j].sig, sig) == 0;
			if (seen)
				continue;
			snprintf(nodes[n_nodes].path, sizeof nodes[n_nodes].path, "%s", path);
			snprintf(nodes[n_nodes].sig, sizeof nodes[n_nodes].sig, "%s", sig);
			n_nodes++;
		}
	}
	// Field-use screens: the shortest way there is at most 3 levels (P4; quick settings and actions also open on
	// top of deeper screens, which is fine)
	for (int i = 0; i < n_nodes; i++) {
		char name[sizeof nodes[i].sig];
		snprintf(name, sizeof name, "%s", nodes[i].sig);
		char *slash = strchr(name, '/');
		if (slash != NULL)
			*slash = 0;
		int best = 99;
		bool field = false;
		for (int j = 0; j < n_nodes; j++) {
			ui_app_status_t s;
			if (strncmp(nodes[j].sig, name, strlen(name)) != 0 || nodes[j].sig[strlen(name)] != '/')
				continue;
			unsigned d = 0;
			sscanf(nodes[j].sig + strlen(name) + 1, "%u", &d);
			if ((int)d < best)
				best = (int)d;
			if (j == i && replay(g, map, nodes[j].path, &s))
				field = s.field_use;
		}
		if (field && best > 3) {
			printf("FAIL crawl [%s] %s: field-use screen %d levels deep at best (P4)\n", mapname, name, best);
			fails++;
		}
	}
	// Check each screen found
	int screens = 0;
	for (int i = 0; i < n_nodes; i++) {
		ui_app_status_t s0, s1;
		char path[160], err[300];
		if (!replay(g, map, nodes[i].path, &s0) || s0.locked)
			continue;
		screens++;
		// About has no strip while idle (the owner's call); it comes back with RX / TX (checked below with PTT)
		if (!s0.strip_drawn && strcmp(s0.screen, "diag") != 0 && strcmp(s0.screen, "about") != 0) {
			printf("FAIL crawl [%s] %s: no status strip (P2)\n", mapname, s0.screen);
			fails++;
		}
		// Text entry is the one exception to P4 (principles 4): BACK deletes, and leaves once the field is empty
		if (s0.depth > 0 && strcmp(s0.screen, "text") != 0) {
			snprintf(path, sizeof path, "%s back", nodes[i].path);
			if (replay(g, map, path, &s1) && s1.depth >= s0.depth && strcmp(s1.screen, s0.screen) == 0) {
				printf("FAIL crawl [%s] %s: BACK doesn't leave it (P4) via '%s'\n", mapname, s0.screen,
				       nodes[i].path);
				fails++;
			}
		}
		snprintf(path, sizeof path, "%s long:back", nodes[i].path);
		if (replay(g, map, path, &s1) && (s1.depth != 0 || strcmp(s1.screen, "home") != 0) &&
				strcmp(s0.screen, "hotspot") != 0) {
			printf("FAIL crawl [%s] %s: long BACK doesn't reach Home (P4)\n", mapname, s0.screen);
			fails++;
		}
		if (map != SIM_MAP_F) {
			// Long QUICK locks from Home only; in a menu it must not lock mid-task (P3)
			snprintf(path, sizeof path, "%s long:quick", nodes[i].path);
			bool home = s0.depth == 0 && strcmp(s0.screen, "home") == 0;
			if (replay(g, map, path, &s1) && s1.locked != home) {
				printf("FAIL crawl [%s] %s: long QUICK %s (P3)\n", mapname, s0.screen,
				       home ? "doesn't lock on Home" : "locks in a menu");
				fails++;
			}
			snprintf(path, sizeof path, "%s long:ctx", nodes[i].path);
			if (replay(g, map, path, &s1) && sim_view()->ch.mode == RADIO_MODE_DMR &&
					strcmp(s1.screen, "picker") != 0 && strcmp(s0.screen, "hotspot") != 0) {
				printf("FAIL crawl [%s] %s: long CONTEXT isn't the talk target picker (P3)\n", mapname,
				       s0.screen);
				fails++;
			}
		}
		// PTT from any screen: the strip shows TX
		snprintf(path, sizeof path, "%s pttdown", nodes[i].path);
		if (replay(g, map, path, &s1)) {
			if (sim_view()->state != RS_TX && sim_view()->state != RS_TX_DENIED) {
				printf("FAIL crawl [%s] %s: PTT didn't transmit\n", mapname, s0.screen);
				fails++;
			} else if (!s1.strip_drawn && strcmp(s1.screen, "diag") != 0) {
				printf("FAIL crawl [%s] %s: no TX pill while transmitting (P2)\n", mapname, s0.screen);
				fails++;
			}
		}
		(void)err;
	}
	printf("# crawl [%s]: %d screens and states checked\n", mapname, screens);
}

// About (Settings > Advanced): every word readable at every display class (nothing shortened with an ellipsis, all
// the way to the end), no status strip while idle, the strip back while transmitting
static void about_checks(void)
{
	char err[300];
	for (int g = 0; g < sim_geom_count; g++) {
		const char *gn = sim_geoms[g].name;
		ui_app_status_t st;
		sim_reset(&sim_geoms[g], SIM_MAP_K);
		if (!sim_run("ok 7 0 ok", err, sizeof err)) {
			printf("FAIL about [%s]: %s\n", gn, err);
			fails++;
			continue;
		}
		sim_rec.shortened = 0;
		sim_run("wait:200 down down down down down down down down down down down down down down down down down down down "
			"down down down", err, sizeof err);
		ui_app_status(&st);
		if (strcmp(st.screen, "about") != 0 || sim_rec.shortened != 0) {
			printf("FAIL about [%s]: on %s, %u texts shortened\n", gn, st.screen, (unsigned)sim_rec.shortened);
			fails++;
		}
		if (st.strip_drawn) {
			printf("FAIL about [%s]: the status strip is drawn while idle\n", gn);
			fails++;
		}
		sim_run("pttdown wait:200", err, sizeof err);
		ui_app_status(&st);
		if (!st.strip_drawn) {
			printf("FAIL about [%s]: no strip while transmitting (P2)\n", gn);
			fails++;
		}
		sim_run("pttup", err, sizeof err);
	}
}

// Transmitting, at every display class: a private call shows the contact's name, even a 16-character one (the ID
// gives way first), and its title says "Private" where a group call's has "TG"; a group call never says "Private"
static void call_screen_checks(void)
{
	static const struct { const char *name, *script, *expect; } cases[] = {
		{ "private to a contact", "3 1 0 1 2 3 4 ok wait:5000 pttdown", "txpriv=1 shows=Joe_Smith shows=Private_310" },
		{ "private, long name", "worst 3 1 0 9 9 9 9 ok wait:5000 pttdown",
		  "txpriv=1 shows=Christo shows=Wood shows=Private_310" },
		{ "private, no contact", "3 1 0 5 5 5 5 down ok wait:5000 pttdown", "txpriv=1 shows=3105555 shows=Private_310" },
		{ "group", "3 1 0 0 ok wait:5000 pttdown", "txpriv=0 shows=TG_3100 hides=Private" },
	};
	char err[300], buf[200];
	for (int g = 0; g < sim_geom_count; g++) {
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			sim_reset(&sim_geoms[g], SIM_MAP_K);
			snprintf(buf, sizeof buf, "%s", cases[i].script);
			bool ok = sim_run(buf, err, sizeof err);
			char *o = buf;	// "a=b c=d" -> "expect:a=b expect:c=d"
			for (const char *p = cases[i].expect; ok && *p && o < buf + sizeof buf - 16;) {
				while (*p == ' ')
					p++;
				memcpy(o, "expect:", 7);
				o += 7;
				while (*p && *p != ' ' && o < buf + sizeof buf - 2)
					*o++ = *p++;
				*o++ = ' ';
			}
			*o = 0;
			if (ok)
				ok = sim_run(buf, err, sizeof err);
			if (!ok) {
				printf("FAIL call screen %s [%s]: %s\n", cases[i].name, sim_geoms[g].name, err);
				fails++;
			}
		}
	}
}

// A radio without a battery (CAP_NO_BATTERY), at every display class: no percentage in the status strip, no battery
// warning, and no Battery page, Battery saver or Low battery warning in the lists (Tools keep their numbers, Battery
// is the last one; in Settings the other rows move up). With a
// battery they are all there
static void no_battery_checks(void)
{
	static const struct { const char *name, *script, *expect; } cases[] = {
		{ "strip with a battery", "", "shows=84%" },
		{ "strip", "nobattery", "hides=%" },
		{ "no warning", "nobattery battery:14", "banner=none" },
		{ "Tools with a battery", "ok 6 9", "screen=info shows=Battery" },
		{ "Tools", "nobattery ok 6", "hides=Battery" },
		{ "Tools keep their numbers", "nobattery ok 6 7", "screen=diag" },
		{ "Tools: Roaming is 6", "nobattery ok 6 6", "screen=roaming" },
		{ "Tools: no Battery to open", "nobattery ok 6 9", "toast=No_item" },
		{ "Power settings with a battery", "ok 7 7", "shows=Battery_saver shows=Low_battery" },
		{ "Power settings", "nobattery ok 7 7", "hides=Battery_saver hides=Low_battery shows=Auto_power_off" },
	};
	char err[300], buf[200];
	for (int g = 0; g < sim_geom_count; g++) {
		for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			sim_reset(&sim_geoms[g], SIM_MAP_K);
			snprintf(buf, sizeof buf, "%s", cases[i].script);
			bool ok = sim_run(buf, err, sizeof err);
			char *o = buf;	// "a=b c=d" -> "expect:a=b expect:c=d"
			for (const char *p = cases[i].expect; ok && *p && o < buf + sizeof buf - 16;) {
				while (*p == ' ')
					p++;
				memcpy(o, "expect:", 7);
				o += 7;
				while (*p && *p != ' ' && o < buf + sizeof buf - 2)
					*o++ = *p++;
				*o++ = ' ';
			}
			*o = 0;
			if (ok)
				ok = sim_run(buf, err, sizeof err);
			if (!ok) {
				printf("FAIL no battery %s [%s]: %s\n", cases[i].name, sim_geoms[g].name, err);
				fails++;
			}
		}
	}
}

// Home's talk target line drops a contact name that only repeats the ID ("TG 201 TG 201" on the DB25-D, 2026-10-17)
static int check_name_is_id(void)
{
	int bad = 0;
	static const struct { const char *name; uint32_t id; bool priv, want; } c[] = {
		{ "TG 201", 201, false, true }, { "TG201", 201, false, true }, { "tg 201 ", 201, false, true }, { "201", 201, false, true },
		{ "3101276", 3101276, true, true }, { "TG 202", 201, false, false }, { "Main Ops", 201, false, false },
		{ "TG 201 Ops", 201, false, false }, { "TG 3101276", 3101276, true, false }, { "", 201, false, false },
		{ "99999999999", 201, false, false },
	};
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		if (ui_name_is_id(c[i].name, c[i].id, c[i].priv) != c[i].want) {
			printf("FAIL ui_name_is_id(\"%s\", %u, %d)\n", c[i].name, (unsigned)c[i].id, c[i].priv);
			bad++;
		}
	}
	return bad;
}

int main(void)
{
	if (check_name_is_id() != 0)
		return 1;
	static const struct { sim_map_t map; const char *name, *geom; } runs[] = {
		{ SIM_MAP_K, "K DM-1701", "160x120i5" },
		{ SIM_MAP_M, "M GD-77", "128x64" },
		{ SIM_MAP_V, "V MD-9600 + mic", "128x64" },
		{ SIM_MAP_K, "K 240x320", "240x320" },
	};
	for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
		const sim_geom_t *g = sim_geom_find(runs[r].geom);
		for (size_t i = 0; i < sizeof flows / sizeof flows[0]; i++)
			run_flow(&flows[i], g, runs[r].map, runs[r].name);
	}
	for (size_t i = 0; i < sizeof flows_mono / sizeof flows_mono[0]; i++)
		run_flow(&flows_mono[i], sim_geom_find("128x64"), SIM_MAP_M, "M GD-77");
	for (size_t i = 0; i < sizeof flows_k / sizeof flows_k[0]; i++)
		run_flow(&flows_k[i], sim_geom_find("160x120i5"), SIM_MAP_K, "K DM-1701");
	for (size_t i = 0; i < sizeof flows_p / sizeof flows_p[0]; i++)
		run_flow(&flows_p[i], sim_geom_find("128x64"), SIM_MAP_P, "P MD-9600 panel");
	const sim_geom_t *gf = sim_geom_find("160x128");
	for (size_t i = 0; i < sizeof flows_f / sizeof flows_f[0]; i++)
		run_flow(&flows_f[i], gf, SIM_MAP_F, "F few keys");
	printf("# %d flows run\n", flows_run);
	about_checks();
	call_screen_checks();
	no_battery_checks();

	crawl(sim_geom_find("160x120i5"), SIM_MAP_K, "K DM-1701");
	crawl(sim_geom_find("128x64"), SIM_MAP_M, "M GD-77");
	crawl(gf, SIM_MAP_F, "F few keys");
	crawl(sim_geom_find("128x64"), SIM_MAP_P, "P MD-9600 panel");

	printf(fails ? "# FAILED uiFlowTest: %d\n" : "# PASSED uiFlowTest\n", fails);
	return fails != 0;
}
