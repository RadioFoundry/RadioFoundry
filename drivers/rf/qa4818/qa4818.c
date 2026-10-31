// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// QA4818N / WX4818N-class transceiver (qa4818.h). Sequences in the stock firmware's order; the register meanings in
// the comments are inferred (docs/spec/hardware-d578uv.md 5)
#include "qa4818.h"
#include "rf/subaudio/subaudio.h"

#define MHZ(x)		((uint32_t)(x) * 1000000u)
#define INIT_HZ		435125000u	// where both inits run the VCO calibration
#define CAL_WAIT_MS	20u

static void wr(qa4818_t *c, uint8_t reg, uint16_t val)
{
	rfbus_write(c->bus, reg, val);
}

static void page(qa4818_t *c, uint16_t p)
{
	wr(c, QA4818_REG_PAGE, p);
	c->page = (uint8_t)p;
}

// VCO sub-bands: 0x3C, 0x3D, 0x1A, 0x2A. Sub-band 0's 0x1A differs between the roles
typedef struct {
	uint16_t r3c, r3d, r1a, r2a;
} subband_t;

static const subband_t subbands[QA4818_SUBBANDS] = {
	{ 0x1000, 0x9D8A, 0x1F50, 0x22F2 },
	{ 0x1000, 0xEC4F, 0x1F30, 0x92F2 },
	{ 0x1001, 0x3B14, 0x1F48, 0x32F2 },
	{ 0x1001, 0xD89E, 0x1F28, 0xA2F2 },
	{ 0x1002, 0x7627, 0x1F44, 0x42F2 },
	{ 0x1003, 0xB13B, 0x1F24, 0xB2F2 },
	{ 0x1004, 0xEC4F, 0x1F42, 0x52F2 },
	{ 0x1007, 0x6276, 0x1F22, 0xC2F2 },
	{ 0x1009, 0xD89E, 0x1F41, 0x62F2 },
};
#define SUBBAND0_LO_1A	0xEF50u

// The CTCSS tones the detector has thresholds for (Hz x 10), and 0x52 for each; a tone above the last gets the
// catch-all
static const uint16_t ctcss_tones[] = {
	625, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
	1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966,
	1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
};
#define CTCSS_TONES	(sizeof(ctcss_tones) / sizeof(ctcss_tones[0]))
static const uint16_t ctcss_detect[CTCSS_TONES + 1] = {
	0x22A8, 0x22A8, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2328, 0x2316,
	0x23DE, 0x23DE, 0x24A8, 0x24A0, 0x24A0, 0x24A0, 0x24A0, 0x2528, 0x2526, 0x2526, 0x2526, 0x2528, 0x23E8, 0x2395,
	0x2395, 0x23D6, 0x23D6, 0x23E8, 0x2417, 0x2417, 0x2417, 0x2458, 0x2468, 0x24DA, 0x24DA, 0x24DA, 0x24DA, 0x2528,
	0x255C, 0x2568, 0x25A8, 0x25E0, 0x25E8, 0x25E8, 0x2628, 0x2667, 0x2667, 0x200A,
};

#define CSS_DETECT_IDLE	0x028Au		// 0x52 without sub-audio
#define SUBAUDIO_OFF	0x1050u		// 0x51 without sub-audio
#define SUBAUDIO_CTCSS	0x90C5u		// 0x51 receiving CTCSS
#define SUBAUDIO_DCS	0x80B0u		// 0x51 receiving DCS
#define SUBAUDIO_TX_CTCSS 0x9080u	// | the calibrated level
#define SUBAUDIO_TX_DCS	0x8080u
#define TONE2_RX	0x2470u		// 0x07 second word receiving CTCSS (a 55 Hz tail tone detector, inferred)
#define DCS_RATE	0x0AD7u		// 0x07: 134.4 Hz, the DCS bit rate
#define TONE_LATCH	0x2000u
#define TX_NO_TONE	0x0473u		// 0x07 transmitting without sub-audio (55.2 Hz, latched off with 0x51)
#define TONE_GEN_ON	0xA000u		// 0x70: the tone generator on
#define TX_AUDIO_MIC	0x03B8u		// page 1 0x17 in FM TX: the microphone
#define TX_AUDIO_LINE	0x03B9u		// ... bit 0 set: the audio input pin (the stock DTMF, inferred)
#define TX_PATH_VOICE	0x0004u		// page 1 0x1B in FM TX
#define TX_PATH_TONE	0x0005u		// ... while a tone sends (inferred: the microphone's path off)

uint32_t qa4818_freq_word(uint32_t hz)
{
	return (hz + 5u) / 10u;
}

void qa4818_rx_plan(uint32_t hz, uint32_t if_hz, qa4818_rx_plan_t *p)
{
	if (hz >= if_hz && hz - if_hz >= MHZ(300)) {
		p->lo_hz = hz - if_hz;
		p->lo_subband = 0;
	} else {
		p->lo_hz = hz + if_hz;
		p->lo_subband = p->lo_hz >= MHZ(160) ? 2 : 3;
	}
}

uint8_t qa4818_tx_subband(uint32_t hz)
{
	if (hz >= MHZ(400)) {
		return 0;
	}
	return hz >= MHZ(200) ? 2 : 3;
}

uint16_t qa4818_ctcss_detect(uint16_t hz_x10)
{
	size_t i = 0;
	while (i < CTCSS_TONES && ctcss_tones[i] < hz_x10) {
		i++;
	}
	return ctcss_detect[i];
}

uint16_t qa4818_drive_word(uint32_t hz, bool narrow, uint8_t drive)
{
	uint16_t base = hz >= MHZ(300) ? 0x3900u : hz >= MHZ(200) ? 0x3800u : 0x3700u;
	if (narrow) {
		base -= 0x100u;
	}
	uint16_t d = (uint16_t)(drive << 1);
	if (d & 0xFF00u) {
		base -= 0x100u;
	}
	return (uint16_t)(base | (d & 0xFFu));
}

int16_t qa4818_rssi_dbm(uint16_t raw)
{
	return (int16_t)((int16_t)(raw & 0x1FFu) / 2 - 160);
}

uint16_t qa4818_drive_raw(uint16_t base, uint8_t level, bool narrow)
{
	if (narrow) {
		base = (uint16_t)(base - 0x100u);
	}
	uint16_t d = (uint16_t)(level << 1);
	if (d & 0xFF00u) {
		base = (uint16_t)(base - 0x100u);
	}
	return (uint16_t)(base | (d & 0xFFu));
}

// Hz x 10.32 in integers: the stock firmware multiplies by the double 10.32 and truncates, which is the same for every
// 16-bit hz (the double is a hair above 10.32, so exact products stay exact)
uint16_t qa4818_tone_word(uint16_t hz)
{
	return (uint16_t)((((uint32_t)hz * 1032u / 100u) & 0xFFFFu) >> 1);
}

// The stock firmware compares the frequency against 300 and 200 MHz with "above", not "at least", in both
uint16_t qa4818_tone_drive_word(uint32_t hz, bool narrow)
{
	if (hz > MHZ(300)) {
		return qa4818_drive_raw(0x3900, 0x32, narrow);
	}
	if (hz > MHZ(200)) {
		return qa4818_drive_raw(0x3900, 0xB4, narrow);
	}
	return qa4818_drive_raw(0x3800, 0x82, narrow);
}

uint16_t qa4818_line_drive_word(uint32_t hz, bool narrow, uint8_t level)
{
	uint16_t base = hz > MHZ(300) ? 0x3900u : hz > MHZ(200) ? 0x3800u : 0x3700u;
	return qa4818_drive_raw(base, level, narrow);
}

void qa4818_attach(qa4818_t *c, rfbus_t *bus, bool lo)
{
	c->bus = bus;
	c->lo = lo;
	c->subband = QA4818_SUBBAND_NONE;
	c->gpio = 0;
	c->page = 0;
}

static void set_subband(qa4818_t *c, uint8_t idx)
{
	if (idx >= QA4818_SUBBANDS || idx == c->subband) {
		return;
	}
	c->subband = idx;
	const subband_t *s = &subbands[idx];
	wr(c, 0x3C, s->r3c);
	wr(c, 0x3D, s->r3d);
	wr(c, 0x1A, idx == 0 && c->lo ? SUBBAND0_LO_1A : s->r1a);
	wr(c, 0x2A, s->r2a);
}

void qa4818_tune(qa4818_t *c, uint8_t subband, uint32_t hz)
{
	set_subband(c, subband);
	uint32_t w = qa4818_freq_word(hz);
	wr(c, QA4818_REG_FREQ_LO, (uint16_t)w);
	wr(c, QA4818_REG_FREQ_HI, (uint16_t)(w >> 16));
}

static void reset(qa4818_t *c)
{
	page(c, 0);
	wr(c, QA4818_REG_RESET, 0x8000);
	wr(c, QA4818_REG_RESET, 0x0000);
	c->subband = QA4818_SUBBAND_NONE;
}

// VCO calibration (inferred): enable for calibration, start it, wait
static void vco_cal(qa4818_t *c, uint16_t a, uint16_t b, bool settle)
{
	wr(c, QA4818_REG_ENABLE, 0xBDF1);
	wr(c, 0x04, a);
	wr(c, 0x04, b);
	rfbus_delay_ms(c->bus, CAL_WAIT_MS);
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_OFF);
	if (settle) {
		rfbus_delay_ms(c->bus, CAL_WAIT_MS);
	}
}

void qa4818_init_lo(qa4818_t *c)
{
	reset(c);
	static const rfbus_regval_t p0[] = {
		{ 0x30, 0x5FE0 }, { 0x34, 0x086F }, { 0x35, 0xF708 }, { 0x40, 0x0000 }, { 0x1B, 0x8200 }, { 0x1F, 0x3EC8 },
		{ QA4818_REG_PAGE, 1 }, { 0x16, 0x9AE8 }, { QA4818_REG_PAGE, 0 }, { 0x40, 0x0000 },
	};
	rfbus_write_table(c->bus, p0, sizeof(p0) / sizeof(p0[0]));
	qa4818_tune(c, 0, INIT_HZ);
	vco_cal(c, 0x0000, 0x0300, true);
	qa4818_out(c, QA4818_OUT_RX);
	wr(c, 0x28, 0x0022);
}

// Mixer and IF filter constants (0x10-0x13) and 0x49, written at init and at every FM set-up
static void if_constants(qa4818_t *c, uint16_t r49)
{
	static const rfbus_regval_t t[] = {
		{ 0x10, 0x0038 }, { 0x11, 0x031A }, { 0x11, 0x025A }, { 0x12, 0x035B }, { 0x13, 0x0396 },
	};
	rfbus_write_table(c->bus, t, sizeof(t) / sizeof(t[0]));
	wr(c, 0x49, r49);
}

// Page 1 constants of the analog (FM) or digital (DMR) path; the analog 0x36 differs between RX and TX
static void path_page1(qa4818_t *c, bool tx, bool digital)
{
	page(c, 1);
	if (digital) {
		static const rfbus_regval_t d[] = {
			{ 0x1B, 0x07FF }, { 0x1C, 0x04D3 }, { 0x1D, 0x29A1 }, { 0x27, 0x891B }, { 0x21, 0xC8F7 },
			{ 0x22, 0x2136 }, { 0x2A, 0x8537 }, { 0x2B, 0x3B20 }, { 0x36, 0x1D59 },
		};
		rfbus_write_table(c->bus, d, sizeof(d) / sizeof(d[0]));
	} else {
		static const rfbus_regval_t a[] = {
			{ 0x1B, 0x0004 }, { 0x1C, 0x04D7 }, { 0x1D, 0x39FD }, { 0x27, 0x091B },
		};
		rfbus_write_table(c->bus, a, sizeof(a) / sizeof(a[0]));
		wr(c, 0x36, tx ? 0x1D08 : 0x9D08);
		static const rfbus_regval_t b[] = {
			{ 0x21, 0xC8F7 }, { 0x22, 0x2136 }, { 0x2A, 0x8537 }, { 0x2B, 0x3B20 },
		};
		rfbus_write_table(c->bus, b, sizeof(b) / sizeof(b[0]));
	}
	page(c, 0);
}

void qa4818_init_trx(qa4818_t *c, uint16_t gpio, uint16_t r49)
{
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_OFF);
	reset(c);
	wr(c, 0x10, 0x0038);
	wr(c, 0x18, 0x4125);
	wr(c, 0x26, 0x0000);
	wr(c, 0x27, 0x0000);
	c->gpio = gpio;
	wr(c, QA4818_REG_GPIO, gpio);
	static const rfbus_regval_t p0[] = {
		{ 0x1F, 0x0EF8 }, { 0x30, 0x5FE0 }, { 0x34, 0x086F }, { 0x35, 0xF208 }, { 0x40, 0x39D0 },
	};
	rfbus_write_table(c->bus, p0, sizeof(p0) / sizeof(p0[0]));
	wr(c, 0x49, r49);
	static const rfbus_regval_t p0b[] = {
		{ 0x4B, 0x2442 }, { 0x50, 0x073C }, { 0x77, 0x88EF }, { 0x7B, 0xC3A8 }, { 0x7D, 0x4200 },
	};
	rfbus_write_table(c->bus, p0b, sizeof(p0b) / sizeof(p0b[0]));
	if_constants(c, r49);
	static const rfbus_regval_t p1[] = {
		{ QA4818_REG_PAGE, 1 }, { 0x10, 0x8308 }, { 0x15, 0xC9A7 }, { 0x17, 0x0AA8 }, { 0x18, 0x0A18 },
		{ 0x19, 0x7900 }, { 0x1A, 0x0728 }, { 0x1D, 0x39FD }, { 0x35, 0x100F }, { QA4818_REG_PAGE, 0 },
	};
	rfbus_write_table(c->bus, p1, sizeof(p1) / sizeof(p1[0]));
	path_page1(c, false, false);
	qa4818_tune(c, 0, INIT_HZ);
	vco_cal(c, 0x0044, 0x0344, false);
	qa4818_out(c, QA4818_OUT_RX);
	// AGC steps (0x09, one per index, inferred) and two page 1 values
	wr(c, 0x04, 0x0302);
	static const uint16_t agc[] = {
		0x0000, 0x105B, 0x205F, 0x3056, 0x4061, 0x5054, 0x604C, 0x7067, 0x807B, 0x9076,
	};
	for (size_t i = 0; i < sizeof(agc) / sizeof(agc[0]); i++) {
		wr(c, 0x09, agc[i]);
	}
	page(c, 1);
	wr(c, 0x11, 0x06FF);
	wr(c, 0x14, 0x8C29);
	page(c, 0);
	(void)rfbus_read(c->bus, 0x44);
}

void qa4818_enable(qa4818_t *c, uint16_t value)
{
	wr(c, QA4818_REG_ENABLE, value);
}

void qa4818_rx_on(qa4818_t *c)
{
	wr(c, 0x34, 0x086F);
	wr(c, 0x35, 0xF208);
	wr(c, 0x47, 0x6140);
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_RX);
}

void qa4818_tx_on(qa4818_t *c)
{
	wr(c, 0x47, 0x6140);
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_TX);
}

void qa4818_lo_on(qa4818_t *c)
{
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_OFF);
	wr(c, QA4818_REG_ENABLE, QA4818_ENABLE_LO);
}

void qa4818_out(qa4818_t *c, uint16_t value)
{
	wr(c, 0x28, value);
}

void qa4818_gpio(qa4818_t *c, uint16_t mask, uint16_t val)
{
	uint16_t g = (uint16_t)((c->gpio & ~mask) | (val & mask));
	if (g != c->gpio) {
		c->gpio = g;
		// Page 1 has its own 0x25 (an audio value): a caller on another task that came in between a page-1 block's
		// writes (under the caller's bus lock, so the three writes stay together) must reach page 0's
		uint8_t was = c->page;
		if (was != 0) {
			page(c, 0);
		}
		wr(c, QA4818_REG_GPIO, g);
		if (was != 0) {
			page(c, was);
		}
	}
}

void qa4818_path(qa4818_t *c, bool tx, bool digital, bool narrow, uint16_t r49)
{
	if_constants(c, r49);
	if (digital)
		wr(c, 0x13, 0x0375);	// the digital path's IF value (0x0396 in the stock firmware only with a second receiver)
	path_page1(c, tx, digital);
	wr(c, 0x44, digital ? 0x53EC : 0x43EC);
	wr(c, 0x4B, 0x7EB7);
	if (digital) {
		wr(c, 0x73, 0x4E8A);	// 0x569A in the stock firmware only with the second receiver on
		wr(c, 0x7E, 0x361E);
	} else {
		wr(c, 0x73, narrow ? 0x564A : 0x464A);
		wr(c, 0x7E, 0x342E);
	}
}

// Voice audio without the scrambler (0x04 bit 5 clear, inferred)
static void audio_plain(qa4818_t *c)
{
	wr(c, 0x04, 0x0302);
	static const rfbus_regval_t t[] = {
		{ QA4818_REG_PAGE, 1 }, { 0x25, 0xC02A }, { 0x26, 0x169C }, { 0x28, 0xECD8 }, { 0x29, 0x0AC9 },
		{ 0x30, 0x8B0B }, { 0x31, 0x35C0 }, { QA4818_REG_PAGE, 0 }, { 0x50, 0x073C }, { 0x53, 0x57DD },
	};
	rfbus_write_table(c->bus, t, sizeof(t) / sizeof(t[0]));
}

// 0x3A and 0x43 for an analog or a digital channel (0x43 is not the FM bandwidth: the same for 12.5 and 25 kHz)
static void channel_mode(qa4818_t *c, bool digital)
{
	wr(c, 0x3A, digital ? 0x00E0 : 0x0040);
	wr(c, 0x43, 0x6009);
}

void qa4818_rx_setup(qa4818_t *c, bool digital)
{
	if (digital) {
		wr(c, 0x53, 0x97DD);
		wr(c, 0x23, 0x8F85);
	} else {
		wr(c, 0x53, 0x57DD);
		// 0x23 low bits: 2 + 5 for an analog channel, from a user setting at its default (inferred)
		wr(c, 0x23, 0x8F87);
	}
	channel_mode(c, digital);
	page(c, 1);
	wr(c, 0x16, 0x04EE);
	wr(c, 0x18, 0x0A18);
	page(c, 0);
	audio_plain(c);
}

void qa4818_gain(qa4818_t *c, uint8_t code)
{
	wr(c, 0x4F, 0x7E2B);
	wr(c, QA4818_REG_SQ, (uint16_t)(code << 8 | 0xFFu));
}

static void dcs_word(qa4818_t *c, uint16_t code, bool invert)
{
	uint32_t w = subaudio_dcs_word(code);
	if (invert) {
		w = ~w & 0x7FFFFFu;
	}
	wr(c, QA4818_REG_DCS, (uint16_t)(w & 0xFFFu));
	wr(c, QA4818_REG_DCS, (uint16_t)(0x8000u | ((w >> 12) & 0x7FFu)));
	wr(c, QA4818_REG_CTCSS, DCS_RATE);
	wr(c, QA4818_REG_CTCSS, DCS_RATE | TONE_LATCH);
}

void qa4818_rx_tone(qa4818_t *c, qa4818_tone_t type, uint16_t value)
{
	switch (type) {
	case QA4818_TONE_CTCSS:
		wr(c, QA4818_REG_CSS_DETECT, qa4818_ctcss_detect(value));
		wr(c, QA4818_REG_CTCSS, subaudio_ctcss_word(value, false));
		wr(c, QA4818_REG_CTCSS, TONE2_RX);
		wr(c, QA4818_REG_SUBAUDIO, SUBAUDIO_CTCSS);
		break;
	case QA4818_TONE_DCS:
	case QA4818_TONE_DCS_INV:
		dcs_word(c, value, type == QA4818_TONE_DCS_INV);
		wr(c, QA4818_REG_SUBAUDIO, SUBAUDIO_DCS);
		wr(c, QA4818_REG_CSS_DETECT, CSS_DETECT_IDLE);
		break;
	default:
		wr(c, QA4818_REG_SUBAUDIO, SUBAUDIO_OFF);
		wr(c, QA4818_REG_CSS_DETECT, CSS_DETECT_IDLE);
		break;
	}
}

void qa4818_fm_tx(qa4818_t *c, uint32_t hz)
{
	static const rfbus_regval_t t[] = {
		{ QA4818_REG_PAGE, 1 }, { 0x17, TX_AUDIO_MIC }, { 0x19, 0x7800 }, { 0x22, 0x1B00 }, { 0x1B, TX_PATH_VOICE },
		{ 0x27, 0x88D3 }, { 0x1E, 0x4494 }, { 0x1F, 0xC1F3 }, { 0x20, 0x22A7 }, { 0x23, 0xD124 }, { 0x24, 0x087F },
		{ QA4818_REG_PAGE, 0 },
	};
	rfbus_write_table(c->bus, t, sizeof(t) / sizeof(t[0]));
	channel_mode(c, false);
	qa4818_out(c, hz < MHZ(200) ? QA4818_OUT_TX_VHF : QA4818_OUT_TX_UHF);
}

// The stock digital TX skips the analog writes (page 1 TX set, audio, tone): the digital set from qa4818_path stays.
// The baseband modulates, so no sub-audio and no drive word; the PA level is the board's
void qa4818_dmr_tx(qa4818_t *c, uint32_t hz)
{
	qa4818_out(c, hz < MHZ(200) ? QA4818_OUT_TX_VHF : QA4818_OUT_TX_UHF);
	wr(c, 0x18, 0x4125);
	page(c, 1);
	wr(c, 0x17, 0x0AA8);
	page(c, 0);
	wr(c, QA4818_REG_SUBAUDIO, SUBAUDIO_OFF);
	wr(c, QA4818_REG_CSS_DETECT, CSS_DETECT_IDLE);
	qa4818_tx_drive_off(c);
}

void qa4818_tx_tone(qa4818_t *c, qa4818_tone_t type, uint16_t value, uint8_t cal_ctcss, uint8_t cal_dcs)
{
	switch (type) {
	case QA4818_TONE_CTCSS: {
		uint16_t w = subaudio_ctcss_word(value, false);
		wr(c, QA4818_REG_CTCSS, w);
		wr(c, QA4818_REG_CTCSS, (uint16_t)(w | TONE_LATCH));
		wr(c, QA4818_REG_SUBAUDIO, (uint16_t)(SUBAUDIO_TX_CTCSS | (cal_ctcss & 0x7Fu)));
		break;
	}
	case QA4818_TONE_DCS:
	case QA4818_TONE_DCS_INV:
		dcs_word(c, value, type == QA4818_TONE_DCS_INV);
		wr(c, QA4818_REG_SUBAUDIO, (uint16_t)(SUBAUDIO_TX_DCS | (cal_dcs & 0x7Fu)));
		break;
	default:
		wr(c, QA4818_REG_CTCSS, TX_NO_TONE);
		wr(c, QA4818_REG_CTCSS, TX_NO_TONE | TONE_LATCH);
		wr(c, QA4818_REG_SUBAUDIO, SUBAUDIO_OFF);
		break;
	}
	wr(c, QA4818_REG_CSS_DETECT, CSS_DETECT_IDLE);
}

static void drive(qa4818_t *c, uint16_t r40, uint8_t level)
{
	wr(c, QA4818_REG_DRIVE, r40);
	uint16_t r51 = rfbus_read(c->bus, QA4818_REG_SUBAUDIO);
	wr(c, QA4818_REG_SUBAUDIO, (uint16_t)((r51 & 0xFF80u) | (level & 0x7Fu)));
}

void qa4818_tx_drive(qa4818_t *c, uint32_t hz, bool narrow, uint8_t d, uint8_t level)
{
	audio_plain(c);
	drive(c, qa4818_drive_word(hz, narrow, d), level);
}

void qa4818_tx_drive_off(qa4818_t *c)
{
	drive(c, 0, 0);
}

void qa4818_tx_aux(qa4818_t *c, bool line, uint16_t r40)
{
	page(c, 1);
	wr(c, 0x19, 0x7800);
	page(c, 0);
	if (line) {
		wr(c, 0x18, 0x4125);
		page(c, 1);
		wr(c, 0x17, TX_AUDIO_LINE);
		page(c, 0);
	}
	wr(c, QA4818_REG_DRIVE, r40);
	page(c, 1);
	wr(c, 0x1B, TX_PATH_TONE);
	page(c, 0);
}

void qa4818_tone_start(qa4818_t *c, uint16_t hz)
{
	wr(c, QA4818_REG_TONE_FREQ, qa4818_tone_word(hz));
	wr(c, QA4818_REG_TONE_GEN, TONE_GEN_ON);
}

void qa4818_tone_gen(qa4818_t *c, bool on)
{
	wr(c, QA4818_REG_TONE_GEN, on ? TONE_GEN_ON : 0);
}

// The stock firmware ends its burst with page 1 0x1B, 0x70 and the drive; its DTMF end rewrites 0x17 (through a
// routine that also sets a calibrated 0x18, not followed here: 0x18 stays the TX value 0x4125)
void qa4818_tx_mic(qa4818_t *c, uint16_t r40, uint8_t level)
{
	page(c, 1);
	wr(c, 0x1B, TX_PATH_VOICE);
	wr(c, 0x17, TX_AUDIO_MIC);
	page(c, 0);
	wr(c, QA4818_REG_TONE_GEN, 0);
	drive(c, r40, level);
}

void qa4818_status(qa4818_t *c, qa4818_status_t *st)
{
	st->signal = (rfbus_read(c->bus, QA4818_REG_STATUS) & 0x0002u) != 0;
	st->noise = (uint8_t)(rfbus_read(c->bus, QA4818_REG_NOISE) & 0x7Fu);
	st->rssi = (uint16_t)(rfbus_read(c->bus, QA4818_REG_RSSI) & 0x1FFu);
	st->tone_flags = rfbus_read(c->bus, QA4818_REG_TONE_FLAGS);
}
