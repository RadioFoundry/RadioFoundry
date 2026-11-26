// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the scan list screens read and ask for (docs/spec/scan-list.md): the store's lists, written by a job for the
// housekeeping task so the UI never waits on the flash
#include "config.h"
#if CONFIG_SCAN_LIST
#include <string.h>
#include "ui_svc.h"
#include "radio/radio.h"
#include "storage/storage.h"

uint16_t ui_svc_scanlist_slots(void)
{
	return scan_list_slots();
}

uint32_t ui_svc_scanlist_generation(void)
{
	return scan_list_generation();
}

bool ui_svc_scanlist_get(uint16_t slot, scan_list_t *out)
{
	return scan_list_read(slot, out);
}

// The one write in flight: a put (slot < 0: a new list) or a delete (the list's count 0xFF)
static struct {
	bool busy;
	bool del;
	int slot;
	scan_list_t list;
} op;

static void job(void *ctx)
{
	(void)ctx;
	if (op.del)
		scan_list_delete((uint16_t)op.slot);
	else if (op.slot < 0)
		scan_list_add(&op.list);
	else
		scan_list_write((uint16_t)op.slot, &op.list);
	op.busy = false;
}

static bool post(void)
{
	op.busy = true;
	if (storage_job_post(job, NULL))
		return true;
	op.busy = false;
	return false;
}

bool ui_svc_scanlist_put(int slot, const scan_list_t *l)
{
	if (op.busy || !scan_list_valid(l))
		return false;	// a list that can't be stored is refused here, not by the job
	op.del = false;
	op.slot = slot;
	op.list = *l;
	return post();
}

bool ui_svc_scanlist_delete(uint16_t slot)
{
	if (op.busy)
		return false;
	op.del = true;
	op.slot = slot;
	return post();
}

bool ui_svc_scanlist_start(uint16_t slot)
{
	radio_req_scan_list(slot);
	return true;
}
#endif
