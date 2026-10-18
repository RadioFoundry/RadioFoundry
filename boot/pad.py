#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Pad a first stage to its flash sector with 0xFF (erased flash) and check it starts with the signature its loader
# looks for before it starts an application (boot/README.md). Fails, and writes nothing, when either is wrong.
#   pad.py RAW OUT SIZE SIGNATURE_HEX
import sys


def main():
	raw, out, size, sig = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), bytes.fromhex(sys.argv[4])
	data = open(raw, "rb").read()
	if len(data) > size:
		sys.exit(f"{raw}: {len(data)} bytes, more than its {size}-byte sector")
	if not data.startswith(sig):
		sys.exit(f"{raw} does not start with {sig.hex(' ')}: the loader would not start it")
	open(out, "wb").write(data + b"\xff" * (size - len(data)))
	print(f"first stage {out}: {len(data)} of {size} bytes")


if __name__ == "__main__":
	main()
