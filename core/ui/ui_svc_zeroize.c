// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The emergency wipe for the UI (ui_svc.h, docs/spec/zeroize.md): the code and the settings rows over core/zeroize,
// and the wipe as a storage job on the housekeeping task, which restarts the radio when every step went
#include "config.h"
#if CONFIG_ZEROIZE
#include "ui/ui_svc.h"
#include "board.h"
#include "storage/storage.h"
#include "zeroize/zeroize.h"

_Static_assert(UI_WIPE_FAIL_KEYS == ZEROIZE_FAIL_KEYS && UI_WIPE_FAIL_SETTINGS == ZEROIZE_FAIL_SETTINGS &&
	       UI_WIPE_FAIL_CODEPLUG == ZEROIZE_FAIL_CODEPLUG && UI_WIPE_FAIL_NATIVE == ZEROIZE_FAIL_NATIVE &&
	       UI_WIPE_FAIL_PURGE == ZEROIZE_FAIL_PURGE && UI_WIPE_FAIL_RADIO == ZEROIZE_FAIL_RADIO, "the same bits");

static volatile uint8_t state;		// UI_WIPE_*: set by the UI task, then by the job
static volatile uint8_t failed;
static volatile bool posted;		// the job is in the slot or running
static volatile bool keys_req;		// this wipe erases the keys only, whatever the scope setting (Clear all keys)

bool ui_svc_wipe_code_set(const char code[4])
{
	return zeroize_code_set(code);
}

bool ui_svc_wipe_code_check(const char code[4])
{
	return zeroize_code_check(code);
}

static void wipe_job(void *ctx)
{
	(void)ctx;
	uint8_t f = zeroize_run(keys_req || zeroize_keys_only());
	failed = f;
	if (f == 0) {
		state = UI_WIPE_DONE;
		if (board.system != NULL && board.system->reboot != NULL) {
			board.system->reboot(HAL_REBOOT_NORMAL);	// does not return when it works
		}
	} else {
		state = UI_WIPE_FAILED;
	}
	posted = false;
}

// The job slot may hold another write (a channel save): the wipe goes in as soon as it is free
static void post(void)
{
	if (state == UI_WIPE_RUNNING && !posted && storage_job_post(wipe_job, NULL)) {
		posted = true;
	}
}

static bool start(bool keys_only)
{
	if (!posted) {
		keys_req = keys_only;
		failed = 0;
		state = UI_WIPE_RUNNING;
		post();
	}
	return true;
}

bool ui_svc_wipe_start(void)
{
	return start(false);
}

bool ui_svc_clear_keys_start(void)
{
	return start(true);
}

uint8_t ui_svc_wipe_state(uint8_t *out)
{
	post();
	*out = failed;
	return state;
}

// The settings rows (ui_svc_storage.c hands these ids here)
int32_t ui_svc_wipe_get(uint8_t id)
{
	switch (id) {
	case UI_SET_WIPE_CODE: return zeroize_code_is_set();
	case UI_SET_WIPE_SCOPE: return zeroize_keys_only();
	case UI_SET_WIPE_CONFIRM: return zeroize_confirm();
	default: return zeroize_tries();
	}
}

bool ui_svc_wipe_set(uint8_t id, int32_t value)
{
	switch (id) {
	case UI_SET_WIPE_CODE:
		if (value != 0) {
			return false;	// a code is set with its digits (ui_svc_wipe_code_set)
		}
		zeroize_code_clear();
		return true;
	case UI_SET_WIPE_SCOPE:
		zeroize_set_keys_only(value != 0);
		return true;
	case UI_SET_WIPE_CONFIRM:
		zeroize_set_confirm(value != 0);
		return true;
	default:
		if (value < 0 || value > (int32_t)ZEROIZE_TRIES_MAX) {
			return false;
		}
		zeroize_set_tries((uint8_t)value);
		return true;
	}
}
#endif
