// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The USB debug link (docs/spec/debuglink.md): framed commands on the host port, beside the MMDVM hotspot and the CPS
// protocol (core/hostlink), for bench automation: version, diagnostics and counters, the radio view as key=value
// text, screenshots, key and PTT injection, radio requests, a log ring. CONFIG_DEBUGLINK.
#ifndef DEBUGLINK_H
#define DEBUGLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "app/task_stack.h"
#include "debuglink/dl_fmt.h"
#include "hal_vocoder.h"
#include "radio/radio.h"

// Register with the host link as its side protocol (hostlink_start calls it)
void debuglink_start(void);

// --- The radio view as key=value text (debuglink_view.c), in sections so each fits one reply ---

enum {
	DL_VIEW_CORE,		// state, channel position, squelch, RSSI, power, TX timer
	DL_VIEW_CHANNEL,	// the channel / VFO settings in effect
	DL_VIEW_DMR,		// the DMR call view (debuglink_view_dmr.c)
	DL_VIEW_DMR_CALL,	// the current and the last DMR call (debuglink_view_dmr.c)
	DL_VIEW_DMR_STATS,	// the DMR voice path counters: call stack, AMBE voice (debuglink_view_dmr.c)
	DL_VIEW_HOTSPOT,	// the host link mode and the hotspot engine
	DL_VIEW_LINK,		// input and debug link state
	DL_VIEW_FEATURES,	// scan, FM extras, battery, beeps, the radio settings (debuglink_view_radio.c)
	DL_VIEW_DMR_DATA,	// the DMR data service: state, settings, counters (debuglink_dmr_data.c)
	DL_VIEW_STORAGE,	// settings store, flash journal, codeplug and CPS session counters
	DL_VIEW_DMR_EXT,	// DMR emergency, ETSI short data, the hotspot CW ID (debuglink_view_ext.c)
	DL_VIEW_DMR_DATA_STATS,	// the DMR data service counters (debuglink_dmr_data.c; split from dmr_data for the reply size)
	DL_VIEW_ROAM,		// DMR repeater roaming: state, sites, counters, settings (debuglink_view_roam.c)
	DL_VIEW_APRS,		// APRS: callsign, block reason, position, next beacon, counters (debuglink_aprs.c)
	DL_VIEW_APRS_SET,	// the APRS settings (debuglink_aprs.c; split from aprs for the reply size)
	DL_VIEW_DMR_IP,		// IP packets over DMR: compression, confirmed delivery, their counters (debuglink_dmr_data.c)
	DL_VIEW_NET,		// the radio's own TCP and its services (debuglink_view_net.c)
	DL_VIEW_SNMP,		// the SNMP agent: settings and counters (debuglink_snmp.c)
	DL_VIEW_USRP,		// USRP voice over UDP and the shared PCM stream (core/usbaudio/usrp_net.c)
	DL_VIEW_SFR,		// the MMDVM hotspot's SFR mode (was in DL_VIEW_HOTSPOT until view version 2)
	DL_VIEW_TASKS,		// every task's least free stack (debuglink_view_tasks.c)
	DL_VIEW_CRASH,		// the crash log's record from before this start (debuglink_crash.c)
	DL_VIEW_WATCHDOG,	// the hardware watchdog: timeout, feeds, longest idle gap, raw registers (debuglink_watchdog.c)
	DL_VIEW_COUNT
};
#define DL_VIEW_VERSION		2	// bumped when a key changes meaning or goes away (new keys don't bump it)

// Render one section of v into t. Every section starts with "section=<name>"
void debuglink_view_section(uint8_t section, const radio_view_t *v, dl_text_t *t);
// A section's name ("core", ...), NULL past the last
const char *debuglink_view_name(uint8_t section);

// The DMR sections (part 0: DL_VIEW_DMR, 1: DL_VIEW_DMR_CALL, 2: DL_VIEW_DMR_STATS), in their own file so the DMR
// integration can change them without touching the rest; built without DMR they say "dmr.built=0"
void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t);
// The DMR voice path counters as diagnostics lines (call stack, AMBE voice): line i < DL_DMR_DIAG_LINES
#define DL_DMR_DIAG_LINES	4
bool debuglink_dmr_diag_line(uint8_t i, char *buf, uint8_t len);
// Zero the call stack's and the AMBE voice counters
void debuglink_dmr_reset(void);

// The radio features (debuglink_view_radio.c): the "features" section, and the RADIO operations for scan, beeps, the
// tone burst, one setting, roaming, vibration and volume (DL_OP_*; returns a DL_ST_* status)
void debuglink_view_radio(const radio_view_t *v, dl_text_t *t);
enum {
	DL_OP_SCAN = 20, DL_OP_SCAN_VFO = 21, DL_OP_BEEP = 22, DL_OP_TONE_BURST = 23, DL_OP_SETTING = 24,
	DL_OP_ROAM = 26, DL_OP_VIBRATE = 29, DL_OP_VOLUME = 30, DL_OP_DMR_FIELDS = 31, DL_OP_VFO_COPY = 32,
};
uint8_t debuglink_radio_feature_op(uint8_t op, const uint8_t *args, uint16_t len);
// RADIO ops the dispatcher hands to debuglink_radio_feature_op: a new DL_OP_* goes here too
static inline bool debuglink_is_feature_op(uint8_t op)
{
	return (op >= DL_OP_SCAN && op <= DL_OP_SETTING) || op == DL_OP_ROAM || op == DL_OP_VIBRATE || op == DL_OP_VOLUME ||
	       op == DL_OP_DMR_FIELDS || op == DL_OP_VFO_COPY;
}
// VERSION's codec=: the vocoder profile the board recognized in flash (signature and hash checked by the board),
// "absent" when the board has a vocoder but its check failed, "none" without one (FM-only build). The profile comes
// first: a codec the board has stopped for a stub upload is still in flash, and a flasher asking then must not see it
// absent. Read only
static inline const char *debuglink_codec_id(const hal_vocoder_t *v)
{
	if (v == NULL) {
		return "none";
	}
	const char *id = v->profile != NULL ? v->profile() : NULL;
	if (id != NULL) {
		return id;
	}
	return v->present() ? "present" : "absent";
}
// The DMR data service (debuglink_dmr_data.c): the dmr_data view section, the DMR_DATA command (docs/spec/debuglink.md
// 3.12) and its counter reset. Built without CONFIG_DMR_DATA_CORE: "data.built=0", the command answers unavailable
void debuglink_view_dmr_data(uint8_t part, dl_text_t *t);	// part 0: state and settings, 1: counters, 2: IP packets
size_t debuglink_cmd_dmr_data(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_dmr_data_reset(void);
// The audio probe (debuglink_audio_probe.c, docs/spec/debuglink.md 3.14): built without CONFIG_AUDIO_PROBE it answers
// unavailable
size_t debuglink_cmd_audio_probe(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// The dmr_ext section (debuglink_view_ext.c): emergency state, alert and settings, the ETSI data counters, CW ID
void debuglink_view_dmr_ext(dl_text_t *t);
// The roam section (debuglink_view_roam.c); built without CONFIG_DMR_ROAMING it says "roam.built=0"
void debuglink_view_roam(const radio_view_t *v, dl_text_t *t);
// APRS (debuglink_aprs.c): the aprs section and the APRS command; without CONFIG_APRS "aprs.built=0" / unavailable
void debuglink_view_aprs(uint8_t part, dl_text_t *t);	// part 0: state and counters, 1: settings
size_t debuglink_cmd_aprs(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);

// The net section (debuglink_view_net.c); built without any of the radio's own services (CONFIG_NET_LOCAL) it says "net.built=0"
void debuglink_view_net(dl_text_t *t);
// The SNMP agent (debuglink_snmp.c): the snmp section and the SNMP command; without CONFIG_SNMP "snmp.built=0" /
// unavailable
void debuglink_view_snmp(dl_text_t *t);
size_t debuglink_cmd_snmp(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
// The tasks section (debuglink_view_tasks.c): each task's least free stack in bytes. The formatting takes the table
// apart from the snapshot (task_stack_snapshot) so the host tests can feed it
void debuglink_view_tasks(dl_text_t *t);
void debuglink_view_tasks_format(const task_stack_t *ts, unsigned n, unsigned untracked, dl_text_t *t);

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
// The split duplex command SPLIT and its view text (debuglink_split.c, docs/spec/debuglink.md 3.15)
size_t debuglink_cmd_split(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_view_split(dl_text_t *t);
#endif

// The RAM-monitor loader (debuglink_stub.c, docs/spec/debuglink.md 3.18): the DL_CMD_STUB handler, and the service
// hook the host link task calls each pass so the jump happens after the ENTER reply has gone out. Built without
// CONFIG_STUB_ENTRY (or on a board with no hal_system_t.enter_stub) the command answers unavailable and the hook is empty
size_t debuglink_cmd_stub(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_stub_service(void);

// The crash log (debuglink_crash.c, docs/spec/debuglink.md 3.19): the crash section, DL_CMD_CRASH and the service hook
// that crashes on purpose after the reply has gone out. Without CONFIG_CRASH_LOG: "crash.built=0", unavailable, empty
void debuglink_view_crash(dl_text_t *t);
size_t debuglink_cmd_crash(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_crash_service(void);

// The hardware watchdog (debuglink_watchdog.c, docs/spec/debuglink.md 3.21): the watchdog section, DL_CMD_WATCHDOG's
// bench tests and the service hook that starts them after the reply. Without CONFIG_WATCHDOG: "watchdog.built=0",
// unavailable, empty
void debuglink_view_watchdog(dl_text_t *t);
size_t debuglink_cmd_watchdog(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
void debuglink_watchdog_service(void);

// The debug link's own counters: frames answered, bad CRC, too long, timed out (debuglink.c)
#define DL_LINK_COUNTERS	4
void debuglink_link_counters(uint32_t out[DL_LINK_COUNTERS]);

#endif
