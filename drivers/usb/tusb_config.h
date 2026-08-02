// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// TinyUSB configuration for every radio that uses it (CFG_TUSB_MCU comes from the platform's make file).
// Serial mode only for now: one CDC-ACM function. Network mode (CDC-NCM) will be a second configuration chosen at
// USB start; ACM and NCM can't both fit the STM32F405's three IN endpoints (docs/spec/hardware-dm1701.md 16).
#ifndef ODR_TUSB_CONFIG_H
#define ODR_TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be set by the platform
#endif

#define CFG_TUSB_OS			OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG			0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN		__attribute__((aligned(4)))

#define CFG_TUD_ENABLED			1
#define CFG_TUD_MAX_SPEED		OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE		64
#define CFG_TUD_DWC2_SLAVE_ENABLE	1	// no DMA: the F405 OTG_FS has none
#define CFG_TUD_VBUS_DETECT_HW		0	// VBUS sensing off: the radio is a device whenever it is plugged in

#define CFG_TUD_CDC			1
#define CFG_TUD_CDC_NOTIFY		0
#define CFG_TUD_MSC			0
#define CFG_TUD_HID			0
#define CFG_TUD_MIDI			0
#define CFG_TUD_VENDOR			0
#define CFG_TUD_NCM			0

// An MMDVM DMR frame is 37 bytes and the host can send several back to back; a CPS block read reply is up to about
// 1 KB. 64-byte endpoints (full speed bulk maximum)
#define CFG_TUD_CDC_RX_BUFSIZE		512
#define CFG_TUD_CDC_TX_BUFSIZE		512
#define CFG_TUD_CDC_RX_EPSIZE		64
#define CFG_TUD_CDC_TX_EPSIZE		64

#endif
