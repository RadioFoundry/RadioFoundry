// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 APRS AFSK for the boards (hrc_afsk.c, hal_afsk.h, docs/spec/aprs.md 5): transmit through the modem's prompt
// FIFO, receive from its FM receive audio (fm_rx_audio.h). The board supplies the audio routing.
#ifndef HRC_AFSK_H
#define HRC_AFSK_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_afsk.h"
#include "modem/hrc7000/hrc7000.h"

typedef struct {
	// Transmit routing: on puts the prompt FIFO on the modulator with the microphone muted, off undoes it. Refuses
	// (false) when the radio isn't transmitting FM. Task level
	bool (*tx_route)(bool on);
	// Receive routing while APRS listens: the FM audio stays on the HR_C7000's input (fm_rx_audio.h). May be NULL
	void (*listen)(bool on);
	// PCM amplitude per level step (level 50 is the nominal 3 kHz peak; the modulator gain differs per board)
	int16_t amp_per_level;
} hrc_afsk_board_t;

#if CONFIG_APRS
// Once, from the radio init, after the modem is attached
void hrc_afsk_attach(hrc7000_t *modem, const hrc_afsk_board_t *board);

// Receive works only with CONFIG_APRS_RX and the DMR glue's system interrupt (CONFIG_DMR); otherwise rx_start is NULL
extern const hal_afsk_t hrc_afsk;
#endif

#endif
