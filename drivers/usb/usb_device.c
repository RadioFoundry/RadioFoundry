// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// USB device on TinyUSB: descriptors, the stack's task and the CDC-ACM stream (see usb_device.h)
#include <string.h>
#include "usb/usb_device.h"
#include "usb/usb_port.h"
#include "config.h"
#if CONFIG_USB_NETWORK
#include "usb/usb_ncm.h"
#endif
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define USB_TASK_PRIORITY	3	// with input; above the UI (core/app/app.c task table)
#define USB_TASK_STACK		256	// words
#define WRITE_TIMEOUT_MS	100	// give up on a host that stopped reading
#define POWER_MA		100	// self powered (battery); what it may draw from the bus

enum { STR_LANGUAGE, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_NCM, STR_MAC, STR_COUNT };
enum { ITF_CDC_COMM, ITF_CDC_DATA, ITF_COUNT };
#define STR_MS_OS		0xEE	// Microsoft OS 1.0 string descriptor: Windows 10 binds NCM from it (spec 16)
#define MS_VENDOR_CODE		0x47
#define RECONNECT_GAP_MS	100	// D+ released this long on a personality switch, so the host sees an unplug

// docs/spec/hardware-dm1701.md 16: interrupt IN 0x82 (8 bytes), bulk OUT 0x01 / IN 0x81 (64 bytes)
#define EP_CDC_NOTIFY		0x82
#define EP_CDC_OUT		0x01
#define EP_CDC_IN		0x81
#define CDC_NOTIFY_SIZE		8
#define CDC_DATA_SIZE		64

static volatile usb_mode_t mode = USB_MODE_SERIAL;
static SemaphoreHandle_t rx_ready;
static volatile bool started;

void usb_device_set_mode(usb_mode_t m)
{
#if !CONFIG_USB_NETWORK
	m = USB_MODE_SERIAL;	// not built
#endif
	if (!started) {
		mode = m;
	}
}

bool usb_device_switch_mode(usb_mode_t m)
{
#if CONFIG_USB_NETWORK
	if (!started) {
		mode = m;
		return true;
	}
	if (m == mode) {
		return true;
	}
	tud_disconnect();
	vTaskDelay(pdMS_TO_TICKS(RECONNECT_GAP_MS));
	mode = m;	// the descriptors follow it from here on
	tud_connect();
	return true;
#else
	return m == USB_MODE_SERIAL;
#endif
}

usb_mode_t usb_device_mode(void)
{
	return mode;
}

bool usb_device_configured(void)
{
	return started && tud_mounted();
}

// ---- Descriptors (TinyUSB asks for them from its task) ----

static tusb_desc_device_t device_desc = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	// Interface association: CDC is two interfaces
	.bDeviceClass = TUSB_CLASS_MISC,
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.iManufacturer = STR_MANUFACTURER,
	.iProduct = STR_PRODUCT,
	.iSerialNumber = STR_SERIAL,
	.bNumConfigurations = 1,
};

#define CONFIG_SERIAL_LEN	(TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t config_serial[CONFIG_SERIAL_LEN] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_SERIAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, POWER_MA),
	TUD_CDC_DESCRIPTOR(ITF_CDC_COMM, STR_CDC, EP_CDC_NOTIFY, CDC_NOTIFY_SIZE, EP_CDC_OUT, EP_CDC_IN, CDC_DATA_SIZE),
};

#if CONFIG_USB_NETWORK
// docs/spec/hardware-dm1701.md 16: notification IN 0x82 (16 bytes, 16 ms), bulk OUT 0x01 / IN 0x81 (64 bytes)
enum { ITF_NCM_COMM, ITF_NCM_DATA, ITF_NCM_COUNT };
#define EP_NCM_NOTIFY		0x82
#define EP_NCM_OUT		0x01
#define EP_NCM_IN		0x81
#define NCM_NOTIFY_SIZE		16
#define NCM_NOTIFY_INTERVAL	16
#define NCM_DATA_SIZE		64
#define CONFIG_NETWORK_LEN	(TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static const uint8_t config_network[CONFIG_NETWORK_LEN] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_NCM_COUNT, 0, CONFIG_NETWORK_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, POWER_MA),
	TUD_CDC_NCM_DESCRIPTOR(ITF_NCM_COMM, STR_NCM, STR_MAC, EP_NCM_NOTIFY, NCM_NOTIFY_SIZE, EP_NCM_OUT, EP_NCM_IN,
			       NCM_DATA_SIZE, CFG_TUD_NET_MTU, NCM_NOTIFY_INTERVAL, 0),
};

// Windows 10: the extended compat ID "WINNCM" for interface 0 (Microsoft OS 1.0 descriptors, vendor code 0x47)
static const uint8_t ms_compat_id[40] = {
	40, 0, 0, 0,		// dwLength
	0x00, 0x01,		// bcdVersion 1.00
	0x04, 0x00,		// wIndex: extended compat ID
	1,			// bCount: one function section
	0, 0, 0, 0, 0, 0, 0,	// reserved
	ITF_NCM_COMM, 1,	// bFirstInterfaceNumber, reserved
	'W', 'I', 'N', 'N', 'C', 'M', 0, 0,	// compatibleID
	0, 0, 0, 0, 0, 0, 0, 0,		// subCompatibleID
	0, 0, 0, 0, 0, 0,		// reserved
};

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	if ((mode != USB_MODE_NETWORK) || (request->bRequest != MS_VENDOR_CODE) || (request->wIndex != 0x0004)) {
		return false;	// stalled
	}
	if (stage != CONTROL_STAGE_SETUP) {
		return true;
	}
	uint16_t len = (request->wLength < sizeof(ms_compat_id)) ? request->wLength : sizeof(ms_compat_id);
	return tud_control_xfer(rhport, request, (void *)(uintptr_t)ms_compat_id, len);
}
#endif

uint8_t const *tud_descriptor_device_cb(void)
{
	device_desc.idVendor = board_usb_ids.vid;
	device_desc.idProduct = board_usb_ids.pid_serial;
#if CONFIG_USB_NETWORK
	if (mode == USB_MODE_NETWORK) {
		device_desc.idProduct = board_usb_ids.pid_network;	// a separate PID, so Windows binds another driver
	}
#endif
	device_desc.bcdDevice = board_usb_ids.bcd_device;
	return (const uint8_t *)&device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
#if CONFIG_USB_NETWORK
	if (mode == USB_MODE_NETWORK) {
		return config_network;
	}
#endif
	return config_serial;
}

static uint16_t string_buf[1 + 32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	char serial[25];
	const char *s = NULL;
	uint8_t n = 0;

	switch (index) {
	case STR_LANGUAGE:
		string_buf[1] = 0x0409;	// English (US)
		n = 1;
		break;
	case STR_MANUFACTURER:
		s = board_usb_ids.manufacturer;
		break;
	case STR_PRODUCT:
		s = board_usb_ids.product;
		break;
	case STR_SERIAL:
		serial[0] = '\0';
		if (board_usb_ids.serial_number != NULL) {
			board_usb_ids.serial_number(serial, sizeof(serial));
		}
		if (serial[0] == '\0') {
			strcpy(serial, "0");
		}
		s = serial;
		break;
	case STR_CDC:
		s = board_usb_ids.serial_interface;
		break;
#if CONFIG_USB_NETWORK
	case STR_NCM:
		s = "DMR IP gateway";
		break;
	case STR_MAC: {
		static const char hex[] = "0123456789ABCDEF";
		uint8_t host[6], radio[6];

		usbNcmMacs(host, radio);
		for (int i = 0; i < 6; i++) {
			serial[2 * i] = hex[host[i] >> 4];
			serial[2 * i + 1] = hex[host[i] & 0x0F];
		}
		serial[12] = '\0';
		s = serial;
		break;
	}
	case STR_MS_OS:
		if (mode != USB_MODE_NETWORK) {
			return NULL;
		}
		s = "MSFT100";
		break;
#endif
	default:
		return NULL;
	}

	if (s != NULL) {
		while ((s[n] != '\0') && (n < 32)) {
			string_buf[1 + n] = (uint8_t)s[n];
			n++;
		}
	}
#if CONFIG_USB_NETWORK
	if (index == STR_MS_OS) {
		string_buf[1 + n++] = MS_VENDOR_CODE;	// bMS_VendorCode, bPad 0
	}
#endif
	string_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
	return string_buf;
}

// ---- Stack task and callbacks ----

void tud_cdc_rx_cb(uint8_t itf)
{
	(void)itf;
	if (rx_ready != NULL) {
		xSemaphoreGive(rx_ready);
	}
}

static void usb_task(void *arg)
{
	(void)arg;
	const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };

	tusb_init(0, &init);	// in the task that runs tud_task (TinyUSB's rule with an RTOS)
	started = true;
	for (;;) {
		tud_task();	// blocks on the stack's event queue
	}
}

// ---- The CDC-ACM stream ----

static bool cdc_start(void)
{
	if (rx_ready != NULL) {
		return true;
	}
	rx_ready = xSemaphoreCreateBinary();
	if (rx_ready == NULL) {
		return false;
	}
	usb_port_init();
	return xTaskCreate(usb_task, "usb", USB_TASK_STACK, NULL, USB_TASK_PRIORITY, NULL) == pdPASS;
}

static bool cdc_connected(void)
{
	return started && (mode == USB_MODE_SERIAL) && tud_cdc_connected();
}

static size_t cdc_read(uint8_t *buf, size_t max)
{
	if (!started || (tud_cdc_available() == 0)) {
		return 0;
	}
	return tud_cdc_read(buf, (uint32_t)max);
}

static bool cdc_wait_rx(uint32_t timeout_ms)
{
	if (started && (tud_cdc_available() > 0)) {
		return true;
	}
	if (rx_ready == NULL) {
		vTaskDelay(pdMS_TO_TICKS(timeout_ms));
		return false;
	}
	xSemaphoreTake(rx_ready, pdMS_TO_TICKS(timeout_ms));
	return started && (tud_cdc_available() > 0);
}

static size_t cdc_write(const uint8_t *buf, size_t len)
{
	size_t done = 0;
	uint32_t waited = 0;

	if (!started) {
		return 0;
	}
	while (done < len) {
		uint32_t n = tud_cdc_write(buf + done, (uint32_t)(len - done));

		done += n;
		tud_cdc_write_flush();
		if (n == 0) {
			if (!tud_cdc_connected() || (waited >= WRITE_TIMEOUT_MS)) {
				break;
			}
			vTaskDelay(1);
			waited++;
		}
	}
	return done;
}

const hal_stream_t usb_cdc_stream = {
	.start = cdc_start,
	.connected = cdc_connected,
	.read = cdc_read,
	.wait_rx = cdc_wait_rx,
	.write = cdc_write,
};
