// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "platform.h"

bool stm32_clock_init(const stm32_clock_t *c)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	// Run from HSI so HSE and the PLL can be reprogrammed
	osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	osc.HSIState = RCC_HSI_ON;
	osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	osc.PLL.PLLState = RCC_PLL_NONE;
	HAL_RCC_OscConfig(&osc);
	clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clk.APB1CLKDivider = RCC_HCLK_DIV1;
	clk.APB2CLKDivider = RCC_HCLK_DIV1;
	if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
		return false;
	}

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = c->hse_bypass ? RCC_HSE_BYPASS : RCC_HSE_ON;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLM = c->pll_m;
	osc.PLL.PLLN = c->pll_n;
	osc.PLL.PLLP = c->pll_p;
	osc.PLL.PLLQ = c->pll_q;
	// The PLL must be off before it can be reconfigured
	__HAL_RCC_PLL_DISABLE();
	if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
		return false;
	}

	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = c->ahb_div;
	clk.APB1CLKDivider = c->apb1_div;
	clk.APB2CLKDivider = c->apb2_div;
	if (HAL_RCC_ClockConfig(&clk, c->flash_latency) != HAL_OK) {
		return false;
	}

	if (c->lse) {
		HAL_PWR_EnableBkUpAccess();
		osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
		osc.LSEState = RCC_LSE_ON;
		osc.PLL.PLLState = RCC_PLL_NONE;
		HAL_RCC_OscConfig(&osc);	// a missing crystal only costs the RTC
	}

	SystemCoreClockUpdate();
	return true;
}
