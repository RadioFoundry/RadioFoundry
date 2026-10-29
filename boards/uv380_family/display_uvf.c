// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family display: 160x128 TFT on FSMC bank 1 (8-bit on D0-D7), PWM backlight (docs/spec/hardware-dm1701.md 2.4,
// 2.5, 3.2, 3.3, 9, 10.1). The DM-1701's lens hides 8 rows, the MD-UV380 shows all 128 (hardware-mduv380.md 4): board.mk
// says how many with UVF_LCD_HIDDEN_ROWS. The backlight pin is the board's (PD8, or PC6 on the 10 W radio)
//
// The LCD data lines D0-D7 are also the keypad columns. Every LCD burst holds uvf_lcdbus_lock(), and starts by
// calling uvf_lcd_pins_fsmc() itself, so the keypad scan may leave the lines in GPIO mode when it unlocks. Each
// burst ends with CS high and a 0x00 write to the data address so the lines rest low for the next key scan.
#include "uvf.h"
#include "FreeRTOS.h"
#include "config.h"
#include "pins.h"
#include "hal_time.h"
#include "display/st7735/st7735.h"


#define LCD_AF		12	// GPIO_AF12_FSMC
#define LCD_CTRL_PORTD	(GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_12)	// NOE, NWE, A17 (RS)

#ifndef UVF_LCD_HIDDEN_ROWS
#error "board.mk: UVF_LCD_HIDDEN_ROWS (the panel rows the radio's lens hides)"
#endif
#define VISIBLE_W	160
#define VISIBLE_H	(128 - UVF_LCD_HIDDEN_ROWS)
#define HIDDEN_ROWS	UVF_LCD_HIDDEN_ROWS

// ---- FSMC bus ----

// Bank 1 SRAM, 16-bit bus (so HADDR bit 18 drives A17 = RS), byte writes on D0-D7. HCLK is 72 MHz (13.9 ns):
// address setup 2 + data phase 4 + turnaround 1 cycles, about 100-125 ns per byte, inside an 8080 write cycle
static void fsmc_init(void)
{
	__HAL_RCC_FSMC_CLK_ENABLE();
	FSMC_Bank1->BTCR[0] &= ~FSMC_BCR1_MBKEN;
	FSMC_Bank1->BTCR[1] = (2u << FSMC_BTR1_ADDSET_Pos) | (4u << FSMC_BTR1_DATAST_Pos) | (1u << FSMC_BTR1_BUSTURN_Pos);
	// SRAM type, 16-bit width, writes enabled; bit 7 is reserved and kept at its reset value of 1
	FSMC_Bank1->BTCR[0] = FSMC_BCR1_MWID_0 | FSMC_BCR1_WREN | (1u << 7) | FSMC_BCR1_MBKEN;
}

static void set_af12(GPIO_TypeDef *port, uint16_t pins)
{
	for (uint32_t i = 0; i < 16; i++) {
		if ((pins & (1u << i)) == 0) {
			continue;
		}
		volatile uint32_t *afr = &port->AFR[i >> 3];
		uint32_t shift = (i & 7) * 4;
		*afr = (*afr & ~(0xFu << shift)) | ((uint32_t)LCD_AF << shift);
		port->OSPEEDR |= 3u << (i * 2);
		port->OTYPER &= ~(1u << i);
		port->PUPDR &= ~(3u << (i * 2));
		port->MODER = (port->MODER & ~(3u << (i * 2))) | (2u << (i * 2));
	}
}

// (Re)connect D0-D7 to the FSMC. Called at the start of every LCD burst, with the bus lock held
void uvf_lcd_pins_fsmc(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();	// the port registers are shared with other pins on PD and PE
	set_af12(GPIOD, STM32_FSMC_D0_D3);
	set_af12(GPIOE, STM32_FSMC_D4_D7);
	__set_PRIMASK(primask);
}

// FSMC writes are posted: give the last one time to finish on the pins before CS moves
static void bus_settle(void)
{
	__DSB();
	for (volatile uint32_t i = 0; i < 8; i++) {
	}
}

static void bus_begin(void)
{
	uvf_lcdbus_lock();
	uvf_lcd_pins_fsmc();
	PIN_WRITE(PIN_LCD_CS, false);
}

static void bus_end(void)
{
	bus_settle();
	PIN_WRITE(PIN_LCD_CS, true);
	__DSB();
	*LCD_DATA_ADDR = 0x00;	// panel deselected: only parks the shared lines low
	bus_settle();
	uvf_lcdbus_unlock();
}

static void bus_cmd(uint8_t c)
{
	*LCD_CMD_ADDR = c;
}

static void bus_data(const uint8_t *d, uint32_t n)
{
	volatile uint8_t *port = LCD_DATA_ADDR;
	for (uint32_t i = 0; i < n; i++) {
		*port = d[i];
	}
}

static void bus_pixels(const uint16_t *px, uint32_t n, bool swap_rb)
{
	volatile uint8_t *port = LCD_DATA_ADDR;
	for (uint32_t i = 0; i < n; i++) {
		uint16_t v = px[i];
		if (swap_rb) {
			v = (uint16_t)((v & 0x07E0) | (v >> 11) | (v << 11));
		}
		*port = (uint8_t)(v >> 8);
		*port = (uint8_t)v;
	}
}

static void bus_reset(bool level)
{
	PIN_WRITE(PIN_LCD_RST, level);
}

static const st7735_bus_t bus = {
	.begin = bus_begin,
	.end = bus_end,
	.cmd = bus_cmd,
	.data = bus_data,
	.pixels = bus_pixels,
	.reset = bus_reset,
	.delay_ms = hal_delay_ms,
};

static st7735_t panel = {
	.bus = &bus,
	.type = 0,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.col_offset = 0,
	.row_offset = HIDDEN_ROWS,
};

static bool ready;

// ---- Backlight: TIM1 CC1 paces DMA2 Stream1 channel 6, which copies a 100-word table to the backlight port's BSRR ----
// Entry 0 sets the pin, entry N clears it, the rest are 0: N % duty at about 101 Hz (72 MHz / 7100 / 100)

#define BL_STEPS	100
#define BL_MASK		PIN_MASK(PIN_LCD_BACKLIGHT)

static uint32_t bl_table[BL_STEPS];	// main SRAM (.bss), reachable by DMA2
static uint8_t bl_duty;			// table entry currently clearing the pin, 0 = none
static bool bl_running;

static void bl_dma_stop(void)
{
	DMA2_Stream1->CR &= ~DMA_SxCR_EN;
	while (DMA2_Stream1->CR & DMA_SxCR_EN) {
	}
	DMA2->LIFCR = DMA_LIFCR_CTCIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CFEIF1;
	bl_running = false;
}

static void bl_dma_start(void)
{
	DMA2_Stream1->PAR = (uint32_t)&PIN_PORT(PIN_LCD_BACKLIGHT)->BSRR;
	DMA_RAM_OK(bl_table);
	DMA2_Stream1->M0AR = (uint32_t)bl_table;
	DMA2_Stream1->NDTR = BL_STEPS;
	DMA2_Stream1->FCR = 0;	// direct mode
	DMA2_Stream1->CR = (6u << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 |
		DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_DIR_0;
	DMA2_Stream1->CR |= DMA_SxCR_EN;
	bl_running = true;
}

static void bl_init(void)
{
	stm32_pin_out(PIN_PORT(PIN_LCD_BACKLIGHT), PIN_MASK(PIN_LCD_BACKLIGHT), false, false);
	__HAL_RCC_DMA2_CLK_ENABLE();
	__HAL_RCC_TIM1_CLK_ENABLE();
	for (uint32_t i = 0; i < BL_STEPS; i++) {
		bl_table[i] = 0;
	}
	bl_table[0] = BL_MASK;
	bl_duty = 0;
	bl_dma_stop();

	TIM1->CR1 = 0;
	TIM1->PSC = 0;
	TIM1->ARR = 7100 - 1;
	TIM1->CCR1 = 1;
	TIM1->CCMR1 = 0;		// CC1 output compare, frozen: only the match event is used
	TIM1->DIER = TIM_DIER_CC1DE;	// DMA request on each CC1 match
	TIM1->EGR = TIM_EGR_UG;
	TIM1->CR1 = TIM_CR1_CEN;
}

// 0 = off, 100 or more = fully on, otherwise PWM duty in percent
static void set_backlight(uint8_t percent)
{
	if (!ready || percent == 0 || percent >= BL_STEPS) {
		if (bl_running) {
			bl_dma_stop();
		}
		PIN_WRITE(PIN_LCD_BACKLIGHT, percent > 0);
		return;
	}
	if (bl_duty != 0) {
		bl_table[bl_duty] = 0;
	}
	bl_duty = percent;
	bl_table[bl_duty] = (uint32_t)BL_MASK << 16;
	if (!bl_running) {
		bl_dma_start();
	}
}

// ---- hal_display_t ----

static void init(void)
{
	uint8_t type = 0;

	stm32_pin_out(PIN_PORT(PIN_LCD_CS), PIN_MASK(PIN_LCD_CS), true, false);
	stm32_pin_out(PIN_PORT(PIN_LCD_RST), PIN_MASK(PIN_LCD_RST), true, false);
	stm32_pin_af(GPIOD, LCD_CTRL_PORTD, LCD_AF, false, GPIO_NOPULL);
	stm32_gpio_clock(GPIOE);
	fsmc_init();
	bl_init();

	if (!stm32_spi_flash_read_security(3, 0x1D, &type, 1)) {
		type = 0;
	}
	// hardware-dm1701.md 9.2: what each factory type means is the radio's (the DM-1701 runs its type 2 as type 3 in RGB
	// order, the MD-UV380 keeps every type and is always RGB)
	panel.type = uvf_variant.panel_type[type & 3] & 3;
	panel.force_rgb = uvf_variant.panel_rgb[type & 3];
	st7735_init(&panel);
	ready = true;
}

static void flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	if (!ready || fb == NULL) {
		return;
	}
	st7735_write_rows_indexed(&panel, (const uint8_t *)fb, palette, y0, y1);
}

const hal_display_t uvf_display = {
	// Safe area from the build (the board's config.mk)
	.inset_top = CONFIG_DISPLAY_INSET_TOP, .inset_bottom = CONFIG_DISPLAY_INSET_BOTTOM,
	.inset_left = CONFIG_DISPLAY_INSET_LEFT, .inset_right = CONFIG_DISPLAY_INSET_RIGHT,
	.width = VISIBLE_W,
	.height = VISIBLE_H,
	.bpp = 4,
	.init = init,
	.flush_indexed = flush,
	.set_backlight = set_backlight,
};

uint8_t uvf_display_panel_type(void)
{
	return panel.type;
}
