# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""python -m rfoundry, and the PyInstaller entry point."""
import os
import sys

if __package__ in (None, ""):	# run as a script (PyInstaller): make the package importable
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from rfoundry.cli import main
else:
    from .cli import main

if __name__ == "__main__":
    sys.exit(main())
