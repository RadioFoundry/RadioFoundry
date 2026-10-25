// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host stand-in for platform/stm32f4/platform.h, for md380RfbusTest only: just enough for the real boards/md380/pins.h
// and rfbus_md380.c to compile. The ports and GPIO_PIN_n are fake; every PIN_* definition still comes from pins.h
#ifndef MD380_STUB_PLATFORM_H
#define MD380_STUB_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	uint32_t unused;
} GPIO_TypeDef;

extern GPIO_TypeDef fake_ports[5];
#define GPIOA			(&fake_ports[0])
#define GPIOB			(&fake_ports[1])
#define GPIOC			(&fake_ports[2])
#define GPIOD			(&fake_ports[3])
#define GPIOE			(&fake_ports[4])

#define GPIO_PIN_0		((uint16_t)(1u << 0))
#define GPIO_PIN_1		((uint16_t)(1u << 1))
#define GPIO_PIN_2		((uint16_t)(1u << 2))
#define GPIO_PIN_3		((uint16_t)(1u << 3))
#define GPIO_PIN_4		((uint16_t)(1u << 4))
#define GPIO_PIN_5		((uint16_t)(1u << 5))
#define GPIO_PIN_6		((uint16_t)(1u << 6))
#define GPIO_PIN_7		((uint16_t)(1u << 7))
#define GPIO_PIN_8		((uint16_t)(1u << 8))
#define GPIO_PIN_9		((uint16_t)(1u << 9))
#define GPIO_PIN_10		((uint16_t)(1u << 10))
#define GPIO_PIN_11		((uint16_t)(1u << 11))
#define GPIO_PIN_12		((uint16_t)(1u << 12))
#define GPIO_PIN_13		((uint16_t)(1u << 13))
#define GPIO_PIN_14		((uint16_t)(1u << 14))
#define GPIO_PIN_15		((uint16_t)(1u << 15))

#define __NOP()			((void)0)

// The test's fake GPIO (md380RfbusTest.c)
void stm32_pin_write(GPIO_TypeDef *port, uint16_t pins, bool level);
bool stm32_pin_read(GPIO_TypeDef *port, uint16_t pin);

#endif
