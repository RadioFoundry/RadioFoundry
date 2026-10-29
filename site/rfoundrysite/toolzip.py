# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""downloads/rfoundry-tool.zip: the rfoundry command line tool from the site's own commit, for the web flasher's
manual install kit (static/js/flasher/manual.js, docs/dev/web-flasher.md "The manual install kit").

No release carries an rfoundry executable yet, and the tool must match the site: manifest.verify refuses a codec
profile or a board it doesn't know. So the site ships the sources, in the layout serialflash.py expects (it loads
tools/serial_loader.py, tools/hrc7000_codec.py and stub/stubtool.py by path from the package's grandparent), with a
launcher at the top that needs no install. The zip is reproducible: sorted entries, the commit's time on each.
"""
import hashlib
import io
import os
import time
import zipfile

from . import repo

NAME = "rfoundry-tool.zip"
# what the launcher and the serial route import, and what tells a user what they have
FILES = ("LICENSE", "AUTHORS", "tools/rfoundry/README.md", "tools/rfoundry/pyproject.toml", "tools/serial_loader.py",
	 "tools/hrc7000_codec.py", "stub/stubtool.py", "stub/f4stub.py", "stub/USER-GUIDE.md")
PACKAGE = "tools/rfoundry/rfoundry"

# python3 rfoundry.py ... from the unpacked folder: the package and the tools beside it, wherever the folder is
LAUNCHER = '''\
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""rfoundry from this folder, without installing it: python3 rfoundry.py --help (README.txt)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# this file's own folder would make "import rfoundry" load this script, not the package
sys.path[:] = [p for p in sys.path if os.path.abspath(p or os.curdir) != HERE]
sys.path.insert(0, os.path.join(HERE, "tools", "rfoundry"))

from rfoundry.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
'''

REQUIREMENTS = """\
# rfoundry's dependencies (tools/rfoundry/pyproject.toml): python -m pip install -r requirements.txt
pyusb>=1.2.1
libusb-package>=1.0.26.1
pyserial>=3.5
certifi
"""


def package_files():
	"""The package's files: tracked by git, else everything under it (a source tree without git), never caches or
	local notes."""
	listed = repo.git("ls-files", PACKAGE).splitlines()
	if not listed:
		top = repo.path(*PACKAGE.split("/"))
		for d, dirs, files in os.walk(top):
			dirs[:] = [x for x in dirs if x != "__pycache__"]
			listed += [os.path.relpath(os.path.join(d, f), repo.ROOT).replace(os.sep, "/") for f in files]
	return sorted(p for p in listed if "__pycache__" not in p and not p.endswith((".pyc", ".local.md"))
		      and os.path.isfile(repo.path(*p.split("/"))))


def entries():
	"""[(name in the zip, bytes)], sorted"""
	out = {"rfoundry.py": LAUNCHER.encode(), "requirements.txt": REQUIREMENTS.encode()}
	for rel in list(FILES) + package_files():
		with open(repo.path(*rel.split("/")), "rb") as f:
			out[rel] = f.read()
	return sorted(out.items())


def build():
	"""The zip's bytes"""
	try:
		stamp = time.gmtime(int(repo.git("log", "-1", "--format=%ct", default="0") or 0))[:6]
	except ValueError:
		stamp = (1980, 1, 1, 0, 0, 0)
	stamp = max(stamp, (1980, 1, 1, 0, 0, 0))	# the zip format's first date
	buf = io.BytesIO()
	with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
		for name, data in entries():
			info = zipfile.ZipInfo(name, stamp)
			info.compress_type = zipfile.ZIP_DEFLATED
			info.create_system = 3	# the same bytes from every build machine
			info.external_attr = 0o644 << 16
			z.writestr(info, data)
	return buf.getvalue()


def write(out_dir):
	"""Writes out_dir/rfoundry-tool.zip; returns site.json's "tool": {zip, sha256, size} (zip relative to the site
	root)."""
	data = build()
	os.makedirs(out_dir, exist_ok=True)
	with open(os.path.join(out_dir, NAME), "wb") as f:
		f.write(data)
	return {"zip": "downloads/" + NAME, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
