// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// TinyUSB configuration for every radio that uses it (CFG_TUSB_MCU comes from the platform's make file).
// Serial mode: one CDC-ACM function. Network mode (CONFIG_USB_NETWORK): one CDC-NCM function, the other configuration
// (usb_device.c picks the descriptors); ACM and NCM can't both fit the STM32F405's three IN endpoints
// (docs/spec/hardware-dm1701.md 16).
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
#if defined(CONFIG_USB_NETWORK) && CONFIG_USB_NETWORK
#define CFG_TUD_NCM			1
// NTB16 only. OUT NTBs up to 2048 bytes (Linux needs a full Ethernet frame plus headers), IN NTBs one datagram each
// (the gateway's frames are at most 600 bytes); one buffer each way (spec 16)
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE	2048
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE	1024
#define CFG_TUD_NCM_OUT_NTB_N		1
#define CFG_TUD_NCM_IN_NTB_N		1
#define CFG_TUD_NCM_IN_MAX_DATAGRAMS_PER_NTB	1
#else
#define CFG_TUD_NCM			0
#endif

// An MMDVM DMR frame is 37 bytes and the host can send several back to back; a CPS block read reply is up to about
// 1 KB. 64-byte endpoints (full speed bulk maximum)
#define CFG_TUD_CDC_RX_BUFSIZE		512
#define CFG_TUD_CDC_TX_BUFSIZE		512
#define CFG_TUD_CDC_RX_EPSIZE		64
#define CFG_TUD_CDC_TX_EPSIZE		64

#endif
