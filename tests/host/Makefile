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

TESTS   := dmrFecTest sfrTest dmrDataTest dmrDataServiceTest hotspotDataTest hotspotUplinkTest dmrPrivacyTest \
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

# The data service (replies, signaling, our SMS, ARS, LRRP, the API mailbox) on a fake host
SVC     := dmrDataServiceTest.c $(DATA) $(FRAME) $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c

dmrDataServiceTest: $(SVC) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrDataService.h $(CORE)/dmr/dmr_data_api.h
	$(CC) $(CFLAGS) -o $@ $(SVC) -lm

dmrPrivacyTest: dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/dmrPrivacy.h $(CORE)/dmr/ambe/ambeFec.c
	$(CC) $(CFLAGS) -o $@ dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/ambe/ambeFec.c

# The single frequency repeater's generic logic (with BPTC for the idle message)
SFRSRC  := sfrTest.c $(CORE)/dmr/sfr.c $(DATA) $(FRAME)

sfrTest: $(SFRSRC) $(CORE)/dmr/sfr.h
	$(CC) $(CFLAGS) -DHAS_SFR -o $@ $(SFRSRC)

run: $(TESTS)
	./dmrFecTest
	./dmrDataTest > dmrDataTest.out; status=$$?; grep -E "^(FAIL|# (PASSED|FAILED))" dmrDataTest.out; exit $$status
	./dmrDataServiceTest
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
	rm -rf *.dSYM out

.PHONY: all run clean

# The radio core's state machine and the input gesture engine, against a fake board and codeplug (no RTOS)
CORE_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += radioCoreTest inputTest

# With the DMR call stack, the AMBE voice glue and the simulated modem behind board.dmr
RFEAT   := $(CORE)/radio/scan.c $(CORE)/radio/beep.c $(CORE)/radio/battery.c $(CORE)/radio/roam.c $(CORE)/radio/radio_am.c
RCORE   := radioCoreTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(RFEAT) $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c \
           $(CORE)/dmr/dmrPrivacy.c sim/dmr_sim_modem.c $(CORE)/dmr/dmrData.c $(CORE)/dmr/dmrDataService.c \
           $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c $(CORE)/radio/morse.c \
           $(CORE)/settings/settings.c $(CORE)/storage/storage.c \
           $(CORE)/radio/radio_hotspot.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c \
           $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotData.c $(CORE)/hotspot/dmrDataFrame.c \
           $(CORE)/ui/ui_svc_radio.c $(CORE)/ui/ui_svc_storage.c $(CORE)/ui/ui_svc_data.c \
           $(CORE)/ui/ui_svc_emergency.c

radioCoreTest: $(RCORE) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h) $(CORE)/dmr/dmrDataService.h \
               $(CORE)/ui/ui_svc.h
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -o $@ $(RCORE) -lm

# The everyday features (scan, beeps, battery, TOT warning, BCL, roger beep, 1750 Hz burst, VOX, battery saving)
TESTS   += radioFeaturesTest
RFTEST  := radioFeaturesTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(RFEAT) $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c \
           $(CORE)/dmr/dmrPrivacy.c sim/dmr_sim_modem.c $(CORE)/dmr/dmrData.c $(CORE)/dmr/dmrDataService.c \
           $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c $(CORE)/radio/morse.c \
           $(CORE)/settings/settings.c $(CORE)/storage/storage.c

radioFeaturesTest: $(RFTEST) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -o $@ $(RFTEST) -lm

# DMR repeater roaming: the engine, and through the radio core against fake repeaters on the simulated modem
TESTS   += roamTest
ROAMT   := roamTest.c $(filter-out radioFeaturesTest.c,$(RFTEST))

roamTest: $(ROAMT) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -o $@ $(ROAMT) -lm

# Air band AM receive through the radio core (docs/spec/airband-and-broadcast.md 3), a fake board with CAP_AM_RX
TESTS   += radioAmTest
AMTEST  := radioAmTest.c $(filter-out radioFeaturesTest.c,$(RFTEST))

radioAmTest: $(AMTEST) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -o $@ $(AMTEST) -lm

inputTest: inputTest.c $(CORE)/input/input_engine.c $(CORE)/input/input_engine.h $(CORE)/input/input.h
	$(CC) $(CFLAGS) $(CORE_HAL) -o $@ inputTest.c $(CORE)/input/input_engine.c

run: run-core
run-core: radioCoreTest radioFeaturesTest roamTest radioAmTest inputTest
	./radioCoreTest
	./radioFeaturesTest
	./roamTest
	./radioAmTest
	./inputTest
.PHONY: run-core

# The read-only codeplug (legacy layout and built-in channels) and the DM-1701 calibration reader, on fake flash images
FLASH_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += codeplugTest calibrationTest

codeplugTest: codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/codeplug.h $(CORE)/codeplug/dmr_iddb.c $(CORE)/codeplug/dmr_iddb.h $(CORE)/storage/storage.c
	$(CC) $(CFLAGS) $(FLASH_HAL) -o $@ codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/dmr_iddb.c $(CORE)/storage/storage.c

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
UI      := $(CORE)/ui/gfx.c $(CORE)/ui/fonts.c $(CORE)/ui/fmt.c $(CORE)/ui/ui_screens.c $(CORE)/ui/ui_svc_stub.c
TESTS   += gfxTest

gfxTest: gfxTest.c $(UI) $(wildcard $(CORE)/ui/*.h $(CORE)/ui/fonts/*.inc)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ gfxTest.c $(UI)

# The UI host simulator (tests/host/sim/ui_sim.c): the whole of core/ui against a simulated radio, codeplug and
# board at any display geometry and key map. uiFlowTest runs the task flows of docs/ux/task-flows.md against their
# press budgets, crawls every screen (BACK leaves it, long BACK reaches Home, the status strip is drawn) and checks
# the long-press rules; uiSim renders screens to PNG (make ui-gallery: docs/ux/screens/). Built without the
# __attribute__ override above so the weak service stubs (ui_svc_stub.c) give way to the simulator's.
UIAPP   := $(UI) $(CORE)/ui/ui_app.c $(CORE)/ui/ui_menus.c $(CORE)/ui/ui_find.c $(CORE)/radio/tones.c \
           sim/ui_sim.c sim/ui_png.c
UIFLAGS := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wshadow -g -I$(CORE) -I$(ROOT)/hal -I. \
           '-DGFX_FB_BYTES=(320u*320u*2u)' -DGFX_MAX_ROWS=320 -DCONFIG_FONT_XL=1
UIDEPS  := $(UIAPP) $(wildcard $(CORE)/ui/*.h $(CORE)/ui/fonts/*.inc sim/ui_sim.h)
TESTS   += uiFlowTest uiSim

uiFlowTest: uiFlowTest.c $(UIDEPS)
	$(CC) $(UIFLAGS) -o $@ uiFlowTest.c $(UIAPP) -lz

uiSim: uiSim.c $(UIDEPS)
	$(CC) $(UIFLAGS) -o $@ uiSim.c $(UIAPP) -lz

ui-gallery: uiSim
	rm -rf $(ROOT)/docs/ux/screens/*/
	./uiSim --gallery $(ROOT)/docs/ux/screens

run: run-ui
run-ui: gfxTest uiFlowTest uiSim
	./gfxTest
	./uiFlowTest
.PHONY: run-ui ui-gallery

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
CALL    := dmrCallTest.c sim/dmr_sim_modem.c $(CORE)/dmr/call/dmr_call.c $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/sfr.c $(DATA) $(FRAME) \
           $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/ambe/ambeFec.c
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

# DMR data end to end: chip model -> slot engine -> call stack -> data service, and its answers back out the engine
TESTS   += dmrDataChainTest
DCHAIN  := dmrDataChainTest.c $(HRCDMR)/hrc6000_dmr.c $(HRCDMR)/hrc6000.c $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(DATA) $(FRAME) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrLrrp.c \
           $(CORE)/dmr/dmr_emergency.c

dmrDataChainTest: $(DCHAIN) $(HRCDMR)/hrc6000_dmr.h $(ROOT)/hal/hal_dmr_modem.h $(wildcard $(CORE)/dmr/call/*.h) \
                  $(CORE)/dmr/dmrDataService.h $(CORE)/dmr/dmr_data_api.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DDMR_RX_TRACE=0 -o $@ $(DCHAIN) -lm

run: run-rxchain
run-rxchain: dmrRxChainTest dmrDataChainTest
	./dmrRxChainTest
	./dmrDataChainTest
.PHONY: run-rxchain

# The MMDVM protocol and the simplex hotspot engine (core/hotspot), against a fake DMR modem and radio core, and the
# host link's rule for leaving hotspot mode (core/hostlink/hostlink_policy.c)
MMDVM   := mmdvmTest.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c $(CORE)/hotspot/hotspotUplink.c \
           $(CORE)/hotspot/hotspotData.c $(FRAME) $(CORE)/dmr/call/dmr_lc.c $(CORE)/hostlink/hostlink_policy.c \
           $(CORE)/radio/morse.c
TESTS   += mmdvmTest

mmdvmTest: $(MMDVM) $(wildcard $(CORE)/hotspot/*.h $(CORE)/hostlink/*.h $(CORE)/dmr/call/dmr_lc.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ $(MMDVM)

run: run-mmdvm
run-mmdvm: mmdvmTest
	./mmdvmTest

# The hotspot's SFR mode (core/hotspot/hotspotSfr.c on core/dmr/sfrRunner.c and sfr.c) through mmdvm.c and hotspot.c,
# against the simulated modem
HSFR    := hotspotSfrTest.c $(CORE)/hotspot/hotspotSfr.c $(CORE)/dmr/sfrRunner.c $(CORE)/dmr/sfr.c \
           $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotData.c \
           $(FRAME) $(CORE)/dmr/call/dmr_lc.c sim/dmr_sim_modem.c
TESTS   += hotspotSfrTest

hotspotSfrTest: $(HSFR) $(wildcard $(CORE)/hotspot/*.h $(CORE)/dmr/sfr*.h sim/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -DCONFIG_SFR=1 -o $@ $(HSFR)

run: run-hsfr
run-hsfr: hotspotSfrTest
	./hotspotSfrTest
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

# DM-32UV drivers and board logic: FD6818 frames on a fake three-wire chip, ST7789 init stream and 2x scaling, key
# matrix decode with the column-to-ground side keys, the HR_C7000 DMR engine on plain register arrays, the factory
# calibration decoder (docs/spec/hardware-dm32.md, modem-hrc7000.md)
DM32    := $(ROOT)/boards/dm32
HRC7    := $(ROOT)/drivers/modem/hrc7000
TESTS   += fd6818Test st7789Test keymatrixTest hrc7000DmrTest dm32CalTest

fd6818Test: fd6818Test.c $(ROOT)/drivers/rf/fd6818/fd6818.c $(ROOT)/drivers/rf/fd6818/fd6818.h
	$(CC) $(CFLAGS) -Wshadow -o $@ fd6818Test.c $(ROOT)/drivers/rf/fd6818/fd6818.c

st7789Test: st7789Test.c $(ROOT)/drivers/display/st7789/st7789.c $(ROOT)/drivers/display/st7789/st7789.h
	$(CC) $(CFLAGS) -Wshadow -o $@ st7789Test.c $(ROOT)/drivers/display/st7789/st7789.c

keymatrixTest: keymatrixTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c $(ROOT)/drivers/input/keymatrix/keymatrix.h
	$(CC) $(CFLAGS) -Wshadow -o $@ keymatrixTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c

hrc7000DmrTest: hrc7000DmrTest.c $(HRC7)/hrc7000_dmr.c $(HRC7)/hrc7000.c $(HRC7)/hrc7000_dmr.h $(HRC7)/hrc7000.h $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ hrc7000DmrTest.c $(HRC7)/hrc7000_dmr.c $(HRC7)/hrc7000.c

# The RDA5807M broadcast receiver driver and the bit-banged I2C master, on a simulated chip (airband-and-broadcast.md 4)
TESTS   += rda5807mTest
RDA     := $(ROOT)/drivers/bcast/rda5807m
I2CG    := $(ROOT)/drivers/bus/i2c_gpio

rda5807mTest: rda5807mTest.c $(RDA)/rda5807m.c $(RDA)/rda5807m.h $(I2CG)/i2c_gpio.c $(I2CG)/i2c_gpio.h
	$(CC) $(CFLAGS) -Wshadow -o $@ rda5807mTest.c $(RDA)/rda5807m.c $(I2CG)/i2c_gpio.c

dm32CalTest: dm32CalTest.c $(DM32)/calibration_dm32.c $(DM32)/dm32.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(DM32) -Wshadow -o $@ dm32CalTest.c $(DM32)/calibration_dm32.c

run: run-dm32
run-dm32: fd6818Test st7789Test keymatrixTest hrc7000DmrTest dm32CalTest rda5807mTest
	./rda5807mTest
	./fd6818Test
	./st7789Test
	./keymatrixTest
	./hrc7000DmrTest
	./dm32CalTest
	python3 -I $(ROOT)/tools/dm32_codec.py --self-test
.PHONY: run-dm32

# The host link's network personality: the IP gateway between a fake USB network interface and fake air ops
HLNET   := hostlinkNetTest.c $(CORE)/hostlink/hostlink_net.c $(CORE)/hostlink/hostlink_net_air.c $(CORE)/dmr/ipGateway.c \
           $(DATA) $(FRAME)
TESTS   += hostlinkNetTest

hostlinkNetTest: $(HLNET) $(CORE)/hostlink/hostlink_net.h $(CORE)/hostlink/hostlink_net_air.h $(CORE)/dmr/ipGateway.h $(ROOT)/hal/hal_netif.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ $(HLNET)

run: run-hlnet
run-hlnet: hostlinkNetTest
	./hostlinkNetTest
.PHONY: run-hlnet

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

# DMR Tier III prototype: the PDU layouts, tables and authentication vectors behind docs/spec/dmr-tier3-plan.md
TESTS   += tier3ProtoTest

tier3ProtoTest: tier3ProtoTest.c $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/call/dmr_lc.h $(CORE)/dmr/fec/dmrFec.c
	$(CC) $(CFLAGS) -o $@ tier3ProtoTest.c $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c

run: run-tier3
run-tier3: tier3ProtoTest
	./tier3ProtoTest
.PHONY: run-tier3

# USB / network audio building blocks (docs/spec/usb-audio.md): USRP framing, G.711 mu-law, the link's PTT watchdogs,
# the PCM jitter buffer between drifting clocks (core/usbaudio, not in the firmware yet)
UAUDIO  := usrpTest.c $(CORE)/usbaudio/usrp.c $(CORE)/usbaudio/pcm_slip.c
TESTS   += usrpTest

usrpTest: $(UAUDIO) $(wildcard $(CORE)/usbaudio/*.h)
	$(CC) $(CFLAGS) -Wshadow -o $@ $(UAUDIO) -lm

run: run-usbaudio
run-usbaudio: usrpTest
	./usrpTest
.PHONY: run-usbaudio
