# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Imported first by the test modules that run rfoundry: its settings folder (state.config_dir, with the saved codec
copies, keys and the remembered probe) points into a temporary folder, so a test never reads or writes the user's."""
import atexit
import os
import shutil
import tempfile

DIR = tempfile.mkdtemp(prefix="rfoundry-tests-")
atexit.register(shutil.rmtree, DIR, True)
os.environ.update(HOME=DIR, XDG_CONFIG_HOME=os.path.join(DIR, ".config"), APPDATA=os.path.join(DIR, "appdata"))
