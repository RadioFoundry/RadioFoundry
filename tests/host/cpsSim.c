// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The open-firmware CPS shim on a simulated 16 MB flash with the DM-1701 map, served on stdin / stdout, so host tools
// (tools/cps_client.py --exec) can be tested without a radio:
//
//   cpsSim [--populate] [--image FILE] [--dump FILE] [--list]
//
// --populate stores a small codeplug first, --image loads a flash image (file offset = flash address), --dump
// writes the flash to FILE when stdin closes, --list prints the stored channels, contacts and zones and exits. A "reboot" (C 6 0 / 1) re-reads the settings and the codeplug, as the
// radio would after its restart.
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cps/cps_openfw.h"
#include "codeplug/codeplug.h"
#include "settings/settings.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x1000000u

static const hal_flash_map_t map = {
	.settings_addr = 0x011000u, .settings_size = 0x008000u, .journal_addr = 0x019000u,
	.openfw_eeprom = 0, .openfw_flash = 0x020000u, .openfw_end = 0xE00000u,
	.calibration_addr = 0x010000u, .calibration_size = 0x200u,
	.native_addr = 0xE00000u, .native_size = 0x100000u,
};

static uint32_t radio_type(void) { return 8; }
static const hal_cps_t cps_info = { .radio_type = radio_type };
static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 480000000, true } };
const board_t board = { .name = "sim", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash,
			.cps = &cps_info };

static uint32_t millis(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void out(const uint8_t *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(1, buf, len);
		if (n <= 0) {
			if (errno == EINTR) {
				continue;
			}
			exit(1);
		}
		buf += n;
		len -= (size_t)n;
	}
}

static void boot(void);

static void sim_reboot(bool save)
{
	fprintf(stderr, "cpsSim: %s\n", save ? "save and reboot" : "reboot");
	boot();
}

static const cps_io_t io = { .write = out, .reboot = sim_reboot };

static void boot(void)
{
	storage_init();
	settings_init();
	codeplug_init();
	cps_init(&io);
}

static void populate(void)
{
	for (uint16_t n = 1; n <= 40; n++) {
		channel_t c;
		memset(&c, 0, sizeof(c));
		snprintf(c.name, sizeof(c.name), "Sim %u", n);
		c.rx_hz = c.tx_hz = 446000000u + n * 12500u;
		c.mode = n % 2 ? RADIO_MODE_FM : RADIO_MODE_DMR;
		c.power = CODEPLUG_POWER_DEFAULT;
		c.color_code = 1;
		c.timeslot = 1;
		codeplug_set_channel(n, &c);
	}
	zone_t z;
	memset(&z, 0, sizeof(z));
	strcpy(z.name, "Sim zone");
	z.count = 10;
	for (uint16_t i = 0; i < 10; i++) {
		z.channels[i] = (uint16_t)(1 + i * 3);
	}
	codeplug_set_zone(0, &z);
	codeplug_set_radio_name("N0SIM");
#if CONFIG_DMR
	codeplug_set_dmr_id(3100001);
	contact_t ct = { "TG 91", 91, CONTACT_GROUP, 0 };
	codeplug_set_contact(1, &ct);
#endif
	for (unsigned i = 0; i < 20; i++) {
		sim_flash_data[0x7524 + i] = (uint8_t)(0x40 + i);	// quick keys
	}
}

// What the radio's codeplug holds, one line each, for a test to compare
static void list(void)
{
	printf("source %d channels %u\n", (int)codeplug_source(), (unsigned)codeplug_channel_count());
	for (uint16_t n = 1; n <= codeplug_channel_count(); n++) {
		channel_t c;
		if (codeplug_channel(n, &c)) {
			printf("channel %u \"%s\" rx %u tx %u mode %u bw %u cc %u ts %u contact %u\n", (unsigned)n, c.name,
			       (unsigned)c.rx_hz, (unsigned)c.tx_hz, c.mode, c.bw, c.color_code, c.timeslot, (unsigned)c.contact);
		}
	}
#if CONFIG_DMR
	for (uint16_t n = 1; n <= 4; n++) {
		contact_t ct;
		if (codeplug_contact(n, &ct)) {
			printf("contact %u \"%s\" id %u type %u\n", (unsigned)n, ct.name, (unsigned)ct.id, ct.type);
		}
	}
#endif
	for (uint16_t z = 0; z < 4; z++) {
		zone_t zn;
		if (codeplug_zone(z, &zn)) {
			printf("zone %u \"%s\" %u:", (unsigned)z, zn.name, (unsigned)zn.count);
			for (uint16_t i = 0; i < zn.count; i++) {
				printf(" %u", (unsigned)zn.channels[i]);
			}
			printf("\n");
		}
	}
}

int main(int argc, char **argv)
{
	const char *image = NULL, *dump = NULL;
	bool demo = false, listing = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--populate") == 0) {
			demo = true;
		} else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
			image = argv[++i];
		} else if (strcmp(argv[i], "--list") == 0) {
			listing = true;
		} else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dump = argv[++i];
		} else {
			fprintf(stderr, "usage: cpsSim [--populate] [--image FILE] [--dump FILE] [--list]\n");
			return 2;
		}
	}
	sim_flash_create(FLASH_SIZE, &map);
	if (image != NULL) {
		FILE *f = fopen(image, "rb");
		if (f == NULL) {
			perror(image);
			return 1;
		}
		size_t n = fread(sim_flash_data, 1, FLASH_SIZE, f);
		(void)n;
		fclose(f);
	}
	boot();
	if (demo) {
		populate();
		boot();
	}

	if (listing) {
		list();
		sim_flash_free();
		return 0;
	}
	uint8_t buf[512];
	for (;;) {
		struct pollfd p = { .fd = 0, .events = POLLIN };
		int r = poll(&p, 1, 2);
		if (r > 0) {
			ssize_t n = read(0, buf, sizeof(buf));
			if (n <= 0) {
				break;
			}
			cps_feed(buf, (size_t)n, millis());
		} else {
			cps_feed(NULL, 0, millis());
		}
	}
	if (dump != NULL) {
		FILE *f = fopen(dump, "wb");
		if (f == NULL || fwrite(sim_flash_data, 1, FLASH_SIZE, f) != FLASH_SIZE) {
			perror(dump);
			return 1;
		}
		fclose(f);
	}
	sim_flash_free();
	return 0;
}
