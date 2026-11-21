// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning's arithmetic (docs/spec/power-tune.md): band names and default tuning frequencies, watts in level names,
// the reading's error in dB and the next drive to try. No DOM, so the host tests run it.

// Amateur bands a transmit band may hold, and the frequency a tuning starts at (the band's middle, on a channel step)
const HAM = [
	{ lo: 28000, hi: 29700, name: "10 m", khz: 29000 },
	{ lo: 50000, hi: 54000, name: "6 m", khz: 52000 },
	{ lo: 144000, hi: 148000, name: "2 m", khz: 146000 },
	{ lo: 219000, hi: 225000, name: "1.25 m", khz: 223500 },
	{ lo: 420000, hi: 450000, name: "70 cm", khz: 445000 },
	{ lo: 902000, hi: 928000, name: "33 cm", khz: 915000 },
];

// The usual name of a radio band, by where it sits
function region(midKhz) {
	if (midKhz < 30000) return "HF";
	if (midKhz < 88000) return "Low VHF";
	if (midKhz < 300000) return midKhz > 200000 ? "220" : "VHF";
	if (midKhz < 600000) return "UHF";
	return "900";
}

// {name, ham, khz}: "2 m / VHF" where an amateur band is inside, else the region; khz where tuning starts
export function bandInfo(loKhz, hiKhz) {
	const ham = HAM.find((h) => h.lo >= loKhz && h.hi <= hiKhz) ||
		HAM.find((h) => h.khz >= loKhz && h.khz <= hiKhz);
	const reg = region((loKhz + hiKhz) / 2);
	if (ham) return { name: ham.name === reg ? reg : `${ham.name} / ${reg}`, ham: ham.name, khz: ham.khz };
	return { name: reg, ham: null, khz: snapKhz((loKhz + hiKhz) / 2) };
}

// A frequency on a 12.5 kHz step (every radio's FM steps include it)
export function snapKhz(khz) {
	return Math.round(khz / 12.5) * 12.5;
}

// kHz as MHz text: "146.000", "446.0125"
export function mhzText(khz) {
	const mhz = khz / 1000;
	const s = mhz.toFixed(4);
	return s.endsWith("0") ? mhz.toFixed(3) : s;
}

// MHz text a person typed ("146.52", "446,0125") as whole kHz, or null
export function parseMhz(text) {
	const m = String(text ?? "").trim().replace(",", ".").match(/^(\d{1,4})(?:\.(\d{0,6}))?$/);
	if (!m) return null;
	const khz = Math.round(parseFloat(`${m[1]}.${m[2] || "0"}`) * 1000);
	return khz > 0 ? khz : null;
}

// Watts in a level name ("50mW" 0.05, "2.5W" 2.5, "5 W" 5), else null
export function parseWatts(name) {
	const m = String(name ?? "").trim().replace(",", ".").match(/^([0-9]*\.?[0-9]+)\s*(m?)W$/i);
	if (!m) return null;
	const w = parseFloat(m[1]) / (m[2] ? 1000 : 1);
	return w > 0 ? w : null;
}

// Watts as a short name, at most 6 characters: 0.05 "50mW", 2.5 "2.5W", 25 "25W"
export function wattsName(w) {
	if (!(w > 0)) return "";
	if (w < 1) return `${+(w * 1000).toPrecision(3)}mW`.slice(0, 6);
	return `${+w.toPrecision(3)}W`.slice(0, 6);
}

// Watts a person typed ("4.8", "500 mW", "0,5"), or null
export function parseReading(text) {
	const s = String(text ?? "").trim().replace(",", ".");
	if (!s) return null;
	const m = s.match(/^([0-9]*\.?[0-9]+)\s*(m?)w?$/i);
	if (!m) return null;
	const w = parseFloat(m[1]) / (m[2] ? 1000 : 1);
	return w > 0 ? w : null;
}

export function db(watts, target) {
	return watts > 0 && target > 0 ? 10 * Math.log10(watts / target) : NaN;
}

// "good" within 0.5 dB, "near" within 1 dB (2 dB under 1 W: meters read low powers coarsely), else "off"
export function grade(watts, target) {
	const e = Math.abs(db(watts, target));
	if (!Number.isFinite(e)) return null;
	if (e <= 0.5) return "good";
	return e <= (target < 1 ? 2 : 1) ? "near" : "off";
}

// The slope of ln(watts) over drive from the levels around one, by their factory drives and the watts their names
// give; null when no two levels name their power. levels: [{factory, watts}]
export function neighborSlope(levels, level) {
	const known = levels.map((l, i) => ({ i, d: l.factory, w: l.watts })).filter((l) => l.w > 0 && l.d > 0);
	if (known.length < 2) return null;
	known.sort((a, b) => Math.abs(a.i - level) - Math.abs(b.i - level) || a.i - b.i);
	const [a, b] = known;
	if (a.d === b.d) return null;
	const k = (Math.log(b.w) - Math.log(a.w)) / (b.d - a.d);
	return Number.isFinite(k) && k > 0 ? k : null;
}

// The next drive to try for target watts: readings [{drive, watts}] at this level and frequency (the latest last).
// Two readings at different drives: the secant of ln(watts) over drive through the two nearest the target. One: that
// reading with the slope of the neighboring levels, else power as the drive squared. Clamped to [lo, hi]. null when
// nothing can be said
export function suggestDrive(readings, target, { lo, hi, slope = null }) {
	const rs = readings.filter((r) => r.watts > 0 && r.drive > 0);
	if (!(target > 0) || !rs.length) return null;
	const lt = Math.log(target);
	let d = null;
	const near = [...rs].sort((a, b) => Math.abs(Math.log(a.watts) - lt) - Math.abs(Math.log(b.watts) - lt));
	const other = near.find((r) => r.drive !== near[0].drive);
	if (other) {
		const k = (Math.log(other.watts) - Math.log(near[0].watts)) / (other.drive - near[0].drive);
		if (Number.isFinite(k) && k > 0) d = near[0].drive + (lt - Math.log(near[0].watts)) / k;
	}
	if (d === null) {
		const r = rs[rs.length - 1];
		d = slope ? r.drive + (lt - Math.log(r.watts)) / slope : r.drive * Math.sqrt(target / r.watts);
	}
	if (!Number.isFinite(d)) return null;
	return Math.max(lo, Math.min(hi, Math.round(d)));
}

// The offset at khz of one level from its points [{khz, delta}] in the band: linear between the two around khz, flat
// past the outer ones, 0 with none (the radio's own rule, power-tune.md 2)
export function offsetAt(points, khz) {
	if (!points.length) return 0;
	const ps = [...points].sort((a, b) => a.khz - b.khz);
	if (khz <= ps[0].khz) return ps[0].delta;
	if (khz >= ps[ps.length - 1].khz) return ps[ps.length - 1].delta;
	const i = ps.findIndex((p) => p.khz >= khz);
	const a = ps[i - 1], b = ps[i];
	return Math.round(a.delta + (b.delta - a.delta) * (khz - a.khz) / (b.khz - a.khz));
}
