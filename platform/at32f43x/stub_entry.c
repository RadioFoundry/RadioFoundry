// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Runtime entry to the AT32F43x flash stub (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18, docs/spec/stub-at32f43x.md 3).
//
// These radios have no RAM to spare for a staging buffer, so the core loads the image into the display's frame buffer
// with the UI held (core/stubentry, hal_system_t.enter_stub_staged) and calls here once it verifies. The image is linked
// to run at the SRAM base, which RadioFoundry's .data and .bss hold until the jump, so this masks everything, stops the
// DMA channels (one still writing into SRAM would corrupt the copy), copies the image down to the SRAM base and jumps
// there. Code stays in flash and the calling task's stack is above the copy (checked), so the copy overwrites nothing
// it still needs. The clocks, the GPIO levels (power latch) and the USB core's state are kept: the stub polls the same
// OTGFS1 core and adopts the configured CDC-ACM device when it can.
#include "config.h"
#if CONFIG_STUB_ENTRY
#include <stdint.h>
#include "platform.h"
#include "board.h"
#include "hal_watchdog.h"
#include "../../stub/at32f43x/at32stub_header.h"	// the stub's image header and entry registers: one definition
#if CONFIG_CRASH_LOG
#include "hal_crash.h"
#endif
#if CONFIG_USB
#include "usb/usb_device.h"
#endif

// The two DMA controllers, 7 channels each, a channel's control word at 0x08 + 0x14 per channel (AT32F435/437
// reference manual, "DMA controller"; RadioFoundry uses no DMA on this platform yet, so this guards what a later driver
// adds)
#define DMA1_BASE		0x40026400u
#define DMA2_BASE		0x40026600u
#define DMA_CHANNELS		7u
#define DMA_CCTRL(b, ch)	AT32_REG((b) + 0x08u + 0x14u * (ch))
#define DMA_CCTRL_CHEN		(1u << 0)
#define STACK_MARGIN		256u		// what the copy loop's frame may use below the local it is checked by

_Static_assert(AT32STUB_RUN_ADDR == 0x20000000u, "system.c's stub_run_addr");

static uint32_t handoff_flags(void)
{
#if CONFIG_USB
	// In network mode the endpoints look the same (bulk 0x81 / 0x01, interrupt 0x82) but the host bound NCM, so the
	// stub must enumerate again as a serial port
	if (usb_device_mode() == USB_MODE_SERIAL) {
		return AT32STUB_ENTRY_USB_ADOPT;
	}
#endif
	return 0;
}

// Where the stub's RESET marks the restart as asked for: RadioFoundry's restart-cause word in the kept RAM
static uint32_t soft_mark_addr(void)
{
#if CONFIG_CRASH_LOG
	return (uint32_t)(uintptr_t)&hal_crash_boot()->soft;
#else
	return 0;
#endif
}

bool at32_stub_image_ok(const uint8_t *img, uint32_t len)
{
#if CONFIG_WATCHDOG
	return at32stub_image_ok(img, len, hal_watchdog_timeout_ms() != 0);
#else
	return at32stub_image_ok(img, len, false);
#endif
}

static void dma_stop(void)
{
	// Only a controller whose clock is on can be running
	for (uint32_t ch = 0; ch < DMA_CHANNELS; ch++) {
		if (CRM_AHBEN1 & CRM_AHBEN1_DMA1EN) {
			DMA_CCTRL(DMA1_BASE, ch) &= ~DMA_CCTRL_CHEN;
		}
		if (CRM_AHBEN1 & CRM_AHBEN1_DMA2EN) {
			DMA_CCTRL(DMA2_BASE, ch) &= ~DMA_CCTRL_CHEN;
		}
	}
}

bool at32_enter_stub_staged(const uint8_t *img, uint32_t len, uint32_t entry)
{
	const uint32_t run = AT32STUB_RUN_ADDR;
	const uint32_t src = (uint32_t)(uintptr_t)img;
	uint32_t here = (uint32_t)(uintptr_t)&here;	// the calling task's stack
	if (len == 0 || len > AT32STUB_IMAGE_MAX || entry < run || entry >= run + len) {
		return false;
	}
	// The copy goes downward (destination at or below the source), so a forward word copy is safe even where the
	// spans overlap; the stack must lie above what is overwritten
	if (src < run || here - STACK_MARGIN < run + len) {
		return false;
	}
	if (!at32_stub_image_ok(img, len)) {
		return false;		// checked at ENTER already; again here, whoever calls this
	}
	uint32_t flags = handoff_flags();
	uint32_t mark = soft_mark_addr();
	uint32_t hclk = SystemCoreClock;
	board_rf_safe();		// transmitter off; the power latch and the display stay as they are
#if CONFIG_WATCHDOG
	at32_watchdog_stretch();	// the watchdog keeps running: the stub feeds it (docs/spec/stub-at32f43x.md 3)
#endif

	__disable_irq();
	SysTick->CTRL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	dma_stop();			// the OTGFS1 core runs without DMA (TinyUSB slave mode); its FIFOs are apart from SRAM
	__DSB();

	volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)run;
	const volatile uint32_t *from = (const volatile uint32_t *)(uintptr_t)src;
	for (uint32_t i = 0; i < (len + 3u) / 4u; i++) {
		dst[i] = from[i];
	}
	__DSB();
	__ISB();

	// Thread mode on the main stack (CONTROL = 0 also drops the FPU context flag); the stub sets its own SP first
	register uint32_t r0 __asm("r0") = flags;
	register uint32_t r1 __asm("r1") = hclk;
	register uint32_t r2 __asm("r2") = mark;
	uint32_t pc = entry | 1u;
	__asm volatile(
		"msr control, %[zero]\n\t"
		"isb\n\t"
		"bx %[pc]\n\t"
		:
		: [zero] "r"(0u), [pc] "r"(pc), "r"(r0), "r"(r1), "r"(r2)
		: "memory");
	__builtin_unreachable();
}

#endif	// CONFIG_STUB_ENTRY
