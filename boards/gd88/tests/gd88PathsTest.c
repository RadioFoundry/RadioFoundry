// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SoC A's audio routing (boards/gd88/paths_gd88.c) against a recorded serial chain. The chain is the board's analog
// crosspoint: bit 4 of bytes 2-5 connects the microphone to one transmit audio input (byte 2 FM A, 4 DMR A). The
// microphone's path follows A's mode, is held through a transmission (DMR transmitted silence while the path was
// cleared at key-up), changes make-before-break, and a transmission never writes bytes 6 and 7 (the A/B repeat's
// cross-feed). Built against a host stand-in for the platform header (hostplat/)
#include <stdio.h>
#include <string.h>
#include "../paths_gd88.c"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

uint32_t host_io_diplex0, host_io_diplex2, host_dac_data_b, host_dac_pd_ctrl;
static int8_t level[128];
static uint64_t chain;
static unsigned shifts;
static bool mic_gap;		// a shifted word with no microphone path (after the first)

void hrc_pin_out(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
void hrc_pin_in(uint8_t pin) { (void)pin; }
void hrc_pin_write(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
void hrc_pin_force(uint8_t pin, bool on) { level[pin & 0x7F] = on; }
uint32_t hal_millis(void) { return 0; }
void gd88_chain_init(void) { chain = 0; shifts = 0; mic_gap = false; }

#define MIC_BITS	((1ull << 20) | (1ull << 28) | (1ull << 36) | (1ull << 44))
#define FM_A		(1ull << 20)
#define DMR_A		(1ull << 36)
#define REPEAT_BYTES	0xFFFF000000000000ull

void gd88_chain_set(uint8_t b, bool on)
{
	uint64_t v = on ? (chain | (1ull << b)) : (chain & ~(1ull << b));
	if (v != chain) {
		chain = v;
		if (shifts++ > 0 && (chain & MIC_BITS) == 0) {
			mic_gap = true;
		}
	}
}

int main(void)
{
	// Start-up: one path, A's FM (the mode before the first tune)
	gd88_path_init();
	CHECK((chain & MIC_BITS) == FM_A);
	CHECK(level[PIN_TX_RX & 0x7F] == 0);

	// A tuned to DMR: the DMR path only
	gd88_path_rx(461137500u, true, true);
	CHECK((chain & MIC_BITS) == DMR_A);

	// A DMR transmission, burst by burst: PTB2 follows the bursts, the chain never changes
	for (int burst = 0; burst < 3; burst++) {
		unsigned before = shifts;
		gd88_path_rx_front_off();
		CHECK(level[PIN_TX_RX & 0x7F] == 1);
		CHECK((chain & MIC_BITS) == DMR_A);
		gd88_path_tx_off();
		gd88_path_rx(461137500u, true, true);
		CHECK(level[PIN_TX_RX & 0x7F] == 0);
		CHECK(shifts == before);
	}
	CHECK((chain & REPEAT_BYTES) == 0);

	// B taking the speaker doesn't move the microphone
	gd88_audio_b(true, false);
	CHECK((chain & MIC_BITS) == DMR_A);
	gd88_audio_b(false, false);

	// The receiver stopped keeps the last mode's path: a transmission may follow
	gd88_path_rx(461137500u, false, false);
	CHECK((chain & MIC_BITS) == DMR_A);

	// FM: the FM path, held through the transmission
	gd88_path_rx(146520000u, true, false);
	CHECK((chain & MIC_BITS) == FM_A);
	gd88_path_rx_front_off();
	CHECK((chain & MIC_BITS) == FM_A);
	gd88_path_tx_off();
	CHECK((chain & MIC_BITS) == FM_A);
	CHECK((chain & REPEAT_BYTES) == 0);

	// Every mode change kept a path connected
	CHECK(!mic_gap);

	if (failures) {
		printf("gd88PathsTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("gd88PathsTest: ok\n");
	return 0;
}
