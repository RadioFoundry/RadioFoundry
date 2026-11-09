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

#define MODEM_TASK_STACK	288
#define MODEM_TASK_PRIORITY	(configMAX_PRIORITIES - 2)
#define MODEM_POLL_MS		2
// The chip answers about 2.15 s after the boot block (measured at HOBIB on a radio): 2 s gave up just before it
#define BOOT_TIMEOUT_MS		5000
#define BOOT_POLL_MS		10
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

static volatile bool boot_done;		// the boot has ended, either way (modem_task)
static bool booted;
static sct_boot_result_t boot_result = SCT_BOOT_SILENT;
static SemaphoreHandle_t lock;
static sct_rx_t rx;

// The bus has no wait line, so the stock firmware's timing is the guide: a counted loop of about 0.7 us inside each
// strobe (0x080932C2). Strobes of about 30 ns got no answer on a radio
#define STROBE_US		1u
#define RESET_RUN_MS		100u	// after RSTN goes high, before the boot block
#define SUPPLY_OFF_MS		300u	// supply and lines held low before the power-up
static inline void settle(void)
{
	hal_delay_us(STROBE_US);
}

// PD8-PD15: outputs only while a write runs; between writes inputs with pull-up, as the stock firmware leaves them
static void data_dir(bool out)
{
	if (out) {
		mcu_port_high_byte_dir(HPI_DATA_PORT, true);
	} else {
		mcu_port_high_byte_pullup(HPI_DATA_PORT);
	}
}

// The stock write (0x080933A0): CS low once for the whole transfer; per byte WR low, then the data, then WR high
// (the chip latches on WR's rise, inferred). No handshake: the boot block's 2048 bytes go out back to back
static bool hpi_write(const uint8_t *buf, uint16_t len, void *ctx)
{
	(void)ctx;
	data_dir(true);
	mcu_pin_write(PIN_HPI_CS, false);
	for (uint16_t i = 0; i < len; i++) {
		uint8_t v = buf[i];

		mcu_pin_write(PIN_HPI_WR, false);
		settle();
		mcu_port_put(HPI_DATA_PORT, (uint16_t)(v << HPI_DATA_SHIFT), (uint16_t)((uint8_t)~v << HPI_DATA_SHIFT));
		settle();
		mcu_pin_write(PIN_HPI_WR, true);
		settle();
	}
	mcu_pin_write(PIN_HPI_CS, true);
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

// The stock power-up: supply on, strobes idle high, PIO1 driven low (the Intel bus strap), RSTN high 100 ms, low
// 500 ms (the reset), then high again: the chip runs and latches PIO1 (seen on a radio: left low, it never answered)
static void hpi_reset(void *ctx)
{
	(void)ctx;
	// Off first, as the stock firmware starts (0x08092E98): supply low and every line low, so the chip powers up from
	// cold even after a warm reset of the MCU (the GPIO reset state floats the supply enable and may leave it on)
	mcu_pin_out(PIN_SCT_ENABLE, false, false);
	mcu_pin_out(PIN_HPI_CS, false, false);
	mcu_pin_out(PIN_HPI_WR, false, false);
	mcu_pin_out(PIN_HPI_RD, false, false);
	mcu_pin_out(PIN_HPI_RESET, false, false);
	mcu_pin_out(PIN_HPI_PD1, false, false);
	mcu_pin_out(PIN_HPI_PD7, false, false);
	mcu_pin_out(PIN_SCT_PIO1, false, false);
	mcu_pin_out(PIN_SCT_PC7, false, false);
	mcu_pin_out(PIN_SCT_PC6, false, false);
	data_dir(true);
	mcu_port_put(HPI_DATA_PORT, 0, (uint16_t)(0xFFu << HPI_DATA_SHIFT));
	hal_delay_ms(SUPPLY_OFF_MS);
	data_dir(false);
	mcu_pin_out(PIN_SCT_ENABLE, true, false);
	mcu_pin_out(PIN_HPI_CS, true, false);
	mcu_pin_out(PIN_HPI_WR, true, false);
	mcu_pin_out(PIN_HPI_RD, true, false);
	mcu_pin_out(PIN_HPI_PD1, false, false);
	mcu_pin_out(PIN_HPI_PD7, false, false);
	mcu_pin_out(PIN_SCT_PIO1, false, false);	// the strap: driven low through RSTN's rise selects the Intel bus
	mcu_pin_out(PIN_SCT_PC7, false, false);
	mcu_pin_out(PIN_SCT_PC6, true, false);
	mcu_pin_in(PIN_HPI_HOBI, MCU_PULL_UP);	// as the stock firmware: an undriven line reads idle, not "a word waiting"
	data_dir(false);
	hal_delay_ms(100);
	mcu_pin_out(PIN_HPI_RESET, true, false);
	hal_delay_ms(100);
	mcu_pin_write(PIN_HPI_RESET, false);
	hal_delay_ms(500);
	mcu_pin_write(PIN_HPI_RESET, true);
	// The chip has latched PIO1: released to the pull-down, as the stock firmware does after the rise (0x0809304C);
	// a pull-down alone loses to the chip's side and the strap read Motorola (seen on a radio: no answer)
	mcu_pin_in(PIN_SCT_PIO1, MCU_PULL_DOWN);
	// The stock firmware sends the boot block from a task well after this; the chip's ROM gets time to start
	hal_delay_ms(RESET_RUN_MS);
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

// The modulator's gain, by band: the VCO's modulation gain differs, so one value gives the outer symbols' 1944 Hz on
// one band only. At the chip's default (0 dB) the deviation is 1255 Hz at 446 MHz and about 430 Hz at 151 MHz. 0x0C60
// (+3.8 dB) gives 1941 Hz at 446 MHz, 0x2475 (+13.1 dB) 1944 Hz at 151.5 MHz and about 1910 Hz at 146.5 MHz
// (measured). Sent as a control field before each transmission (tx_fields): in the DMR start it changed nothing
static const uint8_t mod_gain_uhf[2] = { 0x0C, 0x60 };
static const uint8_t mod_gain_vhf[2] = { 0x24, 0x75 };

// MODOUT_CONFIG: byte 0 the delay of LOR against LOL in 10.4 us steps, signed; bytes 1-4 as the stock firmware sends
// them. The 4FSK is on LOL, and LOL later by 21 steps (0xEB) puts the inbound burst on the repeater's grid with the DAC
// filter of codec_start (hardware-d578uv.md 5.1). Positive values moved it by less than a sample
static const uint8_t modout_config[5] = { 0xEB, 0x10, 0x00, 0x00, 0x00 };

static const sct_field_t tx_uhf[] = {
	{ SCT_F_MOD_GAIN, sizeof(mod_gain_uhf), mod_gain_uhf },
	{ SCT_F_MODOUT_CONFIG, sizeof(modout_config), modout_config },
};
static const sct_field_t tx_vhf[] = {
	{ SCT_F_MOD_GAIN, sizeof(mod_gain_vhf), mod_gain_vhf },
	{ SCT_F_MODOUT_CONFIG, sizeof(modout_config), modout_config },
};

static uint8_t port_tx_fields(const sct_field_t **fields, void *ctx)
{
	(void)ctx;
	// The core keys the RF chips before the modem's transmission begins, so the TX frequency is this one's
	*fields = d578uv_tx_hz() >= D578UV_UHF_FROM_HZ ? tx_uhf : tx_vhf;
	return sizeof(tx_uhf) / sizeof(tx_uhf[0]);
}

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

// The rest of the stock DMR start (hardware-d578uv.md 4.4) but MODOUT_CONFIG (in tx_fields): DMR_OFFSET from
// calibration, the squelch's fixed levels (no gain adjustment, LNA 2, AGC, OOBE every 80 ms, carrier thresholds
// 0x1770 / 0x157C for digital and analog) and the demodulator's auto calibration
static uint8_t dmr_offset[8];
static const uint8_t sq_level[12] = { 0x00, 0x02, 0x00, 0x3C, 0x17, 0x70, 0x15, 0x7C, 0x17, 0x70, 0x15, 0x7C };
static const uint8_t demod_gain[2] = { 0x00, 0x00 };

static const sct_field_t start_fields[] = {
	{ SCT_F_DMR_OFFSET, sizeof(dmr_offset), dmr_offset },
	{ SCT_F_SQ_LEVEL, sizeof(sq_level), sq_level },
	{ SCT_F_DEMOD_GAIN, sizeof(demod_gain), demod_gain },
};

// The codec's DAC on processing block PRB_P17 (page 0 register 60): interpolation filter C, 13 samples of delay where
// the baseband's own set-up has 18, so 0.41 ms less on the 4FSK's way out (hardware-d578uv.md 5.1). It takes effect
// with the DAC running
static const sct_codec_write_t codec_start[] = { { 0, 0x3C, 17 } };

// The headphone driver muted while transmitting; codec_writes[CODEC_HPL] restores it
static const sct_codec_write_t hpl_mute = { 1, 0x10, 0x40 };

// The calibrated values: the four codec levels into the first four writes, the digital mode gains, and DMR_OFFSET
static void cal_values(void)
{
	uint8_t v[sizeof(cal_audio_default)];
	uint32_t base = !d578uv_nand_ready() ? 0 : cal_current(CAL_A) ? CAL_A : cal_current(CAL_B) ? CAL_B : 0;

	if (base == 0 || !d578uv_nand_read_any(base + CAL_AUDIO, v, sizeof(v), NULL))
		memcpy(v, cal_audio_default, sizeof(v));
	for (int i = 0; i < 4; i++)
		codec_writes[i].value = v[i];
	memcpy(spk_gain, &v[CAL_SPK], sizeof(spk_gain));
	memcpy(mic_gain, &v[CAL_MIC], sizeof(mic_gain));
	if (base == 0 || !d578uv_nand_read_any(base + CAL_OFFSET, dmr_offset, sizeof(dmr_offset), NULL))
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

#define FIELD_WAIT_MS	200	// the manual's 40 ms for most fields, with room for a busy modem task

static bool port_send(const uint8_t *buf, uint16_t len, void *ctx)
{
	(void)ctx;
	return sct_host_send(&host, buf, len);
}

static bool port_ready(void *ctx)
{
	(void)ctx;
	// The boot takes seconds and runs in modem_task, so the screen comes up first: the DMR start waits for its result
	while (!boot_done)
		vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
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

// DMR_CALL_OPTION and REPORT_FIELD as the original firmware sends them (hardware-d578uv.md 4.4), but for bytes 0 and
// 3. Byte 0: the chip's own squelch off (bit 6 clear), so the board's carrier (d578uv_modem_carrier) and the return
// to receive below start the sync search; with it on, a lone burst soon after a transmission went undecoded. No RF
// control (the MCU keys the RF chips). Byte 2: four good CACH, two LC header repeats. Byte 3: every idle frame, CSBK,
// LC header and terminator reported. The stack takes a repeater as up for 300 ms after its last burst, so with idle
// frames reported once a repeater's idle outbound never showed as up. The original 0x44 reports identical LC
// headers and terminators once, and whether the chip forgets them between calls is not known: a second call with
// the same LC could go unreported. The stack takes repeats as from a burst modem. Bit 6 of bytes 1 and 3 and byte 4
// are reserved in the Packet Interface document; sent as the original firmware sends them.
// CARRIER_READY 2 (CR_READY below) 14 ms after each transmission. With the burst line's edges the transmission ends
// about 2 ms after the last burst's fall and the receiver is set up about 8 ms after it, so CARRIER_READY goes out
// about 16 ms after the fall. A one-burst reply 62.5 ms after our last burst then decodes; one 32.5 ms after it does
// not (hardware-d578uv.md 4.4)
#define CR_READY	2
static const sct_dmr_port_t port = {
	.send = port_send,
	.call_option = { 0x01, 0x40, 0x42, 0x4F, 0x08 },
	.report_field = 0xFF,
	.rx_ready = CR_READY,
	.rx_settle_ms = 14,
	.burst_edges = d578uv_burst_edges,
	.start_fields = start_fields,
	.start_count = sizeof(start_fields) / sizeof(start_fields[0]),
	.codec_tx = &hpl_mute,
	.codec_rx = &codec_writes[CODEC_HPL],
	.codec_start = codec_start,
	.codec_start_count = sizeof(codec_start) / sizeof(codec_start[0]),
	.tx_fields = port_tx_fields,
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
	boot_result = sct_host_boot(&host, &rx, BOOT_TIMEOUT_MS);
	booted = boot_result == SCT_BOOT_OK;
	boot_done = true;
	while (!booted)
		vTaskDelay(portMAX_DELAY);	// FM only: nothing to poll
	for (;;) {
		port_lock(NULL);
		sct_host_poll(&host, &rx, on_packet, NULL);
		port_unlock(NULL);
		sct_dmr_poll(hal_millis());
		vTaskDelay(pdMS_TO_TICKS(MODEM_POLL_MS));
	}
}

// board.chip_reg's baseband fields (hardware-d578uv.md 4.4): a field of up to 4 bytes, MSB first
bool d578uv_modem_field(uint8_t type, uint8_t id, uint32_t *val, bool write)
{
	int len = sct_field_len(id);
	uint8_t d[4], n = 0;

	if (!booted || (write && (len < 0 || len > 4)))
		return false;
	if (write) {
		if (len < 4 && (*val >> (8 * len)) != 0)
			return false;
		for (int i = 0; i < len; i++)
			d[i] = (uint8_t)(*val >> (8 * (len - 1 - i)));
	}
	if (!sct_dmr_field_begin(type, id, d, write ? (uint8_t)len : 0, !write))
		return false;
	for (int t = 0; t < FIELD_WAIT_MS; t += 2) {
		int r = sct_dmr_field_poll(d, &n);

		if (r < 0)
			return false;
		if (r > 0) {
			if (!write) {
				*val = 0;
				for (uint8_t i = 0; i < n; i++)
					*val = (*val << 8) | d[i];
			}
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	sct_dmr_field_cancel();
	return false;
}

// SCRATCH (never commit): raw read of an SCT field that carries a 16-bit argument (0x3F READ_MM, a debug memory
// peek). Sends TYPE|READ, id, arg_hi, arg_lo; returns the reply's data assembled MSB-first. Reached via chip 0x7F in
// d578uv_chip_reg. Needs field_len[0x3F]=2 and the read-arg path in sct_dmr_field_begin.
bool d578uv_modem_raw_read(uint8_t type, uint8_t id, uint16_t arg, uint32_t *val)
{
	uint8_t req[2] = { (uint8_t)(arg >> 8), (uint8_t)arg };
	uint8_t d[4], n = 0;

	if (!booted)
		return false;
	if (!sct_dmr_field_begin(type, id, req, 2, true))
		return false;
	for (int t = 0; t < FIELD_WAIT_MS; t += 2) {
		int r = sct_dmr_field_poll(d, &n);

		if (r < 0)
			return false;
		if (r > 0) {
			*val = 0;
			for (uint8_t i = 0; i < n; i++)
				*val = (*val << 8) | d[i];
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
	}
	sct_dmr_field_cancel();
	return false;
}

bool d578uv_modem_init(void)
{
	sct_dmr_init(&port);
	if (crc32(__codec_slot, SCT_BOOT_BLOCK_BYTES) != BOOT_BLOCK_CRC32) {
		boot_result = SCT_BOOT_NO_BLOCK;
		boot_done = true;
		return false;
	}
	cal_values();
	lock = xSemaphoreCreateRecursiveMutex();
	xTaskCreate(modem_task, "modem", MODEM_TASK_STACK, NULL, MODEM_TASK_PRIORITY, NULL);
	return true;
}

// 2, ready with the phase reversed: with the RF chip's DMR AF output (inverted below 300 MHz, qa4818.h), that is the
// polarity the baseband gets on both bands. With 1, no call decodes (hardware-d578uv.md 4.4)
void d578uv_modem_carrier(bool carrier)
{
	if (booted)
		sct_dmr_carrier(carrier ? CR_READY : 0);
}

// board_t.modem_codec: the boot block the codec slot holds, the same check as the start's
const char *d578uv_modem_codec(void)
{
	return crc32(__codec_slot, SCT_BOOT_BLOCK_BYTES) == BOOT_BLOCK_CRC32 ? "d578uv-boot" : NULL;
}

uint8_t d578uv_modem_boot_result(void)
{
	return (uint8_t)boot_result;
}

#endif
