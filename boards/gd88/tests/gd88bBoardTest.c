// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD-88's SoC B is a receiver only (boards/gd88b/board.c, docs/spec/hardware-gd88.md 10): every band of its
// descriptor is receive only, so the shared radio code's tx() (board_in_band(hz, true), common/radio_gd88.c) refuses
// every frequency it can tune, and it is built with GD88_TX=0 besides. Its transmitter-off hook drives every TX
// switch low and powers the PA drive down. Built against a host stand-in for the platform header (hostplat/)
#include <stdio.h>
#include <string.h>
#include "../../gd88b/board.c"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

uint32_t host_io_diplex0, host_io_diplex2, host_dac_data_b, host_dac_pd_ctrl;
static int8_t level[128];	// -1: never driven

void hrc_pin_out(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
void hrc_pin_in(uint8_t pin) { (void)pin; }
void hrc_pin_write(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
void hrc_pin_force(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
const hal_system_t hrc_system;
const hal_radio_t gd88_radio;
void gd88_path_init(void) {}
bool gd88b_diag_line(uint8_t i, char *buf, uint8_t len) { (void)i; (void)buf; (void)len; return false; }
bool gd88_cal_load(const uint8_t *flash_page, gd88_cal_read_fn nand) { (void)flash_page; (void)nand; return false; }

int main(void)
{
	// Receive only: no band may transmit, and every tunable frequency is refused for transmit
	CHECK(board.band_count == 2);
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		CHECK(!b->tx);
		for (uint32_t hz = b->lo_hz; hz <= b->hi_hz; hz += 12500u) {
			if (!board_in_band(hz, false) || board_in_band(hz, true)) {
				printf("FAIL %u Hz: receive %d, transmit %d\n", (unsigned)hz, board_in_band(hz, false),
				       board_in_band(hz, true));
				failures++;
				break;
			}
		}
	}
	CHECK(board_in_band(146520000, false) && !board_in_band(146520000, true));
	CHECK(board_in_band(446000000, false) && !board_in_band(446000000, true));
#if GD88_TX
	CHECK(!"SoC B must build without the transmit path (GD88_TX=0)");
#endif

	// The transmitter-off hook: every TX switch low, the PA drive at 0 and powered down
	memset(level, -1, sizeof level);
	host_dac_data_b = 0xFFFu;
	host_dac_pd_ctrl = 0;
	board_rf_safe();
	const uint8_t tx_pins[] = { PIN_PA_SUP_UHF, PIN_PA_SUP_VHF, PIN_PA_SEL_UHF, PIN_PA_SEL_VHF, PIN_TX_PATH_UHF,
				    PIN_TX_PATH_VHF };
	for (size_t i = 0; i < sizeof tx_pins; i++) {
		CHECK(level[tx_pins[i] & 0x7F] == 0);
	}
	CHECK(host_dac_data_b == 0 && (host_dac_pd_ctrl & DAC_B_OFF) != 0);

	if (failures) {
		printf("gd88bBoardTest: %d failures\n", failures);
		return 1;
	}
	printf("gd88bBoardTest: all passed\n");
	return 0;
}
