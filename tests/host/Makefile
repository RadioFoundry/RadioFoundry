# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## Host unit tests for the hardware independent parts of the firmware
##
##   make -C tests/host

ROOT    := ../..
CORE    := $(ROOT)/core
CC      ?= cc
CFLAGS  := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -g -I$(CORE) -I$(ROOT)/drivers '-D__attribute__(x)='  # the firmware's linker section attributes are ARM ELF only

DATA    := $(CORE)/dmr/dmrData.c
FRAME   := $(CORE)/hotspot/dmrDataFrame.c $(CORE)/dmr/fec/dmrFec.c

TSHARK  ?= $(firstword $(wildcard /Applications/Wireshark*.app/Contents/MacOS/tshark) $(shell command -v tshark))

TESTS   := dmrFecTest sfrTest dmrDataTest dmrDataServiceTest dmrDataServiceArsTest hotspotDataTest hotspotUplinkTest dmrPrivacyTest \
           ipGatewayTest ipGatewayTest16 ipGatewayTest31 ipGatewayTestShared

all: run

dmrFecTest: dmrFecTest.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/fec/dmrFec.h
	$(CC) $(CFLAGS) -o $@ dmrFecTest.c $(CORE)/dmr/fec/dmrFec.c

SOURCES := dmrDataTest.c $(DATA) $(FRAME)

dmrDataTest: $(SOURCES) $(CORE)/dmr/dmrData.h $(CORE)/hotspot/dmrDataFrame.h
	$(CC) $(CFLAGS) -o $@ $(SOURCES)

HSDATA  := hotspotDataTest.c $(CORE)/hotspot/hotspotData.c $(DATA) $(FRAME)

hotspotDataTest: $(HSDATA) $(CORE)/hotspot/hotspotData.h
	$(CC) $(CFLAGS) -o $@ $(HSDATA)

# The simplex hotspot's RF -> MMDVMHost call tracking, against a model of MMDVMHost's DMRSlot
hotspotUplinkTest: hotspotUplinkTest.c $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotUplink.h
	$(CC) $(CFLAGS) -o $@ hotspotUplinkTest.c $(CORE)/hotspot/hotspotUplink.c

IPGW    := ipGatewayTest.c $(CORE)/dmr/ipGateway.c

ipGatewayTest: $(IPGW) $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) -o $@ $(IPGW)

# The same gateway test with a /16 plan: radios 10.250.0.0/16, talkgroups 10.251.0.0/16, multicast 239.250.0.0/16
IPGW16  := -DIPGW_INDIVIDUAL_NET=0x0AFA0000u -DIPGW_INDIVIDUAL_PREFIX=16 -DIPGW_GROUP_NET=0x0AFB0000u -DIPGW_GROUP_PREFIX=16 \
           -DIPGW_MULTICAST_NET=0xEFFA0000u -DIPGW_MULTICAST_PREFIX=16 -DIPGW_LINK_PREFIX=15 '-DPCAP_NAME="ipGatewayTest16.pcap"'

ipGatewayTest16: $(IPGW) $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) $(IPGW16) -o $@ $(IPGW)

# The default ranges on a /31 link, and on a shared /8 link, instead of the /32
ipGatewayTest31: $(IPGW) $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) -DIPGW_LINK_PREFIX=31 '-DPCAP_NAME="ipGatewayTest31.pcap"' -o $@ $(IPGW)

ipGatewayTestShared: $(IPGW) $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) -DIPGW_LINK_PREFIX=8 '-DPCAP_NAME="ipGatewayTestShared.pcap"' -o $@ $(IPGW)

# The service's reply queue and retried message handling, with the firmware it uses stubbed (stubs/)
SVC     := dmrDataServiceTest.c $(DATA) $(FRAME) $(CORE)/dmr/dmrLrrp.c

dmrDataServiceTest: $(SVC) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrDataService.h $(wildcard stubs/*.h stubs/*/*.h)
	$(CC) $(CFLAGS) -Istubs -o $@ $(SVC) -lm

# The same with Motorola ARS registration built in
dmrDataServiceArsTest: $(SVC) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrDataService.h $(wildcard stubs/*.h stubs/*/*.h)
	$(CC) $(CFLAGS) -Istubs -DHAS_DMR_ARS -DHAS_DMR_GPS_REPORTS -DHAS_DMR_LRRP -o $@ $(SVC) -lm

dmrPrivacyTest: dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/dmrPrivacy.h
	$(CC) $(CFLAGS) -o $@ dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c

# The single frequency repeater's generic logic (with BPTC for the idle message)
SFRSRC  := sfrTest.c $(CORE)/dmr/sfr.c $(DATA) $(FRAME)

sfrTest: $(SFRSRC) $(CORE)/dmr/sfr.h
	$(CC) $(CFLAGS) -DHAS_SFR -o $@ $(SFRSRC)

run: $(TESTS)
	./dmrFecTest
	./dmrDataTest > dmrDataTest.out; status=$$?; grep -E "^(FAIL|# (PASSED|FAILED))" dmrDataTest.out; exit $$status
	./dmrDataServiceTest
	./dmrDataServiceArsTest
	./hotspotDataTest
	./hotspotUplinkTest
	./dmrPrivacyTest
	./ipGatewayTest
	./ipGatewayTest16
	./ipGatewayTest31
	./ipGatewayTestShared
	./sfrTest
ifneq ($(TSHARK),)
	@# Independent check of the frames the gateway sent: no malformed packets or bad checksums
	@for f in ipGatewayTest.pcap ipGatewayTest16.pcap ipGatewayTest31.pcap ipGatewayTestShared.pcap; do "$(TSHARK)" -r $$f -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE -z expert,note -q; done > ipGatewayTest.expert
	@if grep -q -i -E "malformed|bad|incorrect|error" ipGatewayTest.expert; then echo "# tshark found problems"; cat ipGatewayTest.expert; exit 1; else echo "# tshark: no malformed packets or bad checksums"; fi
endif

clean:
	rm -f $(TESTS) dmrDataTest.out ipGatewayTest.pcap ipGatewayTest16.pcap ipGatewayTest31.pcap ipGatewayTestShared.pcap ipGatewayTest.expert
	rm -rf *.dSYM

.PHONY: all run clean
