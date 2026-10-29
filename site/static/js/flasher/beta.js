// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The beta notice (templates/partials/beta_notice.html): shown when the flasher page opens, once per browser session and
// again once 12 hours have passed since it was closed, so a tab left open still sees it. Storage that throws (a private
// window, blocked site data) only means it shows again next time.

const KEY = "rfoundry.betaNoticeSeen";	// when it was last closed, in ms since the epoch
export const BETA_NOTICE_EVERY_MS = 12 * 3600 * 1000;

export function showBetaNotice(doc = globalThis.document, store = () => globalThis.sessionStorage, now = Date.now) {
	const dlg = doc?.getElementById("beta-notice");
	if (!dlg?.showModal) return false;
	try {
		const seen = Number(store()?.getItem(KEY));
		if (seen && now() - seen < BETA_NOTICE_EVERY_MS) return false;
	} catch (e) { /* no storage: show it */ }
	// the button, Escape and the form all close it; any close counts as read
	dlg.addEventListener("close", () => {
		try {
			store()?.setItem(KEY, String(now()));
		} catch (e) { /* not kept: shown again on the next visit */ }
	}, { once: true });
	dlg.showModal();
	dlg.scrollTop = 0;	// opened at its title (focused), not scrolled to a link further down
	return true;
}
