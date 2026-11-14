// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV diagnostics page lines: the raw readings the bring-up needs (docs/testing/d578uv-bringup.md): the
// baseband's boot result, the key ladders and microphone lines (to place the keys), the supply and volume pots, the
// NAND's ID, the baseband's CARRIER_READY and received frames, the DMR PA gate's bursts, the GPS receiver's sentences,
// whether the KTMicro receiver answered, side A's raw RSSI, the ID database's NAND range, the serial hand
// microphone's frames and the baseband's own product, versions, customer ID and serial. Each line keeps its
// number whatever is built. The gps and rssi lines are in the shape the HIL health tests read on every radio
// (tests/hil/test_health.py)
#include <stddef.h>
#include "d578uv.h"
#include "pins.h"
#include "mcu_io.h"
#include "config.h"
#if CONFIG_GPS
#include "gps_uart/gps_uart.h"
#endif
#if CONFIG_DMR
#include "modem/sct/sct_dmr.h"
#endif

#if CONFIG_DIAG

#define DIAG_LINES	13u

typedef struct {
	char *p, *end;
} out_t;

static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end) {
		*o->p++ = *s++;
	}
}

static void put_u(out_t *o, uint32_t v)
{
	char t[11];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 10);
	while (n && o->p < o->end) {
		*o->p++ = t[--n];
	}
}

static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF];
	}
}

#if CONFIG_DMR
// sct_boot_result_t in order (drivers/modem/sct/sct_host.h)
static const char *boot_name(uint8_t r)
{
	static const char *const names[] = { "ok", "no block", "bus", "no app", "silent", "no voice" };
	return r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}
// The baseband's answers to the Packet Interface's identity queries (Q_PRO_STR, Q_HW_VER, Q_SW_VER: text; Q_CID_SN:
// a 3-byte BCD customer ID and a 5-byte BCD serial; hardware-d578uv.md 4.4), asked once, the first time a line needs
// them, in a TYPE 3 packet. A query that goes unanswered (not booted yet) is asked again later, three times at most
enum { SCT_PRO, SCT_HW, SCT_SW, SCT_CID, SCT_QUERIES };
static const uint8_t sct_query_id[SCT_QUERIES] = { 0x30, 0x31, 0x32, 0x33 };
static struct {
	uint8_t d[SCT_QUERIES][SCT_DMR_FIELD_MAX];
	int8_t n[SCT_QUERIES];	// bytes answered; -1 not yet
	uint8_t tries;
} sct_id = { .n = { -1, -1, -1, -1 } };

static void sct_identity(void)
{
	bool missing = false;
	for (int q = 0; q < SCT_QUERIES; q++)
		missing |= sct_id.n[q] < 0;
	if (!missing || sct_id.tries >= 3u || !d578uv_modem_booted())
		return;
	sct_id.tries++;
	for (int q = 0; q < SCT_QUERIES; q++) {
		if (sct_id.n[q] < 0) {
			int r = d578uv_modem_field_bytes(0x03, sct_query_id[q], NULL, 0, sct_id.d[q], SCT_DMR_FIELD_MAX);
			sct_id.n[q] = (int8_t)(r < 0 ? -1 : r);
		}
	}
}

// A text answer: printable characters up to a NUL, trailing spaces dropped; "?" for none
static void put_text(out_t *o, int q)
{
	int n = sct_id.n[q];
	while (n > 0 && (sct_id.d[q][n - 1] == ' ' || sct_id.d[q][n - 1] == 0))
		n--;
	if (n <= 0) {
		put(o, "?");
		return;
	}
	for (int k = 0; k < n && sct_id.d[q][k] != 0 && o->p < o->end; k++) {
		uint8_t c = sct_id.d[q][k];
		*o->p++ = c >= 0x20 && c < 0x7F ? (char)c : '.';
	}
}

// BCD bytes from..to of the Q_CID_SN answer, two digits each
static void put_bcd(out_t *o, int from, int to)
{
	if (sct_id.n[SCT_CID] < to) {
		put(o, "?");
		return;
	}
	for (int k = from; k < to; k++)
		put_x(o, sct_id.d[SCT_CID][k], 2);
}
#endif

bool d578uv_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0 || i >= DIAG_LINES) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	switch (i) {
	case 0:
		put(&o, "bb ");
#if CONFIG_DMR
		put(&o, boot_name(d578uv_modem_boot_result()));
		{
			// The last CARRIER_READY and how many went, the frames reported and those after an FEC error
			sct_dmr_stats_t st;
			sct_dmr_get_stats(&st);
			put(&o, " cr ");
			put_u(&o, st.carrier);
			put(&o, "/");
			put_u(&o, st.carrier_sent);
			put(&o, " rx ");
			put_u(&o, st.rx_frames);
			put(&o, " fec ");
			put_u(&o, st.rx_fec);
			put(&o, " nak ");
			put_u(&o, st.naks);
			put(&o, "/");
			put_u(&o, st.last_nak);
			put(&o, " rst ");
			put_u(&o, st.chip_restarts);
			put(&o, " wm ");
			put_u(&o, st.mode_timeouts);
			put(&o, " rej ");
			put_u(&o, st.tx_rejected);
			put(&o, " uns ");
			put_u(&o, st.rx_unsupported);
		}
#else
		put(&o, "-");
#endif
		break;
	case 1:
		// The ladders' 12-bit samples (4095: no key), PA1 and the microphone lines PA9, PA10: 1 = pulled low
		put(&o, "keys ");
		put_u(&o, mcu_adc_read(ADC_KEY_LADDER0));
		put(&o, " ");
		put_u(&o, mcu_adc_read(ADC_KEY_LADDER1));
		put(&o, " a1 ");
		put_u(&o, !mcu_pin_read(PIN_KEY_SINGLE));
		put(&o, " mic ");
		put_u(&o, !mcu_pin_read(PIN_MIC_LINE9));
		put_u(&o, !mcu_pin_read(PIN_MIC_LINE10));
		break;
	case 2:
		put(&o, "sup ");
		put_u(&o, d578uv_power.supply_mv());
		put(&o, " temp ");
		put_u(&o, mcu_adc_read(ADC_PA_TEMP));
		break;
	case 3:
		put(&o, "vol ");
		put_u(&o, mcu_adc_read(ADC_VOL_A));
		put(&o, " ");
		put_u(&o, mcu_adc_read(ADC_VOL_B));
		break;
	case 4: {
		uint8_t fqca = 0;
		char copy = d578uv_cal_copy(&fqca);
		put(&o, "nand ");
		put_x(&o, d578uv_nand_id(), 4);
		put(&o, copy == 'A' ? " cal A fqca " : copy == 'B' ? " cal B fqca " : " cal none");
		if (copy != 0) {
			put_x(&o, fqca, 2);
		}
		break;
	}
	case 5: {
		// Bursts the gate switched the PA on for, key-ups refused with the burst line already high, and from the last
		// burst's line fall to tx_off() and to the receiver back, in 0.1 ms
		put(&o, "burst ");
#if CONFIG_DMR
		uint32_t bursts, refused, off_us, rx_us;
		d578uv_pa_counts(&bursts, &refused);
		d578uv_rx_back(&off_us, &rx_us);
		put_u(&o, bursts);
		put(&o, " ref ");
		put_u(&o, refused);
		put(&o, " rx ");
		put_u(&o, off_us / 100u);
		put(&o, "/");
		put_u(&o, rx_us / 100u);
#else
		put(&o, "-");
#endif
		break;
	}
	case 6: {
#if CONFIG_GPS
		uint32_t good, bad, bytes;
		mcu_gps_counts(&good, &bad, &bytes);
		put(&o, mcu_gps_present() ? "gps yes " : "gps no ");
		put_u(&o, good);
		put(&o, "/");
		put_u(&o, bad);
		put(&o, " b");
		put_u(&o, bytes);
#else
		put(&o, "gps not built");
#endif
		break;
	}
	case 7:
		put(&o, "bc ");
#if CONFIG_AM_RX || CONFIG_BCAST_FM
		put(&o, d578uv_bc_chip());
#if CONFIG_BCAST_FM
		{
			uint16_t r[3];
			d578uv_bc_raw(r);
			for (unsigned k = 0; k < 3u; k++) {
				put(&o, " ");
				put_x(&o, r[k], 4);
			}
		}
#endif
#else
		put(&o, "-");
#endif
		break;
	case 8: {
		// Chip 0's raw RSSI and noise as the radio task last read them, and its signal flag; a dead bus reads a constant.
		// Then the baseband's DMR sync (no beep while it holds, radio_d578uv.c beep_tone())
		uint16_t rssi, noise;
		bool signal;
		d578uv_rx_raw(&rssi, &noise, &signal);
		put(&o, "rssi ");
		put_u(&o, rssi);
		put(&o, " nz ");
		put_u(&o, noise);
		put(&o, signal ? " sig" : " -");
		put(&o, d578uv_modem_synced() ? " sync" : " -");
		break;
	}
	case 9: {
		// The ID database's NAND range (nand-blocks.md): logical blocks holding data, spare blocks, bad blocks, and
		// pages lost (unreadable when copied); "off" when it didn't mount (foreign data in it, the stats' first block)
		put(&o, "iddb ");
#if CONFIG_NAND_STORAGE
		nand_blocks_stats_t st;
		d578uv_flash_ext_stats(&st);
		if (st.status == NAND_BLOCKS_MOUNTED) {
			put_u(&o, st.blocks_used);
			put(&o, " free ");
			put_u(&o, st.blocks_free);
			put(&o, " bad ");
			put_u(&o, st.blocks_bad);
			put(&o, " lost ");
			put_u(&o, st.lost);
		} else {
			put(&o, "off ");
			put_u(&o, st.status);
			put(&o, " ");
			put_u(&o, st.foreign_block);
		}
#else
		put(&o, "-");
#endif
		break;
	}
	case 10: {
		// The serial hand microphone: present, its frames accepted, bytes dropped and the key code held (hex)
		hal_hand_mic_stats_t hm;
		d578uv_hand_mic(&hm);
		put(&o, hm.present ? "hmic yes " : "hmic no ");
		put_u(&o, hm.frames);
		put(&o, "/");
		put_u(&o, hm.errors);
		put(&o, " k");
		put_x(&o, hm.key, 2);
		break;
	}
	case 11:
		// The baseband's product, hardware and software versions as it reports them
		put(&o, "sct ");
#if CONFIG_DMR
		sct_identity();
		put_text(&o, SCT_PRO);
		put(&o, " hw ");
		put_text(&o, SCT_HW);
		put(&o, " sw ");
		put_text(&o, SCT_SW);
#else
		put(&o, "-");
#endif
		break;
	default:
		// Its customer ID and serial number
		put(&o, "sct id ");
#if CONFIG_DMR
		sct_identity();
		put_bcd(&o, 0, 3);
		put(&o, " sn ");
		put_bcd(&o, 3, 8);
#else
		put(&o, "-");
#endif
		break;
	}
	*o.p = '\0';
	return true;
}

void d578uv_diag_reset(void)
{
#if CONFIG_DMR
	d578uv_pa_counts_reset();
#endif
}

#endif
