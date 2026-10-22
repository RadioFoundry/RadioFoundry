#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Builds a csky-elf cross toolchain for the DM-32 (C-SKY CK803S): binutils, GCC (C only) and newlib, into
# ~/toolchains/csky-elf. Tested on Ubuntu 22.04 arm64 (4 cores, about 45 minutes):
#   sudo apt install build-essential libgmp-dev libmpfr-dev libmpc-dev libisl-dev texinfo bison flex
# newlib is built for -mcpu=ck803s only (no multilib): GCC 14 runs away (gigabytes of memory, no end) on the
# ck801 variants of strstr.c / memmem.c, and newlib's system.c needs -Wno-error=implicit-function-declaration.
# On macOS the build also needs --with-system-zlib and MAKEINFO=true; Linux is the tested route.
# CLEAN=1 deletes each stage's source and build tree once it's installed (peak about 4 GB instead of about 7 GB).
# tools/docker/csky-toolchain/ wraps this in a container and makes a CI image from the result.
set -e
PREFIX=${PREFIX:-$HOME/toolchains/csky-elf}
SRC=${SRC:-$HOME/toolchains/src}
# GCC's release is platform/hrc7000/platform.toml [toolchain] version; docker/csky-toolchain/build.sh passes it in GV,
# since only this script is in its container
GV=${GV:-$("$(dirname "$0")/platform_toolchain.sh" hrc7000 version)}
BV=2.43.1; NV=4.5.0.20241231; J=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
export PATH=$PREFIX/bin:$PATH
mkdir -p $SRC && cd $SRC
[ -f binutils-$BV.tar.xz ] || curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-$BV.tar.xz
[ -f gcc-$GV.tar.xz ] || curl -fsSLO https://ftp.gnu.org/gnu/gcc/gcc-$GV/gcc-$GV.tar.xz
[ -f newlib-$NV.tar.gz ] || curl -fsSLO https://sourceware.org/pub/newlib/newlib-$NV.tar.gz
# Checked against the GNU signatures (binutils, gcc) and sourceware's sha512.sum (newlib) when pinned
if command -v sha256sum >/dev/null; then SHA="sha256sum -c -"; else SHA="shasum -a 256 -c -"; fi
$SHA <<SUMS
13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd  binutils-$BV.tar.xz
a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9  gcc-$GV.tar.xz
33f12605e0054965996c25c1382b3e463b0af91799001f5bb8c0630f2ec8c852  newlib-$NV.tar.gz
SUMS
done_stage() { [ "${CLEAN:-0}" = 1 ] && rm -rf "$SRC/$1" "$SRC/$2"; cd "$SRC"; }

[ -f $PREFIX/bin/csky-elf-as ] || [ -d binutils-$BV ] || tar xf binutils-$BV.tar.xz
mkdir -p $SRC/b-binutils && cd $SRC/b-binutils
[ -f $PREFIX/bin/csky-elf-as ] || { ../binutils-$BV/configure --target=csky-elf --prefix=$PREFIX --disable-nls \
	--disable-werror --disable-gdb --disable-sim; make -j$J; make install-strip; }
done_stage binutils-$BV b-binutils

[ -f $PREFIX/bin/csky-elf-gcc ] || [ -d gcc-$GV ] || tar xf gcc-$GV.tar.xz
mkdir -p $SRC/b-gcc && cd $SRC/b-gcc
[ -f $PREFIX/bin/csky-elf-gcc ] || { ../gcc-$GV/configure --target=csky-elf --prefix=$PREFIX --enable-languages=c \
	--without-headers --with-newlib --disable-nls --disable-shared --disable-threads --disable-libssp \
	--disable-libquadmath --disable-libgomp --disable-libatomic; make -j$J all-gcc all-target-libgcc; \
	make install-strip-gcc install-target-libgcc; }
done_stage gcc-$GV b-gcc

[ -f $PREFIX/csky-elf/lib/libc.a ] || [ -d newlib-$NV ] || tar xf newlib-$NV.tar.gz
mkdir -p $SRC/b-newlib && cd $SRC/b-newlib
[ -f $PREFIX/csky-elf/lib/libc.a ] || { ../newlib-$NV/configure --target=csky-elf --prefix=$PREFIX --disable-multilib \
	--enable-newlib-nano-malloc --disable-newlib-supplied-syscalls --enable-newlib-reent-small --disable-nls \
	CFLAGS_FOR_TARGET="-mcpu=ck803s -O2 -g -Wno-error=implicit-function-declaration"; make -j$J; make install; }
done_stage newlib-$NV b-newlib

csky-elf-gcc --version | head -1
