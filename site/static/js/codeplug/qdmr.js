// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// qdmr's YAML codeplug (docs/spec/qdmr-yaml.md) to and from the editing model (model.js). An import replaces every
// table of the model and the general settings qdmr has, as one change; an export writes what the model holds in the
// form qdmr 0.15 writes for this codeplug. Both return notes: what the other side can't hold, so the page can say what
// was left out. The mapping was learned from qdmr's manual and from files qdmr writes and reads, never its source.

import { parse, stringify, flow, Tag } from "./yaml.js";
import { LIMITS } from "./records.js";
import { parseTone } from "./fields.js";

export class QdmrError extends Error {
	constructor(code, params = {}) {
		super(code);
		this.code = code;
		this.params = params;
	}
}

export const QDMR_VERSION = "0.15.1";

// Power: qdmr's level names and the channel's power byte (open-firmware-cps.md 7.3.3)
const POWER = { Min: 1, Low: 3, Mid: 6, High: 9, Max: 10 };
const ALIAS = ["None", "APRS", "Text", "Both"];
const CALL = { GroupCall: "group", PrivateCall: "private", AllCall: "all" };
const ALL_CALL_ID = 0xFFFFFF;

// qdmr's APRS icon names and the symbol each stands for in the primary table ('/'), as qdmr encodes them
const ICONS = {
	PoliceStation: "!", None: '"', Digipeater: "#", Phone: "$", DXCluster: "%", HFGateway: "&", SmallPlane: "'",
	MobileSatelliteStation: "(", WheelChair: ")", Snowmobile: "*", RedCross: "+", BoyScout: ",", Home: "-", X: ".",
	RedDot: "/", Circle0: "0", Circle1: "1", Circle2: "2", Circle3: "3", Circle4: "4", Circle5: "5", Circle6: "6",
	Circle7: "7", Circle8: "8", Circle9: "9", Fire: ":", Campground: ";", Motorcycle: "<", RailEngine: "=", Car: ">",
	FileServer: "?", HCFuture: "@", AidStation: "A", BBS: "B", Canoe: "C", Eyeball: "E", Tractor: "F", GridSquare: "G",
	Hotel: "H", TCPIP: "I", School: "K", Logon: "L", MacOS: "M", NTSStation: "N", Balloon: "O", PoliceCar: "P", TBD: "Q",
	RV: "R", Shuttle: "S", SSTV: "T", Bus: "U", ATV: "V", WXService: "W", Helo: "X", Yacht: "Y", Windows: "Z",
	Jogger: "[", Triangle: "\\", PBBS: "]", LargePlane: "^", WXStation: "_", DishAntenna: "`", Ambulance: "a", Bike: "b",
	ICP: "c", FireStation: "d", Horse: "e", FireTruck: "f", Glider: "g", Hospital: "h", IOTA: "i", Jeep: "j",
	SmallTruck: "k", Laptop: "l", MicE: "m", Node: "n", EOC: "o", Rover: "p", Grid: "q", Antenna: "r", PowerBoat: "s",
	TruckStop: "t", TruckLarge: "u", Van: "v", Water: "w", XAPRS: "x", Yagi: "y", Shelter: "z",
};
const ICON_OF = Object.fromEntries(Object.entries(ICONS).map(([k, v]) => [v, k]));

// --- Values ---

const isDefault = (v) => v instanceof Tag && v.tag === "!default";
const untag = (v) => (v instanceof Tag ? v.value : v);
const text = (v) => (v == null ? "" : String(untag(v)));

// A frequency in Hz on the 10 Hz grid: "439.5625 MHz", "145 kHz", or a bare number in MHz; null if not one
export function parseFrequency(v) {
	v = untag(v);
	if (typeof v === "number") return v > 0 ? Math.round(v * 1e5) * 10 : null;
	const m = /^\s*([0-9]*\.?[0-9]+)\s*(GHz|MHz|kHz|Hz)?\s*$/i.exec(String(v ?? ""));
	if (!m) return null;
	const scale = { ghz: 1e9, mhz: 1e6, khz: 1e3, hz: 1 }[(m[2] || "MHz").toLowerCase()];
	const hz = Math.round((Number(m[1]) * scale) / 10) * 10;
	return hz > 0 && hz < 1e10 ? hz : null;
}

export function frequencyText(hz) {
	return `${Number((hz / 1e6).toFixed(5))} MHz`;
}

// A duration in milliseconds: "3 min", "45 s", "500 ms", a bare number in seconds; 0 for off or infinite; null for
// !default or nothing
export function parseDuration(v) {
	if (v == null || isDefault(v)) return null;
	v = untag(v);
	if (typeof v === "number") return v * 1000;
	const s = String(v).trim();
	if (/^(infinite|off|none|disabled?)$/i.test(s)) return 0;
	const m = /^([0-9]*\.?[0-9]+)\s*(ms|s|sec|min|h)?$/i.exec(s);
	if (!m) return null;
	return Number(m[1]) * { ms: 1, s: 1000, sec: 1000, min: 60000, h: 3600000 }[(m[2] || "s").toLowerCase()];
}

function seconds(ms) {
	return ms % 60000 === 0 && ms ? `${ms / 60000} min` : ms % 1000 === 0 ? `${ms / 1000} s` : `${ms} ms`;
}

// Maidenhead locator (4, 6 or 8 characters) to the center of its square; null if not a locator
export function fromLocator(loc) {
	const s = String(loc ?? "").trim().toUpperCase();
	if (!/^[A-R]{2}[0-9]{2}([A-X]{2}([0-9]{2})?)?$/.test(s)) return null;
	let lon = (s.charCodeAt(0) - 65) * 20 - 180 + Number(s[2]) * 2;
	let lat = (s.charCodeAt(1) - 65) * 10 - 90 + Number(s[3]);
	let w = 2, h = 1;
	if (s.length >= 6) {
		w = 2 / 24;
		h = 1 / 24;
		lon += (s.charCodeAt(4) - 65) * w;
		lat += (s.charCodeAt(5) - 65) * h;
	}
	if (s.length === 8) {
		w /= 10;
		h /= 10;
		lon += Number(s[6]) * w;
		lat += Number(s[7]) * h;
	}
	return { lat: round4(lat + h / 2), lon: round4(lon + w / 2) };
}

// The 6-character locator of a position
export function toLocator(lat, lon) {
	const x = Math.min(Math.max(lon + 180, 0), 359.9999), y = Math.min(Math.max(lat + 90, 0), 179.9999);
	const A = (n) => String.fromCharCode(65 + n), a = (n) => String.fromCharCode(97 + n);
	return A(Math.floor(x / 20)) + A(Math.floor(y / 10)) + Math.floor((x % 20) / 2) + Math.floor(y % 10) +
		a(Math.floor((x % 2) * 12)) + a(Math.floor((y % 1) * 24));
}

const round4 = (x) => Math.round(x * 10000) / 10000;

// A sub-tone: {ctcss: 67} or {ctcss: 67 Hz}, {dcs: 23} (negative = inverted); "" for none, null if not a tone
function parseSubTone(v) {
	v = untag(v);
	if (v == null || v === "") return "";
	if (typeof v !== "object") return null;
	if ("ctcss" in v) {
		const f = parseFloat(String(untag(v.ctcss)));
		return parseTone(Number.isFinite(f) ? f.toFixed(1) : "x");
	}
	if ("dcs" in v) {
		const raw = String(untag(v.dcs)).trim();
		const m = /^([-+niNI])?0*([0-7]{1,3})$/.exec(raw);
		if (!m) return null;
		return "D" + m[2].padStart(3, "0") + (m[1] === "-" || /i/i.test(m[1] || "") ? "I" : "N");
	}
	return null;
}

function subTone(t) {
	if (!t) return null;
	if (t.startsWith("D")) return flow({ dcs: (t.endsWith("I") ? -1 : 1) * Number(t.slice(1, 4)) });
	return flow({ ctcss: `${Number(t)} Hz` });
}

// A call sign and SSID: "N0CALL-7" to {call, ssid}
function callSsid(v) {
	const [call, ssid] = text(v).toUpperCase().split("-");
	const n = Number(ssid);
	return { call: call || "", ssid: Number.isInteger(n) && n >= 0 && n <= 15 ? n : 0 };
}

// --- Import ---

// The single element of {dmr: {...}} and its type
function element(x) {
	if (!x || typeof x !== "object" || Array.isArray(x)) return null;
	const keys = Object.keys(x);
	if (keys.length !== 1 || !x[keys[0]] || typeof x[keys[0]] !== "object") return null;
	return { type: keys[0], v: x[keys[0]] };
}

// The model built from a qdmr YAML file, written into m: {counts, notes}. Throws QdmrError
export function importQdmr(m, yamlText) {
	let doc;
	try {
		doc = parse(yamlText);
	} catch (e) {
		throw new QdmrError("bad_yaml", { error: e.message });
	}
	if (!doc || typeof doc !== "object" || Array.isArray(doc) || !(doc.channels || doc.contacts || doc.radioIDs)) {
		throw new QdmrError("not_qdmr");
	}
	const notes = new Map();
	const note = (code, n = 1) => notes.set(code, (notes.get(code) || 0) + n);
	const list = (k) => (Array.isArray(doc[k]) ? doc[k] : []);
	const settings = doc.settings && typeof doc.settings === "object" ? doc.settings : {};
	const shorten = (s, max) => {
		s = text(s);
		if (s.length <= max) return s;
		note("shortened");
		return s.slice(0, max);
	};

	// Radio IDs: the default one is the radio's; another one a channel names becomes that channel's DMR ID
	const ids = new Map();
	for (const x of list("radioIDs")) {
		const e = element(x);
		if (e && (e.type === "dmr" || e.type === "digital")) ids.set(text(e.v.id), { name: text(e.v.name), number: Number(untag(e.v.number)) || 0 });
		else note("radio_id_type");
	}
	const defId = ids.get(text(settings.defaultID)) || [...ids.values()][0] || null;

	// Contacts
	const contacts = [], dtmfContacts = [], contactById = new Map();
	for (const x of list("contacts")) {
		const e = element(x);
		if (!e) continue;
		if (e.type === "dmr" || e.type === "digital") {
			const type = CALL[text(e.v.type)];
			if (!type) {
				note("contact_type");
				continue;
			}
			const ov = text(e.v.openGD77?.timeSlotOverride);
			const c = { number: null, name: shorten(e.v.name, LIMITS.name), id: type === "all" ? ALL_CALL_ID : Number(untag(e.v.number)) || 0, type, timeslot: ov === "TS1" ? 1 : ov === "TS2" ? 2 : 0 };
			contacts.push(c);
			contactById.set(text(e.v.id), c);
		} else if (e.type === "dtmf") {
			dtmfContacts.push({ number: null, name: shorten(e.v.name, LIMITS.name), code: text(e.v.number).toUpperCase() });
		} else note("contact_type");
	}

	const rxLists = [], listById = new Map();
	for (const g of list("groupLists")) {
		if (!g || typeof g !== "object") continue;
		const members = (Array.isArray(g.contacts) ? g.contacts : []).map((r) => contactById.get(text(r))).filter(Boolean);
		if (members.length > LIMITS.rxListContacts) note("list_cut");
		const l = { number: null, name: shorten(g.name, LIMITS.name), contacts: members.slice(0, LIMITS.rxListContacts) };
		rxLists.push(l);
		listById.set(text(g.id), l);
	}

	// Positioning: APRS systems become APRS configurations; DMR position reporting has no counterpart
	const gnss = settings.gnss && typeof settings.gnss === "object" ? settings.gnss : {};
	const fixed = gnss.fixedPositionEnabled === true ? fromLocator(text(gnss.fixedPosition)) : null;
	const aprs = [], aprsById = new Map(), reverts = [];
	for (const x of list("positioning")) {
		const e = element(x);
		if (!e) continue;
		if (e.type !== "aprs") {
			note("dmr_positioning");
			continue;
		}
		const v = e.v;
		const path = (Array.isArray(v.path) ? v.path : []).map((p) => text(p).toUpperCase());
		if (path.length > 2) note("path_cut");
		const icon = ICONS[text(v.icon)];
		if (v.icon != null && icon === undefined) note("icon_unknown");
		const o = v.opengd77 || v.openGD77 || {};
		const a = {
			number: null, name: shorten(v.name, LIMITS.aprsName), ssid: callSsid(v.source).ssid,
			lat: fixed ? fixed.lat : 0, lon: fixed ? fixed.lon : 0, path1: path[0] || "", path2: path[1] || "",
			symTable: "/", symCode: icon && icon !== '"' ? icon : ">", comment: shorten(v.message, LIMITS.aprsComment),
			txHz: 0, baud300: Number(untag(o.baud)) === 300, useFixed: !!fixed, qsy: o.transmitQSY === true, silent: false, ambiguity: 0,
		};
		if (parseDuration(v.period)) note("aprs_period");
		aprs.push(a);
		aprsById.set(text(v.id), a);
		reverts.push([a, text(untag(v.revert))]);
	}

	// Channels
	const audio = settings.audio && typeof settings.audio === "object" ? settings.audio : {};
	const voxOn = (v) => {
		if (v == null || isDefault(v)) return audio.vox != null && text(audio.vox) !== "off" && Number(untag(audio.vox)) !== 0;
		v = untag(v);
		return typeof v === "number" ? v > 0 : v === true;
	};
	const channels = [], chById = new Map();
	for (const x of list("channels")) {
		const e = element(x);
		if (!e) continue;
		const dmr = e.type === "dmr" || e.type === "digital";
		if (!dmr && e.type !== "fm" && e.type !== "analog") {
			note("channel_type");
			continue;
		}
		const v = e.v;
		const rxHz = parseFrequency(v.rxFrequency), txHz = parseFrequency(v.txFrequency ?? v.rxFrequency);
		if (!rxHz || !txHz) {
			note("bad_frequency");
			continue;
		}
		const o = v.openGD77 && typeof v.openGD77 === "object" ? v.openGD77 : {};
		const loc = fromLocator(text(o.location));
		const tot = parseDuration(v.timeout);
		const c = {
			number: null, name: shorten(v.name, LIMITS.name), rxHz, txHz, mode: dmr ? "dmr" : "fm",
			power: POWER[text(v.power)] ?? 0, tot: tot ? Math.min(Math.ceil(tot / 15000) * 15, 255 * 15) : 0,
			rxTone: "", txTone: "", bw: 12.5, rxOnly: v.rxOnly === true, allSkip: o.scanAllSkip === true,
			zoneSkip: o.scanZoneSkip === true, vox: voxOn(v.vox), squelch: 0, colorCode: 0, timeslot: 1, contact: null,
			rxList: null, aprs: null, dmrId: 0, noBeep: o.beep === false, noEco: o.powerSave === false, dmo: false,
			roam: false, aliasTs1: Math.max(0, ALIAS.indexOf(text(o.talkerAliasTS1))),
			aliasTs2: Math.max(0, ALIAS.indexOf(text(o.talkerAliasTS2))), useLocation: !!loc, lat: loc ? loc.lat : 0,
			lon: loc ? loc.lon : 0, sfr: false, sfrA: 0, sfrB: 0,
		};
		if (v.scanList != null) note("scan_lists");
		if (dmr) {
			c.colorCode = Math.min(Math.max(Number(untag(v.colorCode)) || 0, 0), 15);
			c.timeslot = text(v.timeSlot) === "TS2" ? 2 : 1;
			c.contact = contactById.get(text(v.contact)) || null;
			c.rxList = listById.get(text(v.groupList)) || null;
			const id = v.radioId ?? v.radioID;
			if (id != null && !isDefault(id)) {
				const r = ids.get(text(id));
				if (r && r !== defId) c.dmrId = r.number;
			}
			c.roam = v.roaming != null;
			c.dmo = v.extended?.talkaround === true;
		} else {
			c.bw = text(v.bandwidth) === "Wide" ? 25 : 12.5;
			for (const k of ["rxTone", "txTone"]) {
				const t = parseSubTone(v[k]);
				if (t === null) note("bad_tone");
				else c[k] = t;
			}
			const sq = untag(v.squelch);
			if (typeof sq === "number" && sq > 0) c.squelch = Math.min(sq * 2, 21);
		}
		const ap = aprsById.get(text(v.aprs));
		if (ap) c.aprs = ap;
		channels.push(c);
		chById.set(text(v.id), c);
	}
	for (const [a, ref] of reverts) {
		const ch = chById.get(ref);
		if (ch) a.txHz = ch.txHz;
	}

	// Zones: qdmr's A and B lists; a zone with both becomes two, "<name> A" and "<name> B", as qdmr writes them here
	const zones = [];
	for (const z of list("zones")) {
		if (!z || typeof z !== "object") continue;
		const pick = (k) => (Array.isArray(z[k]) ? z[k] : []).map((r) => chById.get(text(r))).filter(Boolean);
		const A = pick("A"), B = pick("B");
		const name = text(z.name);
		if (B.length) {
			zones.push({ name: shorten(`${name} A`, LIMITS.name), channels: A, slot: null });
			zones.push({ name: shorten(`${name} B`, LIMITS.name), channels: B, slot: null });
		} else zones.push({ name: shorten(name, LIMITS.name), channels: A, slot: null });
	}
	for (const z of zones) {
		if (z.channels.length > LIMITS.zoneChannels) {
			note("zone_cut");
			z.channels = z.channels.slice(0, LIMITS.zoneChannels);
		}
	}

	for (const k of ["scanLists", "roaming"]) {
		const v = doc[k];
		if (v && (Array.isArray(v) ? v.length : Object.values(v).some((x) => Array.isArray(x) ? x.length : x))) note(k === "scanLists" ? "scan_lists" : "roaming");
	}
	if (doc.commercial?.encryptionKeys?.length) note("encryption");

	// Into the model: numbers in file order, so the radio shows the channels as qdmr lists them
	const tables = { channels, contacts, rxLists, dtmfContacts, aprs, zones };
	const max = { channels: LIMITS.channels, contacts: LIMITS.contacts, rxLists: LIMITS.rxLists, dtmfContacts: LIMITS.dtmfContacts, aprs: LIMITS.aprs, zones: LIMITS.zones };
	for (const [t, rows] of Object.entries(tables)) {
		if (rows.length > max[t]) note("too_many_" + t, rows.length - max[t]);
		if (t !== "zones") rows.forEach((r, i) => (r.number = i < max[t] ? i + 1 : null));
	}
	Object.assign(m, tables);
	// the VFOs keep their frequencies; references into the old tables can't stay
	for (const v of m.vfo) if (v) Object.assign(v, { contact: null, rxList: null, aprs: null });
	const g = m.general;
	if (defId) {
		g.dmrId = defId.number;
		g.radioName = shorten(defId.name, LIMITS.radioName);
	}
	const boot = settings.boot && typeof settings.boot === "object" ? settings.boot : {};
	if (boot.message1 != null) g.bootLine1 = shorten(boot.message1, LIMITS.bootText);
	if (boot.message2 != null) g.bootLine2 = shorten(boot.message2, LIMITS.bootText);
	const dmrSet = settings.dmr && typeof settings.dmr === "object" ? settings.dmr : {};
	const hang = (v) => {
		const ms = parseDuration(v);
		return ms == null ? null : Math.min(Math.round(ms / 500), 255);
	};
	g.groupHang = hang(dmrSet.groupCallHangTime) ?? g.groupHang;
	g.privateHang = hang(dmrSet.privateCallHangTime) ?? g.privateHang;

	const counts = Object.fromEntries(Object.entries(tables).map(([t, rows]) => [t, rows.length]));
	return { counts, notes: [...notes].map(([code, n]) => ({ code, n })) };
}

// --- Export ---

// qdmr YAML for the model: {text, notes}
export function exportQdmr(m) {
	const notes = new Map();
	const note = (code, n = 1) => notes.set(code, (notes.get(code) || 0) + n);
	const g = m.general;
	const num = (list) => new Map(list.map((x, i) => [x, i + 1]));

	// Radio IDs: the radio's own, then one per channel DMR ID
	const radioIDs = [{ dmr: flow({ id: "id1", name: g.radioName || String(g.dmrId || "ID"), number: g.dmrId || 0 }) }];
	const idRef = new Map();
	for (const c of m.channels) {
		if (!c.dmrId || c.dmrId === g.dmrId || idRef.has(c.dmrId)) continue;
		const id = `id${radioIDs.length + 1}`;
		idRef.set(c.dmrId, id);
		radioIDs.push({ dmr: flow({ id, name: String(c.dmrId), number: c.dmrId }) });
	}

	const cn = num(m.contacts);
	const contacts = m.contacts.map((c) => ({
		dmr: flow({
			id: `cont${cn.get(c)}`, name: c.name, ring: false,
			type: { group: "GroupCall", private: "PrivateCall", all: "AllCall" }[c.type],
			number: c.type === "all" ? ALL_CALL_ID : c.id,
			openGD77: flow({ timeSlotOverride: c.timeslot ? `TS${c.timeslot}` : "None" }),
		}),
	}));
	m.dtmfContacts.forEach((d, i) => contacts.push({ dtmf: flow({ id: `cont${m.contacts.length + i + 1}`, name: d.name, number: d.code }) }));
	const cref = (c) => (c && typeof c === "object" && cn.has(c) ? `cont${cn.get(c)}` : undefined);

	const ln = num(m.rxLists);
	const groupLists = m.rxLists.map((l) => flow({ id: `grp${ln.get(l)}`, name: l.name, contacts: flow(l.contacts.map(cref).filter(Boolean)) }));

	const an = num(m.aprs);
	const chn = num(m.channels);
	const chRef = (c) => `ch${chn.get(c)}`;

	const channels = m.channels.map((c) => {
		const og = {
			scanZoneSkip: c.zoneSkip, scanAllSkip: c.allSkip, beep: !c.noBeep, powerSave: !c.noEco,
			location: c.useLocation ? toLocator(c.lat, c.lon) : "",
			talkerAliasTS1: ALIAS[c.aliasTs1] || "None", talkerAliasTS2: ALIAS[c.aliasTs2] || "None",
		};
		if (c.useLocation) note("location_rounded");
		if (c.sfr || c.sfrA || c.sfrB) note("sfr");
		const powerName = Object.keys(POWER).find((k) => POWER[k] === c.power);
		if (c.power && !powerName) note("power_level");
		const common = {
			id: chRef(c), name: c.name, rxFrequency: frequencyText(c.rxHz), txFrequency: frequencyText(c.txHz),
			timeout: c.tot ? seconds(c.tot * 1000) : "infinite", rxOnly: c.rxOnly, vox: c.vox ? 1 : "off", openGD77: og,
		};
		const aprsRef = c.aprs && typeof c.aprs === "object" && an.has(c.aprs) ? `aprs${an.get(c.aprs)}` : undefined;
		const power = powerName || new Tag("!default", "");
		if (c.mode === "dmr") {
			return {
				dmr: {
					...common, admit: "Always", colorCode: c.colorCode, timeSlot: `TS${c.timeslot}`,
					radioId: c.dmrId && idRef.has(c.dmrId) ? idRef.get(c.dmrId) : new Tag("!default", ""),
					groupList: c.rxList && typeof c.rxList === "object" && ln.has(c.rxList) ? `grp${ln.get(c.rxList)}` : undefined,
					contact: cref(c.contact), aprs: aprsRef, roaming: c.roam ? new Tag("!default", "") : undefined,
					extended: { talkaround: c.dmo }, power,
				},
			};
		}
		return {
			fm: {
				...common, squelch: c.squelch ? Math.min(Math.max(Math.round(c.squelch / 2), 1), 10) : new Tag("!default", ""),
				admit: "Always", rxTone: subTone(c.rxTone), txTone: subTone(c.txTone), bandwidth: c.bw === 25 ? "Wide" : "Narrow",
				aprs: aprsRef, power,
			},
		};
	});

	const zones = m.zones.map((z, i) => ({
		id: `zone${i + 1}`, name: z.name,
		A: flow(z.channels.filter((c) => c && typeof c === "object" && chn.has(c)).map(chRef)), B: flow([]),
	}));

	// APRS: the call comes from the radio name, the revert channel is an FM channel on the configuration's frequency
	const call = (g.radioName || "").toUpperCase().replace(/[^A-Z0-9]/g, "").slice(0, 6) || "N0CALL";
	const positioning = m.aprs.map((a) => {
		const revert = a.txHz ? m.channels.find((c) => c.mode === "fm" && c.txHz === a.txHz) : null;
		if (a.txHz && !revert) note("aprs_no_channel");
		const icon = a.symTable === "/" ? ICON_OF[a.symCode] : undefined;
		if (!icon) note("icon_export");
		if (a.silent || a.ambiguity) note("aprs_flags");
		return {
			aprs: {
				id: `aprs${an.get(a)}`, name: a.name, revert: revert ? chRef(revert) : undefined, icon: icon || "None",
				message: a.comment, opengd77: { transmitQSY: a.qsy, baud: a.baud300 ? 300 : 1200 },
				destination: "APN000-0", source: `${call}-${a.ssid}`, path: flow([a.path1, a.path2].filter(Boolean)),
			},
		};
	});
	const fixedAprs = m.aprs.find((a) => a.useFixed);

	const settings = { defaultID: "id1", boot: { message1: g.bootLine1, message2: g.bootLine2 } };
	if (fixedAprs) settings.gnss = { fixedPositionEnabled: true, fixedPosition: toLocator(fixedAprs.lat, fixedAprs.lon) };
	const dmrSet = {};
	if (g.privateHang) dmrSet.privateCallHangTime = seconds(g.privateHang * 500);
	if (g.groupHang) dmrSet.groupCallHangTime = seconds(g.groupHang * 500);
	if (Object.keys(dmrSet).length) settings.dmr = dmrSet;

	const doc = { version: QDMR_VERSION, settings, radioIDs, contacts, groupLists, channels, zones, positioning };
	return { text: "---\n" + stringify(doc) + "...\n", notes: [...notes].map(([code, n]) => ({ code, n })) };
}
