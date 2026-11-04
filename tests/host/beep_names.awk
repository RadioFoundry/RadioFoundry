# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Generates beep_names.h for tests/host/beepReel.c from beep.h. beepReel needs each beep's name for its captions and
# WAV file names, and C can't list an enum's names, so we pull them out of the header's text. For every BEEP_X in the
# first enum, except BEEP_NONE and BEEP_COUNT, this prints:
#
#	{ "X", BEEP_X },
#
# This is a rough parse, not a C parser. beepReel checks the result against BEEP_COUNT when it starts, so a beep.h
# this can't handle gives a clear error instead of a missing beep.
#
# Usage: awk -f beep_names.awk beep.h > beep_names.h

# Strip comments so a beep mentioned in one isn't picked up. This only handles /* */ comments that open and close on
# the same line; beep.h uses // comments.
{
	sub(/\/\/.*/, "")
	gsub(/\/\*[^*]*\*\//, "")
}

# Start collecting at the first line that says "enum"
/enum/ {
	in_enum = 1
}

# Print every BEEP_ name on the line. There can be more than one
in_enum {
	rest = $0
	while (match(rest, /BEEP_[A-Z0-9_]+/)) {
		name = substr(rest, RSTART, RLENGTH)
		rest = substr(rest, RSTART + RLENGTH)
		if (name != "BEEP_NONE" && name != "BEEP_COUNT")
			printf "\t{ \"%s\", %s },\n", substr(name, length("BEEP_") + 1), name
	}
}

# Stop at the closing brace
in_enum && /}/ {
	in_enum = 0
}
