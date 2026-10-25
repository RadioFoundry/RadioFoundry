// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 debug link view keys (the `board` section, docs/spec/debuglink.md 3.7; key list in
// docs/spec/hardware-md9600.md 19.6): what a first power-up and a HIL test need to see without a screen. The board
// revision and where it came from, the clock that started, the flash, the calibration and option blocks, the key
// matrix, the ADC scan and the levels the radio code drives on the supply and transmitter switches.
#include "config.h"
#if CONFIG_DEBUGLINK	// the whole file
#include "md9600.h"
#include "spi_flash/spi_flash.h"
#include "pins.h"
#include "debuglink/dl_fmt.h"
#include "gps_uart/gps_uart.h"

static const char *const sources[] = { "none", "stored", "probe", "default" };
static const char *const adc_names[MD9600_ADC_COUNT] = { "noise_u", "noise_v", "rssi_u", "rssi_v", "mic", "supply",
							 "mic_row", "mic_col" };

static const char *rev_name(md9600_rev_t r)
{
	const md9600_rev_desc_t *d = md9600_rev_desc(r);
	return d != NULL ? d->name : "none";
}

// The clock the start-up code ended on: the PLL from the crystal, from an external clock (bypass), or no PLL (HSI)
static const char *clock_name(void)
{
	if ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
		return "hsi";
	}
	if ((RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) == 0) {
		return "hsi_pll";
	}
	return (RCC->CR & RCC_CR_HSEBYP) ? "bypass" : "crystal";
}

static uint8_t latch(GPIO_TypeDef *port, uint16_t pin)
{
	return (port->ODR & pin) ? 1u : 0u;
}

#define LATCH(p)	latch(PIN_PORT(p), PIN_MASK(p))

// The HR_C6000, the speaker routing, the hand-mic keypad and the GPS receiver (c6000_md9600.c, input_md9600.c,
// gps_md9600.c)
static bool audio_line(uint8_t i, char *buf, uint8_t len)
{
	md9600_c6000_diag_t c;
	uint32_t good = 0, bad = 0, bytes = 0;
	md9600_c6000_diag(&c);
#if CONFIG_GPS
	stm32_gps_counts(&good, &bad, &bytes);
#endif
	switch (i) {
	case 0:		// the chip answered after its set-up, and its 4:0x10
		dl_fmt(buf, len, "c6000=%u r10=0x%02x", c.up ? 1u : 0u, c.r10);
		return true;
	case 1:
		dl_fmt(buf, len, "c6000.dmr=%u", c.dmr ? 1u : 0u);
		return true;
	case 2:
		dl_fmt(buf, len, "c6000.errors=%u", (unsigned)c.errors);
		return true;
	case 3:		// DMR bursts the RF switch would not key (0 unless something asked to transmit)
		dl_fmt(buf, len, "c6000.tx_refused=%u", (unsigned)c.tx_refused);
		return true;
	case 4:		// bit 0 amplifier on, bit 1 FM audio through, bit 2 codec line out
		dl_fmt(buf, len, "speaker=%u", c.speaker);
		return true;
	case 5:		// the hand-mic key decoded on the last scan, 255 for none (the md9600.h key number)
		dl_fmt(buf, len, "mic_key=%u", md9600_mic_key());
		return true;
	case 6:		// GPS not built: the lines stay, as a radio without a module (0)
		dl_fmt(buf, len, "gps=%u", CONFIG_GPS && stm32_gps_present() ? 1u : 0u);
		return true;
	case 7:		// the rate listened at, 0 once given up
		dl_fmt(buf, len, "gps.baud=%u", (unsigned)(CONFIG_GPS ? stm32_gps_baud() : 0u));
		return true;
	case 8:
		dl_fmt(buf, len, "gps.nmea=%u/%u/%u", (unsigned)good, (unsigned)bad, (unsigned)bytes);
		return true;
	default:
		return false;
	}
}

bool md9600_view_line(uint8_t i, char *buf, uint8_t len)
{
	md9600_rev_state_t r = md9600_rev_state();
	md9600_radio_diag_t rd;
	md9600_band_edges_t e;
	bool edges = md9600_opt_band_edges(&e);

	md9600_radio_diag(&rd);
	switch (i) {
	case 0:
		dl_fmt(buf, len, "rev=%s", rev_name(r.rev));
		return true;
	case 1:
		dl_fmt(buf, len, "rev.source=%s", r.source < 4 ? sources[r.source] : "?");
		return true;
	case 2:
		dl_fmt(buf, len, "rev.usable=%u", r.usable ? 1u : 0u);
		return true;
	case 3:
		dl_fmt(buf, len, "rev.confirmed=%u", r.confirmed ? 1u : 0u);
		return true;
	case 4:		// the record as stored now (a change takes effect at the next start)
		dl_fmt(buf, len, "rev.stored=%s", rev_name(md9600_flash_rev_load()));
		return true;
	case 5:
		dl_fmt(buf, len, "probe=%s", rd.probe < 0 ? "off" : rev_name((md9600_rev_t)rd.probe));
		return true;
	case 6:
		dl_fmt(buf, len, "clock=%s", clock_name());
		return true;
	case 7:
		dl_fmt(buf, len, "sysclk_hz=%u", (unsigned)SystemCoreClock);
		return true;
	case 8:		// JEDEC memory type and capacity (0x4018: a 25Q128 class part), 0 = no answer
		dl_fmt(buf, len, "flash.id=0x%04x", stm32_spi_flash_device());
		return true;
	case 9:
		dl_fmt(buf, len, "flash.size=%u", (unsigned)stm32_spi_flash.size);
		return true;
	case 10:
		dl_fmt(buf, len, "flash.map=%u", stm32_spi_flash.map != NULL ? 1u : 0u);
		return true;
	case 11:
		dl_fmt(buf, len, "cal=%s", md9600_cal_source());
		return true;
	case 12:
		dl_fmt(buf, len, "opt=%s", md9600_opt_source());
		return true;
	case 13:
		dl_fmt(buf, len, "opt.backlight_switched=%u", md9600_opt_backlight_switched() ? 1u : 0u);
		return true;
	case 14:	// band edges, 0 when the option block has none
		dl_fmt(buf, len, "opt.vhf_hz=%u-%u", edges ? (unsigned)e.vhf_lo_hz : 0u, edges ? (unsigned)e.vhf_hi_hz : 0u);
		return true;
	case 15:
		dl_fmt(buf, len, "opt.uhf_hz=%u-%u", edges ? (unsigned)e.uhf_lo_hz : 0u, edges ? (unsigned)e.uhf_hi_hz : 0u);
		return true;
	case 16: {	// matrix rows K0-K3, columns K4-K6 in bits 0-2 of each nibble, 1 = pressed
		uint8_t rows[4];
		md9600_input_raw(rows);
		dl_fmt(buf, len, "keys=0x%x%x%x%x", rows[0] & 0xFu, rows[1] & 0xFu, rows[2] & 0xFu, rows[3] & 0xFu);
		return true;
	}
	case 17:
		dl_fmt(buf, len, "ptt=%u", PIN_READ(PIN_PTT) ? 0u : 1u);
		return true;
	case 18:	// the radio core's synthesizer state
		dl_fmt(buf, len, "lo_hz=%u", (unsigned)rd.lo_hz);
		return true;
	case 19:
		dl_fmt(buf, len, "retunes=%u", (unsigned)rd.retunes);
		return true;
	case 20:
		dl_fmt(buf, len, "refused=%u", (unsigned)rd.refused);
		return true;
	case 21:	// output levels: Power_Control and the receiver supplies
		dl_fmt(buf, len, "pin.power=%u", LATCH(PIN_POWER));
		return true;
	case 22:
		dl_fmt(buf, len, "pin.c5=%u%u", LATCH(PIN_C5_U), LATCH(PIN_C5_V));
		return true;
	case 23:
		dl_fmt(buf, len, "pin.r5=%u%u", LATCH(PIN_R5_U), LATCH(PIN_R5_V));
		return true;
	case 24:	// the transmitter: both T5 supplies and the PA loop, all 0 while transmit is refused
		dl_fmt(buf, len, "pin.tx=%u%u%u", LATCH(PIN_T5_U), LATCH(PIN_T5_V), LATCH(PIN_APC_EN));
		return true;
	default:
		break;
	}
	uint8_t a = (uint8_t)(i - 25u);
	if (a < MD9600_ADC_COUNT) {
		dl_fmt(buf, len, "adc.%s=%u", adc_names[a], md9600_adc_raw(a));
		return true;
	}
	return audio_line((uint8_t)(a - MD9600_ADC_COUNT), buf, len);
}

#endif	// CONFIG_DEBUGLINK
