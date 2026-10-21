// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Runtime entry to the K22F flash stub (CONFIG_STUB_ENTRY, docs/spec/debuglink.md 3.18, docs/spec/stub-k22f.md).
//
// The debug link loads the stub image into a staging buffer in ODR's .bss while ODR runs. The image is linked to run
// at SRAM_L's base, which ODR's .data and .bss hold until the jump, so enter_stub masks everything, stops what writes
// into RAM on its own (the eDMA with the I2S audio streams, the USB controller), copies the staging buffer down and
// jumps there. The code is in flash and the task stack in SRAM_U, so the copy overwrites nothing still needed. The
// clocks (stepped down to RUN, which the flash controller needs), the pin levels (the power hold) and the USB device
// are kept: the stub adopts the configured CDC-ACM device and carries on with its data toggles (stub_handoff.h).
//
// The USB controller writes received packets into the buffers its descriptor table names, which are in the RAM the
// copy overwrites. So before the copy the controller is parked on a table of descriptors the CPU owns (in SRAM_U):
// it answers every token with NAK until the stub points it at its own table, and the host retries meanwhile.
#include "config.h"
#if CONFIG_STUB_ENTRY
#include <stdint.h>
#include <string.h>
#include "platform.h"
#include "k22f_usb_regs.h"
#include "stub_handoff.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#include "tusb.h"
#endif

#ifndef ODR_CPU_RAM_SECTION
#error "the K22F stub entry parks the USB controller's table in SRAM_U: the board must name ODR_CPU_RAM_SECTION"
#endif

#define PARK_SETTLE_US		1000u	// a packet in flight when the table moves is 64 bytes at 12 Mbit/s: about 50 us

static uint8_t stage[K22F_STUB_STAGE_SIZE] __attribute__((aligned(8)));
static uint8_t park[512] __attribute__((aligned(512), section(ODR_CPU_RAM_SECTION)));

__attribute__((weak)) void board_stub_prepare(void)
{
}

uint8_t *k22f_stub_region(uint32_t *size)
{
	*size = sizeof(stage);
	return stage;
}

static void wait_us(uint32_t us)
{
	uint32_t start = DWT->CYCCNT;
	uint32_t cycles = us * (SystemCoreClock / 1000000u);
	while (DWT->CYCCNT - start < cycles) {
	}
}

// Park the USB controller and say what the stub needs to carry on with it: r0's flags, *toggles for r1
static uint32_t usb_handoff(uint32_t *toggles)
{
	*toggles = 0;
	if ((SIM_SCGC4 & SIM_SCGC4_USBOTG) == 0) {
		return 0;
	}
	uint32_t old = ((uint32_t)USB0_BDTPAGE3 << 24) | ((uint32_t)USB0_BDTPAGE2 << 16) |
		       ((uint32_t)(USB0_BDTPAGE1 & 0xFEu) << 8);
	memset(park, 0, sizeof(park));
	uint32_t a = (uint32_t)(uintptr_t)park;
	USB0_BDTPAGE1 = (uint8_t)(a >> 8);
	USB0_BDTPAGE2 = (uint8_t)(a >> 16);
	USB0_BDTPAGE3 = (uint8_t)(a >> 24);
	wait_us(PARK_SETTLE_US);
	uint32_t flags = 0;
#if CONFIG_USB
	// In network mode the endpoints look alike but the host bound NCM: the stub must enumerate again as a serial port
	if (usb_device_mode() == USB_MODE_SERIAL && tud_mounted()) {
		flags = K22F_STUB_USB_ADOPT;
		// The old table and the driver's state after it are intact until the copy. A token that completed but wasn't
		// handled (interrupts are off) moved the controller past what the driver recorded: then the toggles are left
		// to the stub's fallback
		if (old >= K22F_SRAM_L && old + K22F_STUB_MEM_LEN <= K22F_SRAM_END &&
		    (USB0_ISTAT & USB_ISTAT_TOKDNE_MASK) == 0) {
			*toggles = k22f_stub_toggles((const uint8_t *)(uintptr_t)old);
		}
	}
#endif
	return flags;
}

bool k22f_enter_stub(uint32_t entry)
{
	if (entry < K22F_STUB_RUN_ADDR || entry >= K22F_STUB_RUN_ADDR + K22F_STUB_STAGE_SIZE) {
		return false;
	}
	// The copy goes downward (destination at or below the source), so a forward word copy is safe even if the
	// staging buffer overlaps the run span; and it must stay in SRAM_L
	uint32_t src = (uint32_t)(uintptr_t)stage;
	if (src < K22F_STUB_RUN_ADDR || src + sizeof(stage) > K22F_STUB_RUN_ADDR + K22F_STUB_SPAN) {
		return false;
	}
	board_stub_prepare();		// transmitter off; the power hold and the display stay as they are
	// The flash controller runs no command in HSRUN; the stub never steps back up
	if (!k22f_clock_step_down(true)) {
		return false;
	}

	__disable_irq();
	SysTick->CTRL = 0;
	for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}
	// No new DMA request (the I2S streams run circular into RAM), the I2S block stopped, the channel routing off
	if (SIM_SCGC7 & SIM_SCGC7_DMA) {
		DMA_ERQ = 0;
		for (uint32_t i = 0; i < 100000u && (DMA_CR & DMA_CR_ACTIVE); i++) {
		}
	}
	if (SIM_SCGC6 & SIM_SCGC6_I2S) {
		I2S0_TCSR = 0;
		I2S0_RCSR = 0;
	}
	if (SIM_SCGC6 & SIM_SCGC6_DMAMUX) {
		for (unsigned ch = 0; ch < 16u; ch++) {
			DMAMUX_CHCFG(ch) = 0;
		}
	}
	uint32_t toggles;
	uint32_t flags = usb_handoff(&toggles);
	uint32_t core_hz = SystemCoreClock;
	__DSB();

	volatile uint32_t *dst = (volatile uint32_t *)K22F_STUB_RUN_ADDR;
	const volatile uint32_t *from = (const volatile uint32_t *)(uintptr_t)src;
	for (uint32_t i = 0; i < K22F_STUB_STAGE_SIZE / 4u; i++) {
		dst[i] = from[i];
	}
	FMC_PFB0CR |= FMC_PFB0CR_INVALIDATE;
	__DSB();
	__ISB();

	// Thread mode on the main stack (CONTROL = 0 also drops the FPU context flag); the stub sets its own SP first
	register uint32_t r0 __asm("r0") = flags;
	register uint32_t r1 __asm("r1") = toggles;
	register uint32_t r2 __asm("r2") = core_hz;
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
