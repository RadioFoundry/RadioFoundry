// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Start-up and transport for the SCT basebands (docs/spec/modem-sct.md 1-2). The two chips speak the same packet
// protocol (sct_pkt.h) and differ below it:
//
// | Chip    | Host link                       | Data waiting (HOBIB) | Boot                                        |
// |---------|---------------------------------|----------------------|---------------------------------------------|
// | SCT3258 | 8-bit parallel port, 16-bit mode | active low           | the host sends a 1024-word block every reset |
// | SCT3288 | SPI mode 0, 16-bit transfers     | active high          | boot ROM in the chip; the host may start it  |
//
// The board owns the link (pins, strobes, the HOBIB polarity, the reset sequence) and says which boot the chip
// takes and how its audio is set up (sct_voice_t). The boot here waits on the chip; it runs once at start-up. Everything
// after it is non-blocking: sct_host_poll hands each received packet on.
#ifndef SCT_HOST_H
#define SCT_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "sct_pkt.h"

#define SCT_BOOT_BLOCK_BYTES	2048	// SCT3258: exactly 1024 16-bit words

typedef enum {
	SCT_BOOT_BLOCK,		// SCT3258: send boot_block, then the chip reports ACK RESET_READY
	SCT_BOOT_ROM,		// SCT3288: the boot ROM greets with ACK NOT_LOADED; the start command runs the application
} sct_boot_t;

// The voice set-up the boot sends once the application runs (docs/spec/modem-sct.md 2). A zero selection is not sent
// and leaves the chip's default
typedef struct {
	uint8_t codec;				// CODEC_SELECT: the codec chip on the baseband's audio port
	uint8_t vocoder;			// VOCODER_SEL: the vocoder for voice calls
	// Then these control fields (TYPE 0), one packet each: the digital mode gains, for example
	const sct_field_t *fields;
	uint8_t field_count;
	const sct_codec_write_t *codec_writes;	// then these, in order: gains and routing for the board's audio wiring
	uint8_t codec_count;
} sct_voice_t;

typedef struct {
	// Bytes to the chip, an even count (16-bit words, low byte first on the parallel port). False on a bus fault
	bool (*write)(const uint8_t *buf, uint16_t len, void *ctx);
	// What the chip has waiting, up to max bytes (even); 0 when HOBIB says nothing waits
	uint16_t (*read)(uint8_t *buf, uint16_t max, void *ctx);
	// Power and reset sequence (enables, strap, RSTN), ending with the chip out of reset and ready for its boot
	void (*reset)(void *ctx);
	void (*delay_ms)(uint32_t ms);
	uint32_t (*now_ms)(void);
	void *ctx;
	sct_boot_t boot;
	const uint8_t *boot_block;	// SCT_BOOT_BLOCK: the user's own block (never shipped), SCT_BOOT_BLOCK_BYTES
	const sct_voice_t *voice;	// NULL: no voice set-up
} sct_host_t;

typedef enum {
	SCT_BOOT_OK,		// the application runs
	SCT_BOOT_NO_BLOCK,	// SCT_BOOT_BLOCK without a block
	SCT_BOOT_BUS,		// a write failed
	SCT_BOOT_NO_APP,	// the chip answered but has no application (ACK OK after the block, or NOT_LOADED stays)
	SCT_BOOT_SILENT,	// no answer in time
	SCT_BOOT_NO_VOICE,	// the application runs, but a voice set-up write was refused or not answered in time
} sct_boot_result_t;

// Reset the chip, boot it and send the voice set-up. Packets that come with the answers are dropped
sct_boot_result_t sct_host_boot(const sct_host_t *h, sct_rx_t *rx, uint32_t timeout_ms);

// Send one built packet
bool sct_host_send(const sct_host_t *h, const uint8_t *buf, uint16_t len);

typedef void (*sct_packet_cb_t)(const uint8_t *pkt, uint16_t len, void *ctx);
// Read what the chip has waiting and pass on each whole packet. Returns the bytes read
uint16_t sct_host_poll(const sct_host_t *h, sct_rx_t *rx, sct_packet_cb_t cb, void *ctx);

// The ACK_MESSAGE code in a packet, -1 if it has none
int sct_ack_code(const uint8_t *pkt, uint16_t len);

#endif
