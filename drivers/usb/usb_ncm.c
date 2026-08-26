// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The network personality's frames on TinyUSB's NCM class (see usb_ncm.h). TinyUSB's network calls run in the USB
// task only: frames from the host are copied into a small ring in its receive callback, and a frame to the host is
// handed over through usbd_defer_func(), so the consumer's task never touches the class driver.
#include "config.h"
#if CONFIG_USB_NETWORK	// the whole file
#include <string.h>
#include "usb/usb_ncm.h"
#include "usb/usb_device.h"
#include "usb/usb_port.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "FreeRTOS.h"
#include "task.h"

#define COLD_BOOT_QUIET_MS	5000	// configured but nothing from the host this long after mounting: re-enumerate once
#define RECONNECT_GAP_MS	100

typedef struct {
	uint16_t len;
	uint8_t data[USB_NCM_FRAME_MAX];
} frame_t;

static frame_t rx_ring[USB_NCM_RX_FRAMES];
static volatile uint8_t rx_in, rx_out;		// producer: the USB task; consumer: usbNcmReceive's task
static frame_t tx_frame;
static volatile bool tx_pending;		// tx_frame holds a frame not yet given to the class driver
static volatile bool tx_deferred;		// a kick is queued for the USB task
static usb_ncm_stats_t stats;
static uint32_t mounted_ms;			// when the host configured the device, 0 = not
static bool host_heard;				// a frame came from the host since mounting
static bool reconnected;			// the cold boot re-enumeration was done

static void make_macs(uint8_t host[6], uint8_t radio[6])
{
	char serial[25] = "0";
	uint32_t h = 2166136261u;	// FNV-1a over the serial number string (the chip's unique ID)

	if (board_usb_ids.serial_number != NULL) {
		board_usb_ids.serial_number(serial, sizeof(serial));
	}
	for (const char *s = serial; *s != '\0'; s++) {
		h = (h ^ (uint8_t)*s) * 16777619u;
	}
	host[0] = 0x02;			// locally administered, unicast
	host[1] = 0x44;
	host[2] = (uint8_t)(h >> 24);
	host[3] = (uint8_t)(h >> 16);
	host[4] = (uint8_t)(h >> 8);
	host[5] = (uint8_t)(h & 0xFE);
	memcpy(radio, host, 6);
	radio[5] |= 0x01;
}

void usbNcmMacs(uint8_t host[6], uint8_t radio[6])
{
	make_macs(host, radio);
}

bool usbNcmIsUp(void)
{
	return (usb_device_mode() == USB_MODE_NETWORK) && usb_device_configured();
}

// ---- TinyUSB's callbacks (USB task) ----

void tud_network_init_cb(void)
{
	rx_in = rx_out = 0;
	tx_pending = false;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
	uint8_t next = (uint8_t)((rx_in + 1) % USB_NCM_RX_FRAMES);

	host_heard = true;
	if ((size > USB_NCM_FRAME_MAX) || (next == rx_out)) {
		stats.rx_dropped++;		// too big for the gateway, or the consumer is behind: dropped, never held
	} else {
		rx_ring[rx_in].len = size;
		memcpy(rx_ring[rx_in].data, src, size);
		__sync_synchronize();
		rx_in = next;
		stats.rx_frames++;
	}
	return true;	// always taken: the driver's buffer is free again at once
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
	(void)ref;
	memcpy(dst, tx_frame.data, arg);
	return arg;
}

static void tx_kick(void *param)
{
	(void)param;
	tx_deferred = false;
	if (!tx_pending) {
		return;
	}
	if (!tud_ready() || !tud_network_can_xmit(tx_frame.len)) {
		stats.tx_busy++;
		return;		// usbNcmTick asks again
	}
	tud_network_xmit(NULL, tx_frame.len);
	stats.tx_frames++;
	__sync_synchronize();
	tx_pending = false;
}

static void defer_kick(void)
{
	if (!tx_deferred) {
		tx_deferred = true;
		usbd_defer_func(tx_kick, NULL, false);
	}
}

// ---- The consumer's side ----

bool usbNcmSendFrame(const uint8_t *frame, int length)
{
	if (!usbNcmIsUp() || (length <= 0) || (length > USB_NCM_FRAME_MAX) || tx_pending) {
		return false;
	}
	memcpy(tx_frame.data, frame, (size_t)length);
	tx_frame.len = (uint16_t)length;
	__sync_synchronize();
	tx_pending = true;
	defer_kick();
	return true;
}

int usbNcmReceive(uint8_t *buf, int max)
{
	if (rx_out == rx_in) {
		return 0;
	}
	__sync_synchronize();
	const frame_t *f = &rx_ring[rx_out];
	int n = (f->len < max) ? f->len : max;

	memcpy(buf, f->data, (size_t)n);
	rx_out = (uint8_t)((rx_out + 1) % USB_NCM_RX_FRAMES);
	return n;
}

void usbNcmTick(uint32_t now_ms)
{
	if (!usbNcmIsUp()) {
		mounted_ms = 0;
		return;
	}
	if (tx_pending) {
		defer_kick();
	}
	if (mounted_ms == 0) {
		mounted_ms = (now_ms != 0) ? now_ms : 1;
		host_heard = false;
	} else if (!host_heard && !reconnected && ((now_ms - mounted_ms) >= COLD_BOOT_QUIET_MS)) {
		// macOS sometimes never activates the link after a cold boot in network mode: one re-enumeration fixes it
		reconnected = true;
		stats.reconnects++;
		tud_disconnect();
		vTaskDelay(pdMS_TO_TICKS(RECONNECT_GAP_MS));
		tud_connect();
		mounted_ms = 0;
	}
}

void usbNcmStats(usb_ncm_stats_t *s)
{
	*s = stats;
}

static bool netif_select(bool network)
{
	return usb_device_switch_mode(network ? USB_MODE_NETWORK : USB_MODE_SERIAL);
}

static bool netif_selected(void)
{
	return usb_device_mode() == USB_MODE_NETWORK;
}

const hal_netif_t usb_ncm_netif = {
	.select = netif_select, .selected = netif_selected, .up = usbNcmIsUp, .send = usbNcmSendFrame,
	.receive = usbNcmReceive, .tick = usbNcmTick, .macs = usbNcmMacs, .frame_max = USB_NCM_FRAME_MAX,
};

#endif	// CONFIG_USB_NETWORK
