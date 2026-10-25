#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Print a Python 3.11 or later (tools/boards.py needs tomllib): $RFOUNDRY_PYTHON if set, else the newest python3.N on PATH
# with N >= 11, else python3 if it is new enough. Use as: PY=$(tools/rfoundry_python.sh) || exit 1
ok() { "$1" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; }
if [ -n "$RFOUNDRY_PYTHON" ]; then
	if ok "$RFOUNDRY_PYTHON"; then echo "$RFOUNDRY_PYTHON"; exit 0; fi
	echo "rfoundry_python: RFOUNDRY_PYTHON=$RFOUNDRY_PYTHON is not a Python 3.11 or later" >&2
	exit 1
fi
best= bestn=0
IFS=:
for d in $PATH; do
	for p in "$d"/python3.[0-9]*; do
		n=${p##*/python3.}
		case $n in ''|*[!0-9]*) continue ;; esac
		[ "$n" -ge 11 ] && [ "$n" -gt "$bestn" ] && [ -x "$p" ] && ok "$p" && { best=$p; bestn=$n; }
	done
done
unset IFS
[ -n "$best" ] && { echo "$best"; exit 0; }
p=$(command -v python3) && ok "$p" && { echo "$p"; exit 0; }
echo "rfoundry_python: no Python 3.11 or later found (tried \$RFOUNDRY_PYTHON, python3.N on PATH, python3); install one or set RFOUNDRY_PYTHON" >&2
exit 1
