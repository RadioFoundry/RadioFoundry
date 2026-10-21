// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F USB glue's pure parts (platform/k22f/k22f_usb_regs.h): every token's STAT value selects the buffer
// descriptor the full-speed driver set up for that endpoint, direction and even / odd half; the bit masks of each
// register don't overlap; the serial number string; a buffer descriptor's first word; and the data toggles the flash
// stub carries on with, read from the driver's state at the hand-over (platform/k22f/stub_handoff.h). The driver itself
// (TinyUSB ci_fs) keeps a 32-bit buffer address in each 8-byte descriptor, so it only builds for the radio
#include <stdio.h>
#include <string.h>
#include "k22f_usb_regs.h"
#include "stub_handoff.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void bdt_indexing(void)
{
	// The driver's table is bdt[16][2][2] and it reads the descriptor at (STAT >> 2): the same entry for every token
	uint8_t seen[64] = { 0 };
	for (uint8_t ep = 0; ep < 16u; ep++) {
		for (uint8_t dir = 0; dir < 2u; dir++) {
			for (uint8_t odd = 0; odd < 2u; odd++) {
				uint8_t s = k22f_usb_stat(ep, dir, odd);
				uint8_t i = k22f_usb_bdt_index(ep, dir, odd);
				CHECK((s >> 2) == i);
				CHECK(i == (ep * 2u + dir) * 2u + odd);	// the C array's own order
				CHECK((s >> USB_STAT_ENDP_SHIFT) == ep);
				CHECK(((s & USB_STAT_TX_MASK) >> USB_STAT_TX_SHIFT) == dir);
				CHECK(((s & USB_STAT_ODD_MASK) != 0) == (odd != 0));
				seen[i]++;
			}
		}
	}
	for (unsigned i = 0; i < 64u; i++) {
		CHECK(seen[i] == 1u);
	}
	CHECK((USB_STAT_TX_MASK >> USB_STAT_TX_SHIFT) == 1u);
}

static unsigned bits(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1u) {
		n++;
	}
	return n;
}

static void masks(void)
{
	// One bit each, none shared within a register
	static const uint8_t istat[] = { USB_ISTAT_USBRST_MASK, USB_ISTAT_ERROR_MASK, USB_ISTAT_SOFTOK_MASK,
					 USB_ISTAT_TOKDNE_MASK, USB_ISTAT_SLEEP_MASK, USB_ISTAT_RESUME_MASK,
					 USB_ISTAT_STALL_MASK };
	static const uint8_t inten[] = { USB_INTEN_USBRSTEN_MASK, USB_INTEN_ERROREN_MASK, USB_INTEN_TOKDNEEN_MASK,
					 USB_INTEN_SLEEPEN_MASK, USB_INTEN_RESUMEEN_MASK, USB_INTEN_STALLEN_MASK };
	static const uint8_t endpt[] = { USB_ENDPT_EPHSHK_MASK, USB_ENDPT_EPSTALL_MASK, USB_ENDPT_EPTXEN_MASK,
					 USB_ENDPT_EPRXEN_MASK, USB_ENDPT_EPCTLDIS_MASK };
	static const uint8_t ctl[] = { USB_CTL_USBENSOFEN_MASK, USB_CTL_ODDRST_MASK, USB_CTL_RESUME_MASK,
				       USB_CTL_TXSUSPENDTOKENBUSY_MASK };
	static const uint8_t trc[] = { USB_USBTRC0_USB_RESUME_INT_MASK, USB_USBTRC0_USBRESMEN_MASK,
				       USB_USBTRC0_USBRESET_MASK };
	const struct {
		const uint8_t *m;
		unsigned n;
	} regs[] = { { istat, sizeof istat }, { inten, sizeof inten }, { endpt, sizeof endpt }, { ctl, sizeof ctl },
		     { trc, sizeof trc } };
	for (unsigned r = 0; r < sizeof regs / sizeof regs[0]; r++) {
		uint32_t all = 0;
		for (unsigned k = 0; k < regs[r].n; k++) {
			CHECK(bits(regs[r].m[k]) == 1u);
			CHECK((all & regs[r].m[k]) == 0);
			all |= regs[r].m[k];
		}
	}
	// Each interrupt enable sits on its status bit
	CHECK(USB_INTEN_USBRSTEN_MASK == USB_ISTAT_USBRST_MASK && USB_INTEN_TOKDNEEN_MASK == USB_ISTAT_TOKDNE_MASK);
	CHECK(USB_INTEN_SLEEPEN_MASK == USB_ISTAT_SLEEP_MASK && USB_INTEN_STALLEN_MASK == USB_ISTAT_STALL_MASK);
	CHECK(USB_INTEN_RESUMEEN_MASK == USB_ISTAT_RESUME_MASK && USB_INTEN_ERROREN_MASK == USB_ISTAT_ERROR_MASK);
}

static void serial(void)
{
	const uint32_t uid[3] = { 0x0012ABCDu, 0x89ABCDEFu, 0x01234567u };
	char buf[32];
	k22f_uid_hex(uid, buf, sizeof buf);
	CHECK(strcmp(buf, "0012ABCD89ABCDEF01234567") == 0);
	k22f_uid_hex(uid, buf, 5);	// cut to fit, terminated
	CHECK(strcmp(buf, "0012") == 0);
	buf[0] = 'x';
	k22f_uid_hex(uid, buf, 1);
	CHECK(buf[0] == '\0');
}

static void bd_words(void)
{
	CHECK(k22f_usb_bd_word(64, true, true) == 0x004000C8u);
	CHECK(k22f_usb_bd_word(0, false, false) == USB_BD_OWN);
	CHECK(k22f_usb_bd_bc(0x02340034u) == 0x234u && k22f_usb_bd_pid(0x02340034u) == USB_TOKEN_PID_SETUP);
	CHECK((USB_BD_OWN | USB_BD_DATA1 | USB_BD_DTS | USB_BD_STALL) == 0xCCu);
}

// The driver's memory as TinyUSB's full-speed driver leaves it: each endpoint opened with its next half's descriptor at
// DATA0 and the other at DATA1, then some transfers done (the next half flips with each)
static void open_ep(uint8_t *mem, unsigned ep, unsigned dir, unsigned mps, unsigned transfers)
{
	unsigned odd_at_open = 0;
	mem[(ep * 4u + dir * 2u + odd_at_open) * 8u] = 0x08;			// DTS, DATA0
	mem[(ep * 4u + dir * 2u + (odd_at_open ^ 1u)) * 8u] = 0x48;		// DTS, DATA1
	uint32_t state = mps | (((odd_at_open + transfers) & 1u) << 16);
	memcpy(mem + K22F_STUB_EP_STATE + (ep * 2u + dir) * 8u, &state, 4);
}

static void handoff(void)
{
	static uint8_t mem[K22F_STUB_MEM_LEN];
	memset(mem, 0, sizeof mem);
	open_ep(mem, 0, 0, 64, 7);
	open_ep(mem, 0, 1, 64, 5);
	open_ep(mem, 1, 0, 64, 2);	// bulk OUT: two packets in, DATA0 next
	open_ep(mem, 1, 1, 64, 3);	// bulk IN: three packets out, DATA1 next
	open_ep(mem, 2, 1, 8, 0);	// interrupt IN: never used
	CHECK(k22f_stub_toggles(mem) == (K22F_STUB_TOGGLE_VALID | K22F_STUB_TOGGLE_IN1));
	open_ep(mem, 1, 0, 64, 5);
	open_ep(mem, 1, 1, 64, 4);
	open_ep(mem, 2, 1, 8, 1);
	CHECK(k22f_stub_toggles(mem) == (K22F_STUB_TOGGLE_VALID | K22F_STUB_TOGGLE_OUT1 | K22F_STUB_TOGGLE_IN2));
	// Not the layout described (another driver version, the network personality): nothing is claimed
	open_ep(mem, 2, 1, 64, 1);
	CHECK(k22f_stub_toggles(mem) == 0);
	memset(mem, 0, sizeof mem);
	CHECK(k22f_stub_toggles(mem) == 0);
	// The stub's span is SRAM_L, under the boundary SRAM_U starts at, and the staging limit fits it
	CHECK(K22F_STUB_RUN_ADDR + K22F_STUB_SPAN == 0x20000000u && K22F_STUB_STAGE_SIZE < K22F_STUB_SPAN);
}

int main(void)
{
	bdt_indexing();
	masks();
	serial();
	bd_words();
	handoff();
	if (failures) {
		printf("k22fUsbTest: %d failures\n", failures);
		return 1;
	}
	printf("k22fUsbTest: all passed\n");
	return 0;
}
