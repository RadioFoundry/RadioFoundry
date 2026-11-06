// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USB port glue for TinyUSB on the AT32F43x OTGFS1 core, a Synopsys DWC2 like the STM32F4's OTG_FS (TinyUSB's dwc2
// driver, OPT_MCU_AT32F435_437; at32f435_437.h here stands in for Artery's header). Device only, PA11 (DM) / PA12 (DP)
// on multiplexed function 10, the core's own D+ pull-up, VBUS not sensed. The 48 MHz clock is the HICK, kept on
// frequency by the ACC from the host's start of frame, as the stock firmware runs it (docs/spec/hardware-d578uv.md 10).
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"
#include "tusb.h"

#define OTGFS_PRIORITY		6	// at or below the RTOS max syscall level 4
#define OTGFS_AF		10u
// HICK / 6 cycles in a 1 ms frame: 8000 at 48 MHz, with the stock firmware's margin either side
#define ACC_WINDOW_LOW		7980u
#define ACC_WINDOW_MID		8000u
#define ACC_WINDOW_HIGH		8020u

void usb_port_init(void)
{
	at32_pin_af(GPIOA, AT32_PIN(11) | AT32_PIN(12), OTGFS_AF, false);
	// The HICK's 48 MHz to the USB, in the stock firmware's order, then the ACC with its window
	CRM_MISC1 |= CRM_MISC1_USB_HICK_A;
	CRM_MISC1 |= CRM_MISC1_USB_HICK_B;
	CRM_MISC1 |= CRM_MISC1_USB_HICK_C;
	CRM_APB2EN |= CRM_APB2EN_ACCEN;
	(void)CRM_APB2EN;
	ACC_C1 = ACC_WINDOW_LOW;
	ACC_C2 = ACC_WINDOW_MID;
	ACC_C3 = ACC_WINDOW_HIGH;
	ACC_CTRL1 |= 1u << 1;
	ACC_CTRL1 |= 1u << 0;
	CRM_AHBEN2 |= CRM_AHBEN2_OTGFS1EN;
	(void)CRM_AHBEN2;
	// The bootloader may have left the core running: start from reset values
	CRM_AHBRST2 |= CRM_AHBRST2_OTGFS1RST;
	CRM_AHBRST2 &= ~CRM_AHBRST2_OTGFS1RST;
	// No VBUS pin: the session is always valid (TinyUSB also forces the B session valid, vbus_sensing off)
	OTGFS1_GCCFG |= OTGFS_GCCFG_VBUSIG;

	NVIC_SetPriority(OTGFS1_IRQn, OTGFS_PRIORITY);
	// TinyUSB enables the interrupt itself (dcd_int_enable) once the core is set up
}

void usb_port_soft_disconnect(bool disconnect)
{
	if (disconnect) {
		tud_disconnect();
	} else {
		tud_connect();
	}
}

void OTGFS1_IRQHandler(void)
{
	tud_int_handler(0);
}

void at32_usb_serial_number(char *buf, uint8_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)AT32_UID_BASE;
	uint8_t n = 0;

	for (uint8_t i = 0; (i < 12) && (n + 2 < len); i++) {
		buf[n++] = hex[uid[i] >> 4];
		buf[n++] = hex[uid[i] & 0x0F];
	}
	buf[n] = '\0';
}

#endif	// CONFIG_USB
