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

# The radio core's state machine and the input gesture engine, against a fake board and codeplug (no RTOS)
CORE_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += radioCoreTest inputTest

# With the DMR call stack, the AMBE voice glue and the simulated modem behind board.dmr
RCORE   := radioCoreTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c \
           sim/dmr_sim_modem.c $(CORE)/settings/settings.c $(CORE)/storage/storage.c

radioCoreTest: $(RCORE) $(CORE)/radio/radio_sm.h $(CORE)/radio/radio.h $(CORE)/radio/tones.h $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -o $@ $(RCORE)

inputTest: inputTest.c $(CORE)/input/input_engine.c $(CORE)/input/input_engine.h $(CORE)/input/input.h
	$(CC) $(CFLAGS) $(CORE_HAL) -o $@ inputTest.c $(CORE)/input/input_engine.c

run: run-core
run-core: radioCoreTest inputTest
	./radioCoreTest
	./inputTest
.PHONY: run-core

# The read-only codeplug (legacy layout and built-in channels) and the DM-1701 calibration reader, on fake flash images
FLASH_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += codeplugTest calibrationTest

codeplugTest: codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/codeplug.h $(CORE)/storage/storage.c
	$(CC) $(CFLAGS) $(FLASH_HAL) -o $@ codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/storage/storage.c

calibrationTest: calibrationTest.c $(ROOT)/boards/dm1701/calibration_dm1701.c $(ROOT)/boards/dm1701/dm1701.h
	$(CC) $(CFLAGS) $(FLASH_HAL) -I$(ROOT)/boards/dm1701 -o $@ calibrationTest.c $(ROOT)/boards/dm1701/calibration_dm1701.c

run: run-flash
run-flash: codeplugTest calibrationTest
	./codeplugTest
	./calibrationTest
.PHONY: run-flash

# The AT1846S driver: frequency word, DCS Golay word, register cache and write sequences, on a fake bus
TESTS   += at1846sTest

at1846sTest: at1846sTest.c $(ROOT)/drivers/rf/at1846s/at1846s.c $(ROOT)/drivers/rf/at1846s/at1846s.h
	$(CC) $(CFLAGS) -Wshadow -o $@ at1846sTest.c $(ROOT)/drivers/rf/at1846s/at1846s.c

run: run-rf
run-rf: at1846sTest
	./at1846sTest
.PHONY: run-rf

# UI drawing: text helpers, changed-row flushing, Home and quick settings at each display class (out/*.ppm to look at)
UI      := $(CORE)/ui/gfx.c $(CORE)/ui/fonts.c $(CORE)/ui/fmt.c $(CORE)/ui/ui_screens.c
TESTS   += gfxTest

gfxTest: gfxTest.c $(UI) $(wildcard $(CORE)/ui/*.h $(CORE)/ui/fonts/*.inc)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ gfxTest.c $(UI)

run: run-ui
run-ui: gfxTest
	./gfxTest
.PHONY: run-ui

# AMBE+2 3600x2450 air frame FEC and interleaving (core/dmr/ambe), with vectors from the stock DM-1701 codec's FEC
# and the DMR voice glue between the call stack, the FEC, the vocoder and the audio driver (fakes)
TESTS   += ambeFecTest ambeVoiceTest

ambeFecTest: ambeFecTest.c $(CORE)/dmr/ambe/ambeFec.c $(CORE)/dmr/ambe/ambeFec.h
	$(CC) $(CFLAGS) -Wshadow -o $@ ambeFecTest.c $(CORE)/dmr/ambe/ambeFec.c

ambeVoiceTest: ambeVoiceTest.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeVoice.h $(CORE)/dmr/ambe/ambeFec.c
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ ambeVoiceTest.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c

run: run-ambe
run-ambe: ambeFecTest ambeVoiceTest
	./ambeFecTest
	./ambeVoiceTest
.PHONY: run-ambe

# The DMR voice call stack (core/dmr/call) against a simulated modem; LC coding cross-checked with sfr.c and dmrData.c
CALL    := dmrCallTest.c sim/dmr_sim_modem.c $(CORE)/dmr/call/dmr_call.c $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/sfr.c $(DATA) $(FRAME)
TESTS   += dmrCallTest

dmrCallTest: $(CALL) $(wildcard $(CORE)/dmr/call/*.h sim/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -DHAS_SFR -DDMR_RX_TRACE=0 -o $@ $(CALL)

run: run-call
run-call: dmrCallTest
	./dmrCallTest
.PHONY: run-call

# The HR_C6000 DMR modem: register sequences, burst status decode, RX de-duplication, TX queue and slot scheduling
TESTS   += hrc6000DmrTest
HRCDMR  := $(ROOT)/drivers/modem/hrc6000

hrc6000DmrTest: hrc6000DmrTest.c $(HRCDMR)/hrc6000_dmr.c $(HRCDMR)/hrc6000.c $(HRCDMR)/hrc6000_dmr.h $(HRCDMR)/hrc6000.h $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ hrc6000DmrTest.c $(HRCDMR)/hrc6000_dmr.c $(HRCDMR)/hrc6000.c

run: run-hrcdmr
run-hrcdmr: hrc6000DmrTest
	./hrc6000DmrTest
.PHONY: run-hrcdmr

# The DMR receive chain: a model of the HR_C6000's reports through the slot engine, a batching board task and the call
# stack to the voice sink (every burst on air out once, in order, nothing concealed)
TESTS   += dmrRxChainTest
RXCHAIN := dmrRxChainTest.c $(HRCDMR)/hrc6000_dmr.c $(HRCDMR)/hrc6000.c $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c

dmrRxChainTest: $(RXCHAIN) $(HRCDMR)/hrc6000_dmr.h $(HRCDMR)/hrc6000.h $(ROOT)/hal/hal_dmr_modem.h $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DDMR_RX_TRACE=1 -o $@ $(RXCHAIN)

run: run-rxchain
run-rxchain: dmrRxChainTest
	./dmrRxChainTest
.PHONY: run-rxchain

# The MMDVM protocol and the simplex hotspot engine (core/hotspot), against a fake DMR modem and radio core, and the
# host link's rule for leaving hotspot mode (core/hostlink/hostlink_policy.c)
MMDVM   := mmdvmTest.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c $(CORE)/hotspot/hotspotUplink.c \
           $(CORE)/hotspot/hotspotData.c $(FRAME) $(CORE)/dmr/call/dmr_lc.c $(CORE)/hostlink/hostlink_policy.c
TESTS   += mmdvmTest

mmdvmTest: $(MMDVM) $(wildcard $(CORE)/hotspot/*.h $(CORE)/hostlink/*.h $(CORE)/dmr/call/dmr_lc.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ $(MMDVM)

run: run-mmdvm
run-mmdvm: mmdvmTest
	./mmdvmTest
.PHONY: run-mmdvm

# The debug link's portable parts: printf subset, CRC, frame parser / dispatcher, dlog ring (core/debuglink)
DLINK   := debuglinkTest.c $(CORE)/debuglink/debuglink_proto.c $(CORE)/debuglink/dl_fmt.c $(CORE)/debuglink/dlog.c \
	$(CORE)/debuglink/dl_chip.c
TESTS   += debuglinkTest

debuglinkTest: $(DLINK) $(wildcard $(CORE)/debuglink/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DDLOG_NO_RTOS -o $@ $(DLINK)

run: run-debuglink
run-debuglink: debuglinkTest
	./debuglinkTest
.PHONY: run-debuglink

# The sector journal and the settings store on a simulated NOR flash, with power cuts at every point of a write
STORE   := $(CORE)/storage/storage.c $(CORE)/settings/settings.c sim/sim_flash.c
TESTS   += settingsTest

settingsTest: settingsTest.c $(STORE) $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h sim/sim_flash.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ settingsTest.c $(STORE)

run: run-settings
run-settings: settingsTest
	./settingsTest
.PHONY: run-settings

# Codeplug editing: the legacy record encoder, the built-in set made a stored codeplug, zones / contacts / lists,
# the round trip read -> write -> read, power cuts during an edit
CPEDIT  := $(CORE)/codeplug/codeplug.c $(CORE)/storage/storage.c sim/sim_flash.c
TESTS   += codeplugEditTest

codeplugEditTest: codeplugEditTest.c $(CPEDIT) $(wildcard $(CORE)/codeplug/*.h $(CORE)/storage/*.h sim/sim_flash.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ codeplugEditTest.c $(CPEDIT)

run: run-cpedit
run-cpedit: codeplugEditTest
	./codeplugEditTest
.PHONY: run-cpedit

# The legacy CPS shim against a simulated flash, driven like qdmr's UV380 driver (core/cps)
CPS     := $(CORE)/cps/cps_legacy.c $(CORE)/codeplug/codeplug.c $(CORE)/settings/settings.c $(CORE)/storage/storage.c \
           sim/sim_flash.c
TESTS   += cpsTest

cpsTest: cpsTest.c $(CPS) $(wildcard $(CORE)/cps/*.h $(CORE)/codeplug/*.h $(CORE)/settings/*.h $(CORE)/storage/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ cpsTest.c $(CPS)

run: run-cps
run-cps: cpsTest
	./cpsTest
.PHONY: run-cps

# tools/cps_client.py against the shim on stdin / stdout (cpsSim), when python3 is there
PYTHON  ?= $(shell command -v python3)
TESTS   += cpsSim

cpsSim: cpsSim.c $(CPS) $(wildcard $(CORE)/cps/*.h $(CORE)/codeplug/*.h $(CORE)/settings/*.h $(CORE)/storage/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ cpsSim.c $(CPS)

run: run-cpsclient
run-cpsclient: cpsSim
ifneq ($(PYTHON),)
	$(PYTHON) -B cpsClientTest.py
else
	@echo "# cpsClientTest skipped (no python3)"
endif
.PHONY: run-cpsclient
