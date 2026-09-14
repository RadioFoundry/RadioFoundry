// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380G / MD-390G / RT3 GPS: the module's NMEA output on USART3 RX (PD9), detected at run time
// (docs/spec/hardware-md380.md 13).
//
// Radios with and without the module share the board. PD8 is the module's power switch on the G models but is
// reported as a flash chip select on others, so nothing drives it until NMEA has been heard: the UART listens for
// LISTEN_MS after start-up with PD8 left as an input. One checksum-valid sentence makes the module present; PD8 is then
// held at the level it already had (the level the module runs at, whatever its polarity), and every sentence goes to
// the APRS position source (CONFIG_APRS). Nothing heard: the UART is switched off again and the radio has no GPS.
#include "md380.h"
#include "pins.h"
#include "config.h"
#include "hal_time.h"
#include "gps/nmea_line/nmea_line.h"
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif

#define GPS_BAUD		9600u	// the usual default of these modules (?)
#define LISTEN_MS		5000u
#define GPS_TASK_STACK		384	// words: the APRS NMEA parser runs on it
#define GPS_TASK_PRIORITY	1
#define GPS_POLL_MS		20
#define GPS_IRQ_PRIORITY	7	// numerically >= 5: may not call FreeRTOS, doesn't need to
#define RING			256u

static volatile uint8_t ring[RING];
static volatile uint16_t head, tail;
static volatile uint32_t bytes, uart_errors;
static nmea_line_t line;
static volatile bool present;
static TaskHandle_t task;

void USART3_IRQHandler(void)
{
	uint32_t sr = USART3->SR;
	uint8_t c = (uint8_t)USART3->DR;	// reading SR then DR clears RXNE and the error flags
	if (sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
		uart_errors++;
		return;
	}
	if (sr & USART_SR_RXNE) {
		uint16_t h = head, n = (uint16_t)((h + 1u) % RING);
		if (n != tail) {
			ring[h] = c;
			head = n;
		}
		bytes++;
	}
}

static void uart_on(bool on)
{
	if (on) {
		__HAL_RCC_USART3_CLK_ENABLE();
		stm32_pin_af(PIN_PORT(PIN_GPS_RX), PIN_MASK(PIN_GPS_RX), GPIO_AF7_USART3, false, GPIO_PULLUP);
		USART3->CR1 = 0;
		USART3->BRR = (HAL_RCC_GetPCLK1Freq() + GPS_BAUD / 2u) / GPS_BAUD;	// oversampling by 16
		USART3->CR2 = 0;
		USART3->CR3 = 0;
		USART3->CR1 = USART_CR1_UE | USART_CR1_RE | USART_CR1_RXNEIE;
		HAL_NVIC_SetPriority(USART3_IRQn, GPS_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(USART3_IRQn);
	} else {
		HAL_NVIC_DisableIRQ(USART3_IRQn);
		USART3->CR1 = 0;
		__HAL_RCC_USART3_CLK_DISABLE();
		stm32_pin_in(PIN_PORT(PIN_GPS_RX), PIN_MASK(PIN_GPS_RX), GPIO_NOPULL);
	}
}

static void gps_task(void *arg)
{
	(void)arg;
	uint32_t start = hal_millis();
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
		while (tail != head) {
			uint8_t c = ring[tail];
			tail = (uint16_t)((tail + 1u) % RING);
			if (!nmea_line_feed(&line, c)) {
				continue;
			}
			if (!present) {
				// Heard: hold the power pin where it is (hardware-md380.md 13.2)
				bool level = PIN_READ(PIN_GPS_POWER);
				stm32_pin_out(PIN_PORT(PIN_GPS_POWER), PIN_MASK(PIN_GPS_POWER), level, false);
				present = true;
			}
#if CONFIG_APRS
			aprs_nmea(line.buf, line.len, true, hal_millis());
#endif
		}
		if (!present && (uint32_t)(hal_millis() - start) >= LISTEN_MS) {
			uart_on(false);		// no module: nothing more to do
			task = NULL;
			vTaskDelete(NULL);
		}
	}
}

void md380_gps_init(void)
{
	nmea_line_init(&line);
	head = tail = 0;
	present = false;
	uart_on(true);
	if (task == NULL) {
		xTaskCreate(gps_task, "gps", GPS_TASK_STACK, NULL, GPS_TASK_PRIORITY, &task);
	}
}

bool md380_gps_present(void)
{
	return present;
}

void md380_gps_counts(uint32_t *good, uint32_t *bad, uint32_t *n)
{
	*good = line.good;
	*bad = line.bad + uart_errors;
	*n = bytes;
}
