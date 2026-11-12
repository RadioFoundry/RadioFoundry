// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Periodic interrupt timer (reference manual chapter 41): four channels counting the bus clock down
#include "platform.h"

#define PIT_CHANNELS	4u

static void (*callbacks[PIT_CHANNELS])(void);

k22f_status_t k22f_pit_start(uint8_t ch, uint32_t period_us, void (*fn)(void), uint8_t irq_prio)
{
	if (ch >= PIT_CHANNELS || period_us == 0 || fn == NULL) {
		return K22F_ERR_ARG;
	}
	SIM_SCGC6 |= SIM_SCGC6_PIT;
	PIT_MCR = PIT_MCR_FRZ;			// running (MDIS clear), stopped while the debugger halts the core
	PIT_TCTRL(ch) = 0;
	callbacks[ch] = fn;
	PIT_LDVAL(ch) = k22f_pit_ldval(k22f_bus_clock(), period_us);
	PIT_TFLG(ch) = PIT_TFLG_TIF;
	NVIC_SetPriority((IRQn_Type)(PIT0_IRQn + ch), irq_prio);
	NVIC_EnableIRQ((IRQn_Type)(PIT0_IRQn + ch));
	PIT_TCTRL(ch) = PIT_TCTRL_TIE | PIT_TCTRL_TEN;
	return K22F_OK;
}

void k22f_pit_stop(uint8_t ch)
{
	if (ch >= PIT_CHANNELS) {
		return;
	}
	PIT_TCTRL(ch) = 0;
	NVIC_DisableIRQ((IRQn_Type)(PIT0_IRQn + ch));
	PIT_TFLG(ch) = PIT_TFLG_TIF;
	callbacks[ch] = NULL;
}

static void service(uint8_t ch)
{
	PIT_TFLG(ch) = PIT_TFLG_TIF;
	if (callbacks[ch] != NULL) {
		callbacks[ch]();
	}
}

void PIT0_IRQHandler(void) { service(0); }
void PIT1_IRQHandler(void) { service(1); }
void PIT2_IRQHandler(void) { service(2); }
void PIT3_IRQHandler(void) { service(3); }
