# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## Host unit tests for the hardware independent parts of the firmware
##
##   make -C tests/host
##   make -C tests/host COVERAGE=1     instrumented for line coverage (tools/coverage.sh makes the report)
##   make -C tests/host SANITIZE=1     under AddressSanitizer and UndefinedBehaviorSanitizer (any report fails the test)

ROOT    := ../..
CORE    := $(ROOT)/core
CC      ?= cc
# COVERAGE=1: gcov instrumentation (GCC and clang both take --coverage); each test writes its .gcda files here
COVFLAGS := $(if $(filter 1,$(COVERAGE)),--coverage -O0)
# SANITIZE=1: ASan + UBSan (Apple clang and GCC); UBSan stops at the first report instead of printing and going on.
# Build from clean when switching (make clean): the binaries don't depend on the flags. Not optimized, like the plain
# build: with __OPTIMIZE__ glibc's headers use extern inline and fortified functions, which don't build with
# __attribute__ defined away below.
SANITIZERS := address,undefined
SANFLAGS := $(if $(filter 1,$(SANITIZE)),-fsanitize=$(SANITIZERS) -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
export ASAN_OPTIONS ?= abort_on_error=0:detect_stack_use_after_return=1:strict_string_checks=1
export UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1
CFLAGS  := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -g $(COVFLAGS) $(SANFLAGS) -I$(CORE) -I$(ROOT)/drivers '-D__attribute__(x)='  # the firmware's linker section attributes are ARM ELF only

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
SVC     := dmrDataServiceTest.c $(DATA) $(FRAME) $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c $(CORE)/dmr/call/dmr_lc.c

dmrDataServiceTest: $(SVC) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrDataService.h $(CORE)/dmr/dmr_data_api.h
	$(CC) $(CFLAGS) -o $@ $(SVC) -lm

# The same without Motorola TMS format support (CONFIG_DMR_TMS=0, docs/spec/dmr-data.md 5.1)
TESTS   += dmrDataServiceTestNoTms
dmrDataServiceTestNoTms: $(SVC) $(CORE)/dmr/dmrDataService.c $(CORE)/dmr/dmrDataService.h $(CORE)/dmr/dmr_data_api.h
	$(CC) $(CFLAGS) -DCONFIG_DMR_TMS=0 -o $@ $(SVC) -lm

# LRRP on its own: request decoding (malformed and mutated too), the answers' bounds, the distance
TESTS   += lrrpTest
lrrpTest: lrrpTest.c fuzz.h $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmrLrrp.h
	$(CC) $(CFLAGS) -Wshadow -o $@ lrrpTest.c $(CORE)/dmr/dmrLrrp.c -lm

dmrPrivacyTest: dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/dmrPrivacy.h $(CORE)/dmr/ambe/ambeFec.c
	$(CC) $(CFLAGS) -o $@ dmrPrivacyTest.c $(CORE)/dmr/dmrPrivacy.c $(CORE)/dmr/ambe/ambeFec.c

# The single frequency repeater's generic logic (with BPTC for the idle message)
SFRSRC  := sfrTest.c $(CORE)/dmr/sfr.c $(DATA) $(FRAME)

sfrTest: $(SFRSRC) $(CORE)/dmr/sfr.h
	$(CC) $(CFLAGS) -DHAS_SFR -o $@ $(SFRSRC)

# APRS protocol layers (AX.25, HDLC, AFSK, KISS, formats, NMEA, beacon triggers, iGate rules)
TESTS   += aprsTest
APRSLIB := $(CORE)/aprs/ax25.c $(CORE)/aprs/hdlc.c $(CORE)/aprs/afsk.c $(CORE)/aprs/kiss.c $(CORE)/aprs/aprs_fmt.c \
           $(CORE)/aprs/aprs_nmea.c $(CORE)/aprs/aprs_beacon.c $(CORE)/aprs/aprs_igate.c

aprsTest: aprsTest.c $(APRSLIB) $(wildcard $(CORE)/aprs/*.h)
	$(CC) $(CFLAGS) -o $@ aprsTest.c $(APRSLIB) -lm

run: $(TESTS)
	./dmrFecTest
	./dmrDataTest > dmrDataTest.out; status=$$?; grep -E "^(FAIL|# (PASSED|FAILED))" dmrDataTest.out; exit $$status
	./dmrDataServiceTest
	./dmrDataServiceTestNoTms
	./hotspotDataTest
	./hotspotUplinkTest
	./dmrPrivacyTest
	./lrrpTest
	./ipGatewayTest
	./ipGatewayTest16
	./ipGatewayTest31
	./ipGatewayTestShared
	./sfrTest
	./aprsTest
ifneq ($(TSHARK),)
	@# Independent check of the frames the gateway sent: no malformed packets or bad checksums
	@for f in ipGatewayTest.pcap ipGatewayTest16.pcap ipGatewayTest31.pcap ipGatewayTestShared.pcap; do "$(TSHARK)" -r $$f -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE -z expert,note -q; done > ipGatewayTest.expert
	@if grep -q -i -E "malformed|bad|incorrect|error" ipGatewayTest.expert; then echo "# tshark found problems"; cat ipGatewayTest.expert; exit 1; else echo "# tshark: no malformed packets or bad checksums"; fi
endif

clean:
	rm -f mdnsTest.pcap tftpTest.pcap netTest.expert
	rm -rf gen
	rm -f $(TESTS) dmrDataTest.out ipGatewayTest.pcap ipGatewayTest16.pcap ipGatewayTest31.pcap ipGatewayTestShared.pcap ipGatewayTest.expert
	rm -rf *.dSYM out
	rm -f *.gcda *.gcno

.PHONY: all run clean

# The radio core's state machine and the input gesture engine, against a fake board and codeplug (no RTOS)
CORE_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += radioCoreTest inputTest quadratureTest serialmicTest

# With the DMR call stack, the AMBE voice glue and the simulated modem behind board.dmr
RFEAT   := $(CORE)/radio/scan.c $(CORE)/radio/beep.c $(CORE)/radio/battery.c $(CORE)/radio/roam.c $(CORE)/radio/radio_am.c \
           $(CORE)/radio/vibe.c $(CORE)/radio/mic_gain.c $(CORE)/codeplug/codeplug_talk.c
RCORE   := radioCoreTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(RFEAT) $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c \
           $(CORE)/dmr/dmrPrivacy.c sim/dmr_sim_modem.c $(CORE)/dmr/dmrData.c $(CORE)/dmr/dmrDataService.c \
           $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c $(CORE)/radio/morse.c \
           $(CORE)/settings/settings.c $(CORE)/storage/storage.c \
           $(CORE)/radio/radio_hotspot.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c \
           $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotData.c $(CORE)/hotspot/dmrDataFrame.c \
           $(CORE)/hotspot/hotspotSfr.c $(CORE)/dmr/sfrRunner.c $(CORE)/dmr/sfr.c \
           $(CORE)/ui/ui_svc_radio.c $(CORE)/ui/ui_svc_storage.c $(CORE)/ui/ui_svc_data.c \
           $(CORE)/ui/ui_svc_emergency.c $(CORE)/input/keyassign.c sim/sim_flash.c \
           $(CORE)/debuglink/debuglink_view_radio.c $(CORE)/debuglink/dl_fmt.c

radioCoreTest: $(RCORE) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h) $(CORE)/dmr/dmrDataService.h \
               $(CORE)/ui/ui_svc.h
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_SFR=1 -DCONFIG_APRS=0 -o $@ $(RCORE) -lm

# The same with the DMR VFO off (CONFIG_VFO_DMR=0): the VFO stays analog and nothing about it changes
TESTS   += radioCoreTestNoVfoDmr
radioCoreTestNoVfoDmr: $(RCORE) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h) $(CORE)/dmr/dmrDataService.h \
                       $(CORE)/ui/ui_svc.h
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_SFR=1 -DCONFIG_APRS=0 -DCONFIG_VFO_DMR=0 -o $@ $(RCORE) -lm

# The everyday features (scan, beeps, battery, TOT warning, BCL, roger beep, 1750 Hz burst, VOX, battery saving)
TESTS   += radioFeaturesTest
RFTEST  := radioFeaturesTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(RFEAT) $(CORE)/dmr/call/dmr_call.c \
           $(CORE)/dmr/call/dmr_lc.c $(CORE)/dmr/fec/dmrFec.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c \
           $(CORE)/dmr/dmrPrivacy.c sim/dmr_sim_modem.c $(CORE)/dmr/dmrData.c $(CORE)/dmr/dmrDataService.c \
           $(CORE)/dmr/dmrLrrp.c $(CORE)/dmr/dmr_emergency.c $(CORE)/radio/morse.c \
           $(CORE)/settings/settings.c $(CORE)/storage/storage.c $(CORE)/usbaudio/audio_mux.c \
           $(CORE)/usbaudio/pcm_slip.c

# CONFIG_NET_USRP without the gateway: only the radio core's half (host audio PTT, the shared stream) is tested here;
# the network half is usrpNetTest
radioFeaturesTest: $(RFTEST) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h) $(wildcard $(CORE)/usbaudio/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -DCONFIG_APRS=0 \
		-DCONFIG_NET_USRP=1 -o $@ $(RFTEST) -lm

# The volume knob power switch read from the battery sense line (DM-1701, MD-380)
TESTS   += powerSwitchTest

powerSwitchTest: powerSwitchTest.c $(CORE)/radio/power_switch.c $(CORE)/radio/power_switch.h
	$(CC) $(CFLAGS) -o $@ powerSwitchTest.c $(CORE)/radio/power_switch.c

# DMR repeater roaming: the engine, and through the radio core against fake repeaters on the simulated modem
TESTS   += roamTest
ROAMT   := roamTest.c $(filter-out radioFeaturesTest.c,$(RFTEST))

roamTest: $(ROAMT) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -DCONFIG_APRS=0 -o $@ \
		$(ROAMT) -lm

# APRS through the radio core, FM only (the engine, radio_sm.c's APRS block, a fake AFSK transmitter)
TESTS   += aprsRadioTest
FMONLY  := -DCONFIG_DMR=0 -DCONFIG_DMR_PRIVACY=0 -DCONFIG_DMR_TALKER_ALIAS=0 -DCONFIG_DMR_SMS=0 -DCONFIG_DMR_ARS=0 \
           -DCONFIG_DMR_LRRP=0 -DCONFIG_DMR_SIGNALING=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_SFR=0 -DCONFIG_HOTSPOT=0
APRSRT  := aprsRadioTest.c $(CORE)/radio/radio_sm.c $(CORE)/radio/tones.c $(CORE)/radio/scan.c $(CORE)/radio/beep.c \
           $(CORE)/radio/vibe.c $(CORE)/codeplug/codeplug_talk.c \
           $(CORE)/radio/battery.c $(CORE)/radio/morse.c $(CORE)/settings/settings.c $(CORE)/storage/storage.c \
           $(CORE)/radio/radio_am.c $(CORE)/aprs/aprs.c $(CORE)/aprs/aprs_dmr.c $(CORE)/aprs/aprs_host.c $(APRSLIB) \
           $(CORE)/usbaudio/audio_mux.c $(CORE)/usbaudio/pcm_slip.c

aprsRadioTest: $(APRSRT) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/aprs/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. $(FMONLY) -o $@ $(APRSRT) -lm

# Air band AM receive through the radio core (docs/spec/airband-and-broadcast.md 3), a fake board with CAP_AM_RX
TESTS   += radioAmTest
AMTEST  := radioAmTest.c $(filter-out radioFeaturesTest.c,$(RFTEST))

radioAmTest: $(AMTEST) $(wildcard $(CORE)/radio/*.h) $(wildcard $(CORE)/dmr/call/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -I. -DDMR_RX_TRACE=0 -DCONFIG_DMR_IP_GATEWAY=0 -DCONFIG_HOTSPOT=0 -DCONFIG_APRS=0 -o $@ $(AMTEST) -lm

# The broadcast FM receiver's behavior (core/bcast, docs/spec/airband-and-broadcast.md 4) on a fake receiver
TESTS   += bcastTest

bcastTest: bcastTest.c $(CORE)/bcast/bcast.c $(CORE)/bcast/bcast.h $(ROOT)/hal/hal_bcast.h
	$(CC) $(CFLAGS) $(CORE_HAL) -o $@ bcastTest.c $(CORE)/bcast/bcast.c

inputTest: inputTest.c $(CORE)/input/input_engine.c $(CORE)/input/input_engine.h $(CORE)/input/input.h
	$(CC) $(CFLAGS) $(CORE_HAL) -o $@ inputTest.c $(CORE)/input/input_engine.c

# Side key assignment (core/input/keyassign.c) and its key map in the gesture engine
TESTS   += keyassignTest
keyassignTest: keyassignTest.c $(CORE)/input/keyassign.c $(CORE)/input/keyassign.h $(CORE)/input/input_engine.c
	$(CC) $(CFLAGS) $(CORE_HAL) -o $@ keyassignTest.c $(CORE)/input/keyassign.c $(CORE)/input/input_engine.c

# The rotary encoder's quadrature decoder (drivers/input/quadrature), shared by the boards
quadratureTest: quadratureTest.c $(ROOT)/drivers/input/quadrature/quadrature.c $(ROOT)/drivers/input/quadrature/quadrature.h
	$(CC) $(CFLAGS) -I$(ROOT)/drivers -Wshadow -o $@ quadratureTest.c $(ROOT)/drivers/input/quadrature/quadrature.c

# The hand mic key decoder on a UART shared with the PC link (drivers/input/serialmic; the DB25-D)
serialmicTest: serialmicTest.c $(ROOT)/drivers/input/serialmic/serialmic.c $(ROOT)/drivers/input/serialmic/serialmic.h
	$(CC) $(CFLAGS) -I$(ROOT)/drivers -Wshadow -o $@ serialmicTest.c $(ROOT)/drivers/input/serialmic/serialmic.c

# The VOX mic level (drivers/audio/miclevel): the boards' analog-tap scale and the level from PCM blocks
TESTS   += miclevelTest
miclevelTest: miclevelTest.c $(ROOT)/drivers/audio/miclevel/miclevel.h
	$(CC) $(CFLAGS) -Wshadow -o $@ miclevelTest.c -lm

run: run-core
run-core: radioCoreTest radioCoreTestNoVfoDmr radioFeaturesTest roamTest radioAmTest bcastTest inputTest aprsRadioTest quadratureTest serialmicTest miclevelTest
	./aprsRadioTest
	./radioCoreTest
	./radioCoreTestNoVfoDmr
	./radioFeaturesTest
	./roamTest
	./radioAmTest
	./bcastTest
	./inputTest
	./quadratureTest
	./serialmicTest
	./miclevelTest
.PHONY: run-core

# The read-only codeplug (open-firmware layout and built-in channels) and the DM-1701 calibration reader, on fake flash images
FLASH_HAL := -I$(ROOT)/hal -Wshadow
TESTS   += codeplugTest calibrationTest

codeplugTest: codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/codeplug.h $(CORE)/codeplug/dmr_iddb.c $(CORE)/codeplug/dmr_iddb.h $(CORE)/storage/storage.c
	$(CC) $(CFLAGS) $(FLASH_HAL) -o $@ codeplugTest.c $(CORE)/codeplug/codeplug.c $(CORE)/codeplug/dmr_iddb.c $(CORE)/storage/storage.c

calibrationTest: calibrationTest.c $(ROOT)/boards/dm1701/calibration_dm1701.c $(ROOT)/boards/dm1701/dm1701.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) $(FLASH_HAL) -I$(ROOT)/boards/dm1701 -o $@ calibrationTest.c $(ROOT)/boards/dm1701/calibration_dm1701.c

# The calibration curve every board's reader shares
TESTS   += calCurveTest

calCurveTest: calCurveTest.c $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -Wshadow -o $@ calCurveTest.c

run: run-flash
run-flash: codeplugTest calibrationTest calCurveTest
	./codeplugTest
	./calibrationTest
	./calCurveTest
.PHONY: run-flash

# The AT1846S driver: frequency word, register cache and write sequences, on a fake bus
TESTS   += at1846sTest subaudioTest

SUBAUDIO := $(ROOT)/drivers/rf/subaudio/subaudio.c

at1846sTest: at1846sTest.c $(ROOT)/drivers/rf/at1846s/at1846s.c $(ROOT)/drivers/rf/at1846s/at1846s.h $(SUBAUDIO)
	$(CC) $(CFLAGS) -Wshadow -o $@ at1846sTest.c $(ROOT)/drivers/rf/at1846s/at1846s.c $(SUBAUDIO)

# The sub-audio math the RF drivers share: the DCS Golay word
subaudioTest: subaudioTest.c $(SUBAUDIO) $(ROOT)/drivers/rf/subaudio/subaudio.h
	$(CC) $(CFLAGS) -Wshadow -o $@ subaudioTest.c $(SUBAUDIO)

run: run-rf
run-rf: at1846sTest subaudioTest
	./at1846sTest
	./subaudioTest
.PHONY: run-rf

# The MD-380's chips (docs/spec/hardware-md380.md): the SKY72310 synthesizer's divider words against the data sheet
# and a register model, the HR_C5000 variant of the HR_C6000 driver, and the GPS NMEA line framing and receive ring the
# MD-380, DM-32UV and DB25-D GPS drivers share
TESTS   += sky72310Test hrc5000Test nmeaLineTest gpsRxTest md380RfbusTest

sky72310Test: sky72310Test.c $(ROOT)/drivers/rf/sky72310/sky72310.c $(ROOT)/drivers/rf/sky72310/sky72310.h
	$(CC) $(CFLAGS) -Wshadow -o $@ sky72310Test.c $(ROOT)/drivers/rf/sky72310/sky72310.c

hrc5000Test: hrc5000Test.c $(ROOT)/drivers/modem/hrc6000/hrc6000_dmr.c $(ROOT)/drivers/modem/hrc6000/hrc6000.c \
             $(wildcard $(ROOT)/drivers/modem/hrc6000/*.h) $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ hrc5000Test.c $(ROOT)/drivers/modem/hrc6000/hrc6000_dmr.c \
		$(ROOT)/drivers/modem/hrc6000/hrc6000.c

nmeaLineTest: nmeaLineTest.c $(ROOT)/drivers/gps/nmea_line/nmea_line.c $(ROOT)/drivers/gps/nmea_line/nmea_line.h
	$(CC) $(CFLAGS) -Wshadow -o $@ nmeaLineTest.c $(ROOT)/drivers/gps/nmea_line/nmea_line.c

gpsRxTest: gpsRxTest.c $(ROOT)/drivers/gps/gps_rx/gps_rx.c $(ROOT)/drivers/gps/gps_rx/gps_rx.h \
           $(ROOT)/drivers/gps/nmea_line/nmea_line.c $(ROOT)/drivers/gps/nmea_line/nmea_line.h
	$(CC) $(CFLAGS) -Wshadow -o $@ gpsRxTest.c $(ROOT)/drivers/gps/gps_rx/gps_rx.c \
		$(ROOT)/drivers/gps/nmea_line/nmea_line.c

# The MD-380's bit-banged RF bus (boards/md380/rfbus_md380.c, with the real pins.h) against fake chips on the documented
# wiring; md380_stub/ holds the stand-in platform.h and FreeRTOS.h the board code includes
md380RfbusTest: md380RfbusTest.c $(ROOT)/boards/md380/rfbus_md380.c $(ROOT)/boards/md380/rfbus_md380.h \
                $(ROOT)/boards/md380/pins.h $(wildcard md380_stub/*.h) $(ROOT)/drivers/modem/hrc6000/hrc6000.c \
                $(wildcard $(ROOT)/drivers/modem/hrc6000/*.h) $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -Imd380_stub -I$(ROOT)/boards/md380 -I$(ROOT)/hal -Wshadow -o $@ md380RfbusTest.c \
		$(ROOT)/boards/md380/rfbus_md380.c $(ROOT)/drivers/modem/hrc6000/hrc6000.c

run: run-md380
run-md380: sky72310Test hrc5000Test nmeaLineTest gpsRxTest md380RfbusTest
	./sky72310Test
	./hrc5000Test
	./nmeaLineTest
	./gpsRxTest
	./md380RfbusTest
.PHONY: run-md380

# UI drawing: text helpers, changed-row flushing, Home and quick settings at each display class (out/*.ppm to look at)
UI      := $(CORE)/ui/gfx.c $(CORE)/ui/fonts.c $(CORE)/ui/fmt.c $(CORE)/ui/ui_screens.c $(CORE)/ui/ui_svc_stub.c
TESTS   += gfxTest

gfxTest: gfxTest.c $(UI) $(wildcard $(CORE)/ui/*.h $(CORE)/ui/fonts/*.inc)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DGFX_INDEXED=1 -o $@ gfxTest.c $(UI)

# The UI host simulator (tests/host/sim/ui_sim.c): the whole of core/ui against a simulated radio, codeplug and
# board at any display geometry and key map. uiFlowTest runs the task flows of docs/ux/task-flows.md against their
# press budgets, crawls every screen (BACK leaves it, long BACK reaches Home, the status strip is drawn) and checks
# the long-press rules; uiSim renders screens to PNG (make ui-gallery: docs/ux/screens/, contact sheets in
# docs/ux/screens/sheets/). Built without the __attribute__ override above so the weak service stubs
# (ui_svc_stub.c) give way to the simulator's.
UIAPP   := $(UI) $(CORE)/ui/ui_app.c $(CORE)/ui/ui_menus.c $(CORE)/ui/ui_find.c $(CORE)/ui/ui_aprs.c $(CORE)/radio/tones.c \
           $(CORE)/codeplug/codeplug_talk.c \
           sim/ui_sim.c sim/ui_png.c
# The About page's data header, as the firmware build makes it (tools/gen_about.py; the STM32F4 list stands in)
ABOUT_H := out/gen/odr_about.h
$(shell python3 $(ROOT)/tools/gen_about.py $(ROOT)/AUTHORS $(ROOT)/platform/stm32f4/THIRD_PARTY $(ABOUT_H) >/dev/null)
UIFLAGS := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wshadow -g $(COVFLAGS) $(SANFLAGS) -I$(CORE) -I$(ROOT)/hal -I. -Iout/gen \
           '-DGFX_FB_BYTES=(320u*320u*2u)' -DGFX_MAX_ROWS=320 -DCONFIG_FONT_XL=1 -DFONT_BOTH -DGFX_INDEXED=1 -DGFX_FIT_TRACE
UIDEPS  := $(UIAPP) $(ABOUT_H) $(wildcard $(CORE)/ui/*.h $(CORE)/ui/fonts/*.inc sim/ui_sim.h)
TESTS   += uiFlowTest uiFlowTestNoColor uiSim

uiFlowTest: uiFlowTest.c $(UIDEPS)
	$(CC) $(UIFLAGS) -o $@ uiFlowTest.c $(UIAPP) -lz

# The same flows built as a radio with only a monochrome screen builds them (CONFIG_UI_COLOR=n): no color rows
uiFlowTestNoColor: uiFlowTest.c $(UIDEPS)
	$(CC) $(UIFLAGS) -DCONFIG_UI_COLOR=0 -o $@ uiFlowTest.c $(UIAPP) -lz

uiSim: uiSim.c $(UIDEPS)
	$(CC) $(UIFLAGS) -o $@ uiSim.c $(UIAPP) -lz

# The highlight colors the gallery shows its color pages in: the list in .github/workflows/ui-gallery.yml, so the
# committed gallery and a pull request's before / after pictures show the same colors
UI_GALLERY_COLORS ?= $(shell sed -n 's/^ *UI_GALLERY_COLORS: *"\([^"]*\)".*/\1/p' $(ROOT)/.github/workflows/ui-gallery.yml)

ui-gallery: uiSim
	rm -rf $(ROOT)/docs/ux/screens/*/
	UI_GALLERY_COLORS="$(UI_GALLERY_COLORS)" ./uiSim --gallery $(ROOT)/docs/ux/screens

# Contact sheets only (every screen of a display class on one page, to audit layouts), outside docs/: GEOM=128x64
ui-sheets: uiSim
	UI_GALLERY_COLORS="$(UI_GALLERY_COLORS)" ./uiSim --sheets out/sheets $(GEOM)

run: run-ui
run-ui: gfxTest uiFlowTest uiFlowTestNoColor uiSim
	./gfxTest
	./uiFlowTest
	./uiFlowTestNoColor
	./uiSim --layout
.PHONY: run-ui ui-gallery ui-sheets

# AMBE+2 3600x2450 air frame FEC and interleaving (core/dmr/ambe), with vectors from the stock DM-1701 codec's FEC
# and the DMR voice glue between the call stack, the FEC, the vocoder and the audio driver (fakes)
TESTS   += ambeFecTest ambeVoiceTest

ambeFecTest: ambeFecTest.c $(CORE)/dmr/ambe/ambeFec.c $(CORE)/dmr/ambe/ambeFec.h
	$(CC) $(CFLAGS) -Wshadow -o $@ ambeFecTest.c $(CORE)/dmr/ambe/ambeFec.c

ambeVoiceTest: ambeVoiceTest.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeVoice.h $(CORE)/dmr/ambe/ambeFec.c
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ ambeVoiceTest.c $(CORE)/dmr/ambe/ambeVoice.c $(CORE)/dmr/ambe/ambeFec.c

# The mic gain setting (core/radio/mic_gain.c): to the board's hook or to DMR's sample scaling, never both
TESTS   += micGainTest
micGainTest: micGainTest.c $(CORE)/radio/mic_gain.c $(CORE)/radio/mic_gain.h $(ROOT)/hal/hal_radio.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ micGainTest.c $(CORE)/radio/mic_gain.c

run: run-ambe
run-ambe: ambeFecTest ambeVoiceTest micGainTest
	./ambeFecTest
	./ambeVoiceTest
	./micGainTest
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

# The split duplex extension (CONFIG_HOTSPOT_SPLIT, docs/spec/split-duplex.md): the CACH / short LC coding against
# the standard's tables, the continuous outbound, the RX half's labels, the roles through mmdvm.c and hotspot.c on the
# simulated split modem; and the standard-host suites again with the extension built in and registered (nothing a
# host that never sends 0xD6 can see changes but the version suffix)
SPLITF  := -DCONFIG_HOTSPOT_SPLIT=1
SPLIT   := $(CORE)/hotspot/hotspotSplit.c $(CORE)/dmr/bsOutbound.c $(CORE)/dmr/slotEpoch.c $(CORE)/dmr/fec/dmrCach.c
SPLITT  := splitDuplexTest.c $(SPLIT) sim/split_sim_modem.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c \
           $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotData.c $(FRAME) $(CORE)/dmr/call/dmr_lc.c \
           $(CORE)/debuglink/debuglink_split.c $(CORE)/debuglink/dl_fmt.c
TESTS   += splitDuplexTest mmdvmTestSplit hotspotSfrTestSplit hrc6000BsTest hrc6000DmrTestSplit
SPLITH  := $(wildcard $(CORE)/hotspot/*.h $(CORE)/dmr/bsOutbound.h $(CORE)/dmr/slotEpoch.h $(CORE)/dmr/fec/*.h sim/*.h) \
           $(ROOT)/hal/hal_dmr_modem.h

splitDuplexTest: $(SPLITT) $(SPLITH)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow $(SPLITF) -o $@ $(SPLITT)

mmdvmTestSplit: $(MMDVM) $(SPLIT) $(SPLITH)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow $(SPLITF) -o $@ $(MMDVM) $(SPLIT)

hotspotSfrTestSplit: $(HSFR) $(SPLIT) $(SPLITH)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -DCONFIG_SFR=1 $(SPLITF) -o $@ $(HSFR) $(SPLIT)

# The simulated pair (tests/host/splitPairTest.py): one radio per splitSim process, two in lockstep virtual time with
# tools/split_duplex_host.py as the host; run when python3 is there
SPLITSIM := sim/splitSim.c $(SPLIT) sim/split_sim_modem.c $(CORE)/hotspot/mmdvm.c $(CORE)/hotspot/hotspot.c \
            $(CORE)/hotspot/hotspotUplink.c $(CORE)/hotspot/hotspotData.c $(FRAME) $(CORE)/dmr/call/dmr_lc.c
TESTS   += splitSim
splitSim: $(SPLITSIM) $(SPLITH)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Isim -Wshadow $(SPLITF) -o $@ $(SPLITSIM)

# The HR_C6000 BS outbound (drivers/modem/hrc6000/hrc6000_bs.c), and the slot engine's suite again with its hooks built in
HRCBS   := $(HRCDMR)/hrc6000_bs.c $(HRCDMR)/hrc6000_dmr.c $(HRCDMR)/hrc6000.c
hrc6000BsTest: hrc6000BsTest.c $(HRCBS) $(wildcard $(HRCDMR)/*.h) $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow $(SPLITF) -o $@ hrc6000BsTest.c $(HRCBS)

hrc6000DmrTestSplit: hrc6000DmrTest.c $(HRCBS) $(wildcard $(HRCDMR)/*.h) $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow $(SPLITF) -o $@ hrc6000DmrTest.c $(HRCBS)

run: run-split
run-split: splitDuplexTest mmdvmTestSplit hotspotSfrTestSplit hrc6000BsTest hrc6000DmrTestSplit splitSim
	./splitDuplexTest
	./mmdvmTestSplit
	./hotspotSfrTestSplit
	./hrc6000BsTest
	./hrc6000DmrTestSplit
	@if command -v python3 > /dev/null; then python3 sim/splitPairTest.py ./splitSim && \
		python3 $(ROOT)/tools/bench/dmr_cach.py selftest; \
	else echo "# splitPairTest, dmr_cach selftest: skipped (no python3)"; fi
.PHONY: run-split

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

# The runtime RAM-monitor loader's portable state machine (core/debuglink/dl_stub.c, CONFIG_STUB_ENTRY)
TESTS   += stubEntryTest
stubEntryTest: stubEntryTest.c $(CORE)/debuglink/dl_stub.c $(CORE)/debuglink/dl_stub.h $(CORE)/debuglink/debuglink_proto.h
	$(CC) $(CFLAGS) -Wshadow -o $@ stubEntryTest.c $(CORE)/debuglink/dl_stub.c

run: run-stubEntry
run-stubEntry: stubEntryTest
	./stubEntryTest
.PHONY: run-stubEntry

# The debug link's tasks view section: each task's least free stack (core/debuglink/debuglink_view_tasks.c; the
# kernel's task table is faked)
TSTACK  := taskStackTest.c $(CORE)/debuglink/debuglink_view_tasks.c $(CORE)/debuglink/dl_fmt.c
TESTS   += taskStackTest
taskStackTest: $(TSTACK) $(CORE)/debuglink/debuglink.h $(CORE)/app/task_stack.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DCONFIG_TASK_STACKS=1 -o $@ $(TSTACK)

run: run-taskStack
run-taskStack: taskStackTest
	./taskStackTest
.PHONY: run-taskStack

# The STM32F4 flash stub's protocol engine (stub/stm32f4/f4stub_proto.c, docs/spec/stub-stm32f4.md)
TESTS   += f4stubTest
f4stubTest: f4stubTest.c $(ROOT)/stub/stm32f4/f4stub_proto.c $(ROOT)/stub/stm32f4/f4stub.h
	$(CC) $(CFLAGS) -Wshadow -I$(ROOT)/stub/stm32f4 -o $@ f4stubTest.c $(ROOT)/stub/stm32f4/f4stub_proto.c

TESTS   += f4stubSim
f4stubSim: f4stubSim.c $(ROOT)/stub/stm32f4/f4stub_proto.c $(ROOT)/stub/stm32f4/f4stub.h
	$(CC) $(CFLAGS) -Wshadow -I$(ROOT)/stub/stm32f4 -o $@ f4stubSim.c $(ROOT)/stub/stm32f4/f4stub_proto.c

run: run-f4stub
run-f4stub: f4stubTest f4stubSim
	./f4stubTest
	python3 f4stub_wire_check.py ./f4stubSim
.PHONY: run-f4stub

# DM-32UV drivers and board logic: FD6818 frames on a fake three-wire chip, ST7789 init stream and 2x scaling, key
# matrix decode with the column-to-ground side keys, the HR_C7000 DMR engine on plain register arrays, the factory
# calibration decoder (docs/spec/hardware-dm32.md, modem-hrc7000.md)
DM32    := $(ROOT)/boards/dm32
HRC7    := $(ROOT)/drivers/modem/hrc7000
TESTS   += fd6818Test st7567Test st7789Test keymatrixTest sharedPttTest hrc7000DmrTest dm32CalTest

fd6818Test: fd6818Test.c $(ROOT)/drivers/rf/fd6818/fd6818.c $(ROOT)/drivers/rf/fd6818/fd6818_dm1801pro.c $(ROOT)/drivers/rf/fd6818/fd6818.h $(SUBAUDIO)
	$(CC) $(CFLAGS) -Wshadow -o $@ fd6818Test.c $(ROOT)/drivers/rf/fd6818/fd6818.c $(ROOT)/drivers/rf/fd6818/fd6818_dm1801pro.c $(SUBAUDIO)

st7567Test: st7567Test.c $(ROOT)/drivers/display/st7567/st7567.c $(ROOT)/drivers/display/st7567/st7567.h
	$(CC) $(CFLAGS) -Wshadow -o $@ st7567Test.c $(ROOT)/drivers/display/st7567/st7567.c

st7789Test: st7789Test.c $(ROOT)/drivers/display/st7789/st7789.c $(ROOT)/drivers/display/st7789/st7789.h
	$(CC) $(CFLAGS) -Wshadow -o $@ st7789Test.c $(ROOT)/drivers/display/st7789/st7789.c

sharedPttTest: sharedPttTest.c $(ROOT)/drivers/input/sharedptt/shared_ptt.c $(ROOT)/drivers/input/sharedptt/shared_ptt.h
	$(CC) $(CFLAGS) -Wshadow -o $@ sharedPttTest.c $(ROOT)/drivers/input/sharedptt/shared_ptt.c
keymatrixTest: keymatrixTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c $(ROOT)/drivers/input/keymatrix/keymatrix.h
	$(CC) $(CFLAGS) -Wshadow -o $@ keymatrixTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c

hrc7000DmrTest: hrc7000DmrTest.c $(HRC7)/hrc7000_dmr.c $(HRC7)/hrc7000.c $(HRC7)/hrc7000_dmr.h $(HRC7)/hrc7000.h $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ hrc7000DmrTest.c $(HRC7)/hrc7000_dmr.c $(HRC7)/hrc7000.c

# The RDA5807M broadcast receiver driver and the bit-banged I2C master, on a simulated chip (airband-and-broadcast.md 4)
TESTS   += rda5807mTest
RDA     := $(ROOT)/drivers/bcast/rda5807m
I2CG    := $(ROOT)/drivers/bus/i2c_gpio

rda5807mTest: rda5807mTest.c $(RDA)/rda5807m.c $(RDA)/rda5807m.h $(RDA)/rda5807m_hal.c $(RDA)/rda5807m_hal.h \
		$(I2CG)/i2c_gpio.c $(I2CG)/i2c_gpio.h $(ROOT)/hal/hal_bcast.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ rda5807mTest.c $(RDA)/rda5807m.c $(RDA)/rda5807m_hal.c $(I2CG)/i2c_gpio.c

# The LIS3DH accelerometer driver on a register model
TESTS   += lis3dhTest
lis3dhTest: lis3dhTest.c $(ROOT)/drivers/sensor/lis3dh/lis3dh.c $(ROOT)/drivers/sensor/lis3dh/lis3dh.h
	$(CC) $(CFLAGS) -Wshadow -o $@ lis3dhTest.c $(ROOT)/drivers/sensor/lis3dh/lis3dh.c

dm32CalTest: dm32CalTest.c $(ROOT)/drivers/cal/baofeng/baofeng_cal.c $(ROOT)/drivers/cal/baofeng/baofeng_cal.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ dm32CalTest.c $(ROOT)/drivers/cal/baofeng/baofeng_cal.c

# The DM-1801 Pro's start-up pin levels (hardware-dm1801pro.md 3.1)
TESTS   += dm1801proPinsTest
dm1801proPinsTest: dm1801proPinsTest.c $(ROOT)/boards/dm1801pro/boot_pins.h $(ROOT)/boards/dm1801pro/pins.h
	$(CC) $(CFLAGS) -I$(ROOT)/boards/dm1801pro -Wshadow -o $@ dm1801proPinsTest.c

run: run-dm32
run-dm32: fd6818Test st7567Test st7789Test keymatrixTest sharedPttTest hrc7000DmrTest dm32CalTest rda5807mTest lis3dhTest \
          dm1801proPinsTest
	./dm1801proPinsTest
	./lis3dhTest
	./rda5807mTest
	./fd6818Test
	./st7567Test
	./st7789Test
	./keymatrixTest
	./sharedPttTest
	./hrc7000DmrTest
	./dm32CalTest
	python3 -I $(ROOT)/tools/dm32_codec.py --self-test
.PHONY: run-dm32

# The DB25-D PRO's BK4819 driver: main and LO chips on one shared three-wire bus (docs/spec/hardware-db25d.md 4)
TESTS   += bk4819Test

bk4819Test: bk4819Test.c $(ROOT)/drivers/rf/bk4819/bk4819.c $(ROOT)/drivers/rf/bk4819/bk4819.h
	$(CC) $(CFLAGS) -Wshadow -o $@ bk4819Test.c $(ROOT)/drivers/rf/bk4819/bk4819.c

# Its factory calibration page (hardware-db25d.md 6)
TESTS   += db25dCalTest
DB25D   := $(ROOT)/boards/db25d

db25dCalTest: db25dCalTest.c $(DB25D)/calibration_db25d.c $(DB25D)/db25d.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(DB25D) -Wshadow -o $@ db25dCalTest.c $(DB25D)/calibration_db25d.c

# Its first stage in front of the application (boot/db25d): the stub load, dropped frames, the window, a cold UART
TESTS   += bootDb25dTest

bootDb25dTest: bootDb25dTest.c $(ROOT)/boot/db25d/boot.c
	$(CC) $(CFLAGS) -Wshadow -o $@ bootDb25dTest.c

# Its clock arithmetic (platform/hrc7000/clock_math.h): the PLL words, UART divisor, bit-bang phase length
TESTS   += hrc7000ClockTest

hrc7000ClockTest: hrc7000ClockTest.c $(ROOT)/platform/hrc7000/clock_math.h
	$(CC) $(CFLAGS) -Wshadow -o $@ hrc7000ClockTest.c

run: run-db25d
run-db25d: bk4819Test db25dCalTest bootDb25dTest hrc7000ClockTest
	./bk4819Test
	./db25dCalTest
	./bootDb25dTest
	./hrc7000ClockTest
.PHONY: run-db25d

# The host link's network personality: the IP gateway between a fake USB network interface and fake air ops (the
# radio's own services are mdnsTest's and tftpTest's)
HLNET   := hostlinkNetTest.c $(CORE)/hostlink/hostlink_net.c $(CORE)/hostlink/hostlink_net_air.c $(CORE)/dmr/ipGateway.c \
           $(CORE)/net/net.c $(CORE)/net/tcp.c $(CORE)/debuglink/dl_fmt.c $(DATA) $(FRAME)
HLNET_OFF := -DCONFIG_NET_KISS=0 -DCONFIG_NET_HTTP=0 -DCONFIG_NET_RIGCTL=0 -DCONFIG_NET_APRSIS=0 -DCONFIG_MDNS=0 -DCONFIG_TFTP=0 \
             -DCONFIG_NET_USRP=0
TESTS   += hostlinkNetTest

hostlinkNetTest: $(HLNET) $(CORE)/hostlink/hostlink_net.h $(CORE)/hostlink/hostlink_net_air.h $(CORE)/dmr/ipGateway.h $(ROOT)/hal/hal_netif.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow $(HLNET_OFF) -o $@ $(HLNET)

run: run-hlnet
run-hlnet: hostlinkNetTest
	./hostlinkNetTest
	python3 -I $(ROOT)/tools/bench/dmr_packet.py --self-test
.PHONY: run-hlnet

# PPP on the serial host link (core/net/ppp.c) and its glue to the real gateway (core/hostlink/hostlink_ppp.c), against
# a scripted peer, with a seeded fuzz loop (ODR_FUZZ_ITERS); pppPeer runs the same on a pseudo-terminal for pppd
# (tools/odr_ppp_interop.sh)
PPPT    := $(CORE)/net/ppp.c $(CORE)/hostlink/hostlink_ppp.c $(CORE)/dmr/ipGateway.c
TESTS   += pppTest

pppTest: pppTest.c fuzz.h $(PPPT) $(CORE)/net/ppp.h $(CORE)/hostlink/hostlink_ppp.h $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DCONFIG_NET_PPP=1 -o $@ pppTest.c $(PPPT)

PPPPEER := pppPeer.c $(PPPT) $(CORE)/net/tcp.c $(CORE)/debuglink/debuglink_proto.c $(CORE)/snmp/snmp.c \
           $(CORE)/snmp/snmpBer.c $(CORE)/snmp/snmpMib.c
TESTS   += pppPeer

pppPeer: $(PPPPEER) $(CORE)/net/ppp.h $(CORE)/net/tcp.h $(CORE)/hostlink/hostlink_ppp.h $(wildcard $(CORE)/snmp/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DCONFIG_NET_PPP=1 -o $@ $(PPPPEER)

run: run-ppp
run-ppp: pppTest pppPeer
	./pppTest
.PHONY: run-ppp

# The radio's own services on the USB network link, behind the real gateway (core/net): the mDNS responder, and the
# TFTP server with codeplug.bin on a simulated flash. Their frames go to pcaps that tshark checks when it's there
MDNST   := mdnsTest.c $(CORE)/net/mdns.c $(CORE)/dmr/ipGateway.c
TFTPT   := tftpTest.c $(CORE)/net/tftp.c $(CORE)/net/tftp_codeplug.c $(CORE)/dmr/ipGateway.c $(CORE)/codeplug/codeplug.c \
           $(CORE)/storage/storage.c sim/sim_flash.c
TESTS   += mdnsTest tftpTest

mdnsTest: $(MDNST) $(CORE)/net/mdns.h $(CORE)/dmr/ipGateway.h
	$(CC) $(CFLAGS) -Wshadow -o $@ $(MDNST)

tftpTest: $(TFTPT) $(CORE)/net/tftp.h $(CORE)/net/tftp_codeplug.h $(CORE)/dmr/ipGateway.h $(ROOT)/hal/hal_flash.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ $(TFTPT)

run: run-net
run-net: mdnsTest tftpTest
	./mdnsTest
	./tftpTest
ifneq ($(TSHARK),)
	@"$(TSHARK)" -r mdnsTest.pcap -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE -z expert,note -q > netTest.expert
	@"$(TSHARK)" -r tftpTest.pcap -o ip.check_checksum:TRUE -o udp.check_checksum:TRUE -z expert,note -q >> netTest.expert
	@# the TFTP ERROR packets the tests ask for are expected; anything else malformed or bad is not
	@if grep -i -E "malformed|bad|incorrect|error" netTest.expert | grep -v -E "TFTP ERROR packet|^Errors" | grep -q .; then \
		echo "# tshark found problems"; cat netTest.expert; exit 1; else echo "# tshark: mDNS and TFTP frames well formed"; fi
endif
.PHONY: run-net

# The sector journal and the settings store on a simulated NOR flash, with power cuts at every point of a write
STORE   := $(CORE)/storage/storage.c $(CORE)/settings/settings.c sim/sim_flash.c
TESTS   += settingsTest

settingsTest: settingsTest.c $(STORE) $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h sim/sim_flash.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ settingsTest.c $(STORE)

run: run-settings
run-settings: settingsTest spiNorTest spiNandTest nandSectorsTest
	./settingsTest
	./spiNorTest
	./spiNandTest
	./nandSectorsTest
.PHONY: run-settings

# The SPI NOR driver against a model of the chip at the bus, and the journal and settings store on it (16 MB and
# 1 MB parts, power cuts tearing page programs and erases)
TESTS   += spiNorTest

spiNorTest: spiNorTest.c $(ROOT)/drivers/flash/spi_nor/spi_nor.c $(ROOT)/drivers/flash/spi_nor/spi_nor.h \
            $(ROOT)/drivers/bus/spi_bus.h $(STORE) $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ spiNorTest.c $(ROOT)/drivers/flash/spi_nor/spi_nor.c \
		$(CORE)/storage/storage.c $(CORE)/settings/settings.c

# The SPI NAND driver against a model of the chip at the bus: ID, page reads through the cache, ECC status, bad-block
# marks, a chip that stays busy or doesn't answer; program loads, internal data moves, erase, the block lock, the write
# window, reported failures
TESTS   += spiNandTest

spiNandTest: spiNandTest.c $(ROOT)/drivers/flash/spi_nand/spi_nand.c $(ROOT)/drivers/flash/spi_nand/spi_nand.h \
             $(ROOT)/drivers/bus/spi_bus.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ spiNandTest.c $(ROOT)/drivers/flash/spi_nand/spi_nand.c

# NOR-like 4 KB sectors over NAND (drivers/flash/nand_sectors) on a simulated NAND: bad and worn blocks, garbage
# collection, wear leveling, a power cut at every operation of a write, an erase and a garbage collection (torn pages
# of several kinds), then core/storage and core/settings on top across reboots and cuts
# and the open-firmware CPS writing a whole codeplug through the DB25-D's sector replace, with cuts
NSECT   := $(ROOT)/drivers/flash/nand_sectors/nand_sectors.c $(CORE)/storage/storage.c $(CORE)/settings/settings.c \
           $(CORE)/cps/cps_openfw.c $(CORE)/codeplug/codeplug.c
TESTS   += nandSectorsTest

nandSectorsTest: nandSectorsTest.c $(NSECT) $(ROOT)/drivers/flash/nand_sectors/nand_sectors.h \
                 $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h $(CORE)/cps/*.h $(CORE)/codeplug/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -o $@ nandSectorsTest.c $(NSECT)

# Codeplug editing: the open-firmware record encoder, the built-in set made a stored codeplug, zones / contacts / lists,
# the round trip read -> write -> read, power cuts during an edit
CPEDIT  := $(CORE)/codeplug/codeplug.c $(CORE)/storage/storage.c sim/sim_flash.c
TESTS   += codeplugEditTest

codeplugEditTest: codeplugEditTest.c $(CPEDIT) $(wildcard $(CORE)/codeplug/*.h $(CORE)/storage/*.h sim/sim_flash.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -Wshadow -o $@ codeplugEditTest.c $(CPEDIT)

run: run-cpedit
run-cpedit: codeplugEditTest
	./codeplugEditTest
.PHONY: run-cpedit

# The open-firmware CPS shim against a simulated flash, driven like qdmr's UV380 driver (core/cps)
CPS     := $(CORE)/cps/cps_openfw.c $(CORE)/codeplug/codeplug.c $(CORE)/settings/settings.c $(CORE)/storage/storage.c \
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

# The shared PCM stream and USRP on the network address (core/usbaudio/audio_mux.c, usrp_net.c) against fake
# gateway, radio core and board
UNET    := usrpNetTest.c $(CORE)/usbaudio/usrp_net.c $(CORE)/usbaudio/usrp.c $(CORE)/usbaudio/pcm_slip.c \
           $(CORE)/usbaudio/audio_mux.c $(CORE)/debuglink/dl_fmt.c
TESTS   += usrpNetTest

usrpNetTest: $(UNET) $(wildcard $(CORE)/usbaudio/*.h)
	$(CC) $(CFLAGS) $(CORE_HAL) -DCONFIG_NET_USRP=1 -o $@ $(UNET) -lm

run: run-usbaudio
run-usbaudio: usrpTest usrpNetTest
	./usrpTest
	./usrpNetTest
.PHONY: run-usbaudio

# The minimal TCP (core/net/tcp.c) against a scripted peer; tshark checks the segments it wrote to tcpTest.pcap
TESTS   += tcpTest

tcpTest: tcpTest.c $(CORE)/net/tcp.c $(CORE)/net/tcp.h
	$(CC) $(CFLAGS) -Wshadow -o $@ tcpTest.c $(CORE)/net/tcp.c

run: run-tcp
run-tcp: tcpTest
	./tcpTest
ifneq ($(TSHARK),)
	@"$(TSHARK)" -r tcpTest.pcap -o ip.check_checksum:TRUE -o tcp.check_checksum:TRUE -T fields -e _ws.malformed -e _ws.expert.message 2>&1 \
		| grep -i -E "malformed|bad checksum|checksum.*incorrect" > tcpTest.expert || true
	@if [ -s tcpTest.expert ]; then echo "# tshark found problems in tcpTest.pcap"; cat tcpTest.expert; exit 1; else echo "# tshark: tcpTest.pcap has no malformed segments or bad checksums"; fi
endif
.PHONY: run-tcp

# The TCP services (core/net) through the real gateway and TCP, against a scripted TCP peer and fakes of the APRS
# engine and the radio core; tshark checks netSvcTest.pcap
NETSVC  := netSvcTest.c netSvcCases.c $(CORE)/net/net.c $(CORE)/net/tcp.c $(CORE)/net/kiss_tcp.c $(CORE)/aprs/kiss.c \
           $(CORE)/net/http.c $(CORE)/net/rigctld.c $(CORE)/net/aprsis.c \
           $(CORE)/dmr/ipGateway.c $(CORE)/debuglink/dl_fmt.c
TESTS   += netSvcTest

netSvcTest: $(NETSVC) netSvcFakes.h $(wildcard $(CORE)/net/*.h) $(CORE)/dmr/ipGateway.h gen/gz/http_assets_gen.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Igen/gz -Wshadow -DCONFIG_MDNS=0 -DCONFIG_TFTP=0 -DCONFIG_NET_USRP=0 -o $@ $(NETSVC)

run: run-netsvc
run-netsvc: netSvcTest
	./netSvcTest
ifneq ($(TSHARK),)
	@"$(TSHARK)" -r netSvcTest.pcap -o ip.check_checksum:TRUE -o tcp.check_checksum:TRUE -T fields -e _ws.malformed -e _ws.expert.message 2>&1 \
		| grep -i -E "malformed|bad checksum|checksum.*incorrect" > netSvcTest.expert || true
	@if [ -s netSvcTest.expert ]; then echo "# tshark found problems in netSvcTest.pcap"; cat netSvcTest.expert; exit 1; else echo "# tshark: netSvcTest.pcap has no malformed segments or bad checksums"; fi
endif
.PHONY: run-netsvc
# The SNMP agent (docs/spec/snmp.md): the BER codec against hand-worked X.690 encodings, SNMPv2c processing, the MIB and
# the notifications (snmpTest); then the agent on a loopback UDP socket against tools/odr_snmp.py and, when installed,
# the system's SNMP command line tools (snmp_peer_check.sh)
SNMPLIB := $(CORE)/snmp/snmp.c $(CORE)/snmp/snmpBer.c $(CORE)/snmp/snmpMib.c
TESTS   += snmpTest snmpPeer

snmpTest: snmpTest.c $(SNMPLIB) $(wildcard $(CORE)/snmp/*.h)
	$(CC) $(CFLAGS) -Wshadow -o $@ snmpTest.c $(SNMPLIB)

snmpPeer: snmpPeer.c $(SNMPLIB) $(wildcard $(CORE)/snmp/*.h)
	$(CC) $(CFLAGS) -Wshadow -o $@ snmpPeer.c $(SNMPLIB)

# The agent's firmware glue (snmp_agent.c: the USB and RF request paths, the RF mailbox and limits, settings, traps)
# on fakes of the radio, the settings and the gateway
TESTS   += snmpAgentTest
snmpAgentTest: snmpAgentTest.c fuzz.h $(CORE)/snmp/snmp_agent.c $(SNMPLIB) $(wildcard $(CORE)/snmp/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -DCONFIG_SNMP=1 -DCONFIG_DMR_IP_GATEWAY=1 -DCONFIG_HOTSPOT=0 -o $@ \
		snmpAgentTest.c $(CORE)/snmp/snmp_agent.c $(SNMPLIB)

run: run-snmp
run-snmp: snmpTest snmpPeer snmpAgentTest
	./snmpTest
	./snmpAgentTest
ifneq ($(PYTHON),)
	./snmp_peer_check.sh
else
	@echo "# snmp_peer_check skipped (no python3)"
endif
.PHONY: run-snmp

# The HTTP status server's asset table (tools/gen_http_assets.py), gzip (CONFIG_NET_HTTP_GZIP) and plain, as the
# firmware compiles it: every entry decompresses (or is) its file in core/net/http_assets, at its path, with its type
HTTP_ASSETS := $(sort $(wildcard $(ROOT)/core/net/http_assets/*))
TESTS   += httpAssetsTest httpAssetsTestPlain

gen/gz/http_assets_gen.h: $(HTTP_ASSETS) $(ROOT)/tools/gen_http_assets.py
	@python3 -I $(ROOT)/tools/gen_http_assets.py --gzip $@ $(HTTP_ASSETS) > /dev/null
	@touch $@
gen/plain/http_assets_gen.h: $(HTTP_ASSETS) $(ROOT)/tools/gen_http_assets.py
	@python3 -I $(ROOT)/tools/gen_http_assets.py $@ $(HTTP_ASSETS) > /dev/null
	@touch $@
httpAssetsTest: httpAssetsTest.c $(CORE)/net/http_assets.h gen/gz/http_assets_gen.h
	$(CC) $(CFLAGS) -Igen/gz -o $@ httpAssetsTest.c
httpAssetsTestPlain: httpAssetsTest.c $(CORE)/net/http_assets.h gen/plain/http_assets_gen.h
	$(CC) $(CFLAGS) -Igen/plain -o $@ httpAssetsTest.c

run: run-httpassets
run-httpassets: httpAssetsTest httpAssetsTestPlain
	@mkdir -p out/http_gz out/http_plain
	./httpAssetsTest out/http_gz
	python3 -I $(ROOT)/tools/gen_http_assets.py --check out/http_gz $(HTTP_ASSETS)
	./httpAssetsTestPlain out/http_plain
	python3 -I $(ROOT)/tools/gen_http_assets.py --check out/http_plain $(HTTP_ASSETS)
.PHONY: run-httpassets
