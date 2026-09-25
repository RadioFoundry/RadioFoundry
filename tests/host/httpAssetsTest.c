// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HTTP status server's asset table as the firmware compiles it (tools/gen_http_assets.py, gzip or plain): each
// entry's bytes go to DUMP/<index>.bin and its path, type and gzip flag to DUMP/table.txt, which
// `gen_http_assets.py --check` holds against core/net/http_assets (every entry decompresses to its source)
#include <stdio.h>
#include <string.h>
#include "net/http_assets.h"
#include "http_assets_gen.h"

int main(int argc, char **argv)
{
	char name[512];
	int failures = 0;

	if (argc < 2) {
		return 2;
	}
	snprintf(name, sizeof(name), "%s/table.txt", argv[1]);
	FILE *t = fopen(name, "w");
	if (t == NULL) {
		return 2;
	}
	for (int i = 0; i < HTTP_ASSET_COUNT; i++) {
		const http_asset_t *a = &http_assets[i];

		if ((a->path[0] != '/') || (a->length == 0) || (a->gzip != HTTP_ASSETS_GZIP) ||
		    (a->gzip && ((a->data[0] != 0x1F) || (a->data[1] != 0x8B)))) {
			printf("FAIL entry %d (%s)\n", i, a->path);
			failures++;
		}
		fprintf(t, "%d\t%s\t%s\t%u\n", i, a->path, a->type, a->gzip);
		snprintf(name, sizeof(name), "%s/%d.bin", argv[1], i);
		FILE *f = fopen(name, "wb");
		if ((f == NULL) || (fwrite(a->data, 1, a->length, f) != a->length)) {
			failures++;
		}
		if (f != NULL) {
			fclose(f);
		}
	}
	fclose(t);
	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (%d failures)\n", failures);
	return failures ? 1 : 0;
}
