// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV DMR baseband: the SCT3258 on its 8-bit host port (docs/spec/hardware-d578uv.md 4), bit-banged on GPIO as
// the stock firmware does it, and the SCT driver (drivers/modem/sct) on top. The boot block comes from the codec slot
// the flashing tool fills from the user's own stock file; without it the radio runs FM only.
#include <stddef.h>
#include <string.h>
#include "d578uv.h"
#include "config.h"
#include "pins.h"
#include "mcu_io.h"
#include "hal_time.h"
#include "modem/sct/sct_host.h"
#include "modem/sct/sct_dmr.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#if CONFIG_DMR

#define MODEM_TASK_STACK	256
#define MODEM_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define MODEM_POLL_MS		2
#define BOOT_TIMEOUT_MS		2000
// CRC-32 (zlib) of the 2048-byte boot block every stock image holds (SHA-256 743515b9...): the slot holds it, not an
// erased page or something else
#define BOOT_BLOCK_CRC32	0x024F600Au

extern const uint8_t __codec_slot[];

// The voice set-up (hardware-d578uv.md 4.4): what the stock firmware sends after the boot. The codec is a
// TLV320AIC3204, the vocoder AMBE+2
#define VOICE_CODEC		1
#define VOICE_VOCODER		4
// The calibration block (hardware-d578uv.md 7): current copy marked at +CAL_MARKS. The audio bytes start at CAL_AUDIO:
// four codec levels (line out left and right, the right input, the headphone driver), then the digital mode speaker
// and microphone gains (MSB first) at CAL_SPK and CAL_MIC. DMR_OFFSET's 8 bytes sit at CAL_OFFSET
#define CAL_A			0x02F60000u
#define CAL_B			0x02F80000u
#define CAL_MARKS		0x1FFF8u
#define CAL_AUDIO		0x0D2u
#define CAL_SPK			7	// 0x0D9, from CAL_AUDIO
#define CAL_MIC			9	// 0x0DB
#define CAL_OFFSET		0x140u
// No calibration: the stock factory defaults (0x0D7-0x0D8, a demodulator gain, goes unused: the stock firmware sends 0)
static const uint8_t cal_audio_default[11] = {
	0x08, 0x04, 0x14, 0x14, 0x00, 0x00, 0x00, 0x05, 0x46, 0x07, 0xD0,
};
static const uint8_t cal_offset_default[8] = { 0x00, 0x82, 0x02, 0xD0, 0x02, 0xD0, 0x02, 0xD0 };
// The middle of the stock firmware's five microphone gain steps
#define MIC_GAIN		0x20

static bool booted;
static sct_boot_result_t boot_result = SCT_BOOT_SILENT;
static SemaphoreHandle_t lock;
static sct_rx_t rx;

// Strobes a little over 30 ns: the bus has no wait line, so the stock firmware's timing is the guide
static inline void settle(void)
{
	__asm volatile("nop\n nop\n nop\n nop");
}

static void data_dir(bool out)
{
	mcu_port_high_byte_dir(HPI_DATA_PORT, out);	// PD8-PD15
}

static bool hpi_write(const uint8_t *buf, uint16_t len, void *ctx)
{
	(void)ctx;
	data_dir(true);
	for (uint16_t i = 0; i < len; i++) {
		uint8_t v = buf[i];

		mcu_port_put(HPI_DATA_PORT, (uint16_t)(v << HPI_DATA_SHIFT), (uint16_t)((uint8_t)~v << HPI_DATA_SHIFT));
		mcu_pin_write(PIN_HPI_CS, false);
		mcu_pin_write(PIN_HPI_WR, false);
		settle();
		mcu_pin_write(PIN_HPI_WR, true);
		mcu_pin_write(PIN_HPI_CS, true);
		settle();
	}
	data_dir(false);
	return true;
}

static uint8_t read_byte(void)
{
	mcu_pin_write(PIN_HPI_CS, false);
	mcu_pin_write(PIN_HPI_RD, false);
	settle();
	uint8_t v = (uint8_t)(mcu_port_read(HPI_DATA_PORT) >> HPI_DATA_SHIFT);

	mcu_pin_write(PIN_HPI_RD, true);
	mcu_pin_write(PIN_HPI_CS, true);
	settle();
	return v;
}

// HOBIB low: the chip has a word waiting. Whole words only (16-bit mode)
static uint16_t hpi_read(uint8_t *buf, uint16_t max, void *ctx)
{
	(void)ctx;
	uint16_t n = 0;

	while (n + 2 <= max && !mcu_pin_read(PIN_HPI_HOBI)) {
		buf[n++] = read_byte();
		buf[n++] = read_byte();
	}
	return n;
}

// The stock power-up: supply on, strobes idle high, PIO1 low (Intel bus), RSTN high 100 ms then low, 500 ms
static void hpi_reset(void *ctx)
{
	(void)ctx;
	mcu_pin_out(PIN_SCT_ENABLE, true, false);
	mcu_pin_out(PIN_HPI_CS, true, false);
	mcu_pin_out(PIN_HPI_WR, true, false);
	mcu_pin_out(PIN_HPI_RD, true, false);
	mcu_pin_out(PIN_HPI_PD1, false, false);
	mcu_pin_out(PIN_HPI_PD7, false, false);
	mcu_pin_out(PIN_SCT_PIO1, false, false);
	mcu_pin_out(PIN_SCT_PC7, false, false);
	mcu_pin_out(PIN_SCT_PC6, true, false);
	mcu_pin_in(PIN_HPI_HOBI, MCU_PULL_NONE);
	data_dir(false);
	hal_delay_ms(100);
	mcu_pin_out(PIN_HPI_RESET, true, false);
	hal_delay_ms(100);
	mcu_pin_write(PIN_HPI_RESET, false);
	hal_delay_ms(500);
}

static uint32_t crc32(const uint8_t *p, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu;

	while (len-- != 0) {
		crc ^= *p++;
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

#define CODEC_HPL		3	// the headphone driver's write in codec_writes

static sct_codec_write_t codec_writes[] = {
	{ 1, 0x12, 0 },			// level from calibration
	{ 1, 0x13, 0 },			// level from calibration
	{ 1, 0x3C, 0 },			// level from calibration
	{ 1, 0x10, 0 },			// level from calibration (CODEC_HPL)
	{ 1, 0x34, 0x80 },
	{ 0, 0x56, 0x80 | MIC_GAIN },
	{ 0, 0x58, MIC_GAIN },
};

// The digital mode speaker and microphone gains (Q16.11, 2048 = 0 dB), from calibration. The stock firmware sends
// others only for settings RadioFoundry doesn't have (a quieter "indoor" volume, an equalizer preset)
static uint8_t spk_gain[2];
static uint8_t mic_gain[2];

static const sct_field_t voice_fields[] = {
	{ SCT_F_DIGI_SPK_GAIN, sizeof(spk_gain), spk_gain },
	{ SCT_F_DIGI_MIC_GAIN, sizeof(mic_gain), mic_gain },
};

static const sct_voice_t voice = {
	.codec = VOICE_CODEC,
	.vocoder = VOICE_VOCODER,
	.fields = voice_fields,
	.field_count = sizeof(voice_fields) / sizeof(voice_fields[0]),
	.codec_writes = codec_writes,
	.codec_count = sizeof(codec_writes) / sizeof(codec_writes[0]),
};

static bool cal_current(uint32_t base)
{
	uint8_t m[8];

	if (!d578uv_nand_read(base + CAL_MARKS, m, sizeof(m)))
		return false;
	bool superseded = m[0] == 0xAA && m[1] == 0xAA && m[2] == 0x55 && m[3] == 0x55;
	bool written = m[4] == 0x55 && m[5] == 0x55 && m[6] == 0xAA && m[7] == 0xAA;

	return written && !superseded;
}

// The rest of the stock DMR start (hardware-d578uv.md 4.4): MODOUT_CONFIG's fixed bytes, DMR_OFFSET from
// calibration, the squelch's fixed levels (no gain adjustment, LNA 2, AGC, OOBE every 80 ms, carrier thresholds
// 0x1770 / 0x157C for digital and analog) and the demodulator's auto calibration
static const uint8_t modout_config[5] = { 0x00, 0x10, 0x00, 0x00, 0x00 };
static uint8_t dmr_offset[8];
static const uint8_t sq_level[12] = { 0x00, 0x02, 0x00, 0x3C, 0x17, 0x70, 0x15, 0x7C, 0x17, 0x70, 0x15, 0x7C };
static const uint8_t demod_gain[2] = { 0x00, 0x00 };

static const sct_field_t start_fields[] = {
	{ SCT_F_MODOUT_CONFIG, sizeof(modout_config), modout_config },
	{ SCT_F_DMR_OFFSET, sizeof(dmr_offset), dmr_offset },
	{ SCT_F_SQ_LEVEL, sizeof(sq_level), sq_level },
	{ SCT_F_DEMOD_GAIN, sizeof(demod_gain), demod_gain },
};

// The headphone driver muted while transmitting; codec_writes[CODEC_HPL] restores it
static const sct_codec_write_t hpl_mute = { 1, 0x10, 0x40 };

// The calibrated values: the four codec levels into the first four writes, the digital mode gains, and DMR_OFFSET
static void cal_values(void)
{
	uint8_t v[sizeof(cal_audio_default)];
	uint32_t base = !d578uv_nand_ready() ? 0 : cal_current(CAL_A) ? CAL_A : cal_current(CAL_B) ? CAL_B : 0;

	if (base == 0 || !d578uv_nand_read(base + CAL_AUDIO, v, sizeof(v)))
		memcpy(v, cal_audio_default, sizeof(v));
	for (int i = 0; i < 4; i++)
		codec_writes[i].value = v[i];
	memcpy(spk_gain, &v[CAL_SPK], sizeof(spk_gain));
	memcpy(mic_gain, &v[CAL_MIC], sizeof(mic_gain));
	if (base == 0 || !d578uv_nand_read(base + CAL_OFFSET, dmr_offset, sizeof(dmr_offset)))
		memcpy(dmr_offset, cal_offset_default, sizeof(dmr_offset));
}

static const sct_host_t host = {
	.write = hpi_write,
	.read = hpi_read,
	.reset = hpi_reset,
	.delay_ms = hal_delay_ms,
	.now_ms = hal_millis,
	.boot = SCT_BOOT_BLOCK,
	.boot_block = __codec_slot,
	.voice = &voice,
};

static bool port_send(const uint8_t *buf, uint16_t len, void *ctx)
{
	(void)ctx;
	return sct_host_send(&host, buf, len);
}

static bool port_ready(void *ctx)
{
	(void)ctx;
	return booted;
}

static void port_lock(void *ctx)
{
	(void)ctx;
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
		xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

static void port_unlock(void *ctx)
{
	(void)ctx;
	if (lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
		xSemaphoreGiveRecursive(lock);
}

// DMR_CALL_OPTION and REPORT_FIELD as the stock firmware sends them (hardware-d578uv.md 4.4), but for byte 3. Byte
// 0: the chip's squelch detection on (bit 6), no RF control (the MCU keys the RF chips). Byte 2: four good CACH,
// two LC header repeats. Byte 3: every CSBK, LC header and terminator reported. The stock 0x44 reports identical LC
// headers and terminators once, and whether the chip forgets them between calls is not known: a second call with
// the same LC could go unreported. The stack takes repeats as from a burst modem. Bit 6 of bytes 1 and 3 and byte 4
// are reserved in the Packet Interface document; sent as the stock firmware sends them
static const sct_dmr_port_t port = {
	.send = port_send,
	.call_option = { 0x41, 0x40, 0x42, 0x47, 0x08 },
	.report_field = 0xFF,
	.start_fields = start_fields,
	.start_count = sizeof(start_fields) / sizeof(start_fields[0]),
	.codec_tx = &hpl_mute,
	.codec_rx = &codec_writes[CODEC_HPL],
	.ready = port_ready,
	.lock = port_lock,
	.unlock = port_unlock,
};

static void on_packet(const uint8_t *pkt, uint16_t len, void *ctx)
{
	(void)ctx;
	sct_dmr_packet(pkt, len, hal_millis());
}

static void modem_task(void *arg)
{
	(void)arg;
	for (;;) {
		port_lock(NULL);
		sct_host_poll(&host, &rx, on_packet, NULL);
		port_unlock(NULL);
		sct_dmr_poll(hal_millis());
		vTaskDelay(pdMS_TO_TICKS(MODEM_POLL_MS));
	}
}

bool d578uv_modem_init(void)
{
	sct_dmr_init(&port);
	if (crc32(__codec_slot, SCT_BOOT_BLOCK_BYTES) != BOOT_BLOCK_CRC32) {
		boot_result = SCT_BOOT_NO_BLOCK;
		return false;
	}
	cal_values();
	boot_result = sct_host_boot(&host, &rx, BOOT_TIMEOUT_MS);
	booted = boot_result == SCT_BOOT_OK;
	if (!booted)
		return false;
	lock = xSemaphoreCreateRecursiveMutex();
	xTaskCreate(modem_task, "modem", MODEM_TASK_STACK, NULL, MODEM_TASK_PRIORITY, NULL);
	return true;
}

uint8_t d578uv_modem_boot_result(void)
{
	return (uint8_t)boot_result;
}

#endif
