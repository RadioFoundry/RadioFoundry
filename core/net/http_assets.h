// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The HTTP status server's static files (docs/spec/tcp-services.md 5): every file of core/net/http_assets, put into a
// table at build time by tools/gen_http_assets.py (http_assets_gen.h in the build's gen directory). With
// CONFIG_NET_HTTP_GZIP they are stored gzip compressed and served with Content-Encoding: gzip, else as they are.
#ifndef ODR_HTTP_ASSETS_H
#define ODR_HTTP_ASSETS_H

#include <stdint.h>

typedef struct {
	const char *path;	// "/" for index.html, else "/<file name>"
	const char *type;	// Content-Type
	const uint8_t *data;	// the bytes served
	uint32_t length;
	uint8_t gzip;		// data is gzip compressed: served with Content-Encoding: gzip
} http_asset_t;

#endif
