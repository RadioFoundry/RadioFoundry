#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
#
# Installs the pinned Arm GNU Toolchain (Linux x86_64) into ~/opt, where the Makefile looks first, trimmed to what the
# STM32F4 boards use: the Cortex-M4 hard-float multilib (thumb/v7e-m+fp), C and LTO only, no documentation, no
# debugger. About 215 MB instead of 1 GB, which matters on a runner whose job containers share a small disk.
#
#   tools/install-arm-toolchain.sh             (CI: docs/dev/ci.md; release.yml pins the same version)
#   ARM_GCC_VERSION=... ARM_GCC_SHA256=... tools/install-arm-toolchain.sh
#   FULL=1 tools/install-arm-toolchain.sh      every multilib and tool, as Arm ships it
#
# Does nothing when ~/opt/arm-gnu-toolchain-<version>-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc already runs (a CI
# cache restored it). Downloads to $RUNNER_TEMP (or /tmp) and checks the SHA-256 before extracting.
set -eu
VER=${ARM_GCC_VERSION:-13.3.rel1}
SHA=${ARM_GCC_SHA256:-95c011cee430e64dd6087c75c800f04b9c49832cc1000127a92a97f9c8d83af4}
NAME="arm-gnu-toolchain-${VER}-x86_64-arm-none-eabi"
DEST="$HOME/opt"
TC="$DEST/$NAME"

if "$TC/bin/arm-none-eabi-gcc" --version > /dev/null 2>&1; then
	echo "install-arm-toolchain: $TC already there"
	"$TC/bin/arm-none-eabi-gcc" --version | head -1
	exit 0
fi
TMP=${RUNNER_TEMP:-/tmp}
f="$TMP/$NAME.tar.xz"
curl -fsSL --retry 3 -o "$f" "https://developer.arm.com/-/media/Files/downloads/gnu/${VER}/binrel/$NAME.tar.xz"
echo "$SHA  $f" | sha256sum -c -
mkdir -p "$DEST"
if [ "${FULL:-0}" = 1 ]; then
	tar -xJf "$f" -C "$DEST"
else
	# leave out the other multilibs, C++/Fortran front ends, gdb and the manuals while extracting (never more on
	# disk than the trimmed tree), then remove any multilib the patterns missed
	tar -xJf "$f" -C "$DEST" --wildcards \
		--exclude="$NAME/share" --exclude="$NAME/bin/*gdb*" \
		--exclude="$NAME/libexec/gcc/arm-none-eabi/*/cc1plus" --exclude="$NAME/libexec/gcc/arm-none-eabi/*/f951" \
		--exclude="$NAME/arm-none-eabi/lib/arm" --exclude="$NAME/lib/gcc/arm-none-eabi/*/arm" \
		--exclude="$NAME/arm-none-eabi/lib/thumb/v[68]*" --exclude="$NAME/lib/gcc/arm-none-eabi/*/thumb/v[68]*" \
		--exclude="$NAME/arm-none-eabi/lib/thumb/v7-*" --exclude="$NAME/lib/gcc/arm-none-eabi/*/thumb/v7-*" \
		--exclude="$NAME/arm-none-eabi/lib/thumb/nofp" --exclude="$NAME/lib/gcc/arm-none-eabi/*/thumb/nofp"
	for d in "$TC"/arm-none-eabi/lib/thumb/* "$TC"/lib/gcc/arm-none-eabi/*/thumb/*; do
		[ "$(basename "$d")" = "v7e-m+fp" ] || rm -rf "$d"
	done
fi
rm -f "$f"
"$TC/bin/arm-none-eabi-gcc" --version | head -1
[ "${FULL:-0}" = 1 ] || [ "$("$TC/bin/arm-none-eabi-gcc" -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
	-print-multi-directory)" = "thumb/v7e-m+fp/hard" ]
du -sh "$TC"
