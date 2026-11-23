// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The browser's Back button walks the flasher's steps, as the page's own Back button does. Every step shown gets a
// history entry; a step left with the page's own buttons rewinds to its entry, so the two never disagree. Forward is
// refused, because a step's state may no longer hold after going back, and so is Back while a transfer runs.

// show(place) draws a place the browser went back to; busy() says a transfer is running
export function stepHistory(win, { busy, show }) {
	const { history } = win;
	const id = Math.random().toString(36).slice(2);	// entries of an earlier load of the page aren't this trail's
	const trail = [];
	let pos = 0, moving = 0, last = null;
	const state = (n) => ({ rfStepTrail: id, n });

	function sync(place) {
		last = place;
		if (!trail.length) {
			trail.push(place);
			history.replaceState(state(0), "");
			return;
		}
		if (moving || place === trail[pos]) return;	// a rewind under way syncs again when it lands
		const earlier = trail.lastIndexOf(place, pos);
		if (earlier >= 0) {
			const by = earlier - pos;
			pos = earlier;
			trail.length = pos + 1;
			moving++;
			history.go(by);
			return;
		}
		trail.length = pos + 1;
		trail.push(place);
		pos++;
		history.pushState(state(pos), "");
	}

	win.addEventListener("popstate", (e) => {
		if (moving) {
			moving--;
			if (!moving) sync(last);
			return;
		}
		if (e.state?.rfStepTrail !== id || e.state.n === pos) return;
		const to = e.state.n;
		if (to >= trail.length || busy()) {
			moving++;
			history.go(pos - to);
			return;
		}
		pos = to;
		trail.length = pos + 1;
		show(trail[pos]);
	});
	return { sync };
}
