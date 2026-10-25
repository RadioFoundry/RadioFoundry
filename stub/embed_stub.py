#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Put a built stub into stubtool.py between its `# --- STUB <TAG> begin/end` markers (base64 of zlib, plus its sha256).
    embed_stub.py stubtool.py DB25D stub_db25d_rfoundry.bin       (variables STUB_DB25D_SHA256 / STUB_DB25D_B64)
    embed_stub.py stubtool.py DB25D_DMR stub_db25d_rfoundry_dmr.bin"""
import base64
import hashlib
import sys
import zlib

target, tag, binfile = sys.argv[1:4]
blob = open(binfile, "rb").read()
b64 = base64.b64encode(zlib.compress(blob, 9)).decode()
var = "STUB_" + tag
lines = [var + "_SHA256 = \"%s\"" % hashlib.sha256(blob).hexdigest(), var + "_B64 = (",
         *["    \"%s\"" % b64[i:i + 100] for i in range(0, len(b64), 100)], ")"]
src = open(target).read()
a = src.index("# --- STUB %s begin" % tag)
b = src.index("# --- STUB %s end" % tag)
a = src.index("\n", a) + 1
open(target, "w").write(src[:a] + "\n".join(lines) + "\n" + src[b:])
print("embedded %d bytes as %s, sha256 %s" % (len(blob), var, hashlib.sha256(blob).hexdigest()))
