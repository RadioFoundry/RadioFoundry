#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Copyright (c) 2026 RadioFoundry Theme Pack contributors
"""Stage the local theme pack uploader at build/theme-packs/uploader.html."""

from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "theme_pack_uploader.html"
OUTPUT = ROOT / "build" / "theme-packs" / "uploader.html"


def main() -> int:
	OUTPUT.parent.mkdir(parents=True, exist_ok=True)
	shutil.copyfile(SOURCE, OUTPUT)
	print(f"Staged local theme pack uploader: {OUTPUT.relative_to(ROOT)}")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
