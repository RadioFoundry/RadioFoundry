# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The MD-UV380's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included

HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The Plus 10 W marker against the image: a 5 W image warns on the marker, never refuses (#773)
TESTS   += mduv380VariantTest
mduv380VariantTest: $(HERE)variantTest.c $(HERE)../variant_mduv380.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: mduv380VariantTest
	./mduv380VariantTest
