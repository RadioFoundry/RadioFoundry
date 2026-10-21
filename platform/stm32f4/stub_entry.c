// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Runtime entry to the STM32F4 flash stub (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18, docs/spec/stub-stm32f4.md).
//
// The debug link loads the stub image into a staging buffer in ODR's own RAM while ODR runs. The image is linked to
// run at the SRAM base (0x20000000), which ODR's .data and .bss hold until the jump, so enter_stub masks everything,
// stops the DMA controllers (a stream still writing into SRAM would corrupt the copied image), copies the staging buffer
// down to the SRAM base and jumps there. Code stays in flash until the jump and the task stack is in CCM, so the copy
// overwrites nothing it still needs. The clocks, the GPIO levels (power latch) and the USB core's state are kept: the
// stub polls the same OTG_FS core and adopts the configured CDC-ACM device when it can (handoff flag in r0).
#include "config.h"
#if CONFIG_STUB_ENTRY
#include <stdint.h>
#include "platform.h"
#include "board.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#endif

#define STUB_RUN_ADDR		0x20000000u	// SRAM1 base: the stub's link address (stub/stm32f4/f4stub.ld)
#define STUB_STAGE_SIZE		(8u * 1024u)	// the biggest stub image ODR accepts
#define STUB_FLAG_USB_ADOPT	0x1u		// r0 at entry: the OTG_FS core is a configured CDC-ACM device (ours)

static uint8_t stage[STUB_STAGE_SIZE] __attribute__((aligned(8)));

uint8_t *stm32_stub_region(uint32_t *size)
{
	*size = sizeof(stage);
	return stage;
}

static uint32_t handoff_flags(void)
{
#if CONFIG_USB
	// In network mode the endpoints look the same (bulk 0x81 / 0x01, interrupt 0x82) but the host bound NCM, so the
	// stub must enumerate again as a serial port
	if (usb_device_mode() == USB_MODE_SERIAL) {
		return STUB_FLAG_USB_ADOPT;
	}
#endif
	return 0;
}

bool stm32_enter_stub(uint32_t entry)
{
	if (entry < STUB_RUN_ADDR || entry >= STUB_RUN_ADDR + STUB_STAGE_SIZE) {
		return false;
	}
	// The copy below goes downward (destination at or below the source), so a forward word copy is safe even if the
	// staging buffer overlaps the run span
	if ((uint32_t)(uintptr_t)stage < STUB_RUN_ADDR) {
		return false;
	}
	uint32_t flags = handoff_flags();
	board_rf_safe();		// transmitter off; the power latch and the display stay as they are

	__disable_irq();
	SysTick->CTRL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	// Stop every DMA stream (I2S audio, SPI, ADC run circular into SRAM); the USB core has no DMA on the FS port
	RCC->AHB1RSTR |= RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST;
	RCC->AHB1RSTR &= ~(RCC_AHB1RSTR_DMA1RST | RCC_AHB1RSTR_DMA2RST);
	__DSB();

	volatile uint32_t *dst = (volatile uint32_t *)STUB_RUN_ADDR;
	const volatile uint32_t *src = (const volatile uint32_t *)(uintptr_t)stage;
	for (uint32_t i = 0; i < STUB_STAGE_SIZE / 4u; i++) {
		dst[i] = src[i];
	}
	__DSB();
	__ISB();

	// Thread mode on the main stack (CONTROL = 0 also drops the FPU context flag); the stub sets its own SP first
	uint32_t pc = entry | 1u;
	__asm volatile(
		"msr control, %[zero]\n\t"
		"isb\n\t"
		"mov r0, %[flags]\n\t"
		"bx %[pc]\n\t"
		:
		: [zero] "r"(0u), [flags] "r"(flags), [pc] "r"(pc)
		: "r0", "memory");
	__builtin_unreachable();
}

#endif	// CONFIG_STUB_ENTRY
