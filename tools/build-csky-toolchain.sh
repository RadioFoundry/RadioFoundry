#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Builds a csky-elf cross toolchain for the DM-32 (C-SKY CK803S): binutils, GCC (C only) and newlib, into
# ~/toolchains/csky-elf. Tested on Ubuntu 22.04 arm64 (4 cores, about 45 minutes):
#   sudo apt install build-essential libgmp-dev libmpfr-dev libmpc-dev libisl-dev texinfo bison flex
# newlib is built for -mcpu=ck803s only (no multilib): GCC 14 runs away (gigabytes of memory, no end) on the
# ck801 variants of strstr.c / memmem.c, and newlib's system.c needs -Wno-error=implicit-function-declaration.
# On macOS the build also needs --with-system-zlib and MAKEINFO=true; Linux is the tested route.
set -e
PREFIX=${PREFIX:-$HOME/toolchains/csky-elf}
SRC=${SRC:-$HOME/toolchains/src}
BV=2.43.1; GV=14.2.0; NV=4.5.0.20241231; J=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
export PATH=$PREFIX/bin:$PATH
mkdir -p $SRC && cd $SRC
[ -f binutils-$BV.tar.xz ] || curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-$BV.tar.xz
[ -f gcc-$GV.tar.xz ] || curl -fsSLO https://ftp.gnu.org/gnu/gcc/gcc-$GV/gcc-$GV.tar.xz
[ -f newlib-$NV.tar.gz ] || curl -fsSLO https://sourceware.org/pub/newlib/newlib-$NV.tar.gz
[ -d binutils-$BV ] || tar xf binutils-$BV.tar.xz
[ -d gcc-$GV ] || tar xf gcc-$GV.tar.xz
[ -d newlib-$NV ] || tar xf newlib-$NV.tar.gz

mkdir -p $SRC/b-binutils && cd $SRC/b-binutils
[ -f $PREFIX/bin/csky-elf-as ] || { ../binutils-$BV/configure --target=csky-elf --prefix=$PREFIX --disable-nls \
	--disable-werror --disable-gdb --disable-sim; make -j$J; make install; }

mkdir -p $SRC/b-gcc && cd $SRC/b-gcc
[ -f $PREFIX/bin/csky-elf-gcc ] || { ../gcc-$GV/configure --target=csky-elf --prefix=$PREFIX --enable-languages=c \
	--without-headers --with-newlib --disable-nls --disable-shared --disable-threads --disable-libssp \
	--disable-libquadmath --disable-libgomp --disable-libatomic; make -j$J all-gcc all-target-libgcc; \
	make install-gcc install-target-libgcc; }

mkdir -p $SRC/b-newlib && cd $SRC/b-newlib
[ -f $PREFIX/csky-elf/lib/libc.a ] || { ../newlib-$NV/configure --target=csky-elf --prefix=$PREFIX --disable-multilib \
	--enable-newlib-nano-malloc --disable-newlib-supplied-syscalls --enable-newlib-reent-small --disable-nls \
	CFLAGS_FOR_TARGET="-mcpu=ck803s -O2 -g -Wno-error=implicit-function-declaration"; make -j$J; make install; }

csky-elf-gcc --version | head -1
