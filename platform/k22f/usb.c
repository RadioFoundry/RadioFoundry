// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// USB port glue for TinyUSB on the K22F's full-speed USB controller (portable/chipidea/ci_fs, the buffer descriptor
// table driver), docs/spec/hardware-gd77.md 13: device only, the dedicated USB0_DP / USB0_DM pins, the USB0 interrupt
// at priority 6. No PLL plan gives 48 MHz exactly (49.152 and 95.232 MHz), so the controller runs from the internal
// 48 MHz oscillator (IRC48M) trimmed to the host's start-of-frame packets by the controller's clock recovery.
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"
#include "tusb.h"

#define USB0_PRIORITY		6	// at or below the RTOS max syscall level 5, like the STM32 radios
#define PLLFLLSEL_IRC48M	3u

void usb_port_init(void)
{
	SIM_SCGC4 |= SIM_SCGC4_USBOTG;
	// The controller's regulator (on after reset; the radio's bootloader may have changed it)
	SIM_SOPT1CFG |= SIM_SOPT1CFG_URWE;
	SIM_SOPT1 |= SIM_SOPT1_USBREGEN;
	// IRC48M on, then the USB clock from it, divided by 1. The selector also feeds the I2S master clock, which this
	// radio doesn't use (the codec drives the I2S clocks)
	USB0_CLK_RECOVER_IRC_EN = USB_CLK_RECOVER_IRC_EN_REG | USB_CLK_RECOVER_IRC_EN_IRC;
	SIM_CLKDIV2 &= ~SIM_CLKDIV2_USB_MASK;
	SIM_SOPT2 = (SIM_SOPT2 & ~SIM_SOPT2_PLLFLLSEL_MASK) | SIM_SOPT2_PLLFLLSEL(PLLFLLSEL_IRC48M) | SIM_SOPT2_USBSRC;
	// Trimmed to the host's SOF once frames come; untrimmed IRC48M is outside the USB tolerance
	USB0_CLK_RECOVER_CTRL |= USB_CLK_RECOVER_EN;

	NVIC_SetPriority(USB0_IRQn, USB0_PRIORITY);
	// TinyUSB enables the interrupt itself (dcd_int_enable) once the controller is set up
}

void usb_port_soft_disconnect(bool disconnect)
{
	if (disconnect) {
		tud_disconnect();
	} else {
		tud_connect();
	}
}

// The clock recovery ran out of trim range (no SOF for long, or a host far off): counted for the diagnostics page
bool k22f_usb_trim_error(void)
{
	return (USB0_CLK_RECOVER_INT_STATUS & USB_CLK_RECOVER_OVF_ERROR) != 0;
}

void USB0_IRQHandler(void)
{
	tud_int_handler(0);
}

#endif	// CONFIG_USB
