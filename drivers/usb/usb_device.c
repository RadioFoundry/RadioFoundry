// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// USB device on TinyUSB: descriptors, the stack's task and the CDC-ACM stream (see usb_device.h)
#include <string.h>
#include "usb/usb_device.h"
#include "usb/usb_port.h"
#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define USB_TASK_PRIORITY	3	// with input; above the UI (core/app/app.c task table)
#define USB_TASK_STACK		256	// words
#define WRITE_TIMEOUT_MS	100	// give up on a host that stopped reading
#define POWER_MA		100	// self powered (battery); what it may draw from the bus

enum { STR_LANGUAGE, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_COUNT };
enum { ITF_CDC_COMM, ITF_CDC_DATA, ITF_COUNT };

// docs/spec/hardware-dm1701.md 16: interrupt IN 0x82 (8 bytes), bulk OUT 0x01 / IN 0x81 (64 bytes)
#define EP_CDC_NOTIFY		0x82
#define EP_CDC_OUT		0x01
#define EP_CDC_IN		0x81
#define CDC_NOTIFY_SIZE		8
#define CDC_DATA_SIZE		64

static usb_mode_t mode = USB_MODE_SERIAL;
static SemaphoreHandle_t rx_ready;
static volatile bool started;

void usb_device_set_mode(usb_mode_t m)
{
	if (!started) {
		mode = m;
	}
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

uint8_t const *tud_descriptor_device_cb(void)
{
	device_desc.idVendor = board_usb_ids.vid;
	device_desc.idProduct = board_usb_ids.pid_serial;	// network mode will report pid_network
	device_desc.bcdDevice = board_usb_ids.bcd_device;
	return (const uint8_t *)&device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
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
	default:
		return NULL;
	}

	if (s != NULL) {
		while ((s[n] != '\0') && (n < 32)) {
			string_buf[1 + n] = (uint8_t)s[n];
			n++;
		}
	}
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
	return started && tud_cdc_connected();
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
