// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The support info on the debug link (DL_CMD_SUPPORT, docs/spec/debuglink.md 3.29, docs/spec/support-info.md): the
// build's source and the hardware identity (INFO), and the firmware image's SHA-256 a piece per request (HASH), so each
// request stays short and the radio carries on as before. Built with CONFIG_SUPPORT_INFO.
#include <string.h>
#include "config.h"
#if CONFIG_SUPPORT_INFO
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "support/image_hash.h"
#include "support/support_info.h"

static void info(dl_text_t *t)
{
	dl_text(t, "source=%s\n", support_source_key(support_source()));
	dl_text_kv_str(t, "repo", support_repo());
	// hw.<key>, then hw.<key>.2 ... for a key seen before (two RF chips). The hardware options have their own command
	// (HWOPT LIST)
	uint8_t seen[8] = { 0 };
	const char *keys[8] = { NULL };
	support_line_t l;
	for (uint8_t i = 0; support_hw_line(i, &l); i++) {
		if (strcmp(l.key, "hwopt") == 0) {
			continue;
		}
		uint8_t k = 0;
		while (k < 8 && keys[k] != NULL && strcmp(keys[k], l.key) != 0) {
			k++;
		}
		if (k == 8) {
			continue;
		}
		keys[k] = l.key;
		char key[20];
		if (seen[k]++ == 0) {
			(void)dl_fmt(key, sizeof key, "hw.%s", l.key);
		} else {
			(void)dl_fmt(key, sizeof key, "hw.%s.%u", l.key, seen[k]);
		}
		dl_text_kv_str(t, key, l.value);
	}
	uint32_t addr, len;
	if (image_hash_span(&addr, &len)) {
		dl_text(t, "image.addr=0x%08X\nimage.size=%u\n", (unsigned)addr, (unsigned)len);
	}
}

static void hash(dl_text_t *t)
{
	uint8_t d[SHA256_LEN];
	uint32_t addr, len;
	if (!image_hash_span(&addr, &len)) {
		dl_text(t, "image.state=none\n");
		return;
	}
	if (!image_hash_step(IMAGE_HASH_LINK, IMAGE_HASH_STEP_LINK, d)) {
		dl_text(t, "image.state=pending\nimage.done=%u\nimage.size=%u\n", (unsigned)image_hash_done(), (unsigned)len);
		return;
	}
	dl_text(t, "image.state=done\nimage.size=%u\nimage.sha256=", (unsigned)len);
	for (uint8_t i = 0; i < SHA256_LEN; i++) {
		dl_text(t, "%02x", d[i]);
	}
	dl_text(t, "\n");
}

size_t debuglink_cmd_support(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len != 1 || (req[0] != DL_SUPPORT_OP_INFO && req[0] != DL_SUPPORT_OP_HASH)) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	dl_text_t t;
	out[0] = DL_ST_OK;
	dl_text_init(&t, (char *)out + 1, cap - 1);
	if (req[0] == DL_SUPPORT_OP_INFO) {
		info(&t);
	} else {
		hash(&t);
	}
	return 1 + t.len;
}

#endif
