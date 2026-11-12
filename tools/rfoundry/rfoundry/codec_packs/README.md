<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Codec packs

One `rfoundry-codec-pack/1` file per supported stock build of a radio whose codec is spread over several parts of the stock
image, `<board>/<profile>.json`. They are read by `rfoundry/k22fcodec.py` and tell the merge what to move and what to
rewrite: the three parts (main block, low block, table window) with their addresses, lengths and SHA-256, the words
that hold a code address, the branches that cross between parts, and the entry points. Addresses, lengths and hashes
only, never codec bytes. Format and use: [docs/spec/codec-gd77.md](../../../../docs/spec/codec-gd77.md).
