// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// USB port glue for TinyUSB on the STM32F4 OTG_FS core (Synopsys DWC2), docs/spec/hardware-dm1701.md section 16:
// device only, embedded PHY on PA11 (DM) / PA12 (DP), VBUS sensing off, OTG_FS interrupt at priority 6.
// The 48 MHz USB clock is PLL Q (144 / 3 on the DM-1701), set up with the system clock.
#include "config.h"
#if CONFIG_USB	// the whole file
#include "config.h"
#if CONFIG_USB
#include "platform.h"
#include "usb/usb_port.h"
#include "tusb.h"

#define OTG_FS_PRIORITY		6	// docs/spec/hardware-dm1701.md 3.1 (at or below the RTOS max syscall level 5)

void usb_port_init(void)
{
	stm32_pin_af(GPIOA, GPIO_PIN_11 | GPIO_PIN_12, GPIO_AF10_OTG_FS, false, GPIO_NOPULL);
	__HAL_RCC_USB_OTG_FS_CLK_ENABLE();
	(void)RCC->AHB2ENR;

	// The bootloader may have left the core running: start from reset values
	__HAL_RCC_USB_OTG_FS_FORCE_RESET();
	__HAL_RCC_USB_OTG_FS_RELEASE_RESET();

	HAL_NVIC_SetPriority(OTG_FS_IRQn, OTG_FS_PRIORITY, 0);
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

void OTG_FS_IRQHandler(void)
{
	tud_int_handler(0);
}

#endif

#endif	// CONFIG_USB
