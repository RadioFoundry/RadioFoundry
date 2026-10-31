// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MCU services a board uses when it runs on more than one platform: the same names as platform/gd32f30x/mcu_io.h
// (that file says why), here on the AT32F43x. Peripherals are numbered as the Artery manual numbers them, from 1.
#ifndef MCU_IO_H
#define MCU_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "platform.h"

// --- Pins: a port and a mask ---
typedef at32_gpio_t mcu_gpio_t;
typedef at32_pull_t mcu_pull_t;
#define MCU_PIN(n)		AT32_PIN(n)
#define MCU_PULL_NONE		AT32_PULL_NONE
#define MCU_PULL_UP		AT32_PULL_UP
#define MCU_PULL_DOWN		AT32_PULL_DOWN

static inline void mcu_pin_force(mcu_gpio_t *p, uint16_t pins, bool level) { at32_pin_force(p, pins, level); }
static inline void mcu_pin_out(mcu_gpio_t *p, uint16_t pins, bool level, bool od) { at32_pin_out(p, pins, level, od); }
static inline void mcu_pin_in(mcu_gpio_t *p, uint16_t pins, mcu_pull_t pull) { at32_pin_in(p, pins, pull); }
static inline void mcu_pin_analog(mcu_gpio_t *p, uint16_t pins) { at32_pin_analog(p, pins); }
static inline void mcu_pin_write(mcu_gpio_t *p, uint16_t pins, bool level) { at32_pin_write(p, pins, level); }
static inline bool mcu_pin_read(mcu_gpio_t *p, uint16_t pin) { return at32_pin_read(p, pin); }
// af: the multiplexed function number (the data sheet's pin table)
static inline void mcu_pin_af(mcu_gpio_t *p, uint16_t pins, uint8_t af, bool od) { at32_pin_af(p, pins, af, od); }

// One store that sets `set` and clears `clr` (a bus's data lines at once)
static inline void mcu_port_put(mcu_gpio_t *p, uint16_t set, uint16_t clr)
{
	p->SCR = set | ((uint32_t)clr << 16);
}

static inline uint16_t mcu_port_read(mcu_gpio_t *p)
{
	return (uint16_t)p->IDT;
}

// Pins 8..15 all outputs or all inputs (CFGR's upper half: mode 1 or 0). Their output type and pulls stay as the
// board's set-up left them, so a direction change is one read and one store
static inline void mcu_port_high_byte_dir(mcu_gpio_t *p, bool out)
{
	p->CFGR = (p->CFGR & 0x0000FFFFu) | (out ? 0x55550000u : 0u);
}

// The debug pins become GPIO when a pin's mode is set, with no separate switch as on the GD32: nothing to do
static inline void mcu_jtag_release(void) {}
static inline void mcu_swd_release(void) {}

// --- EXINT lines (a mask of lines) ---
static inline void mcu_exti_route(mcu_gpio_t *p, uint8_t line) { at32_exint_route(p, line); }
static inline void mcu_exti_both_edges(uint32_t lines)
{
	EXINT_POLCFG1 |= lines;
	EXINT_POLCFG2 |= lines;
}
static inline void mcu_exti_clear(uint32_t lines) { EXINT_INTSTS = lines; }
static inline void mcu_exti_enable(uint32_t lines) { EXINT_INTEN |= lines; }
static inline void mcu_exti_disable(uint32_t lines) { EXINT_INTEN &= ~lines; }
#define MCU_EXTI0_IRQn		EXINT0_IRQn
#define MCU_EXTI0_HANDLER	EXINT0_IRQHandler
#define MCU_EXTI9_5_IRQn	EXINT9_5_IRQn
#define MCU_EXTI9_5_HANDLER	EXINT9_5_IRQHandler

// --- TMR2..TMR7 on APB1: the STM32 general-purpose timer registers. Their clock is the core's (clock.c) ---
#define MCU_TIM(n)		TMR_BASE(n)
#define MCU_TIM_CR1(t)		AT32_REG((t) + 0x00u)
#define MCU_TIM_DIER(t)		AT32_REG((t) + 0x0Cu)
#define MCU_TIM_SR(t)		AT32_REG((t) + 0x10u)
#define MCU_TIM_EGR(t)		AT32_REG((t) + 0x14u)
#define MCU_TIM_CCMR2(t)	AT32_REG((t) + 0x1Cu)
#define MCU_TIM_CCER(t)		AT32_REG((t) + 0x20u)
#define MCU_TIM_CNT(t)		AT32_REG((t) + 0x24u)
#define MCU_TIM_PSC(t)		AT32_REG((t) + 0x28u)
#define MCU_TIM_ARR(t)		AT32_REG((t) + 0x2Cu)
#define MCU_TIM_CCR4(t)		AT32_REG((t) + 0x40u)
#define MCU_TIM_CR1_CEN		(1u << 0)
#define MCU_TIM_CR1_URS		(1u << 2)	// only an overflow interrupts, not a software update
#define MCU_TIM_CR1_OPM		(1u << 3)
#define MCU_TIM_CR1_ARPE	(1u << 7)
#define MCU_TIM_CCMR2_OC4_PWM1	(6u << 12)
#define MCU_TIM_CCMR2_OC4PE	(1u << 11)
#define MCU_TIM_CCER_CC4E	(1u << 12)
#define MCU_TIM_DIER_UIE	(1u << 0)
#define MCU_TIM_EGR_UG		(1u << 0)
#define MCU_TIM2_IRQn		TMR2_GLOBAL_IRQn
#define MCU_TIM2_HANDLER	TMR2_GLOBAL_IRQHandler
#define MCU_TIM5_IRQn		TMR5_GLOBAL_IRQn
#define MCU_TIM5_HANDLER	TMR5_GLOBAL_IRQHandler
#define MCU_TIM6_IRQn		TMR6_DAC_GLOBAL_IRQn
#define MCU_TIM6_HANDLER	TMR6_DAC_GLOBAL_IRQHandler

static inline void mcu_tim_clock_on(uint32_t n)
{
	CRM_APB1EN |= CRM_APB1EN_TMR2EN << (n - 2u);
	(void)CRM_APB1EN;
}

static inline bool mcu_tim_clocked(uint32_t n)
{
	return (CRM_APB1EN & (CRM_APB1EN_TMR2EN << (n - 2u))) != 0;
}

// --- DAC (channel 0 on PA4, 1 on PA5) and ADC ---
static inline void mcu_dac_init(uint8_t ch) { at32_dac_init(ch); }
static inline void mcu_dac_write(uint8_t ch, uint16_t v) { at32_dac_write(ch, v); }
// Channel 0's data register as one store, for interrupt handlers
static inline void mcu_dac0_put(uint16_t v) { DAC_D1DTH12R = v; }
static inline bool mcu_dac_clocked(void) { return (CRM_APB1EN & CRM_APB1EN_DACEN) != 0; }
static inline void mcu_adc_init(void) { at32_adc_init(); }
static inline uint16_t mcu_adc_read(uint8_t ch) { return at32_adc_read(ch); }
// The sampling channel: ch converted on its own trigger and data register (the preempted group, adc.c), so a timer
// interrupt can sample audio while tasks go on with mcu_adc_read. The interrupt starts a conversion each tick and reads
// the one it started the tick before (done well within a tick at audio rates); neither waits
static inline void mcu_adc_sample_init(uint8_t ch) { at32_adc_sample_init(ch); }
static inline void mcu_adc_sample_start(void) { ADC_CTRL2(ADC1_BASE) |= ADC_CTRL2_PCSWTRG; }
static inline uint16_t mcu_adc_sample_value(void) { return (uint16_t)(ADC_PDT1(ADC1_BASE) & 0xFFFu); }

// --- Clocks, system services, USB identity ---
// The core clock sys_hz from the crystal hxtal_hz through the PLL
static inline bool mcu_clock_init(uint32_t hxtal_hz, uint32_t sys_hz)
{
	const at32_clock_t c = { .hext_hz = hxtal_hz, .sys_hz = sys_hz };
	return at32_clock_init(&c);
}
#define mcu_system		at32_system
#define MCU_USB_VID		AT32_USB_VID
#define MCU_USB_PID_SERIAL	AT32_USB_PID_SERIAL
#define MCU_USB_PID_NETWORK	AT32_USB_PID_NETWORK
#define mcu_usb_serial_number	at32_usb_serial_number

// --- The GPS receiver (gps_uart/gps_uart.h, when the board builds it) and the USART it listens on ---
#define mcu_gps_cfg_t		at32_gps_cfg_t
#define mcu_gps_init		at32_gps_init
#define mcu_gps_irq		at32_gps_irq
#define mcu_gps_present		at32_gps_present
#define mcu_gps_enable		at32_gps_enable
#define mcu_gps_counts		at32_gps_counts
#define mcu_gps_running		at32_gps_running
#define MCU_USART2		USART2
#define MCU_USART2_IRQn		USART2_IRQn
#define MCU_USART2_HANDLER	USART2_IRQHandler

// --- A USART a byte at a time (uart.c), for a board's serial device. MCU_USART1 is the reference manuals' USART1
// (this part's USART1) ---
#define mcu_uart_t		at32_usart_t
#define MCU_UART_NONE		AT32_UART_NONE
#define MCU_UART_ERROR		AT32_UART_ERROR
#define mcu_uart_start		at32_uart_start
#define mcu_uart_stop		at32_uart_stop
#define mcu_uart_take		at32_uart_take
#define mcu_uart_put		at32_uart_put
#define MCU_USART1		USART1
#define MCU_USART1_IRQn		USART1_IRQn
#define MCU_USART1_HANDLER	USART1_IRQHandler
#define MCU_UART4		UART4
#define MCU_UART4_IRQn		UART4_IRQn
#define MCU_UART4_HANDLER	UART4_IRQHandler

#endif
