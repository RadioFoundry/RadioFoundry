// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The beta notice (templates/partials/beta_notice.html): shown when the flasher page opens, until it is dismissed once in
// this browser session. Storage that throws (a private window, blocked site data) only means it shows again next time.

const KEY = "rfoundry.betaNoticeSeen";

export function showBetaNotice(doc = globalThis.document, store = () => globalThis.sessionStorage) {
	const dlg = doc?.getElementById("beta-notice");
	if (!dlg?.showModal) return false;
	try {
		if (store()?.getItem(KEY)) return false;
	} catch (e) { /* no storage: show it */ }
	// the button, Escape and the form all close it; any close counts as read
	dlg.addEventListener("close", () => {
		try {
			store()?.setItem(KEY, "1");
		} catch (e) { /* not kept: shown again on the next visit */ }
	}, { once: true });
	dlg.showModal();
	dlg.scrollTop = 0;	// opened at its title (focused), not scrolled to a link further down
	return true;
}
