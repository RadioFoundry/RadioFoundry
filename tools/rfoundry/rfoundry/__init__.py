# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry: flash RadioFoundry release images, keeping or merging the radio's own codec (tools/rfoundry/README.md).

No module here imports pyusb or pyserial at load time: the planning, manifest and codec code runs (and is tested)
without them.
"""
__version__ = "0.1.0"
