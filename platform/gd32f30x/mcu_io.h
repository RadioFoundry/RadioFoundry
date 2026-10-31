// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MCU services a board uses when it runs on more than one platform (boards/d578uv: the GD32F30x and the AT32F43x
// builds of one radio). Every platform such a board runs on has an mcu_io.h with these names (platform/at32f43x has
// the other); here they are the GD32F30x platform's own calls, so the build is the same as calling them directly.
//
// Peripherals are named as the STM32 and Artery manuals number them, from 1: the GD32 manual's TIMER1 is TIM2 here,
// its USART1 is USART2. The parts put them at the same addresses.
#ifndef MCU_IO_H
#define MCU_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

// --- Pins: a port and a mask ---
typedef gd32_gpio_t mcu_gpio_t;
typedef gd32_pull_t mcu_pull_t;
#define MCU_PIN(n)		GD32_PIN(n)
#define MCU_PULL_NONE		GD32_PULL_NONE
#define MCU_PULL_UP		GD32_PULL_UP
#define MCU_PULL_DOWN		GD32_PULL_DOWN

static inline void mcu_pin_force(mcu_gpio_t *p, uint16_t pins, bool level) { gd32_pin_force(p, pins, level); }
static inline void mcu_pin_out(mcu_gpio_t *p, uint16_t pins, bool level, bool od) { gd32_pin_out(p, pins, level, od); }
static inline void mcu_pin_in(mcu_gpio_t *p, uint16_t pins, mcu_pull_t pull) { gd32_pin_in(p, pins, pull); }
static inline void mcu_pin_analog(mcu_gpio_t *p, uint16_t pins) { gd32_pin_analog(p, pins); }
static inline void mcu_pin_write(mcu_gpio_t *p, uint16_t pins, bool level) { gd32_pin_write(p, pins, level); }
static inline bool mcu_pin_read(mcu_gpio_t *p, uint16_t pin) { return gd32_pin_read(p, pin); }
// af is the AT32's multiplexed function number; the GD32 picks the function by the peripheral that is on
static inline void mcu_pin_af(mcu_gpio_t *p, uint16_t pins, uint8_t af, bool od)
{
	(void)af;
	gd32_pin_af(p, pins, od);
}

// One store that sets `set` and clears `clr` (a bus's data lines at once)
static inline void mcu_port_put(mcu_gpio_t *p, uint16_t set, uint16_t clr)
{
	p->BOP = set | ((uint32_t)clr << 16);
}

static inline uint16_t mcu_port_read(mcu_gpio_t *p)
{
	return (uint16_t)p->ISTAT;
}

// Pins 8..15 all push-pull outputs or all floating inputs, in one store (CTL1: MD 3, or CTL 1)
static inline void mcu_port_high_byte_dir(mcu_gpio_t *p, bool out)
{
	p->CTL1 = out ? 0x33333333u : 0x44444444u;
}

// The JTAG-only pins (PA15, PB3, PB4) as GPIO, SWD kept; and the SWD pins (PA13, PA14) as GPIO too
static inline void mcu_jtag_release(void) { gd32_swj(AFIO_PCF0_SWJ_SWDP); }
static inline void mcu_swd_release(void) { gd32_swj(AFIO_PCF0_SWJ_OFF); }

// --- EXTI lines (a mask of lines) ---
static inline void mcu_exti_route(mcu_gpio_t *p, uint8_t line) { gd32_exti_route(p, line); }
static inline void mcu_exti_both_edges(uint32_t lines)
{
	EXTI_RTEN |= lines;
	EXTI_FTEN |= lines;
}
static inline void mcu_exti_clear(uint32_t lines) { EXTI_PD = lines; }
static inline void mcu_exti_enable(uint32_t lines) { EXTI_INTEN |= lines; }
static inline void mcu_exti_disable(uint32_t lines) { EXTI_INTEN &= ~lines; }
#define MCU_EXTI0_IRQn		EXTI0_IRQn
#define MCU_EXTI0_HANDLER	EXTI0_IRQHandler
#define MCU_EXTI9_5_IRQn	EXTI5_9_IRQn
#define MCU_EXTI9_5_HANDLER	EXTI5_9_IRQHandler

// --- TIM2..TIM7 on APB1 (GD32 TIMER1..6): the STM32 general-purpose timer registers. Their clock is the core's ---
#define MCU_TIM(n)		TIMER_BASE((n) - 1u)
#define MCU_TIM_CR1(t)		TIMER_CTL0(t)
#define MCU_TIM_DIER(t)		TIMER_DMAINTEN(t)
#define MCU_TIM_SR(t)		TIMER_INTF(t)
#define MCU_TIM_EGR(t)		TIMER_SWEVG(t)
#define MCU_TIM_CCMR2(t)	TIMER_CHCTL1(t)
#define MCU_TIM_CCER(t)		TIMER_CHCTL2(t)
#define MCU_TIM_CNT(t)		TIMER_CNT(t)
#define MCU_TIM_PSC(t)		TIMER_PSC(t)
#define MCU_TIM_ARR(t)		TIMER_CAR(t)
#define MCU_TIM_CCR4(t)		TIMER_CH3CV(t)
#define MCU_TIM_CR1_CEN		TIMER_CTL0_CEN
#define MCU_TIM_CR1_URS		TIMER_CTL0_UPS
#define MCU_TIM_CR1_OPM		TIMER_CTL0_SPM
#define MCU_TIM_CR1_ARPE	TIMER_CTL0_ARSE
#define MCU_TIM_CCMR2_OC4_PWM1	TIMER_CHCTL1_CH3_PWM1
#define MCU_TIM_CCMR2_OC4PE	TIMER_CHCTL1_CH3_PRELOAD
#define MCU_TIM_CCER_CC4E	TIMER_CHCTL2_CH3EN
#define MCU_TIM_DIER_UIE	TIMER_DMAINTEN_UPIE
#define MCU_TIM_EGR_UG		TIMER_SWEVG_UPG
#define MCU_TIM2_IRQn		TIMER1_IRQn
#define MCU_TIM2_HANDLER	TIMER1_IRQHandler
#define MCU_TIM5_IRQn		TIMER4_IRQn
#define MCU_TIM5_HANDLER	TIMER4_IRQHandler
#define MCU_TIM6_IRQn		TIMER5_IRQn
#define MCU_TIM6_HANDLER	TIMER5_IRQHandler

static inline void mcu_tim_clock_on(uint32_t n)
{
	RCU_APB1EN |= RCU_APB1EN_TIMER1EN << (n - 2u);
	(void)RCU_APB1EN;
}

static inline bool mcu_tim_clocked(uint32_t n)
{
	return (RCU_APB1EN & (RCU_APB1EN_TIMER1EN << (n - 2u))) != 0;
}

// --- DAC (channel 0 on PA4, 1 on PA5) and ADC ---
static inline void mcu_dac_init(uint8_t ch) { gd32_dac_init(ch); }
static inline void mcu_dac_write(uint8_t ch, uint16_t v) { gd32_dac_write(ch, v); }
// Channel 0's data register as one store, for interrupt handlers
static inline void mcu_dac0_put(uint16_t v) { DAC0_R12DH = v; }
static inline bool mcu_dac_clocked(void) { return (RCU_APB1EN & RCU_APB1EN_DACEN) != 0; }
static inline void mcu_adc_init(void) { gd32_adc_init(); }
static inline uint16_t mcu_adc_read(uint8_t ch) { return gd32_adc_read(ch); }
// The sampling channel: ch converted on its own trigger and data register (the inserted group, adc.c), so a timer
// interrupt can sample audio while tasks go on with mcu_adc_read. The interrupt starts a conversion each tick and reads
// the one it started the tick before (done well within a tick at audio rates); neither waits
static inline void mcu_adc_sample_init(uint8_t ch) { gd32_adc_sample_init(ch); }
static inline void mcu_adc_sample_start(void) { ADC_CTL1(ADC0_BASE) |= ADC_CTL1_SWICST; }
static inline uint16_t mcu_adc_sample_value(void) { return (uint16_t)(ADC_IDATA0(ADC0_BASE) & 0xFFFu); }

// --- Clocks, system services, USB identity ---
// The core clock sys_hz from the crystal hxtal_hz through the PLL (a whole multiple of the crystal here)
static inline bool mcu_clock_init(uint32_t hxtal_hz, uint32_t sys_hz)
{
	const gd32_clock_t c = { .hxtal_hz = hxtal_hz, .pll_mul = (uint8_t)(sys_hz / hxtal_hz) };
	return gd32_clock_init(&c);
}
#define mcu_system		gd32_system
#define MCU_USB_VID		GD32_USB_VID
#define MCU_USB_PID_SERIAL	GD32_USB_PID_SERIAL
#define MCU_USB_PID_NETWORK	GD32_USB_PID_NETWORK
#define mcu_usb_serial_number	gd32_usb_serial_number

// --- The GPS receiver (gps_uart/gps_uart.h, when the board builds it) and the USART it listens on ---
#define mcu_gps_cfg_t		gd32_gps_cfg_t
#define mcu_gps_init		gd32_gps_init
#define mcu_gps_irq		gd32_gps_irq
#define mcu_gps_present		gd32_gps_present
#define mcu_gps_enable		gd32_gps_enable
#define mcu_gps_counts		gd32_gps_counts
#define mcu_gps_running		gd32_gps_running
#define MCU_USART2		USART1
#define MCU_USART2_IRQn		USART1_IRQn
#define MCU_USART2_HANDLER	USART1_IRQHandler

// --- A USART a byte at a time (uart.c), for a board's serial device. MCU_USART1 is the reference manuals' USART1
// (this part's USART0) ---
#define mcu_uart_t		gd32_usart_t
#define MCU_UART_NONE		GD32_UART_NONE
#define MCU_UART_ERROR		GD32_UART_ERROR
#define mcu_uart_start		gd32_uart_start
#define mcu_uart_stop		gd32_uart_stop
#define mcu_uart_take		gd32_uart_take
#define mcu_uart_put		gd32_uart_put
#define MCU_USART1		USART0
#define MCU_USART1_IRQn		USART0_IRQn
#define MCU_USART1_HANDLER	USART0_IRQHandler
#define MCU_UART4		UART3		// the manuals' UART4 (this part's UART3), on PC10 / PC11
#define MCU_UART4_IRQn		UART3_IRQn
#define MCU_UART4_HANDLER	UART3_IRQHandler

#endif
