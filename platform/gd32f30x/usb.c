// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// USB port glue for TinyUSB on the GD32F30x USBD core, which is the STM32F103's full-speed device register for register
// (TinyUSB's stm32_fsdev driver, built as OPT_MCU_STM32F1 through stm32f1xx.h here). Device only, PA11 (DM) / PA12
// (DP); the D+ pull-up is the board's (board_usb_pullup). The 48 MHz clock is CK_PLL divided down (clock.c).
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"
#include "tusb.h"

#define USBD_PRIORITY		6	// at or below the RTOS max syscall level 4

__attribute__((weak)) void board_usb_pullup(bool on)
{
	(void)on;
}

void usb_port_init(void)
{
	gd32_pin_in(GPIOA, GD32_PIN(11) | GD32_PIN(12), GD32_PULL_NONE);	// the core takes the pins when enabled
	RCU_APB1EN |= RCU_APB1EN_USBDEN;
	(void)RCU_APB1EN;
	// The bootloader may have left the core running: start from reset values
	RCU_APB1RST |= RCU_APB1RST_USBDRST;
	RCU_APB1RST &= ~RCU_APB1RST_USBDRST;

	NVIC_SetPriority(USBD_HP_CAN0_TX_IRQn, USBD_PRIORITY);
	NVIC_SetPriority(USBD_LP_CAN0_RX0_IRQn, USBD_PRIORITY);
	board_usb_pullup(true);
	// TinyUSB enables the interrupts itself (dcd_int_enable) once the core is set up
}

void usb_port_soft_disconnect(bool disconnect)
{
	board_usb_pullup(!disconnect);
}

// The fsdev core has no pull-up of its own: TinyUSB's connect and disconnect are the board's pin
void dcd_connect(uint8_t rhport);
void dcd_disconnect(uint8_t rhport);
void dcd_connect(uint8_t rhport)
{
	(void)rhport;
	board_usb_pullup(true);
}

void dcd_disconnect(uint8_t rhport)
{
	(void)rhport;
	board_usb_pullup(false);
}

void USBD_HP_CAN0_TX_IRQHandler(void)
{
	tud_int_handler(0);
}

void USBD_LP_CAN0_RX0_IRQHandler(void)
{
	tud_int_handler(0);
}

void gd32_usb_serial_number(char *buf, uint8_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)GD32_UID_BASE;
	uint8_t n = 0;

	for (uint8_t i = 0; (i < 12) && (n + 2 < len); i++) {
		buf[n++] = hex[uid[i] >> 4];
		buf[n++] = hex[uid[i] & 0x0F];
	}
	buf[n] = '\0';
}

#endif	// CONFIG_USB
