// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A radio in USB network mode (docs/spec/usb-network.md 1) has no serial port, and a page can send it neither UDP nor,
// from an https page, a plain http fetch. Two ways are left to ask it for the serial personality, which lasts until
// the radio restarts or is switched back (Link.usbNetwork):
//   - WebUSB: a vendor request on endpoint 0 (drivers/usb/usb_device.h USB_REQ_SERIAL_MODE). Works where the browser
//     can open a device whose interfaces the OS network driver holds (Android, macOS), not on Windows (no WinUSB
//     driver on it) or on Linux without a udev rule for the device.
//   - a new tab on the radio's own page, SERIAL_MODE_URL (docs/spec/tcp-services.md 5): a navigation is allowed
//     where a fetch is not. The tab closes itself. Needs the link up (the host routes 12.0.0.0/8 through the radio).

import { RFOUNDRY_NET, RFOUNDRY_USB, serialApi, serialSupported } from "./link.js";
import { sleep } from "./bytes.js";

export const SERIAL_MODE_REQUEST = 0x53;
// 12.255.255.254: the radio on this link, whatever its DMR ID (core/dmr/ipGateway.h IPGW_ANY_RADIO_IP). An address,
// not the mDNS name: a sandboxed browser (Flatpak, Snap) resolves no .local names
export const SERIAL_MODE_URL = "http://12.255.255.254/api/usb?mode=serial";

const isNet = (d) => d.vendorId === RFOUNDRY_NET.vendorId && d.productId === RFOUNDRY_NET.productId;

// Radios in network mode this page may use over WebUSB (granted before: no prompt)
export async function grantedNetworkRadios(usb = globalThis.navigator?.usb) {
	if (!usb) return [];
	try {
		return (await usb.getDevices()).filter(isNet);
	} catch {
		return [];
	}
}

// The endpoint 0 request, to each device until one takes it. false: none could be opened (the OS driver has it)
export async function requestOverUsb(devices) {
	for (const d of devices) {
		try {
			await d.open();
			await d.controlTransferOut({ requestType: "vendor", recipient: "device", request: SERIAL_MODE_REQUEST, value: 0, index: 0 });
			try { await d.close(); } catch { /* it left the bus already */ }
			return true;
		} catch {
			try { await d.close(); } catch { /* never opened */ }
		}
	}
	return false;
}

// The radio's page in a new tab. Only from a click (a popup otherwise); noopener: the radio's page gets no handle on
// this one, so whether the tab opened can't be told
export function requestOverHttp(open = globalThis.open?.bind(globalThis)) {
	open?.(SERIAL_MODE_URL, "_blank", "noopener");
}

// The serial port of a radio this page was granted before is there (the browser lists it without a prompt)
export async function serialPortPresent(serial = serialSupported() ? serialApi() : null) {
	if (!serial) return false;
	try {
		return (await serial.getPorts()).some((p) => {
			const i = p.getInfo();
			return i.usbVendorId === RFOUNDRY_USB.usbVendorId && i.usbProductId === RFOUNDRY_USB.usbProductId;
		});
	} catch {
		return false;
	}
}

// Ask a radio in network mode for its serial port and wait for it. http: false leaves the tab out (not in a click).
// {how: "usb" | "http" | null, back}: back is true when a port granted before showed up within ms; a radio this page
// never had a port of can't be seen coming back, so the caller then asks the user to connect
export async function toSerial({ usb, serial, open, http = true, ms = 8000, step = 250 } = {}) {
	let how = (await requestOverUsb(await grantedNetworkRadios(usb))) ? "usb" : null;
	if (!how && http) {
		requestOverHttp(open);
		how = "http";
	}
	if (!how) return { how, back: false };
	for (let waited = 0; waited < ms; waited += step) {
		await sleep(step);
		if (await serialPortPresent(serial)) return { how, back: true };
	}
	return { how, back: false };
}
