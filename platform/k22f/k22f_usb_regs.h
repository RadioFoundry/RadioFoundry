// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The K22F USB controller's register bits (reference manual chapter 47) under the names TinyUSB's full-speed driver
// uses (portable/chipidea/ci_fs), and how a token's STAT value picks its buffer descriptor. Pure: the host tests check
// the bits against the driver's buffer descriptor table layout (tests/host/k22fUsbTest.c).
#ifndef K22F_USB_REGS_H
#define K22F_USB_REGS_H

#include <stdbool.h>
#include <stdint.h>

#define USB_ISTAT_USBRST_MASK		0x01u
#define USB_ISTAT_ERROR_MASK		0x02u
#define USB_ISTAT_SOFTOK_MASK		0x04u
#define USB_ISTAT_TOKDNE_MASK		0x08u
#define USB_ISTAT_SLEEP_MASK		0x10u
#define USB_ISTAT_RESUME_MASK		0x20u
#define USB_ISTAT_STALL_MASK		0x80u

#define USB_INTEN_USBRSTEN_MASK		0x01u
#define USB_INTEN_ERROREN_MASK		0x02u
#define USB_INTEN_TOKDNEEN_MASK		0x08u
#define USB_INTEN_SLEEPEN_MASK		0x10u
#define USB_INTEN_RESUMEEN_MASK		0x20u
#define USB_INTEN_STALLEN_MASK		0x80u

#define USB_STAT_ODD_MASK		0x04u
#define USB_STAT_TX_MASK		0x08u
#define USB_STAT_TX_SHIFT		3u
#define USB_STAT_ENDP_SHIFT		4u

#define USB_CTL_USBENSOFEN_MASK		0x01u
#define USB_CTL_ODDRST_MASK		0x02u
#define USB_CTL_RESUME_MASK		0x04u
#define USB_CTL_TXSUSPENDTOKENBUSY_MASK	0x20u

#define USB_ENDPT_EPHSHK_MASK		0x01u
#define USB_ENDPT_EPSTALL_MASK		0x02u
#define USB_ENDPT_EPTXEN_MASK		0x04u
#define USB_ENDPT_EPRXEN_MASK		0x08u
#define USB_ENDPT_EPCTLDIS_MASK		0x10u

#define USB_USBCTRL_SUSP_MASK		0x80u
#define USB_CONTROL_DPPULLUPNONOTG_MASK	0x10u
#define USB_USBTRC0_USB_RESUME_INT_MASK	0x01u
#define USB_USBTRC0_USBRESMEN_MASK	0x20u
#define USB_USBTRC0_USBRESET_MASK	0x80u
#define USB_USBTRC0_SET_ONE_MASK	0x40u	// reserved: software writes it as 1
#define USB_ADDR_MASK			0x7Fu

// A buffer descriptor's first word (reference manual 47.3.4); the second is the buffer's address. The controller
// writes the token's PID over bits 5:2 (KEEP, NINC, DTS, BDT_STALL) and the byte count when it hands the descriptor back
#define USB_BD_OWN			0x80u	// the controller owns it
#define USB_BD_DATA1			0x40u	// DATA1 (else DATA0) sent, or expected with DTS
#define USB_BD_DTS			0x08u	// check the received data toggle
#define USB_BD_STALL			0x04u	// answer the token with STALL
#define USB_BD_BC_SHIFT			16u
#define USB_BD_BC_MASK			0x3FFu
#define USB_TOKEN_PID_SETUP		0xDu	// the PID a SETUP token leaves (OUT 0x1, IN 0x9)

static inline uint32_t k22f_usb_bd_word(uint32_t bc, bool data1, bool dts)
{
	return ((bc & USB_BD_BC_MASK) << USB_BD_BC_SHIFT) | (data1 ? USB_BD_DATA1 : 0u) | (dts ? USB_BD_DTS : 0u) |
	       USB_BD_OWN;
}

static inline uint8_t k22f_usb_bd_pid(uint32_t w)
{
	return (uint8_t)((w >> 2) & 0xFu);
}

static inline uint32_t k22f_usb_bd_bc(uint32_t w)
{
	return (w >> USB_BD_BC_SHIFT) & USB_BD_BC_MASK;
}


// The buffer descriptor table: 16 endpoints x 2 directions (0 = OUT, 1 = IN) x 2 (even, odd), 8 bytes each, 512-byte
// aligned. STAT (ENDP 7:4, TX 3, ODD 2) shifted right by 2 is the index of the descriptor the token used
static inline uint8_t k22f_usb_bdt_index(uint8_t ep, uint8_t dir, uint8_t odd)
{
	return (uint8_t)(ep * 4u + dir * 2u + odd);
}

static inline uint8_t k22f_usb_stat(uint8_t ep, uint8_t dir, uint8_t odd)
{
	return (uint8_t)((ep << USB_STAT_ENDP_SHIFT) | (dir ? USB_STAT_TX_MASK : 0u) | (odd ? USB_STAT_ODD_MASK : 0u));
}

// The unique ID as the USB serial number: the low 96 bits (words high to low), 24 hex digits, cut to fit len with
// the NUL
static inline void k22f_uid_hex(const uint32_t uid[3], char *buf, uint8_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	uint8_t n = 0;
	if (len == 0u) {
		return;
	}
	for (uint8_t w = 0; w < 3u; w++) {
		for (int8_t shift = 28; shift >= 0 && n + 1u < len; shift -= 4) {
			buf[n++] = hex[(uid[w] >> shift) & 0xFu];
		}
	}
	buf[n] = '\0';
}

#endif
