// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

const BULLETIN_URL = "https://www.amsat.org/tle/daily-bulletin.txt";
const MAX_BYTES = 1000000;

export async function onRequestGet() {
	try {
		const upstream = await fetch(BULLETIN_URL, {
			cache: "no-store",
			headers: { Accept: "text/plain", "User-Agent": "RadioFoundry site TLE updater" },
		});
		if (!upstream.ok) return new Response("AMSAT bulletin is temporarily unavailable", { status: 502 });
		const body = await upstream.arrayBuffer();
		if (body.byteLength > MAX_BYTES) return new Response("AMSAT bulletin exceeds the size limit", { status: 502 });
		return new Response(body, {
			headers: {
				"Content-Type": "text/plain; charset=utf-8",
				"Cache-Control": "no-store",
				"X-Content-Type-Options": "nosniff",
			},
		});
	} catch {
		return new Response("AMSAT bulletin is temporarily unavailable", { status: 502 });
	}
}