#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Build the DB25-D runtime-entry stubs (the ones stubtool loads with --rfoundry-enter) for both RAM layouts:
#   stub_db25d_rfoundry.bin      linked for 0x30000, 128 KB window   (a build without CONFIG_DMR: boards/db25d/fm_only.ld)
#   stub_db25d_rfoundry_dmr.bin  linked for 0x1D000, 72 KB window    (CONFIG_DMR=y: boards/db25d/memory.ld)
# The base and size must match HRC_STUB_BASE / HRC_STUB_SIZE in boards/db25d/config.mk: the running firmware reports
# them in STUB INFO and stubtool picks the image by that base. The monitor is position dependent, so a stub linked for
# the wrong base would run wild.
#
#   STUB_SRC=/path/to/monitor/source STUB_HOST=user@buildhost stub/build_db25d_stubs.sh [OUTDIR]
#
# STUB_SRC holds common/ and hrc7000/ of the monitor (kept with the reverse-engineering notes, not in this tree).
# STUB_HOST is a machine with docker and the csky-elf GCC 14 image (STUB_IMAGE, default rfoundry/csky-gcc14:latest); leave it
# empty to run docker here. STUB_W is the scratch directory on that machine. Then embed the results:
#   python3 stub/embed_stub.py stub/stubtool.py DB25D OUTDIR/stub_db25d_rfoundry.bin
#   python3 stub/embed_stub.py stub/stubtool.py DB25D_DMR OUTDIR/stub_db25d_rfoundry_dmr.bin
set -e
SRC=${STUB_SRC:?set STUB_SRC to the monitor source directory (common/ and hrc7000/)}
HOST=${STUB_HOST-}
W=${STUB_W:-/tmp/rfoundry-stub-build}
IMAGE=${STUB_IMAGE:-rfoundry/csky-gcc14:latest}
OUT=${1:-$(pwd)/stub-out}
mkdir -p "$OUT"

# Pins (UART1 = the K1 jack, power latch PTB13, power key PTB12) and the loader's 36 MHz APB, which RadioFoundry keeps.
COMMON="-DPORT_UART_BASE=0x14040000u -DPORT_POWER_LATCH=45u -DPORT_POWER_KEY=44u -DPORT_DIPLEX0_GPIO_CLR=0x600u -DPORT_TICK_HZ=36000000u -DPORT_UART_HZ=36000000u"
# name:base:length (linker script values take no u suffix)
VARIANTS="stub_db25d_rfoundry:0x30000:0x20000 stub_db25d_rfoundry_dmr:0x1D000:0x12000"

run() {
	if [ -n "$HOST" ]; then ssh "$HOST" "$1"; else sh -c "$1"; fi
}
if [ -n "$HOST" ]; then
	ssh "$HOST" "rm -rf $W/src && mkdir -p $W/src"
	COPYFILE_DISABLE=1 tar -C "$SRC" -cf - common hrc7000 | ssh "$HOST" "tar -C $W/src -xf -"
else
	rm -rf "$W/src" && mkdir -p "$W/src"
	COPYFILE_DISABLE=1 tar -C "$SRC" -cf - common hrc7000 | tar -C "$W/src" -xf -
fi

for v in $VARIANTS; do
	name=${v%%:*}; rest=${v#*:}; base=${rest%%:*}; len=${rest#*:}
	defs="$COMMON -DPORT_LOAD_ADDR=$base -DPORT_RAM_LEN=$len"
	run "cd $W/src && rm -f ../$name.* && docker run --rm -v $W:/w -w /w/src --user \$(id -u):\$(id -g) $IMAGE sh -c '
	  csky-elf-cpp -P $defs hrc7000/link.ld -o hrc7000/$name.ld &&
	  csky-elf-gcc $defs -mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float -Os -ffreestanding -fno-builtin -nostdlib \
	    -Wall -Wextra -Werror -Icommon -Ihrc7000 -Wl,-T,hrc7000/$name.ld,--no-warn-rwx-segments -o ../$name.elf \
	    hrc7000/start.S hrc7000/port.c common/core.c &&
	  csky-elf-objcopy -O binary ../$name.elf ../$name.bin && csky-elf-objdump -d ../$name.elf > ../$name.dis &&
	  csky-elf-size ../$name.elf'"
	if [ -n "$HOST" ]; then scp -q "$HOST:$W/$name.bin" "$HOST:$W/$name.dis" "$OUT/"; else cp "$W/$name.bin" "$W/$name.dis" "$OUT/"; fi
	echo "$name: base $base, window $len, $(wc -c < "$OUT/$name.bin") bytes, sha256 $(shasum -a 256 "$OUT/$name.bin" | cut -d' ' -f1)"
done
