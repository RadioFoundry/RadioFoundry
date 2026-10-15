// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// ST7567 monochrome LCD driver (docs/spec/hardware-dm1801pro.md 3)
#include "st7567.h"

// Command set of the ST7567 / UC1701 family
#define CMD_START_LINE		0x40u	// | line
#define CMD_SEG_NORMAL		0xA0u
#define CMD_SEG_REVERSE		0xA1u
#define CMD_ALL_PIXELS_OFF	0xA4u
#define CMD_NORMAL		0xA6u	// 1 = pixel on
#define CMD_BIAS_1_9		0xA2u
#define CMD_BIAS_1_7		0xA3u
#define CMD_DISPLAY_OFF		0xAEu
#define CMD_DISPLAY_ON		0xAFu
#define CMD_COM_NORMAL		0xC0u
#define CMD_COM_REVERSE		0xC8u
#define CMD_RESET		0xE2u
#define CMD_POWER		0x28u	// | booster 4, regulator 2, follower 1
#define CMD_REGULATOR		0x20u	// | ratio 0..7
#define CMD_VOLUME		0x81u	// followed by 0..63
#define CMD_PAGE		0xB0u	// | page
#define CMD_COL_HIGH		0x10u	// | high nibble
#define CMD_COL_LOW		0x00u	// | low nibble

void st7567_page_bytes(const uint8_t *fb, uint8_t page, uint8_t out[ST7567_WIDTH])
{
	const uint8_t *rows = fb + (uint32_t)page * 8u * (ST7567_WIDTH / 8u);
	for (uint8_t x = 0; x < ST7567_WIDTH; x++) {
		uint8_t b = 0;
		for (uint8_t r = 0; r < 8; r++) {
			if (rows[r * (ST7567_WIDTH / 8u) + (x >> 3)] & (0x80u >> (x & 7u))) {
				b |= (uint8_t)(1u << r);
			}
		}
		out[x] = b;
	}
}

static void go_to(const st7567_t *p, uint8_t page)
{
	p->bus->cmd((uint8_t)(CMD_PAGE | page));
	p->bus->cmd((uint8_t)(CMD_COL_HIGH | (p->col_offset >> 4)));
	p->bus->cmd((uint8_t)(CMD_COL_LOW | (p->col_offset & 0x0Fu)));
}

void st7567_init(const st7567_t *p)
{
	const st7567_bus_t *b = p->bus;
	b->reset(false);
	b->delay_ms(2);
	b->reset(true);
	b->delay_ms(10);
	b->begin();
	b->cmd(CMD_RESET);
	b->cmd(p->bias != 0 ? CMD_BIAS_1_7 : CMD_BIAS_1_9);
	b->cmd(p->seg_reverse ? CMD_SEG_REVERSE : CMD_SEG_NORMAL);
	b->cmd(p->com_reverse ? CMD_COM_REVERSE : CMD_COM_NORMAL);
	b->cmd(CMD_START_LINE);
	b->cmd((uint8_t)(CMD_REGULATOR | (p->regulator & 7u)));
	b->cmd(CMD_VOLUME);
	b->cmd(p->contrast & 0x3Fu);
	b->cmd(CMD_POWER | 7u);
	b->cmd(CMD_ALL_PIXELS_OFF);
	b->cmd(CMD_NORMAL);
	static const uint8_t zero[ST7567_WIDTH];
	for (uint8_t pg = 0; pg < ST7567_PAGES; pg++) {
		go_to(p, pg);
		b->data(zero, ST7567_WIDTH);
	}
	b->cmd(CMD_DISPLAY_ON);
	b->end();
}

void st7567_write_rows(const st7567_t *p, const uint8_t *fb, uint16_t y0, uint16_t y1)
{
	if (y1 > ST7567_HEIGHT) {
		y1 = ST7567_HEIGHT;
	}
	if (y0 >= y1) {
		return;
	}
	uint8_t line[ST7567_WIDTH];
	p->bus->begin();
	for (uint8_t pg = (uint8_t)(y0 / 8u); pg <= (uint8_t)((y1 - 1u) / 8u); pg++) {
		st7567_page_bytes(fb, pg, line);
		go_to(p, pg);
		p->bus->data(line, ST7567_WIDTH);
	}
	p->bus->end();
}

void st7567_set_contrast(const st7567_t *p, uint8_t contrast)
{
	p->bus->begin();
	p->bus->cmd(CMD_VOLUME);
	p->bus->cmd(contrast & 0x3Fu);
	p->bus->end();
}

void st7567_display_on(const st7567_t *p, bool on)
{
	p->bus->begin();
	p->bus->cmd(on ? CMD_DISPLAY_ON : CMD_DISPLAY_OFF);
	p->bus->end();
}
